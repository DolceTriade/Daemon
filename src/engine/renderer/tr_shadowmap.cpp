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

// Global shadow map manager instance
ShadowMapManager shadowMapManager; // Still a singleton, but operates on passed shadowData_t

//
// ShadowMapManager Implementation
//

void ShadowMapManager::Init(shadowData_t *sd) {
	Log::Debug("Initializing shadow mapping system...");
	// Reset state
	memset(&sd->shadowAtlas, 0, sizeof( sd->shadowAtlas));
	memset(sd->lightShadows, 0, sizeof(sd->lightShadows));
	sd->numShadowLights = 0;

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

	Log::Debug("Shadow mapping system shut down");
}

void ShadowMapManager::BeginFrame() {
	if (!IsShadowMappingEnabled()) {
		return;
	}

	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;
	// Reset per-frame data
	sd->numShadowLights = 0;
	// Don't reset numShadowOnlyLights here - they're set during frontend processing

	// Reset atlas regions for dynamic allocation
	for (auto& region : sd->shadowAtlas.regions) {
		region.allocated = false;
		region.lightIndex = -1;
		region.cascadeIndex = -1;
	}
	sd->shadowAtlas.allocatedRegions = 0;
}

void ShadowMapManager::EndFrame() {
	if (!IsShadowMappingEnabled()) {
		return;
	}
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;
}

bool ShadowMapManager::IsShadowMappingEnabled() const {
	// Shadow mapping requires material system to be enabled
	if (!glConfig2.usingMaterialSystem) {
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

	atlas->colorImage = R_CreateImage(va("*shadowAtlas_%d", atlasSize), nullptr, atlasSize, atlasSize, 0, imageParams);

	// Create depth buffer for all techniques
	imageParams_t depthParams = {};
	depthParams.bits = IF_DEPTH24 | IF_NOPICMIP;
	depthParams.filterType = filterType_t::FT_NEAREST;
	depthParams.wrapType = wrapTypeEnum_t::WT_CLAMP;

	atlas->depthImage = R_CreateImage(va("*shadowAtlasDepth_%d", atlasSize), nullptr, atlasSize, atlasSize, 0, depthParams);

	// Create FBO
	CreateShadowMapFBO(atlas);

	Log::Debug("Shadow atlas created successfully");
}

void ShadowMapManager::ShutdownAtlas(shadowAtlas_t* atlas) {
	// FBOs are cleaned up by R_ShutdownFBOs(), images by image manager
	atlas->fbo = nullptr;
	atlas->colorImage = nullptr;
	atlas->depthImage = nullptr;
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

bool ShadowMapManager::AllocateAtlasRegion(shadowAtlas_t* atlas, int width, int height, vec2_t offset, int lightIndex, int cascade) {
	Log::Debug("Attempting to allocate atlas region %dx%d for light %d cascade %d in %dx%d atlas with %d existing regions",
	          width, height, lightIndex, cascade, atlas->size, atlas->size, atlas->allocatedRegions);

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
				newRegion.lightIndex = lightIndex;
				newRegion.cascadeIndex = cascade;

				atlas->allocatedRegions++;

				offset[0] = x;
				offset[1] = y;

				Log::Debug("Successfully allocated region at (%d, %d) size %dx%d for light %d cascade %d",
				          x, y, width, height, lightIndex, cascade);
				return true;
			}
		}
	}

	Log::Warn("Failed to allocate shadow atlas region %dx%d - atlas may be full", width, height);
	return false;
}

// Removed AllocateShadowMap and FreeShadowMap as per previous discussion

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

