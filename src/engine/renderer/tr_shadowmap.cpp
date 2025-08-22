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

#include "tr_local.h"

// Global shadow map manager instance
ShadowMapManager shadowMapManager;

//
// ShadowMapManager Implementation
//

void ShadowMapManager::Init() {
	Log::Debug("Initializing shadow mapping system...");
	
	// Reset state
	shadowAtlas = {};
	memset(lightShadows, 0, sizeof(lightShadows));
	numShadowLights = 0;
	
	// Only initialize if shadow mapping is enabled and material system is active
	if (!IsShadowMappingEnabled()) {
		Log::Debug("Shadow mapping disabled or material system not active");
		return;
	}
	
	// Initialize shadow atlas
	InitAtlas();
	
	Log::Debug("Shadow mapping system initialized");
}

void ShadowMapManager::Shutdown() {
	Log::Debug("Shutting down shadow mapping system...");
	
	ShutdownAtlas();
	
	// Reset state
	shadowAtlas = {};
	memset(lightShadows, 0, sizeof(lightShadows));
	numShadowLights = 0;
	
	Log::Debug("Shadow mapping system shut down");
}

void ShadowMapManager::BeginFrame() {
	if (!IsShadowMappingEnabled()) {
		return;
	}
	
	// Reset per-frame data
	numShadowLights = 0;
	
	// Reset atlas regions for dynamic allocation
	for (auto& region : shadowAtlas.regions) {
		region.allocated = false;
		region.lightIndex = -1;
		region.cascadeIndex = -1;
	}
	shadowAtlas.allocatedRegions = 0;
}

void ShadowMapManager::EndFrame() {
	if (!IsShadowMappingEnabled()) {
		return;
	}
	
	// Nothing to do for now - cleanup happens in BeginFrame
}

bool ShadowMapManager::IsShadowMappingEnabled() const {
	// Shadow mapping requires material system to be enabled
	if (!r_materialSystem.Get()) {
		return false;
	}
	
	shadowingMode_t technique = static_cast<shadowingMode_t>(r_shadows.Get());
	return technique >= shadowingMode_t::SHADOWING_ESM16 && technique <= shadowingMode_t::SHADOWING_EVSM32;
}

shadowingMode_t ShadowMapManager::GetShadowTechnique() const {
	return static_cast<shadowingMode_t>(r_shadows.Get());
}

void ShadowMapManager::InitAtlas() {
	int atlasSize = r_shadowAtlasSize.Get();
	
	Log::Debug("Creating shadow atlas: %dx%d", atlasSize, atlasSize);
	
	shadowAtlas.size = atlasSize;
	shadowAtlas.maxRegions = 64; // Initial reasonable limit
	shadowAtlas.regions.reserve(shadowAtlas.maxRegions);
	shadowAtlas.allocatedRegions = 0;
	
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
	
	shadowAtlas.colorImage = R_CreateImage(va("*shadowAtlas_%d", atlasSize), nullptr, atlasSize, atlasSize, 0, imageParams);
	
	// Create depth buffer for all techniques
	imageParams_t depthParams = {};
	depthParams.bits = IF_DEPTH24 | IF_NOPICMIP;
	depthParams.filterType = filterType_t::FT_NEAREST;
	depthParams.wrapType = wrapTypeEnum_t::WT_CLAMP;
	
	shadowAtlas.depthImage = R_CreateImage(va("*shadowAtlasDepth_%d", atlasSize), nullptr, atlasSize, atlasSize, 0, depthParams);
	
	// Create FBO
	CreateShadowMapFBO();
	
	Log::Debug("Shadow atlas created successfully");
}

void ShadowMapManager::ShutdownAtlas() {
	// FBOs are cleaned up by R_ShutdownFBOs(), images by image manager
	shadowAtlas.fbo = nullptr;
	shadowAtlas.colorImage = nullptr;
	shadowAtlas.depthImage = nullptr;
	
	shadowAtlas.regions.clear();
}

void ShadowMapManager::CreateShadowMapFBO() {
	shadowAtlas.fbo = R_CreateFBO("*shadowAtlas", shadowAtlas.size, shadowAtlas.size);
	
	if (!shadowAtlas.fbo) {
		Sys::Drop("Failed to create shadow atlas FBO");
	}
	
	R_BindFBO(shadowAtlas.fbo);
	
	// Attach color texture (for VSM/EVSM techniques)
	if (shadowAtlas.colorImage) {
		R_AttachFBOTexture2D(GL_TEXTURE_2D, shadowAtlas.colorImage->texnum, 0);
	}
	
	// Attach depth texture
	if (shadowAtlas.depthImage) {
		R_AttachFBOTextureDepth(shadowAtlas.depthImage->texnum);
	}
	
	// Check FBO completeness
	if (!R_CheckFBO(shadowAtlas.fbo)) {
		Sys::Drop("Shadow atlas FBO is not complete");
	}
	
	R_BindNullFBO();
}

bool ShadowMapManager::AllocateAtlasRegion(int width, int height, int* offset, int lightIndex, int cascade) {
	// Simple allocation strategy: find first free region that fits
	// TODO: Implement more sophisticated allocation (bin packing, etc.)
	
	for (int y = 0; y <= shadowAtlas.size - height; y += height) {
		for (int x = 0; x <= shadowAtlas.size - width; x += width) {
			bool canAllocate = true;
			
			// Check if this region overlaps with any allocated region
			for (const auto& region : shadowAtlas.regions) {
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
				atlasRegion_t newRegion;
				newRegion.offset[0] = x;
				newRegion.offset[1] = y;
				newRegion.size[0] = width;
				newRegion.size[1] = height;
				newRegion.allocated = true;
				newRegion.lightIndex = lightIndex;
				newRegion.cascadeIndex = cascade;
				
				shadowAtlas.regions.push_back(newRegion);
				shadowAtlas.allocatedRegions++;
				
				offset[0] = x;
				offset[1] = y;
				
				return true;
			}
		}
	}
	
	Log::Warn("Failed to allocate shadow atlas region %dx%d", width, height);
	return false;
}

