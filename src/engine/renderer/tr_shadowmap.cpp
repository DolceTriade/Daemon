/*
===========================================================================

Daemon GPL Source Code
Copyright (C) 2024 Daemon Developers

This file is part of the Daemon GPL Source Code (Daemon Source Code).

Daemon Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Daemon Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

// tr_shadowmap.cpp - Shadow mapping system implementation

#include "tr_local.h" // Now includes all shadow mapping structs
#include "Material.h"
#include "gl_shader.h"
#include <algorithm>
#include <cmath>
#include <vector>

static constexpr int kPointLightFaceCount = 6;
static Cvar::Cvar<float> r_virtualSpotLightMaxViewDistance(
	"r_virtualSpotLightMaxViewDistance",
	"maximum distance from the camera for a virtual spotlight to be considered (0 to disable)",
	Cvar::NONE,
	2048.0f );

static inline bool IsVirtualSpotLight(const refLight_t& light) {
	return (light.flags & REF_INVERSE_DLIGHT) && light.rlType == refLightType_t::RL_PROJ;
}

static inline bool WithinVirtualSpotlightViewDistance(const refLight_t& light, float maxDist, float maxDistSq, const vec3_t viewOrg) {
	if (maxDistSq < 0.0f) {
		return true;
	}

	vec3_t delta;
	VectorSubtract(light.origin, viewOrg, delta);
	float distanceSq = VectorLengthSquared(delta);
	float allowed = maxDist + std::max(light.radius, 0.0f);
	if (allowed <= 0.0f) {
		return false;
	}
	return distanceSq <= allowed * allowed;
}

namespace {

struct ViewPVSInfo {
	const byte* mask = nullptr;
	bool enabled = false;
};

static ViewPVSInfo BuildViewPVSInfo(const vec3_t viewOrg) {
	ViewPVSInfo info;
	if (!tr.world || !tr.world->vis) {
		return info;
	}
	if (r_novis && r_novis->integer) {
		return info;
	}

	bspNode_t* viewLeaf = R_PointInLeaf(viewOrg);
	if (!viewLeaf || viewLeaf->cluster < 0) {
		return info;
	}

	info.mask = R_ClusterPVS(viewLeaf->cluster);
	info.enabled = info.mask != nullptr;
	return info;
}

static bool LightVisibleFromView(const refLight_t& light, const ViewPVSInfo& viewPVS) {
	if (!viewPVS.enabled) {
		return true;
	}

	if (light.rlType == refLightType_t::RL_DIRECTIONAL) {
		return true;
	}

	bspNode_t* lightLeaf = R_PointInLeaf(light.origin);
	if (!lightLeaf) {
		return true;
	}

	if (lightLeaf->area >= 0) {
		const byte mask = tr.refdef.areamask[lightLeaf->area >> 3];
		if (mask & (1 << (lightLeaf->area & 7))) {
			return false;
		}
	}

	if (lightLeaf->cluster < 0) {
		return true;
	}

	return (viewPVS.mask[lightLeaf->cluster >> 3] & (1 << (lightLeaf->cluster & 7))) != 0;
}

static int PromoteVisibleLights(refLight_t* lights, int count, const ViewPVSInfo& viewPVS) {
	if (!viewPVS.enabled || count <= 1) {
		return count;
	}

	int insertPos = 0;
	for (int i = 0; i < count; ++i) {
		if (LightVisibleFromView(lights[i], viewPVS)) {
			if (i != insertPos) {
				std::rotate(lights + insertPos, lights + i, lights + i + 1);
			}
			insertPos++;
		}
	}

	return insertPos;
}

struct VirtualLightSample {
	vec3_t direction;
	vec3_t color;
	float intensity;
};

static const float kVirtualSampleDirections[][3] = {
	{ 1.0f,  0.0f,  0.0f },
	{-1.0f,  0.0f,  0.0f },
	{ 0.0f,  1.0f,  0.0f },
	{ 0.0f, -1.0f,  0.0f },
	{ 0.0f,  0.0f,  1.0f },
	{ 0.0f,  0.0f, -1.0f },
	{ 0.7071f,  0.7071f,  0.0f },
	{-0.7071f,  0.7071f,  0.0f },
	{ 0.7071f, -0.7071f,  0.0f },
	{-0.7071f, -0.7071f,  0.0f },
	{ 0.0f,  0.7071f,  0.7071f },
	{ 0.0f, -0.7071f,  0.7071f },
	{ 0.7071f,  0.0f,  0.7071f },
	{-0.7071f,  0.0f,  0.7071f }
};

static constexpr int kVirtualSampleCount = sizeof(kVirtualSampleDirections) / sizeof(kVirtualSampleDirections[0]);

}
static void GetCubeFaceBasis(int face, vec3_t forward, vec3_t up) {
    static const vec3_t forwardDirs[kPointLightFaceCount] = {
        {  1.0f,  0.0f,  0.0f }, // +X
        { -1.0f,  0.0f,  0.0f }, // -X
        {  0.0f,  1.0f,  0.0f }, // +Y
        {  0.0f, -1.0f,  0.0f }, // -Y
        {  0.0f,  0.0f,  1.0f }, // +Z
        {  0.0f,  0.0f, -1.0f }  // -Z
    };
    static const vec3_t upDirs[kPointLightFaceCount] = {
        { 0.0f, -1.0f,  0.0f }, // +X
        { 0.0f, -1.0f,  0.0f }, // -X
        { 0.0f,  0.0f,  1.0f }, // +Y
        { 0.0f,  0.0f, -1.0f }, // -Y
        { 0.0f, -1.0f,  0.0f }, // +Z
        { 0.0f, -1.0f,  0.0f }  // -Z
    };

    if (face < 0 || face >= kPointLightFaceCount) {
        VectorSet(forward, 0.0f, 0.0f, -1.0f);
        VectorSet(up, 0.0f, 1.0f, 0.0f);
        return;
    }

    VectorCopy(forwardDirs[face], forward);
    VectorCopy(upDirs[face], up);
}

static float R_ComputeEffectiveLightRange(const refLight_t* light) {
	const float baseRadius = std::max(light->radius, 1.0f);
	if (baseRadius <= 0.0f) {
		return 0.0f;
	}

	float threshold = std::max(r_lightFalloffThreshold.Get(), 1e-6f);
	float maxColor = std::max(light->color[0], std::max(light->color[1], light->color[2]));
	maxColor *= 4.0f * light->scale;

	if (maxColor <= 0.0f) {
		return baseRadius;
	}

	float ratio = threshold / maxColor;
	if (ratio >= 1.0f) {
		return baseRadius;
	}

	ratio = std::max(ratio, 1e-6f);
	float powTerm = std::pow(1.0f / ratio, 0.25f);
	if (!std::isfinite(powTerm) || powTerm <= 1.0f) {
		return baseRadius;
	}

	const float attenuationSlope = 2.57f;
	float distance = baseRadius * (powTerm - 1.0f) / attenuationSlope;
	if (!std::isfinite(distance) || distance <= 0.0f) {
		return baseRadius;
	}

	return std::max(baseRadius, distance);
}

// Global shadow map manager instance
ShadowMapManager shadowMapManager; // Still a singleton, but operates on passed shadowData_t

// Map a 0..1 user scale into a technique-safe exponent to avoid overflow/clamping
float R_ComputeESMExponent( shadowingMode_t technique, float scale ) {
    if (scale < 0.0f) scale = 0.0f; else if (scale > 1.0f) scale = 1.0f;
    float maxK = 0.0f;
    switch (technique) {
        case shadowingMode_t::SHADOWING_ESM16:  maxK = 11.0f; break; // ~ln(max float16)
        case shadowingMode_t::SHADOWING_ESM32:  maxK = 80.0f; break; // conservative under ln(max float32)
        case shadowingMode_t::SHADOWING_EVSM32: maxK = 80.0f; break;
        default: maxK = 0.0f; break;
    }
    return scale * maxK;
}

//
// ShadowMapManager Implementation
//

void ShadowMapManager::Init(shadowData_t *sd) {
	Log::Debug("Initializing shadow mapping system...");
	// Reset state
	memset(&sd->shadowAtlas, 0, sizeof( sd->shadowAtlas));
	memset(sd->lightShadows, 0, sizeof(sd->lightShadows));
	sd->numShadowLights = 0;
	memset(sd->sceneLightHasShadows, 0, sizeof(sd->sceneLightHasShadows));

	// Only initialize if shadow mapping is enabled and material system is active
	if (!IsShadowMappingEnabled()) {
		Log::Debug("Shadow mapping disabled or material system not active");
		return;
	}

	// Initialize shadow atlas
	InitAtlas(&sd->shadowAtlas);

	Log::Debug("Shadow mapping system initialized");
}

void ShadowMapManager::Shutdown(shadowData_t *sd) {
	Log::Debug("Shutting down shadow mapping system...");
	ShutdownAtlas(&sd->shadowAtlas);

	// Reset state
	sd->shadowAtlas = {};
	memset(sd->lightShadows, 0, sizeof(sd->lightShadows));
	sd->numShadowLights = 0;
	memset(sd->sceneLightHasShadows, 0, sizeof(sd->sceneLightHasShadows));

	Log::Debug("Shadow mapping system shut down");
}

void ShadowMapManager::BeginFrame() {
    if (!IsShadowMappingEnabled()) {
        return;
    }

    // Frontend writes to the current frontend buffer (tr.smpFrame)
    shadowData_t *sd = &backEndData[tr.smpFrame]->shadowData;
	// Reset per-frame data
	sd->numShadowLights = 0;
	// Don't reset numShadowOnlyLights here - they're set during frontend processing
	memset(sd->sceneLightHasShadows, 0, sizeof(sd->sceneLightHasShadows));

	// Reset atlas regions for dynamic allocation
	for (auto& region : sd->shadowAtlas.regions) {
		region.allocated = false;
		region.lightIndex = -1;
		region.cascadeIndex = -1;
	}
    sd->shadowAtlas.allocatedRegions = 0;

    // No per-scene shadow map indirection kept on CPU anymore
}

void ShadowMapManager::EndFrame() {
    if (!IsShadowMappingEnabled()) {
        return;
    }
    // Frontend writes to the current frontend buffer (tr.smpFrame)
    shadowData_t *sd = &backEndData[tr.smpFrame]->shadowData;
}

bool ShadowMapManager::IsShadowMappingEnabled() const {
	// Shadow mapping requires material system to be enabled
	if (!glConfig.usingMaterialSystem) {
		return false;
	}

	shadowingMode_t technique = static_cast<shadowingMode_t>(r_shadows.Get());
	return technique >= shadowingMode_t::SHADOWING_ESM16 && technique <= shadowingMode_t::SHADOWING_EVSM32;
}

shadowingMode_t ShadowMapManager::GetShadowTechnique() const {
	return static_cast<shadowingMode_t>(r_shadows.Get());
}

image_t* ShadowMapManager::GetShadowAtlas(const shadowAtlas_t* atlas) const {
	return atlas->colorImage;
}

int ShadowMapManager::GetNumShadowLights() const {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;
	return sd->numShadowLights;
}

void ShadowMapManager::InitAtlas(shadowAtlas_t* atlas) {
	int atlasSize = r_shadowAtlasSize.Get();

	Log::Debug("Creating shadow atlas: %dx%d", atlasSize, atlasSize);

	atlas->size = atlasSize;
	atlas->allocatedRegions = 0;

	// Create atlas images based on technique
	shadowingMode_t technique = GetShadowTechnique();

	// Create atlas images based on technique
	// Disable mipmaps on the atlas to avoid cross-tile sampling from lower mips
    // which can look like write-path contamination in debug views and cause
    // trilinear bleed between adjacent tiles during shading.
	int flags = IF_NOPICMIP;

	switch (technique) {
		case shadowingMode_t::SHADOWING_ESM16:
			flags |= IF_ONECOMP16F;
			break;

		case shadowingMode_t::SHADOWING_ESM32:
			flags |= IF_ONECOMP32F;
			break;

		case shadowingMode_t::SHADOWING_VSM16:
		case shadowingMode_t::SHADOWING_EVSM32:
			flags |= (technique == shadowingMode_t::SHADOWING_VSM16) ? IF_TWOCOMP16F : IF_TWOCOMP32F;
			break;

		case shadowingMode_t::SHADOWING_VSM32:
			flags |= IF_TWOCOMP32F;
			break;

		default:
			Sys::Drop("Invalid shadow mapping technique: %d", static_cast<int>(technique));
			break;
	}

    // Create color texture with appropriate format
    imageParams_t imageParams = {};
    imageParams.bits = flags;
    imageParams.filterType = filterType_t::FT_LINEAR;
    imageParams.wrapType = wrapTypeEnum_t::WT_CLAMP;

    atlas->colorImage = R_CreateImage(va("*shadowAtlas_%d", atlasSize), nullptr, atlasSize, atlasSize, 1, imageParams);

    // Create depth buffer for all techniques
    imageParams_t depthParams = {};
    depthParams.bits = IF_DEPTH24 | IF_NOPICMIP;
    depthParams.filterType = filterType_t::FT_NEAREST;
    depthParams.wrapType = wrapTypeEnum_t::WT_ONE_CLAMP;

    atlas->depthImage = R_CreateImage(va("*shadowAtlasDepth_%d", atlasSize), nullptr, atlasSize, atlasSize, 1, depthParams);

    // Create temporary color texture/FBO for blur passes (same format as colorImage)
    atlas->tempImage = R_CreateImage(va("*shadowAtlasTemp_%d", atlasSize), nullptr, atlasSize, atlasSize, 1, imageParams);
    atlas->tempFBO = R_CreateFBO(va("*shadowAtlasTemp_%d", atlasSize), atlasSize, atlasSize);
    R_BindFBO(atlas->tempFBO);
    if (atlas->tempImage) {
        R_AttachFBOTexture2D(GL_TEXTURE_2D, atlas->tempImage->texnum, 0);
    }
    if (!R_CheckFBO(atlas->tempFBO)) {
        Log::Warn("Shadow atlas temp FBO is not complete");
    }

	// Create FBO
	CreateShadowMapFBO(atlas);

	Log::Debug("Shadow atlas created successfully");
}

void ShadowMapManager::ShutdownAtlas(shadowAtlas_t* atlas) {
    // FBOs are cleaned up by R_ShutdownFBOs(), images by image manager
    atlas->fbo = nullptr;
    atlas->colorImage = nullptr;
    atlas->depthImage = nullptr;
    atlas->tempFBO = nullptr;
    atlas->tempImage = nullptr;
}

void ShadowMapManager::CreateShadowMapFBO(shadowAtlas_t* atlas) {
	atlas->fbo = R_CreateFBO(va("*shadowAtlas_%d", atlas->size), atlas->size, atlas->size);

	if (!atlas->fbo) {
		Sys::Drop("Failed to create shadow atlas FBO");
	}

	R_BindFBO(atlas->fbo);

	// Attach color texture (for VSM/EVSM techniques)
	if (atlas->colorImage) {
		R_AttachFBOTexture2D(GL_TEXTURE_2D, atlas->colorImage->texnum, 0);
	}

	// Attach depth texture
	if (atlas->depthImage) {
		R_AttachFBOTextureDepth(atlas->depthImage->texnum);
	}

	// Check FBO completeness
	if (!R_CheckFBO(atlas->fbo)) {
		Sys::Drop("Shadow atlas FBO is not complete");
	}

	R_BindNullFBO();
}

bool ShadowMapManager::AllocateAtlasRegion(shadowAtlas_t* atlas, int width, int height, vec2_t offset, int sceneIndex, int cascade) {
	Log::Debug("Attempting to allocate atlas region %dx%d for light %d cascade %d in %dx%d atlas with %d existing regions",
	          width, height, sceneIndex, cascade, atlas->size, atlas->size, atlas->allocatedRegions);

	// Simple allocation strategy: find first free region that fits
	// TODO: Implement more sophisticated allocation (bin packing, etc.)

	if (atlas->allocatedRegions >= MAX_SHADOW_REGIONS) {
		Log::Warn("Exceeded number of atlas regions: %d (max: %d)", atlas->allocatedRegions, MAX_SHADOW_REGIONS);
		return false;
	}

	for (int y = 0; y <= atlas->size - height; y += height) {
		for (int x = 0; x <= atlas->size - width; x += width) {
			bool canAllocate = true;

			// Check if this region overlaps with any allocated region
			for (size_t i = 0; i < atlas->allocatedRegions; ++i) {
				atlasRegion_t &region = atlas->regions[i];
				if (!region.allocated) continue;

				if (!(x >= region.offset[0] + region.size[0] ||
				      x + width <= region.offset[0] ||
				      y >= region.offset[1] + region.size[1] ||
				      y + height <= region.offset[1])) {
					canAllocate = false;
					break;
				}
			}

			if (canAllocate) {
				// Allocate the region
				atlasRegion_t &newRegion = atlas->regions[atlas->allocatedRegions];
				newRegion.offset[0] = x;
				newRegion.offset[1] = y;
				newRegion.size[0] = width;
				newRegion.size[1] = height;
                newRegion.allocated = true;
                newRegion.lightIndex = sceneIndex; // store scene light index for stability across sorting
                newRegion.cascadeIndex = cascade;

				atlas->allocatedRegions++;

				offset[0] = x;
				offset[1] = y;

                Log::Debug("Successfully allocated region at (%d, %d) size %dx%d for scene light %d cascade %d",
                          x, y, width, height, sceneIndex, cascade);
                return true;
            }
        }
    }

	Log::Warn("Failed to allocate shadow atlas region %dx%d - atlas may be full", width, height);
	return false;
}

void ShadowMapManager::SetupESMParams(shadowMap_t* shadowMap) {
	shadowMap->params.esm.exponent = r_shadowESMExponent.Get();
}

void ShadowMapManager::SetupVSMParams(shadowMap_t* shadowMap) {
	shadowMap->params.vsm.minVariance = 0.0001f; // Reasonable default
	shadowMap->params.vsm.blurRadius = r_shadowVSMBlur.Get();
}

void ShadowMapManager::SetupEVSMParams(shadowMap_t* shadowMap) {
	shadowMap->params.evsm.exponent = r_shadowESMExponent.Get();
	shadowMap->params.evsm.minVariance = 0.0001f; // Reasonable default
}

void ShadowMapManager::GenerateVirtualInverseShadowLights() {
	int desiredLights = r_inverseShadowLights.Get();
	if (desiredLights <= 0) {
		return;
	}

	if (tr.refdef.numLights >= MAX_REF_LIGHTS) {
		return;
	}

	int existingInverse = 0;
	for (int i = 0; i < tr.refdef.numLights; ++i) {
		if (tr.refdef.lights[i].flags & REF_INVERSE_DLIGHT) {
			existingInverse++;
		}
	}

	if (existingInverse >= desiredLights) {
		return;
	}

	int targetAdditional = desiredLights - existingInverse;

	vec3_t viewOrigin;
	VectorCopy(tr.refdef.vieworg, viewOrigin);

	float sampleRadius = r_inverseShadowSampleRadius.Get();
	float intensityScale = r_inverseShadowIntensityScale.Get();

	std::vector<VirtualLightSample> samples;
	samples.reserve(kVirtualSampleCount);

	for (int i = 0; i < kVirtualSampleCount; ++i) {
		vec3_t dir;
		VectorCopy(kVirtualSampleDirections[i], dir);
		VectorNormalize(dir);

		vec3_t samplePos;
		VectorMA(viewOrigin, sampleRadius, dir, samplePos);

		vec3_t ambient, directed, lightDir;
		if (!R_LightForPoint(samplePos, ambient, directed, lightDir)) {
			continue;
		}

		float intensity = VectorLength(directed);
		if (intensity <= 1e-4f) {
			continue;
		}

		VirtualLightSample sample{};
		VectorCopy(lightDir, sample.direction);
		VectorCopy(directed, sample.color);
		sample.intensity = intensity;
		samples.push_back(sample);
	}

	if (samples.empty()) {
		return;
	}

	std::sort(samples.begin(), samples.end(), [](const VirtualLightSample& a, const VirtualLightSample& b) {
		return a.intensity > b.intensity;
	});

	int availableSlots = MAX_REF_LIGHTS - tr.refdef.numLights;
	int lightCount = std::min({targetAdditional, availableSlots, static_cast<int>(samples.size())});
	if (lightCount <= 0) {
		return;
	}

	refLight_t* lights = tr.refdef.lights;
	int baseIndex = tr.refdef.numLights;

	for (int i = 0; i < lightCount; ++i) {
		const VirtualLightSample& sample = samples[i];
		refLight_t& light = lights[baseIndex + i];
		light = {};
		light.rlType = refLightType_t::RL_PROJ;

		vec3_t dir;
		VectorCopy(sample.direction, dir);
		VectorNormalize(dir);

		vec3_t origin;
		VectorMA(viewOrigin, sampleRadius, dir, origin);
		float backOff = std::min(32.0f, sampleRadius * 0.5f);
		VectorMA(origin, -backOff, dir, origin); // pull back slightly to avoid clipping inside surfaces
		VectorCopy(origin, light.origin);
		light.radius = sampleRadius * 2.0f;
		light.flags = REF_RESTRICT_DLIGHT | REF_INVERSE_DLIGHT;

		// Project towards the camera focus
		VectorSubtract(viewOrigin, light.origin, light.projTarget);

		vec3_t up, temp;
		vec3_t worldUp = {0.0f, 0.0f, 1.0f};
		CrossProduct(worldUp, light.projTarget, temp);
		if (VectorLength(temp) < 1e-3f) {
			worldUp[0] = 0.0f; worldUp[1] = 1.0f; worldUp[2] = 0.0f;
			CrossProduct(worldUp, light.projTarget, temp);
		}
		VectorNormalize(temp);
		CrossProduct(light.projTarget, temp, up);

		float coneAngle = DEG2RAD(55.0f);
		VectorScale(up, tanf(coneAngle) * sampleRadius, light.projUp);

		vec3_t color;
		VectorCopy(sample.color, color);
		float maxComponent = std::max({color[0], color[1], color[2], 1e-6f});
		VectorScale(color, 1.0f / maxComponent, light.color);
		light.scale = sample.intensity * intensityScale;
	}

	tr.refdef.numLights += lightCount;
}

static int AppendVirtualSpotLightsToRefdef() {
	if (!tr.world) {
		return 0;
	}

	const auto& virtualLights = tr.world->virtualSpotLights;
	if (virtualLights.empty()) {
		return 0;
	}

	int available = MAX_REF_LIGHTS - tr.refdef.numLights;
	if (available <= 0) {
		return 0;
	}

	float maxViewDist = r_virtualSpotLightMaxViewDistance.Get();
	float maxViewDistSq = ( maxViewDist > 0.0f ) ? maxViewDist * maxViewDist : -1.0f;

	vec3_t viewOrg;
	VectorCopy( tr.refdef.vieworg, viewOrg );

    struct Candidate {
        const refLight_t* light;
        float priority;
    };

	std::vector<Candidate> candidates;
	candidates.reserve( virtualLights.size() );

    for ( const refLight_t& srcLight : virtualLights ) {
        if ( !WithinVirtualSpotlightViewDistance( srcLight, maxViewDist, maxViewDistSq, viewOrg ) ) {
            continue;
        }

        vec3_t delta;
        VectorSubtract( srcLight.origin, viewOrg, delta );
        float distSq = VectorLengthSquared( delta );
        float distFactor = ( distSq <= 1.0f ) ? 1.0f : 1.0f / distSq;
        float radiusFactor = std::max( srcLight.radius, 1.0f );
        float intensityFactor = std::max( srcLight.scale, 0.1f );
        float priority = distFactor * radiusFactor * intensityFactor;
        candidates.push_back( { &srcLight, priority } );
    }

	if ( candidates.empty() ) {
		return 0;
	}

    std::sort( candidates.begin(), candidates.end(), []( const Candidate& a, const Candidate& b ) {
        return a.priority > b.priority;
    } );

	int toCopy = std::min( available, static_cast<int>( candidates.size() ) );
	for ( int i = 0; i < toCopy; ++i ) {
		tr.refdef.lights[ tr.refdef.numLights + i ] = *candidates[i].light;
	}

	tr.refdef.numLights += toCopy;
	return toCopy;
}

bool ShadowMapManager::SetupLightShadows(refLight_t* light, int sceneIndex) {
    // Frontend builds shadow descriptors in the frontend buffer
    shadowData_t *sd = &backEndData[tr.smpFrame]->shadowData;
    int lightIndex = sd->numShadowLights;
	if (lightIndex >= MAX_SHADOW_LIGHTS) {
		Log::Warn("Light index %d exceeds MAX_SHADOW_LIGHTS (%d)", lightIndex, MAX_SHADOW_LIGHTS);
		return false;
	}

    Log::Debug("Setting up shadow for light slot %d (scene %d): %s at (%.1f, %.1f, %.1f)",
              lightIndex, sceneIndex,
              (light->rlType == refLightType_t::RL_DIRECTIONAL) ? "directional" :
              (light->rlType == refLightType_t::RL_OMNI) ? "point" : "spot",
              light->origin[0], light->origin[1], light->origin[2]);

	lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];
	memset(lightShadow, 0, sizeof(*lightShadow)); // Use sizeof(*lightShadow) for correct size
	lightShadow->castsShadows = true;
    lightShadow->sceneIndex = sceneIndex;
    lightShadow->flags = light->flags;

	// Determine number of cascades based on light type
	if (light->rlType == refLightType_t::RL_DIRECTIONAL) {
		lightShadow->numCascades = r_shadowCascades.Get();
		Log::Debug("Directional light detected, using %d cascades. Direction: (%.2f, %.2f, %.2f)",
		          lightShadow->numCascades,
		          light->projTarget[0], light->projTarget[1], light->projTarget[2]);
	} else if (light->rlType == refLightType_t::RL_OMNI) {
		lightShadow->numCascades = kPointLightFaceCount;
		Log::Debug("Omni light detected, using %d cube faces", lightShadow->numCascades);
	} else {
		lightShadow->numCascades = 1; // Spot lights use single shadow map
		Log::Debug("Non-directional light (%d), using 1 cascade", static_cast<int>(light->rlType));
	}

	// Validate cascade count
	if (lightShadow->numCascades < 1 || lightShadow->numCascades > MAX_SHADOW_CASCADES) {
		Log::Warn("Invalid cascade count %d for light type %d, using 1",
		         lightShadow->numCascades, static_cast<int>(light->rlType));
		lightShadow->numCascades = 1;
	}

	// Set up shadow maps for each cascade
	bool setupSuccess = true;
	for (int cascade = 0; cascade < lightShadow->numCascades; cascade++) {
		shadowMap_t* shadowMap = &lightShadow->cascades[cascade];

		shadowMap->technique = GetShadowTechnique();
		shadowMap->size[0] = r_shadowMapSize.Get();
		shadowMap->size[1] = r_shadowMapSize.Get();
		shadowMap->cascadeIndex = cascade;
		shadowMap->lightIndex = lightIndex;
		shadowMap->cubeFace = (light->rlType == refLightType_t::RL_OMNI) ? cascade : -1;

		Log::Debug("Setting up cascade %d: technique=%d, size=%dx%d",
		          cascade, static_cast<int>(shadowMap->technique),
		          shadowMap->size[0], shadowMap->size[1]);

		// Allocate atlas region for this shadow map
        Log::Debug("Allocating atlas region %dx%d for scene light %d cascade %d",
                  shadowMap->size[0], shadowMap->size[1], sceneIndex, cascade);

        if (!AllocateAtlasRegion(&sd->shadowAtlas, shadowMap->size[0], shadowMap->size[1],
                                 shadowMap->atlasOffset, sceneIndex, cascade)) {
            Log::Warn("Failed to allocate shadow atlas region for scene light %d cascade %d", sceneIndex, cascade);
            lightShadow->castsShadows = false;
            setupSuccess = false;
            // Don't return false immediately, continue to see all failures
        } else {
            Log::Debug("Allocated atlas region at (%d, %d) size %dx%d for scene light %d cascade %d",
                      shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
                      shadowMap->size[0], shadowMap->size[1],
                      sceneIndex, cascade);

			// Set up technique-specific parameters
			switch (shadowMap->technique) {
				case shadowingMode_t::SHADOWING_ESM16:
				case shadowingMode_t::SHADOWING_ESM32:
					SetupESMParams(shadowMap);
					Log::Debug("Set up ESM parameters for cascade %d", cascade);
					break;
				case shadowingMode_t::SHADOWING_VSM16:
				case shadowingMode_t::SHADOWING_VSM32:
					SetupVSMParams(shadowMap);
					Log::Debug("Set up VSM parameters for cascade %d", cascade);
					break;
				case shadowingMode_t::SHADOWING_EVSM32:
					SetupEVSMParams(shadowMap);
					Log::Debug("Set up EVSM parameters for cascade %d", cascade);
					break;
				default:
					Log::Debug("Unknown technique for cascade %d", cascade);
					break;
			}

			// Set up light matrices for this shadow map
			Log::Debug("Setting up light matrix for light type %d cascade %d at (%.1f, %.1f, %.1f)",
			          static_cast<int>(light->rlType), cascade,
			          light->origin[0], light->origin[1], light->origin[2]);
			SetupLightMatrix(light, shadowMap);
			Log::Debug("Set up light matrices for cascade %d", cascade);
		}
	}

    if (setupSuccess) {
        if (lightShadow->sceneIndex >= 0 && lightShadow->sceneIndex < MAX_REF_LIGHTS) {
            sd->sceneLightHasShadows[lightShadow->sceneIndex] = true;
        }
        sd->numShadowLights++;
        Log::Debug("Successfully set up shadow for scene light %d (slot %d), total shadow lights now: %d",
                   sceneIndex, lightIndex, sd->numShadowLights);
        return true;
    } else {
        Log::Debug("Failed to set up shadow for scene light %d (slot %d)", sceneIndex, lightIndex);
        return false;
    }
}

void ShadowMapManager::SetupLightMatrix(refLight_t* light, shadowMap_t* shadowMap, const vec3_t* bounds) {
	matrix_t viewMatrix, projectionMatrix;

	switch (light->rlType) {
		case refLightType_t::RL_DIRECTIONAL:
			SetupDirectionalLightMatrix(light, shadowMap, viewMatrix, projectionMatrix);
			break;

		case refLightType_t::RL_OMNI:
			SetupPointLightMatrix(light, shadowMap, viewMatrix, projectionMatrix);
			break;

		case refLightType_t::RL_PROJ:
			SetupSpotLightMatrix(light, shadowMap, viewMatrix, projectionMatrix);
			break;

		default:
			Log::Warn("Unknown light type for shadow mapping: %d", static_cast<int>(light->rlType));
			MatrixIdentity(viewMatrix);
			MatrixIdentity(projectionMatrix);
			break;
	}

	// Store matrices
	MatrixCopy(viewMatrix, shadowMap->lightViewMatrix);
	MatrixCopy(projectionMatrix, shadowMap->lightProjectionMatrix);
	// Combine matrices: Projection * View (standard OpenGL order)
	MatrixMultiply(projectionMatrix, viewMatrix, shadowMap->lightViewProjectionMatrix);

	// Log combined matrix for debugging
	Log::Debug("Combined light VP matrix:");
	Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightViewProjectionMatrix[0], shadowMap->lightViewProjectionMatrix[4], shadowMap->lightViewProjectionMatrix[8], shadowMap->lightViewProjectionMatrix[12]);
	Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightViewProjectionMatrix[1], shadowMap->lightViewProjectionMatrix[5], shadowMap->lightViewProjectionMatrix[9], shadowMap->lightViewProjectionMatrix[13]);
	Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightViewProjectionMatrix[2], shadowMap->lightViewProjectionMatrix[6], shadowMap->lightViewProjectionMatrix[10], shadowMap->lightViewProjectionMatrix[14]);
	Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightViewProjectionMatrix[3], shadowMap->lightViewProjectionMatrix[7], shadowMap->lightViewProjectionMatrix[11], shadowMap->lightViewProjectionMatrix[15]);
	// Set up light frustum from matrices
	R_SetupFrustum2(shadowMap->lightFrustum, shadowMap->lightViewProjectionMatrix);


	Log::Debug("Set up light matrix for %s light at (%.1f, %.1f, %.1f)",
	          (light->rlType == refLightType_t::RL_DIRECTIONAL) ? "directional" :
	          (light->rlType == refLightType_t::RL_OMNI) ? "point" : "spot",
	          light->origin[0], light->origin[1], light->origin[2]);
}

void ShadowMapManager::AddShadowLight(const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags ) {
    // Frontend-only path, write to frontend buffer
    shadowData_t *sd = &backEndData[tr.smpFrame]->shadowData;
	if (sd->numShadowOnlyLights >= MAX_SHADOW_LIGHTS) {
		Log::Debug("Shadow light limit reached (%d), not adding new light", MAX_SHADOW_LIGHTS);
		return;
	}

	refLight_t* shadowLight = &sd->shadowOnlyLights[sd->numShadowOnlyLights];
	*shadowLight = {};

	// Set up shadow light properties
	VectorCopy(org, shadowLight->origin);
	shadowLight->radius = radius;
	shadowLight->scale = intensity;
	shadowLight->color[0] = r;
	shadowLight->color[1] = g;
	shadowLight->color[2] = b;
    shadowLight->flags = flags;

	// Determine light type (default to omni for now)
	shadowLight->rlType = refLightType_t::RL_OMNI;

	Log::Debug("Added shadow-only light %d at (%.1f, %.1f, %.1f) radius=%.1f",
	          sd->numShadowOnlyLights, org[0], org[1], org[2], radius);

	sd->numShadowOnlyLights++;
}

void ShadowMapManager::UpdateShadowMaps() {
    // Frontend updates current frame buffer
    shadowData_t *sd = &backEndData[tr.smpFrame]->shadowData;

    // Track frame changes
    static int lastFrameCount = -1;
    if (lastFrameCount != tr.frameCount) {
        lastFrameCount = tr.frameCount;
        Log::Debug("=== UPDATE SHADOW MAPS FRAME %d ===", tr.frameCount);
    }

    // Tiled forward alignment: build shadows for the first N scene lights so
    // indices match the light buffer order used by tiling and shading.
    const int maxShadowLights = r_shadowLights.Get();

    GenerateVirtualInverseShadowLights();
	AppendVirtualSpotLightsToRefdef();

	const int originalSceneLights = tr.refdef.numLights;
	refLight_t* sceneLights = tr.refdef.lights;

	vec3_t viewOrg;
	VectorCopy( tr.refdef.vieworg, viewOrg );

	const ViewPVSInfo viewPVS = BuildViewPVSInfo( viewOrg );
	int sceneNumLights = originalSceneLights;
	if ( sceneLights && originalSceneLights > 0 ) {
		const int visibleCount = PromoteVisibleLights( sceneLights, originalSceneLights, viewPVS );
		if ( visibleCount >= 0 && visibleCount < sceneNumLights ) {
			tr.refdef.numLights = visibleCount;
			sceneNumLights = visibleCount;
			Log::Debug("PVS visibility culled %d lights (kept %d of %d)",
			           originalSceneLights - sceneNumLights,
			           sceneNumLights,
			           originalSceneLights);
		}
	}

    Log::Debug("Updating shadow maps from scene lights: %d available, max shadowed: %d", sceneNumLights, maxShadowLights);

    // Compute atlas capacity in tiles of size r_shadowMapSize
    int tileSize = r_shadowMapSize.Get();
    int tilesPerDim = std::max(1, sd->shadowAtlas.size / tileSize);
    int atlasCapacity = tilesPerDim * tilesPerDim;
    int usedTiles = sd->shadowAtlas.allocatedRegions; // reset earlier in BeginFrame

    int lightsProcessed = 0;

    // Pass 0: always try to shadow the first directional light (sun) first so it never
    // loses atlas space to dynamic lights when capacity is tight.
    for (int i = 0; i < sceneNumLights && sd->numShadowLights < maxShadowLights; i++) {
        refLight_t* light = &sceneLights[i];
        if (light->rlType != refLightType_t::RL_DIRECTIONAL) continue;

        Log::Debug("(Priority) Setting up shadow for directional scene light %d", i);

        int requiredTiles = r_shadowCascades.Get();
        if (requiredTiles < 1) requiredTiles = 1;

        if (usedTiles + requiredTiles > atlasCapacity) {
            Log::Warn("Skipping sun light %d: not enough atlas space (need %d tiles, have %d/%d)",
                      i, requiredTiles, atlasCapacity - usedTiles, atlasCapacity);
            break; // no point continuing if even sun doesn't fit
        }

        if (SetupLightShadows(light, i)) {
            Log::Debug("Successfully set up shadow for directional light %d", i);
            lightsProcessed++;
            usedTiles += requiredTiles;
        }
        // Whether success or not, only prioritize the first directional; then continue
        break;
    }

	// Pass 1: process remaining lights in order, skipping any directional already handled.
    for (int i = 0; i < sceneNumLights && sd->numShadowLights < maxShadowLights; i++) {
        refLight_t* light = &sceneLights[i];
        Log::Debug("Setting up shadow for scene light %d at (%.1f, %.1f, %.1f)",
                   i, light->origin[0], light->origin[1], light->origin[2]);

        // Skip additional directional lights in this pass (first one handled above)
        if (light->rlType == refLightType_t::RL_DIRECTIONAL && lightsProcessed > 0) {
            continue;
        }

        // Tiles required for this light
		int requiredTiles;
		if (light->rlType == refLightType_t::RL_DIRECTIONAL) {
			requiredTiles = r_shadowCascades.Get();
		} else if (light->rlType == refLightType_t::RL_OMNI) {
			requiredTiles = kPointLightFaceCount;
		} else {
			requiredTiles = 1;
		}
        if (usedTiles + requiredTiles > atlasCapacity) {
            // Maintain 1:1 index alignment with tiled/UBO light indices: only shadow
            // the first contiguous block of scene lights. If we can't fit this one,
            // stop here so shadow light indices remain a prefix [0..K) of scene lights.
            Log::Warn("Stopping shadow selection at light %d: not enough atlas space (need %d tiles, have %d/%d)",
                      i, requiredTiles, atlasCapacity - usedTiles, atlasCapacity);
            break;
        }

        if (SetupLightShadows(light, i)) {
            Log::Debug("Successfully set up shadow for scene light %d", i);
            lightsProcessed++;
            usedTiles += requiredTiles;
        } else {
            // Same rationale as above: keep indices aligned by only shadowing a contiguous
            // prefix of scene lights. If this light fails to set up, stop adding more.
            Log::Debug("Failed to set up shadow for scene light %d; stopping to preserve index alignment", i);
            break;
        }
    }

	// After gathering, sort shadowed lights by their scene light index so
    // u_ShadowLightInfo[slot].x is strictly ascending (for GPU merge).
    if (sd->numShadowLights > 1) {
        std::sort(sd->lightShadows, sd->lightShadows + sd->numShadowLights,
                  [](const lightShadowInfo_t& a, const lightShadowInfo_t& b) {
                      return a.sceneIndex < b.sceneIndex;
                  });
    }

	Log::Debug("Updated shadow maps: %d shadowed lights prepared from %d visible scene lights (of %d total)",
	          sd->numShadowLights, sceneNumLights, originalSceneLights);

	// Clear any legacy shadow-only lights; they are not part of the tiled light list
	sd->numShadowOnlyLights = 0;
}

void ShadowMapManager::RenderShadowMaps() {
    shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	// Track frame changes to verify we're actually rendering each frame
	static int lastFrameCount = -1;
	static int renderCallCount = 0;

	if (lastFrameCount != tr.frameCount) {
		lastFrameCount = tr.frameCount;
		renderCallCount = 0;
		Log::Debug("=== NEW FRAME %d ===", tr.frameCount);
	}

	renderCallCount++;
    Log::Debug("RenderShadowMaps #%d called with %d shadow lights", renderCallCount, sd->numShadowLights);

	if (!sd->shadowAtlas.fbo) {
		Log::Warn("Shadow atlas FBO not initialized");
		return;
	}

	// Check if shadow mapping is enabled
	if (!IsShadowMappingEnabled()) {
		Log::Debug("Shadow mapping is not enabled, returning early");
		return;
	}

	if (sd->numShadowLights == 0) {
		Log::Debug("No shadow lights to render");
		return;
	}

	shadowingMode_t currentTechnique = GetShadowTechnique();
	Log::Debug("Shadow mapping enabled with technique: %d", static_cast<int>(currentTechnique));

	if (currentTechnique < shadowingMode_t::SHADOWING_ESM16 || currentTechnique > shadowingMode_t::SHADOWING_EVSM32) {
		Log::Warn("Invalid shadow technique: %d", static_cast<int>(currentTechnique));
		return;
	}

	if (!sd->shadowAtlas.colorImage) {
		Log::Warn("Shadow atlas color image not initialized");
		return;
	}

	if (!sd->shadowAtlas.depthImage) {
		Log::Warn("Shadow atlas depth image not initialized");
		return;
	}

	// Save current engine state
	uint32_t oldStateBits = glState.glStateBits;
	int prevViewport[4] = {glState.viewportX, glState.viewportY, glState.viewportWidth, glState.viewportHeight};
	int prevScissorBox[4] = {glState.scissorX, glState.scissorY, glState.scissorWidth, glState.scissorHeight};

    // Bind shadow atlas FBO for rendering
    FBO_t *oldFBO = glState.currentFBO;
    R_BindFBO(sd->shadowAtlas.fbo);


	// Check FBO completeness
	if (!R_CheckFBO(sd->shadowAtlas.fbo)) {
		Log::Warn("Shadow atlas FBO is not complete!");
		R_BindFBO(oldFBO);
		return;
	}
    Log::Debug("Shadow atlas FBO is complete");

	// One-time setup since we only support one technique at a time
	shadowingMode_t technique = sd->lightShadows[0].cascades[0].technique;
	Log::Debug("Using shadow technique: %d", static_cast<int>(technique));

	// Set up state for shadow map generation using engine's state management
	uint32_t shadowState = 0; // Start with default state

	// Enable depth testing (clear GLS_DEPTHTEST_DISABLE bit)
	// Set depth function to LESS
	shadowState |= GLS_DEPTHMASK_TRUE | GLS_DEPTHFUNC_LESS;

	// Set color mask based on technique
	if (technique == shadowingMode_t::SHADOWING_ESM16 || technique == shadowingMode_t::SHADOWING_ESM32) {
		// ESM needs color output for exponential depth - disable green, blue, and alpha masks
		shadowState |= GLS_GREENMASK_FALSE | GLS_BLUEMASK_FALSE | GLS_ALPHAMASK_FALSE;
		Log::Debug("Setting up for ESM technique with RG color mask");
	} else if (technique == shadowingMode_t::SHADOWING_VSM16 || technique == shadowingMode_t::SHADOWING_VSM32 ||
	           technique == shadowingMode_t::SHADOWING_EVSM32) {
		// VSM/EVSM need RG output for moments - disable blue and alpha masks
		shadowState |= GLS_BLUEMASK_FALSE | GLS_ALPHAMASK_FALSE;
		Log::Debug("Setting up for VSM/EVSM technique with RG color mask");
	} else {
		// Depth only - disable all color masks
		shadowState |= GLS_REDMASK_FALSE | GLS_GREENMASK_FALSE | GLS_BLUEMASK_FALSE | GLS_ALPHAMASK_FALSE;
		Log::Debug("Setting up for depth-only technique with no color output");
	}

	// Apply the state
	GL_State(shadowState);

	// Enable polygon offset to reduce shadow acne
	glEnable(GL_POLYGON_OFFSET_FILL);
	GL_PolygonOffset(r_shadowBias.Get(), 1.0f);

	Log::Debug("Enabled polygon offset with bias=%.3f", r_shadowBias.Get());

    // Set scissor to full atlas and clear once
    GL_Scissor( 0, 0, sd->shadowAtlas.size, sd->shadowAtlas.size );
    // For depth-only techniques, we want depth=1.0 (farthest)
    // For ESM/VSM techniques, we want color=1.0 (maximum depth value)
    GL_ClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    GL_ClearDepth(1.0f);
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );

	Log::Debug("Cleared shadow atlas with white color (1,1,1,1) and depth=1.0 - this should happen EVERY frame");

    // Save/restore the current view to avoid leaking matrices/state to main view
    viewParms_t savedView = backEnd.viewParms;

    // Legacy shadow path uses gathered drawSurfs; do not interact with material system here

    // Render shadow map for each shadow light
    int renderedLights = 0;
    for (int lightIndex = 0; lightIndex < sd->numShadowLights; lightIndex++) {
        lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];

        if (!lightShadow->castsShadows) {
            Log::Debug("Light %d does not cast shadows, skipping", lightIndex);
            continue;
        }

		Log::Debug("Rendering light %d with %d cascades", lightIndex, lightShadow->numCascades);
		renderedLights++;

		// Render each cascade for this light
		for (int cascade = 0; cascade < lightShadow->numCascades; cascade++) {
			shadowMap_t* shadowMap = &lightShadow->cascades[cascade];

			// Prepare per-light shadow params for the depth pass.
			backEnd.shadowLightFlags = lightShadow->flags;
			backEnd.shadowParams[0] = r_shadowBias.Get();
			backEnd.shadowParams[1] = R_ComputeESMExponent(shadowMap->technique, r_shadowESMExponent.Get());
			backEnd.shadowParams[2] = r_shadowPCF.Get();
			backEnd.shadowParams[3] = (lightShadow->flags & REF_INVERSE_DLIGHT) ? r_shadowInverseESMScale.Get() : 1.0f;

            // Set up rendering for this shadow map
            GL_Viewport(shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
                       shadowMap->size[0], shadowMap->size[1]);
            GL_Scissor(shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
                       shadowMap->size[0], shadowMap->size[1]);

            // Load matrices for this cascade from gathered view parms
            backEnd.viewParms = shadowMap->viewParms;
            GL_LoadProjectionMatrix( backEnd.viewParms.projectionMatrix );
            GL_LoadModelViewMatrix( backEnd.viewParms.world.modelViewMatrix );


			// Draw using precomputed frontend viewParms for this cascade
			Log::Debug("Rendering precomputed shadow view for light %d cascade %d at atlas (%d, %d) size (%d, %d)",
			          lightIndex, cascade, shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
			          shadowMap->size[0], shadowMap->size[1]);

            {
                // Draw depth surfaces for the precomputed view using depth-only iterator
                // Render only the SS_DEPTH range collected during gather
                // Based on RB_RenderDrawSurfaces but forcing Tess_StageIteratorShadowDepth
                trRefEntity_t *entity = nullptr, *oldEntity = nullptr;
                shader_t *shader = nullptr, *oldShader = nullptr;
                int lightmapNum = -1, oldLightmapNum = -1;
                int fogNum = -1, oldFogNum = -1;
                bool depthRange = false, oldDepthRange = false;
                int i;
                drawSurf_t *drawSurf;
                // Render a broad range of geometry sorts to ensure dynamic entities are included
                int first = backEnd.viewParms.firstDrawSurf[ Util::ordinal(shaderSort_t::SS_ENVIRONMENT_FOG) ];
                int last  = backEnd.viewParms.firstDrawSurf[ Util::ordinal(shaderSort_t::SS_OPAQUE) + 1 ];

                for ( i = first; i < last; i++ ) {
                    drawSurf = &backEnd.viewParms.drawSurfs[ i ];
                    if ( drawSurf->surface == nullptr ) continue;

                    entity = drawSurf->entity;
                    if ( (lightShadow->flags & (REF_RESTRICT_DLIGHT | REF_INVERSE_DLIGHT)) && entity == &tr.worldEntity ) {
                        continue;
                    }
                    shader = drawSurf->shader;
                    lightmapNum = drawSurf->lightmapNum();
                    fogNum = drawSurf->fog;

                    if ( shader != oldShader || lightmapNum != oldLightmapNum || fogNum != oldFogNum || ( entity != oldEntity && !(shader && shader->entityMergable) ) ) {
                        if ( oldShader != nullptr ) {
                            Tess_End();
                        }
                        Tess_Begin( Tess_StageIteratorShadowDepth, shader, false, lightmapNum, fogNum, drawSurf->bspSurface );
                        oldShader = shader;
                        oldLightmapNum = lightmapNum;
                        oldFogNum = fogNum;
                    }

                    if ( entity != oldEntity ) {
                        depthRange = false;
                        if ( entity != &tr.worldEntity ) {
                            backEnd.currentEntity = entity;
                            R_RotateEntityForViewParms( backEnd.currentEntity, &backEnd.viewParms, &backEnd.orientation );
                            if ( backEnd.currentEntity->e.renderfx & RF_DEPTHHACK ) depthRange = true;
                        } else {
                            backEnd.currentEntity = &tr.worldEntity;
                            backEnd.orientation = backEnd.viewParms.world;
                        }
                        GL_LoadModelViewMatrix( backEnd.orientation.modelViewMatrix );

                        if ( oldDepthRange != depthRange ) {
                            if ( depthRange ) glDepthRange( 0, 0.3 ); else glDepthRange( 0, 1 );
                            oldDepthRange = depthRange;
                        }
                        oldEntity = entity;
                    }

                    rb_surfaceTable[ Util::ordinal(*drawSurf->surface) ]( drawSurf->surface );
                }

                if ( oldShader != nullptr ) {
                    Tess_End();
                }

                GL_LoadModelViewMatrix( backEnd.viewParms.world.modelViewMatrix );
                if ( depthRange ) glDepthRange( 0, 1 );
            }

			Log::Debug("Rendered shadow map for light %d cascade %d at atlas (%d, %d) size (%d, %d)",
			          lightIndex, cascade, shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
			          shadowMap->size[0], shadowMap->size[1]);
		}
    }

    // Optional VSM/EVSM blur pass per tile
    if (currentTechnique == shadowingMode_t::SHADOWING_VSM16 ||
        currentTechnique == shadowingMode_t::SHADOWING_VSM32 ||
        currentTechnique == shadowingMode_t::SHADOWING_EVSM32) {
        float radius = r_shadowVSMBlur.Get();
        if (radius > 0.0f && sd->shadowAtlas.colorImage && sd->shadowAtlas.tempImage && gl_blurShader) {
            // Set common state
            GL_State(GLS_DEPTHTEST_DISABLE);
            GL_Cull(cullType_t::CT_TWO_SIDED);

            // Ortho over atlas space
            matrix_t ortho;
            MatrixOrthogonalProjection(ortho, 0, sd->shadowAtlas.size, 0, sd->shadowAtlas.size, -99999, 99999);

            vec2_t texScale;
            texScale[0] = 1.0f / sd->shadowAtlas.size;
            texScale[1] = 1.0f / sd->shadowAtlas.size;

            gl_blurShader->BindProgram(0);
            gl_blurShader->SetUniform_DeformMagnitude(radius);
            gl_blurShader->SetUniform_TexScale(texScale);

            // Horizontal then vertical for each allocated region
            for (int i = 0; i < sd->shadowAtlas.allocatedRegions; ++i) {
                const atlasRegion_t& region = sd->shadowAtlas.regions[i];
                if (!region.allocated) continue;

                int x = region.offset[0];
                int y = region.offset[1];
                int w = region.size[0];
                int h = region.size[1];
                float atlasTexel = 1.0f / sd->shadowAtlas.size;
                gl_blurShader->SetUniform_BlurTexBounds(
                    (x + 0.5f) * atlasTexel,
                    (y + 0.5f) * atlasTexel,
                    (x + w - 0.5f) * atlasTexel,
                    (y + h - 0.5f) * atlasTexel
                );

                // Horizontal pass: src = atlas.colorImage, dst = tempFBO
                gl_blurShader->SetUniform_Horizontal(true);
                gl_blurShader->SetUniform_ColorMapBindless(GL_BindToTMU(0, sd->shadowAtlas.colorImage));

                R_BindFBO(sd->shadowAtlas.tempFBO);
                GL_LoadProjectionMatrix(ortho);
                GL_Viewport(x, y, w, h);
                GL_Scissor(x, y, w, h);
                glClear(GL_COLOR_BUFFER_BIT);
                Tess_InstantScreenSpaceQuad();

                // Vertical pass: src = tempImage, dst = atlas.fbo
                gl_blurShader->SetUniform_Horizontal(false);
                gl_blurShader->SetUniform_ColorMapBindless(GL_BindToTMU(0, sd->shadowAtlas.tempImage));

                R_BindFBO(sd->shadowAtlas.fbo);
                GL_LoadProjectionMatrix(ortho);
                GL_Viewport(x, y, w, h);
                GL_Scissor(x, y, w, h);
                glClear(GL_COLOR_BUFFER_BIT);
                Tess_InstantScreenSpaceQuad();
            }
        }
    }

    // Restore engine state and original view
    R_BindNullFBO();
    glDisable(GL_POLYGON_OFFSET_FILL);
    GL_State(oldStateBits);
    GL_Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    // Restore scissor box (scissor test is always enabled by GL_SetDefaultState)
    GL_Scissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    R_BindFBO(oldFBO);

    // Hard-reset write masks to avoid any leakage if GL_State couldn't restore due to masks
    GL_ColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    GL_DepthMask( GL_TRUE );

    backEnd.viewParms = savedView;
    GL_LoadProjectionMatrix( backEnd.viewParms.projectionMatrix );
    GL_LoadModelViewMatrix( backEnd.viewParms.world.modelViewMatrix );

	Log::Debug("Shadow atlas rendering complete for %d lights (actually rendered: %d)", sd->numShadowLights, renderedLights);
}

void ShadowMapManager::BuildShadowViews() {
    // Frontend builds gathered views in the frontend buffer
    shadowData_t *sd = &backEndData[tr.smpFrame]->shadowData;

    if (sd->numShadowLights == 0) {
        return;
    }

    // Build a gather-only view for each light cascade
    for (int lightIndex = 0; lightIndex < sd->numShadowLights; lightIndex++) {
        lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];
		if (!lightShadow->castsShadows) {
			continue;
		}

        for (int cascade = 0; cascade < lightShadow->numCascades; cascade++) {
            shadowMap_t* shadowMap = &lightShadow->cascades[cascade];

            // Build viewParms based on light view/projection matrices
            viewParms_t inView = {};

            // Use shadow map resolution for viewport; atlas offset handled in backend
            inView.viewportX = 0;
            inView.viewportY = 0;
            inView.viewportWidth = (int)shadowMap->size[0];
            inView.viewportHeight = (int)shadowMap->size[1];
            inView.scissorX = inView.viewportX;
            inView.scissorY = inView.viewportY;
            inView.scissorWidth = inView.viewportWidth;
            inView.scissorHeight = inView.viewportHeight;
            inView.viewID = 0; // avoid interfering with material system view tracking

            // Build orientation directly from the scene light, matching Setup*LightMatrix
            refLight_t* light = &tr.refdef.lights[lightShadow->sceneIndex];
            vec3_t fwd, up, right, left;
            if (light->rlType == refLightType_t::RL_DIRECTIONAL) {
                VectorCopy(light->projTarget, fwd); VectorNormalize(fwd);
                PerpendicularVector(up, fwd);
                CrossProduct(fwd, up, right); // right-handed
                VectorNegate(right, left);     // FLU expects left axis
                // Place origin far along -fwd like in SetupDirectionalLightMatrix
                vec3_t origin;
                VectorMA(vec3_origin, -2000.0f, fwd, origin);
                VectorCopy(origin, inView.orientation.origin);
                VectorCopy(origin, inView.orientation.viewOrigin);
            } else if (light->rlType == refLightType_t::RL_PROJ) {
                VectorCopy(light->origin, inView.orientation.origin);
                VectorCopy(light->origin, inView.orientation.viewOrigin);
                VectorCopy(light->projTarget, fwd); VectorNormalize(fwd);
                PerpendicularVector(up, fwd);
                CrossProduct(fwd, up, right);
                VectorNegate(right, left);
			} else { // RL_OMNI and others
				VectorCopy(light->origin, inView.orientation.origin);
				VectorCopy(light->origin, inView.orientation.viewOrigin);
				GetCubeFaceBasis(shadowMap->cubeFace, fwd, up);
				CrossProduct(fwd, up, right);
				VectorNormalize(right);
				VectorNegate(right, left);
			}
            VectorCopy(fwd,  inView.orientation.axis[0]);
            VectorCopy(left, inView.orientation.axis[1]);
            VectorCopy(up,   inView.orientation.axis[2]);
            VectorCopy(inView.orientation.origin, inView.pvsOrigin);
            inView.portalLevel = 0;
            inView.fovX = 90.0f;
            inView.fovY = 90.0f;

            // Gather using light projection
            viewParms_t outView = {};
            bool entitiesOnly = (lightShadow->flags & (REF_RESTRICT_DLIGHT | REF_INVERSE_DLIGHT)) != 0;
            R_GatherShadowView(&inView, shadowMap->lightProjectionMatrix, &outView, entitiesOnly);

            // Store for backend
            shadowMap->viewParms = outView;
        }
    }
}

void ShadowMapManager::DebugRenderShadowAtlas() {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	if (!r_shadowDebug.Get() || sd->numShadowLights == 0 || !sd->shadowAtlas.colorImage) {
		return;
	}

	Log::Debug("Debug rendering shadow atlas: %d lights, atlas size %d, texture ID %d",
	          sd->numShadowLights, sd->shadowAtlas.size, sd->shadowAtlas.colorImage->texnum);

	// This function is called from RB_RenderDebugUtils which is in 2D mode
	// We can use RE_StretchPic to draw the shadow atlas texture

	// For now, let's just log that we would draw it
	// In a proper implementation, we'd create a shader and use RE_StretchPic
	// But we need to be careful not to interfere with the rendering state

	static int debugFrameCounter = 0;
	debugFrameCounter++;

	// Log periodically to avoid spam
	if (debugFrameCounter % 60 == 0) {  // Every 60 frames (~1 second at 60fps)
		Log::Debug("Would draw shadow atlas debug visualization - texture %d, size %d",
		          sd->shadowAtlas.colorImage->texnum, sd->shadowAtlas.size);
	}
}

void ShadowMapManager::GetShadowMatrices(matrix_t* matrices, int maxMatrices) const {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	// Initialize all matrices to identity
	for (int i = 0; i < maxMatrices; i++) {
		MatrixIdentity(matrices[i]);
	}

	// Fill in actual shadow matrices
	int maxWrittenSlot = -1;
	for (int lightIndex = 0; lightIndex < sd->numShadowLights && lightIndex < MAX_SHADOW_LIGHTS; lightIndex++) {
		const lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];

		for (int cascade = 0; cascade < lightShadow->numCascades && cascade < MAX_SHADOW_CASCADES; cascade++) {
			const int slot = lightIndex * MAX_SHADOW_CASCADES + cascade;
			if (slot >= maxMatrices) {
				Log::Warn("Shadow matrix slot %d (light %d cascade %d) exceeds uniform capacity %d", slot, lightIndex, cascade, maxMatrices);
				continue;
			}

			const shadowMap_t* shadowMap = &lightShadow->cascades[cascade];
			MatrixCopy(shadowMap->lightViewProjectionMatrix, matrices[slot]);
			maxWrittenSlot = std::max(maxWrittenSlot, slot);

			// Log matrix for debugging
			Log::Debug("Shadow matrix slot %d (light %d cascade %d):", slot, lightIndex, cascade);
			Log::Debug("  [%f, %f, %f, %f]", matrices[slot][0], matrices[slot][4], matrices[slot][8], matrices[slot][12]);
			Log::Debug("  [%f, %f, %f, %f]", matrices[slot][1], matrices[slot][5], matrices[slot][9], matrices[slot][13]);
			Log::Debug("  [%f, %f, %f, %f]", matrices[slot][2], matrices[slot][6], matrices[slot][10], matrices[slot][14]);
			Log::Debug("  [%f, %f, %f, %f]", matrices[slot][3], matrices[slot][7], matrices[slot][11], matrices[slot][15]);
		}
	}

	Log::Debug("Copied shadow matrices up to slot %d", maxWrittenSlot);
}

void ShadowMapManager::GetShadowLightInfo(vec4_t* lightInfo, int maxLights) const {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	// Initialize all light info
	for (int i = 0; i < maxLights; i++) {
		Vector4Set(lightInfo[i], 0.0f, 0.0f, 0.0f, 0.0f);
	}

	// Fill in actual light info
	for (int lightIndex = 0; lightIndex < sd->numShadowLights && lightIndex < maxLights && lightIndex < MAX_SHADOW_LIGHTS; lightIndex++) {
		const lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];

		lightInfo[lightIndex][0] = static_cast<float>(lightShadow->sceneIndex); // sceneIndex
		lightInfo[lightIndex][1] = static_cast<float>(lightShadow->numCascades); // slice count
		lightInfo[lightIndex][2] = static_cast<float>(lightIndex * MAX_SHADOW_CASCADES); // base slice
		lightInfo[lightIndex][3] = static_cast<float>(lightShadow->flags); // packed REF_* flags

		Log::Debug("Light %d info: sceneIndex=%d, slices=%d, baseSlice=%d, flags=0x%X", lightIndex,
		          static_cast<int>(lightInfo[lightIndex][0]),
		          static_cast<int>(lightInfo[lightIndex][1]),
		          static_cast<int>(lightInfo[lightIndex][2]),
		          lightShadow->flags);
	}

	// Log all light info for debugging
	for (int i = 0; i < maxLights; i++) {
		Log::Debug("Final light info[%d]: (%.1f, %.1f, %.1f, %.1f)",
		          i, lightInfo[i][0], lightInfo[i][1], lightInfo[i][2], lightInfo[i][3]);
	}
}

void ShadowMapManager::GetCascadeSplits(vec4_t* splits, int maxLights) const {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	// Initialize all splits
	for (int i = 0; i < maxLights; i++) {
		Vector4Set(splits[i], 10.0f, 50.0f, 200.0f, 1000.0f); // Default splits
	}

	// Fill in actual cascade splits (for now, using defaults)
	// TODO: Implement proper cascade split calculation
	for (int lightIndex = 0; lightIndex < sd->numShadowLights && lightIndex < maxLights && lightIndex < MAX_SHADOW_LIGHTS; lightIndex++) {
		// For directional lights, we would set proper cascade splits
		// For now, using reasonable defaults
		Vector4Set(splits[lightIndex], 10.0f, 50.0f, 200.0f, 1000.0f);
	}
}

void ShadowMapManager::GetShadowTileInfo(vec4_t* tileInfo, int maxSlices) const {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	for (int i = 0; i < maxSlices; i++) {
		Vector4Set(tileInfo[i], 0.0f, 0.0f, 0.0f, 0.0f);
	}

	int atlasSize = std::max(1, sd->shadowAtlas.size);

	for (int lightIndex = 0; lightIndex < sd->numShadowLights && lightIndex < MAX_SHADOW_LIGHTS; lightIndex++) {
		const lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];
		if (!lightShadow->castsShadows) {
			continue;
		}

		for (int cascade = 0; cascade < lightShadow->numCascades && cascade < MAX_SHADOW_CASCADES; cascade++) {
			const int slot = lightIndex * MAX_SHADOW_CASCADES + cascade;
			if (slot >= maxSlices) {
				Log::Warn("Shadow tile slot %d exceeds capacity %d", slot, maxSlices);
				continue;
			}

			const shadowMap_t* shadowMap = &lightShadow->cascades[cascade];
			tileInfo[slot][0] = shadowMap->atlasOffset[0] / atlasSize;
			tileInfo[slot][1] = shadowMap->atlasOffset[1] / atlasSize;
			tileInfo[slot][2] = shadowMap->size[0] / atlasSize;
			tileInfo[slot][3] = shadowMap->size[1] / atlasSize;

			Log::Debug("Tile slot %d -> offset=(%.3f, %.3f) scale=(%.3f, %.3f)", slot,
			          tileInfo[slot][0], tileInfo[slot][1], tileInfo[slot][2], tileInfo[slot][3]);
		}
	}
}

// No CPU-side scene->shadow map export; GPU merges tile lists with slot list

// Light matrix calculation functions
void ShadowMapManager::SetupDirectionalLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For directional lights, we need an orthographic projection
	// Light direction comes from projTarget
	vec3_t lightDir;
	VectorCopy(light->projTarget, lightDir);
	VectorNormalize(lightDir);

	Log::Debug("Directional light direction before normalization: (%.3f, %.3f, %.3f)",
	          light->projTarget[0], light->projTarget[1], light->projTarget[2]);
	Log::Debug("Directional light direction after normalization: (%.3f, %.3f, %.3f)",
	          lightDir[0], lightDir[1], lightDir[2]);

	// Create view matrix looking down the light direction
	vec3_t lightPos, up, right;

	// Position the light far away in the opposite direction
	// For directional lights, we don't use the light origin as they represent infinite light
	// Just position it very far away in the opposite direction of the light
	VectorScale(lightDir, -2000.0f, lightPos);

	Log::Debug("Directional light position: (%.1f, %.1f, %.1f)", lightPos[0], lightPos[1], lightPos[2]);

	// Validate that we have a valid light direction
	if (VectorLength(lightDir) < 0.001f) {
		Log::Warn("Invalid light direction vector (length < 0.001), using default down direction");
		VectorSet(lightDir, 0.0f, 0.0f, -1.0f);
		VectorScale(lightDir, -2000.0f, lightPos);
	}

	// Create orthonormal basis
	PerpendicularVector(up, lightDir);
	CrossProduct(lightDir, up, right);

    // Build view matrix
    MatrixLookAtRH(viewMatrix, lightPos, lightDir, up);

    // Fixed, stable orthographic projection for directional light
    float size = 2048.0f;
    MatrixOrthogonalProjection(projectionMatrix, -size, size, -size, size, 1.0f, 4000.0f);
    Log::Debug("Directional ortho (fixed): -/+ %.1f, near=1.0, far=4000.0", size);

	// Log matrix for debugging
	Log::Debug("Directional light view matrix:");
	Log::Debug("  [%f, %f, %f, %f]", viewMatrix[0], viewMatrix[4], viewMatrix[8], viewMatrix[12]);
	Log::Debug("  [%f, %f, %f, %f]", viewMatrix[1], viewMatrix[5], viewMatrix[9], viewMatrix[13]);
	Log::Debug("  [%f, %f, %f, %f]", viewMatrix[2], viewMatrix[6], viewMatrix[10], viewMatrix[14]);
	Log::Debug("  [%f, %f, %f, %f]", viewMatrix[3], viewMatrix[7], viewMatrix[11], viewMatrix[15]);

	Log::Debug("Directional light projection matrix:");
	Log::Debug("  [%f, %f, %f, %f]", projectionMatrix[0], projectionMatrix[4], projectionMatrix[8], projectionMatrix[12]);
	Log::Debug("  [%f, %f, %f, %f]", projectionMatrix[1], projectionMatrix[5], projectionMatrix[9], projectionMatrix[13]);
	Log::Debug("  [%f, %f, %f, %f]", projectionMatrix[2], projectionMatrix[6], projectionMatrix[10], projectionMatrix[14]);
	Log::Debug("  [%f, %f, %f, %f]", projectionMatrix[3], projectionMatrix[7], projectionMatrix[11], projectionMatrix[15]);

	Log::Debug("Set up directional light matrix, direction=(%.2f, %.2f, %.2f)", lightDir[0], lightDir[1], lightDir[2]);
}

void ShadowMapManager::SetupPointLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For point lights, build one cube-map face per cascade index
	vec3_t lightPos, lookDir, up;
	VectorCopy(light->origin, lightPos);

	vec3_t forward;
	GetCubeFaceBasis(shadowMap->cubeFace, forward, up);
	VectorCopy(forward, lookDir);

	MatrixLookAtRH(viewMatrix, lightPos, lookDir, up);

	// Create perspective projection for 90° cube-map face
	float fov = 90.0f; // 90 degree FOV for cube face
	float aspect = 1.0f;

	const float radius = std::max(light->radius, 1.0f);
	float effectiveRange = std::max(radius, R_ComputeEffectiveLightRange(light));
	const float minNear = 1.0f;
	const float nearRatio = 0.04f; // push the near plane out to ~4% of the light radius
	float nearPlane = std::max(minNear, radius * nearRatio);
	// Avoid collapsing the frustum if the light radius is very small.
	float maxReasonableNear = std::max(0.5f, radius * 0.5f);
	nearPlane = std::min(nearPlane, maxReasonableNear);
	nearPlane = std::min(nearPlane, effectiveRange * 0.5f);
	float farPlane = std::max(effectiveRange, nearPlane + std::max(32.0f, 0.25f * effectiveRange));

	// Convert FOV to projection bounds
	float top = nearPlane * tanf(DEG2RAD(fov * 0.5f));
	float bottom = -top;
	float right = top * aspect;
	float left = -right;

	MatrixPerspectiveProjection(projectionMatrix, left, right, bottom, top, nearPlane, farPlane);

	Log::Debug("Set up point light matrix, position=(%.1f, %.1f, %.1f) radius=%.1f, effectiveRange=%.1f",
	          lightPos[0], lightPos[1], lightPos[2], light->radius, effectiveRange);
}

void ShadowMapManager::SetupSpotLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For spot lights, use perspective projection with light direction
	vec3_t lightPos, lightDir, up;
	VectorCopy(light->origin, lightPos);
	VectorCopy(light->projTarget, lightDir);
	VectorCopy(light->projUp, up);
	VectorNormalize(lightDir);
	VectorNormalize(up);

	// Build view matrix
	MatrixLookAtRH(viewMatrix, lightPos, lightDir, up);


	float fov = -1.0f;  // Start with assumption that we have a very wide cone.
	float upLen = VectorLength( light->projUp );
	float tgtLen = VectorLength( light->projTarget );
	if ( upLen > 0.0f && tgtLen > 0.0f ) {
		fov = atan2f( upLen, tgtLen );
	}

	// Create perspective projection based on spot light cone
	float aspect = 1.0f;

	const float radius = std::max(light->radius, 1.0f);
	float effectiveRange = std::max(radius, R_ComputeEffectiveLightRange(light));
	const float minNear = 1.0f;
	const float nearRatio = 0.04f; // push the near plane out to ~4% of the light radius
	float nearPlane = std::max(minNear, radius * nearRatio);
	// Avoid collapsing the frustum if the light radius is very small.
	float maxReasonableNear = std::max(0.5f, radius * 0.5f);
	nearPlane = std::min(nearPlane, maxReasonableNear);
	nearPlane = std::min(nearPlane, effectiveRange * 0.5f);
	float farPlane = std::max(effectiveRange, nearPlane + std::max(32.0f, 0.25f * effectiveRange));

	// Convert FOV to projection bounds
	float safeFov = (fov > 0.0f) ? fov : DEG2RAD(89.0f);
	float top = nearPlane * tanf(safeFov);
	float bottom = -top;
	float right = top * aspect;
	float left = -right;

	MatrixPerspectiveProjection(projectionMatrix, left, right, bottom, top, nearPlane, farPlane);

	Log::Debug("Set up spot light matrix, position=(%.1f, %.1f, %.1f) direction=(%.2f, %.2f, %.2f) effectiveRange=%.1f",
	          lightPos[0], lightPos[1], lightPos[2], lightDir[0], lightDir[1], lightDir[2], effectiveRange);
}

//
// Global shadow mapping functions
//

bool R_ShadowMappingEnabled() {
	return shadowMapManager.IsShadowMappingEnabled();
}

void R_InitShadowMapping() {
	// Initialize shadow data for all SMP frames
	shadowMapManager.Init(&backEndData[0]->shadowData);
	if (r_smp->integer) {
		// TODO: This is probably buggy. We don't handle the shadow atlas well for SMP.
		// For example, we seem to create a new shadow atlas and override the previous one.
		shadowMapManager.Init(&backEndData[1]->shadowData);
	}

}

void R_ShutdownShadowMapping() {
	// Shutdown shadow data for all SMP frames
	shadowMapManager.Shutdown(&backEndData[0]->shadowData);
	if (r_smp->integer) {
		shadowMapManager.Shutdown(&backEndData[1]->shadowData);
	}
}

void R_BeginShadowMapping() {
	shadowMapManager.BeginFrame();
}

void R_EndShadowMapping() {
	shadowMapManager.EndFrame();
}

void R_AddShadowLight( const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags ) {
	shadowMapManager.AddShadowLight(org, radius, intensity, r, g, b, hShader, flags);
}