bool ShadowMapManager::SetupLightShadows(refLight_t* light) {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;
	int lightIndex = sd->numShadowLights;
	if (lightIndex >= MAX_SHADOW_LIGHTS) {
		Log::Warn("Light index %d exceeds MAX_SHADOW_LIGHTS (%d)", lightIndex, MAX_SHADOW_LIGHTS);
		return false;
	}

	Log::Debug("Setting up shadow for light %d: %s at (%.1f, %.1f, %.1f)",
	          lightIndex,
	          (light->rlType == refLightType_t::RL_DIRECTIONAL) ? "directional" :
	          (light->rlType == refLightType_t::RL_OMNI) ? "point" : "spot",
	          light->origin[0], light->origin[1], light->origin[2]);

	lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];
	memset(lightShadow, 0, sizeof(*lightShadow)); // Use sizeof(*lightShadow) for correct size
	lightShadow->castsShadows = true;

	// Determine number of cascades based on light type
	if (light->rlType == refLightType_t::RL_DIRECTIONAL) {
		lightShadow->numCascades = r_shadowCascades.Get();
		Log::Debug("Directional light detected, using %d cascades. Direction: (%.2f, %.2f, %.2f)",
		          lightShadow->numCascades,
		          light->projTarget[0], light->projTarget[1], light->projTarget[2]);
	} else {
		lightShadow->numCascades = 1; // Point/spot lights use single shadow map
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

		Log::Debug("Setting up cascade %d: technique=%d, size=%dx%d",
		          cascade, static_cast<int>(shadowMap->technique),
		          shadowMap->size[0], shadowMap->size[1]);

		// Allocate atlas region for this shadow map
		Log::Debug("Allocating atlas region %dx%d for light %d cascade %d",
		          shadowMap->size[0], shadowMap->size[1], lightIndex, cascade);

		if (!AllocateAtlasRegion(&sd->shadowAtlas, shadowMap->size[0], shadowMap->size[1],
		                         shadowMap->atlasOffset, lightIndex, cascade)) {
			Log::Warn("Failed to allocate shadow atlas region for light %d cascade %d", lightIndex, cascade);
			lightShadow->castsShadows = false;
			setupSuccess = false;
			// Don't return false immediately, continue to see all failures
		} else {
			Log::Debug("Allocated atlas region at (%d, %d) size %dx%d for light %d cascade %d",
			          shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
			          shadowMap->size[0], shadowMap->size[1],
			          lightIndex, cascade);

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
		sd->numShadowLights++;
		Log::Debug("Successfully set up shadow for light %d, total shadow lights now: %d", lightIndex, sd->numShadowLights);
		return true;
	} else {
		Log::Debug("Failed to set up shadow for light %d", lightIndex);
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

	// Removed memset(&shadowMap->lightFrustum, 0, sizeof(shadowMap->lightFrustum));

	Log::Debug("Set up light matrix for %s light at (%.1f, %.1f, %.1f)",
	          (light->rlType == refLightType_t::RL_DIRECTIONAL) ? "directional" :
	          (light->rlType == refLightType_t::RL_OMNI) ? "point" : "spot",
	          light->origin[0], light->origin[1], light->origin[2]);
}

void ShadowMapManager::AddShadowLight(const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags ) {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;
	if (sd->numShadowOnlyLights >= MAX_SHADOW_LIGHTS) {
		Log::Debug("Shadow light limit reached (%d), not adding new light", MAX_SHADOW_LIGHTS);
		return;
	}

	refLight_t* shadowLight = &sd->shadowOnlyLights[sd->numShadowOnlyLights];

	// Set up shadow light properties
	VectorCopy(org, shadowLight->origin);
	shadowLight->radius = radius;
	shadowLight->scale = intensity;
	shadowLight->color[0] = r;
	shadowLight->color[1] = g;
	shadowLight->color[2] = b;

	// Determine light type (default to omni for now)
	shadowLight->rlType = refLightType_t::RL_OMNI;

	Log::Debug("Added shadow-only light %d at (%.1f, %.1f, %.1f) radius=%.1f",
	          sd->numShadowOnlyLights, org[0], org[1], org[2], radius);

	sd->numShadowOnlyLights++;
}

void ShadowMapManager::UpdateShadowMaps() {
	shadowData_t *sd = &backEndData[backEnd.smpFrame]->shadowData;

	// Track frame changes
	static int lastFrameCount = -1;
	if (lastFrameCount != tr.frameCount) {
		lastFrameCount = tr.frameCount;
		Log::Debug("=== UPDATE SHADOW MAPS FRAME %d ===", tr.frameCount);
	}

	Log::Debug("Updating shadow maps: %d shadow-only lights collected, max allowed: %d, current shadow lights: %d",
	          sd->numShadowOnlyLights, r_shadowLights.Get(), sd->numShadowLights);

	int lightsProcessed = 0;
	for (int i = 0; i < sd->numShadowOnlyLights && sd->numShadowLights < r_shadowLights.Get(); i++) {
		refLight_t* light = &sd->shadowOnlyLights[i];
		Log::Debug("Setting up shadow for light %d at (%.1f, %.1f, %.1f)",
		          i, light->origin[0], light->origin[1], light->origin[2]);

		if (SetupLightShadows(light)) {
			Log::Debug("Successfully set up shadow for light %d", i);
			lightsProcessed++;
		} else {
			Log::Debug("Failed to set up shadow for light %d", i);
		}
	}

	Log::Debug("Updated shadow maps for %d shadow lights (%d shadow-only lights collected, %d processed)",
	          sd->numShadowLights, sd->numShadowOnlyLights, lightsProcessed);
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

	if (sd->numShadowLights == 0) {
		Log::Debug("No shadow lights to render, returning early");
		return;
	}

	if (!sd->shadowAtlas.fbo) {
		Log::Warn("Shadow atlas FBO not initialized");
		return;
	}

	// Check if shadow mapping is enabled
	if (!IsShadowMappingEnabled()) {
		Log::Debug("Shadow mapping is not enabled, returning early");
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

	Log::Debug("Atlas textures: color=%p (texnum=%d), depth=%p (texnum=%d)",
	          sd->shadowAtlas.colorImage, sd->shadowAtlas.colorImage->texnum,
	          sd->shadowAtlas.depthImage, sd->shadowAtlas.depthImage->texnum);

	// Additional debug info
	Log::Debug("Shadow atlas size: %d", sd->shadowAtlas.size);
	for (int i = 0; i < sd->numShadowLights && i < MAX_SHADOW_LIGHTS; i++) {
		Log::Debug("Light %d: numCascades=%d", i, sd->lightShadows[i].numCascades);
		for (int j = 0; j < sd->lightShadows[i].numCascades && j < MAX_SHADOW_CASCADES; j++) {
			Log::Debug("  Cascade %d: offset=(%d,%d), size=(%d,%d)", j,
			          static_cast<int>(sd->lightShadows[i].cascades[j].atlasOffset[0]),
			          static_cast<int>(sd->lightShadows[i].cascades[j].atlasOffset[1]),
			          static_cast<int>(sd->lightShadows[i].cascades[j].size[0]),
			          static_cast<int>(sd->lightShadows[i].cascades[j].size[1]));
		}
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
	shadowState |= GLS_DEPTHFUNC_LESS;

	// Enable depth mask
	// This is default, so we don't need to set a specific bit

	// Set color mask based on technique
	if (technique == shadowingMode_t::SHADOWING_ESM16 || technique == shadowingMode_t::SHADOWING_ESM32) {
		// ESM needs color output for exponential depth - disable blue and alpha masks
		shadowState |= GLS_BLUEMASK_FALSE | GLS_ALPHAMASK_FALSE;
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

	// Clear the entire shadow atlas once
	// For depth-only techniques, we want depth=1.0 (farthest)
	// For ESM/VSM techniques, we want color=1.0 (maximum depth value)
	GL_ClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	GL_ClearDepth(1.0);

	Log::Debug("Cleared shadow atlas with white color (1,1,1,1) and depth=1.0 - this should happen EVERY frame");

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

			// Set up rendering for this shadow map (viewport only)
			// Make sure we're setting the full size of the shadow map region
			GL_Viewport(shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
			           shadowMap->size[0], shadowMap->size[1]);

			// Set up matrices for light perspective
			GL_PushMatrix();
			GL_LoadProjectionMatrix(shadowMap->lightProjectionMatrix);
			GL_LoadModelViewMatrix(shadowMap->lightViewMatrix);

			// Log matrix info for debugging
			Log::Debug("Light projection matrix:");
			Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightProjectionMatrix[0], shadowMap->lightProjectionMatrix[4], shadowMap->lightProjectionMatrix[8], shadowMap->lightProjectionMatrix[12]);
			Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightProjectionMatrix[1], shadowMap->lightProjectionMatrix[5], shadowMap->lightProjectionMatrix[9], shadowMap->lightProjectionMatrix[13]);
			Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightProjectionMatrix[2], shadowMap->lightProjectionMatrix[6], shadowMap->lightProjectionMatrix[10], shadowMap->lightProjectionMatrix[14]);
			Log::Debug("  [%f, %f, %f, %f]", shadowMap->lightProjectionMatrix[3], shadowMap->lightProjectionMatrix[7], shadowMap->lightProjectionMatrix[11], shadowMap->lightProjectionMatrix[15]);

			// Render shadow casters for this light/cascade
			Log::Debug("About to render shadow casters for light %d cascade %d at atlas (%d, %d) size (%d, %d)",
			          lightIndex, cascade, shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
			          shadowMap->size[0], shadowMap->size[1]);
			RenderShadowCasters(&sd->shadowAtlas, shadowMap);
			GL_PopMatrix();

			Log::Debug("Rendered shadow map for light %d cascade %d at atlas (%d, %d) size (%d, %d)",
			          lightIndex, cascade, shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
			          shadowMap->size[0], shadowMap->size[1]);
		}
	}

	// Restore engine state
	glDisable(GL_POLYGON_OFFSET_FILL);
	GL_State(oldStateBits);
	GL_Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	// Restore scissor box (scissor test is always enabled by GL_SetDefaultState)
	GL_Scissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
	R_BindFBO(oldFBO);

	Log::Debug("Shadow atlas rendering complete for %d lights (actually rendered: %d)", sd->numShadowLights, renderedLights);
}