shadowMap_t* ShadowMapManager::AllocateShadowMap(refLight_t* light, int cascade) {
	if (!IsShadowMappingEnabled() || numShadowLights >= MAX_SHADOW_LIGHTS) {
		return nullptr;
	}
	
	int lightIndex = numShadowLights++;
	lightShadowInfo_t* lightShadow = &lightShadows[lightIndex];
	
	// Initialize light shadow info
	lightShadow->castsShadows = true;
	lightShadow->numCascades = (light->rlType == refLightType_t::RL_DIRECTIONAL) ? r_shadowCascades.Get() : 1;
	
	// Allocate shadow map for the specified cascade (or first cascade if cascade == -1)
	int cascadeIndex = (cascade == -1) ? 0 : cascade;
	if (cascadeIndex >= lightShadow->numCascades) {
		return nullptr;
	}
	
	shadowMap_t* shadowMap = &lightShadow->cascades[cascadeIndex];
	
	// Set up shadow map properties
	shadowMap->technique = GetShadowTechnique();
	shadowMap->size[0] = r_shadowMapSize.Get();
	shadowMap->size[1] = r_shadowMapSize.Get();
	shadowMap->cascadeIndex = cascadeIndex;
	shadowMap->lightIndex = lightIndex;
	
	// Allocate atlas region
	if (!AllocateAtlasRegion(shadowMap->size[0], shadowMap->size[1], 
	                        shadowMap->atlasOffset, lightIndex, cascadeIndex)) {
		return nullptr;
	}
	
	// Set up technique-specific parameters
	switch (shadowMap->technique) {
		case shadowingMode_t::SHADOWING_ESM16:
		case shadowingMode_t::SHADOWING_ESM32:
			SetupESMParams(shadowMap);
			break;
		case shadowingMode_t::SHADOWING_VSM16:
		case shadowingMode_t::SHADOWING_VSM32:
			SetupVSMParams(shadowMap);
			break;
		case shadowingMode_t::SHADOWING_EVSM32:
			SetupEVSMParams(shadowMap);
			break;
		default:
			break;
	}
	
	// Set up light matrices
	SetupLightMatrix(light, shadowMap);
	
	return shadowMap;
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

void ShadowMapManager::SetupLightShadows(refLight_t* light, int lightIndex) {
	if ( lightIndex >= MAX_SHADOW_LIGHTS ) {
		return;
	}
	
	lightShadowInfo_t* lightShadow = &lightShadows[lightIndex];
	lightShadow->castsShadows = true;
	
	// Determine number of cascades based on light type
	if ( light->rlType == refLightType_t::RL_DIRECTIONAL ) {
		lightShadow->numCascades = r_shadowCascades.Get();
	} else {
		lightShadow->numCascades = 1; // Point/spot lights use single shadow map
	}
	
	// Set up shadow maps for each cascade
	for ( int cascade = 0; cascade < lightShadow->numCascades; cascade++ ) {
		shadowMap_t* shadowMap = &lightShadow->cascades[cascade];
		
		shadowMap->technique = GetShadowTechnique();
		shadowMap->size[0] = r_shadowMapSize.Get();
		shadowMap->size[1] = r_shadowMapSize.Get();
		shadowMap->cascadeIndex = cascade;
		shadowMap->lightIndex = lightIndex;
		
		// Allocate atlas region for this shadow map
		if ( !AllocateAtlasRegion(shadowMap->size[0], shadowMap->size[1], 
		                         shadowMap->atlasOffset, lightIndex, cascade) ) {
			Log::Warn("Failed to allocate shadow atlas region for light %d cascade %d", lightIndex, cascade);
			lightShadow->castsShadows = false;
			return;
		}
		
		// Set up technique-specific parameters
		switch (shadowMap->technique) {
			case shadowingMode_t::SHADOWING_ESM16:
			case shadowingMode_t::SHADOWING_ESM32:
				SetupESMParams(shadowMap);
				break;
			case shadowingMode_t::SHADOWING_VSM16:
			case shadowingMode_t::SHADOWING_VSM32:
				SetupVSMParams(shadowMap);
				break;
			case shadowingMode_t::SHADOWING_EVSM32:
				SetupEVSMParams(shadowMap);
				break;
			default:
				break;
		}
		
		// Set up light matrices for this shadow map
		SetupLightMatrix(light, shadowMap);
	}
}

void ShadowMapManager::SetupLightMatrix(refLight_t* light, shadowMap_t* shadowMap, const vec3_t* bounds) {
	// TODO: Implement proper light matrix setup based on light type
	// For now, just set up identity matrices as placeholder
	MatrixIdentity(shadowMap->lightViewMatrix);
	MatrixIdentity(shadowMap->lightProjectionMatrix);
	MatrixIdentity(shadowMap->lightViewProjectionMatrix);
	
	// TODO: Set up light frustum properly
	memset(&shadowMap->lightFrustum, 0, sizeof(shadowMap->lightFrustum));
}

//
// Global shadow mapping functions
//

bool R_ShadowMappingEnabled() {
	return shadowMapManager.IsShadowMappingEnabled();
}

void R_InitShadowMapping() {
	shadowMapManager.Init();
}

void R_ShutdownShadowMapping() {
	shadowMapManager.Shutdown();
}

void R_BeginShadowMapping() {
	shadowMapManager.BeginFrame();
}

void R_EndShadowMapping() {
	shadowMapManager.EndFrame();
}