void ShadowMapManager::RenderShadowCasters(shadowAtlas_t* atlas, shadowMap_t* shadowMap) {
	GLIMP_LOGCOMMENT("--- ShadowMapManager::RenderShadowCasters ---");

	shadowingMode_t technique = shadowMap->technique;

	// Track frame changes
	static int lastFrameCount = -1;
	static int renderCallCount = 0;

	if (lastFrameCount != tr.frameCount) {
		lastFrameCount = tr.frameCount;
		renderCallCount = 0;
	}

	renderCallCount++;
	Log::Debug("RenderShadowCasters #%d for technique %d at atlas (%d, %d) size (%d, %d)",
	          renderCallCount, static_cast<int>(technique),
	          shadowMap->atlasOffset[0], shadowMap->atlasOffset[1],
	          shadowMap->size[0], shadowMap->size[1]);

	// Enable front-face culling for shadow maps to reduce shadow acne and improve performance
	cullType_t oldCullType = glState.faceCulling;
	GL_Cull(cullType_t::CT_FRONT_SIDED);

	// Save the current entity and surface shader states
	trRefEntity_t *oldEntity = backEnd.currentEntity;
	shader_t *oldShader = nullptr;
	int oldLightmapNum = -1;
	int oldFogNum = -1;
	bool oldDepthRange = false;
	bool depthRange = false;

	// For simplicity, render all opaque surfaces from the main view as shadow casters
	// This is a simplified implementation - ideally we'd do light-space culling here
	int firstSurf = backEnd.viewParms.firstDrawSurf[Util::ordinal(shaderSort_t::SS_ENVIRONMENT_FOG)];
	int lastSurf = backEnd.viewParms.firstDrawSurf[Util::ordinal(shaderSort_t::SS_OPAQUE) + 1];

	// Validate surface range
	if (firstSurf >= lastSurf || firstSurf < 0 || lastSurf > backEnd.viewParms.numDrawSurfs) {
		// Use safer bounds
		firstSurf = std::max(0, firstSurf);
		lastSurf = std::min(backEnd.viewParms.numDrawSurfs, lastSurf);
		if (firstSurf >= lastSurf) {
			Log::Warn("No valid surfaces to process for shadow casting");
			return;
		}
	}

	// Quick validation
	if (backEnd.viewParms.numDrawSurfs <= 0) {
		Log::Warn("No draw surfaces available for shadow casting");
		return;
	}

	Log::Debug("Processing surfaces from index %d to %d", firstSurf, lastSurf);

	int surfaceCount = 0;
	int renderedSurfaceCount = 0;
	int skippedSurfaceCount = 0;

	for (int i = firstSurf; i < lastSurf; i++) {
		drawSurf_t *drawSurf = &backEnd.viewParms.drawSurfs[i];

		// Skip invalid surfaces
		if (drawSurf->surface == nullptr) {
			skippedSurfaceCount++;
			continue;
		}

		surfaceCount++;

		// Log basic info for first surface only
	if (surfaceCount == 1 && drawSurf->shader) {
		Log::Debug("First surface: shader='%s', contentFlags=0x%x",
		          drawSurf->shader->name, drawSurf->shader->contentFlags);
	}

		// Get surface properties
		trRefEntity_t *entity = drawSurf->entity;
		shader_t *shader = drawSurf->shader;
		int lightmapNum = drawSurf->lightmapNum();
		int fogNum = drawSurf->fog;

		// Log surface info for first few surfaces
		if (surfaceCount <= 5) {
			Log::Debug("Processing surface %d: shader=%s, entity=%p",
			          i, shader ? shader->name : "null", entity);
		}

		// For now, render all surfaces as potential shadow casters
		// TODO: Add proper shadow casting flags/checks in the future

		// Skip translucent surfaces (sort is a float value)
		// Note: We check if contentFlags is non-zero first to avoid skipping surfaces
		// that don't have any content flags set. If no flags are set, we assume the
		// surface should be rendered for shadows.
		if (shader && shader->contentFlags && !(shader->contentFlags & CONTENTS_SOLID)) {
			skippedSurfaceCount++;
			continue;
		}



		// Check if we need to switch shader/entity state
		bool needNewBatch = (shader != oldShader || entity != oldEntity ||
		                     lightmapNum != oldLightmapNum || fogNum != oldFogNum);

		if (needNewBatch) {
			// End current batch if any
			if (oldShader != nullptr) {
				// Only call Tess_End if we actually have geometry
				if (tess.numVertexes > 0 && tess.numIndexes > 0) {
					Tess_End();
				}
			}

			// Start new batch with shadow depth stage iterator
			Tess_Begin(Tess_StageIteratorShadowDepth, shader, false, lightmapNum, fogNum, drawSurf->bspSurface);
			oldShader = shader;
			oldLightmapNum = lightmapNum;
			oldFogNum = fogNum;
			renderedSurfaceCount++;
		}

		// Handle entity transformation
		if (entity != oldEntity) {
			depthRange = false;

			if (entity != &tr.worldEntity) {
				backEnd.currentEntity = entity;

				// NOTE: We should NOT skip third-person entities from shadow casting in third-person mode
				// Third-person entities SHOULD cast shadows since they're visible in the scene
				// The RF_THIRD_PERSON flag just indicates it's a third-person view entity
				if (backEnd.currentEntity->e.renderfx & RF_THIRD_PERSON) {
					Log::Debug("Processing third-person entity %p for shadow casting (visible in scene)", entity);
					// Continue processing this entity - DO NOT skip it
				}

				// Also check for third person flag
				if (backEnd.currentEntity->e.renderfx & RF_THIRD_PERSON) {
					Log::Debug("Entity %p has RF_THIRD_PERSON flag set - this might exclude it from shadows!", entity);
				}

				// Set up the transformation matrix for this entity
				R_RotateEntityForViewParms(backEnd.currentEntity, &backEnd.viewParms, &backEnd.orientation);

				if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
					depthRange = true;
				}
			} else {
				backEnd.currentEntity = &tr.worldEntity;
				backEnd.orientation = backEnd.viewParms.world;

				if (surfaceCount <= 10) {
					Log::Debug("Processing world entity");
				}
			}

			GL_LoadModelViewMatrix(backEnd.orientation.modelViewMatrix);

			// Handle depth range changes
			if (oldDepthRange != depthRange) {
				if (depthRange) {
					glDepthRange(0, 0.3);
				} else {
					glDepthRange(0, 1);
				}
				oldDepthRange = depthRange;
			}

			oldEntity = entity;
		}

		// Add the surface geometry to the tessellator
	// Track if we're actually adding geometry
	static int geometryAddCount = 0;
	geometryAddCount++;

	if (geometryAddCount <= 10) {  // Log first 10 geometry additions
		int vertsBefore = tess.numVertexes;
		int indexesBefore = tess.numIndexes;

		rb_surfaceTable[Util::ordinal(*drawSurf->surface)](drawSurf->surface);

		int vertsAdded = tess.numVertexes - vertsBefore;
		int indexesAdded = tess.numIndexes - indexesBefore;

		if (vertsAdded > 0 || indexesAdded > 0) {
			Log::Debug("Added geometry #%d: %d verts, %d indexes",
			          geometryAddCount, vertsAdded, indexesAdded);
		}
	} else {
		rb_surfaceTable[Util::ordinal(*drawSurf->surface)](drawSurf->surface);
	}
	}

	// End the final batch
	if (oldShader != nullptr) {
		// Only call Tess_End if we actually have geometry
		if (tess.numVertexes > 0 && tess.numIndexes > 0) {
			static int tessEndCount = 0;
			tessEndCount++;

			if (tessEndCount <= 5) {
				Log::Debug("Tess_End #%d: %d verts, %d indexes",
				          tessEndCount, tess.numVertexes, tess.numIndexes);
			}

			Tess_End();

			if (tessEndCount <= 5) {
				Log::Debug("Ended final batch with shader %s", oldShader ? oldShader->name : "null");
			}
		}
	}

	// Restore world transformation
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.orientation = backEnd.viewParms.world;
	GL_LoadModelViewMatrix(backEnd.orientation.modelViewMatrix);

	// Reset depth range
	if (oldDepthRange) {
		glDepthRange(0, 1);
	}

	// Restore face culling state
	GL_Cull(oldCullType);

	Log::Debug("Shadow caster rendering complete: processed %d surfaces, rendered %d batches, skipped %d surfaces",
	          surfaceCount, renderedSurfaceCount, skippedSurfaceCount);

	// Add a sanity check - if we processed very few surfaces, log a warning
	if (surfaceCount < 10) {
		Log::Warn("Very few surfaces (%d) processed for shadow casting", surfaceCount);
	}

	// Log if we rendered any batches at all
	if (renderedSurfaceCount == 0) {
		Log::Warn("NO batches rendered for shadow casting!");
	} else {
		Log::Debug("Successfully rendered shadow map with %d batches", renderedSurfaceCount);
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
	int matrixIndex = 0;
	for (int lightIndex = 0; lightIndex < sd->numShadowLights && lightIndex < MAX_SHADOW_LIGHTS; lightIndex++) {
		const lightShadowInfo_t* lightShadow = &sd->lightShadows[lightIndex];

		for (int cascade = 0; cascade < lightShadow->numCascades && cascade < MAX_SHADOW_CASCADES; cascade++) {
			if (matrixIndex >= maxMatrices) {
				Log::Warn("Exceeded maximum shadow matrices: %d", maxMatrices);
				return;
			}

			const shadowMap_t* shadowMap = &lightShadow->cascades[cascade];
			MatrixCopy(shadowMap->lightViewProjectionMatrix, matrices[matrixIndex]);

			// Log matrix for debugging
			Log::Debug("Shadow matrix[%d] for light %d cascade %d:", matrixIndex, lightIndex, cascade);
			Log::Debug("  [%f, %f, %f, %f]", matrices[matrixIndex][0], matrices[matrixIndex][4], matrices[matrixIndex][8], matrices[matrixIndex][12]);
			Log::Debug("  [%f, %f, %f, %f]", matrices[matrixIndex][1], matrices[matrixIndex][5], matrices[matrixIndex][9], matrices[matrixIndex][13]);
			Log::Debug("  [%f, %f, %f, %f]", matrices[matrixIndex][2], matrices[matrixIndex][6], matrices[matrixIndex][10], matrices[matrixIndex][14]);
			Log::Debug("  [%f, %f, %f, %f]", matrices[matrixIndex][3], matrices[matrixIndex][7], matrices[matrixIndex][11], matrices[matrixIndex][15]);

			matrixIndex++;
		}
	}

	Log::Debug("Copied %d shadow matrices", matrixIndex);
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

		lightInfo[lightIndex][0] = static_cast<float>(lightShadow->cascades[0].technique); // technique
		lightInfo[lightIndex][1] = static_cast<float>(lightShadow->numCascades); // numCascades
		lightInfo[lightIndex][2] = static_cast<float>(lightShadow->cascades[0].atlasOffset[0]); // atlasOffset.x
		lightInfo[lightIndex][3] = static_cast<float>(lightShadow->cascades[0].atlasOffset[1]); // atlasOffset.y

		Log::Debug("Light %d info: technique=%d, cascades=%d, offset=(%d,%d)",
		          lightIndex, static_cast<int>(lightInfo[lightIndex][0]),
		          static_cast<int>(lightInfo[lightIndex][1]),
		          static_cast<int>(lightInfo[lightIndex][2]),
		          static_cast<int>(lightInfo[lightIndex][3]));
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

	// Create orthographic projection matrix
	// TODO: Calculate proper bounds based on scene geometry
	float size = 2048.0f; // Larger size for better coverage
	MatrixOrthogonalProjection(projectionMatrix, -size, size, -size, size, 1.0f, 4000.0f);

	Log::Debug("Created orthographic projection with bounds: -/+ %.1f, near=1.0, far=4000.0", size);

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
	// For point lights, we'll use a perspective projection
	// For now, implement as a single face (will expand to cube map later)

	vec3_t lightPos, lookDir, up;
	VectorCopy(light->origin, lightPos);

	// Look down -Z axis for now (will implement 6-face cube mapping later)
	VectorSet(lookDir, 0.0f, 0.0f, -1.0f);
	VectorSet(up, 0.0f, 1.0f, 0.0f);

	// Build view matrix
	MatrixLookAtRH(viewMatrix, lightPos, lookDir, up);

	// Create perspective projection
	float fov = 90.0f; // 90 degree FOV for cube face
	float aspect = 1.0f;
	float nearPlane = 1.0f;
	float farPlane = light->radius;

	// Convert FOV to projection bounds
	float top = nearPlane * tanf(DEG2RAD(fov * 0.5f));
	float bottom = -top;
	float right = top * aspect;
	float left = -right;

	MatrixPerspectiveProjection(projectionMatrix, left, right, bottom, top, nearPlane, farPlane);

	Log::Debug("Set up point light matrix, position=(%.1f, %.1f, %.1f) radius=%.1f",
	          lightPos[0], lightPos[1], lightPos[2], light->radius);
}

void ShadowMapManager::SetupSpotLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For spot lights, use perspective projection with light direction
	vec3_t lightPos, lightDir, up;
	VectorCopy(light->origin, lightPos);
	VectorCopy(light->projTarget, lightDir);
	VectorNormalize(lightDir);

	// Create up vector
	PerpendicularVector(up, lightDir);

	// Build view matrix
	MatrixLookAtRH(viewMatrix, lightPos, lightDir, up);

	// Create perspective projection based on spot light cone
	float fov = 60.0f; // Default spot light cone angle
	float aspect = 1.0f;
	float nearPlane = 1.0f;
	float farPlane = light->radius;

	// Convert FOV to projection bounds
	float top = nearPlane * tanf(DEG2RAD(fov * 0.5f));
	float bottom = -top;
	float right = top * aspect;
	float left = -right;

	MatrixPerspectiveProjection(projectionMatrix, left, right, bottom, top, nearPlane, farPlane);

	Log::Debug("Set up spot light matrix, position=(%.1f, %.1f, %.1f) direction=(%.2f, %.2f, %.2f)",
	          lightPos[0], lightPos[1], lightPos[2], lightDir[0], lightDir[1], lightDir[2]);
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
