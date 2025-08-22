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
	// Don't reset numShadowOnlyLights here - they're set during frontend processing
	
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
	
	// Reset shadow-only lights at end of frame, after rendering is complete
	numShadowOnlyLights = 0;
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
		Log::Warn("Light index %d exceeds MAX_SHADOW_LIGHTS (%d)", lightIndex, MAX_SHADOW_LIGHTS);
		return;
	}
	
	Log::Debug("Setting up shadow for light %d: %s at (%.1f, %.1f, %.1f)", 
	          lightIndex, 
	          (light->rlType == refLightType_t::RL_DIRECTIONAL) ? "directional" :
	          (light->rlType == refLightType_t::RL_OMNI) ? "point" : "spot",
	          light->origin[0], light->origin[1], light->origin[2]);
	
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
		Log::Debug("Allocating atlas region %dx%d for light %d cascade %d", 
		          shadowMap->size[0], shadowMap->size[1], lightIndex, cascade);
		          
		if ( !AllocateAtlasRegion(shadowMap->size[0], shadowMap->size[1], 
		                         shadowMap->atlasOffset, lightIndex, cascade) ) {
			Log::Warn("Failed to allocate shadow atlas region for light %d cascade %d", lightIndex, cascade);
			lightShadow->castsShadows = false;
			return;
		}
		
		Log::Debug("Allocated atlas region at (%d, %d) for light %d cascade %d", 
		          shadowMap->atlasOffset[0], shadowMap->atlasOffset[1], lightIndex, cascade);
		
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
	matrix_t viewMatrix, projectionMatrix;
	
	switch ( light->rlType ) {
		case refLightType_t::RL_DIRECTIONAL:
			SetupDirectionalLightMatrix( light, shadowMap, viewMatrix, projectionMatrix );
			break;
			
		case refLightType_t::RL_OMNI:
			SetupPointLightMatrix( light, shadowMap, viewMatrix, projectionMatrix );
			break;
			
		case refLightType_t::RL_PROJ:
			SetupSpotLightMatrix( light, shadowMap, viewMatrix, projectionMatrix );
			break;
			
		default:
			Log::Warn("Unknown light type for shadow mapping: %d", static_cast<int>(light->rlType));
			MatrixIdentity(viewMatrix);
			MatrixIdentity(projectionMatrix);
			break;
	}
	
	// Store matrices
	MatrixCopy( viewMatrix, shadowMap->lightViewMatrix );
	MatrixCopy( projectionMatrix, shadowMap->lightProjectionMatrix );
	MatrixMultiply( projectionMatrix, viewMatrix, shadowMap->lightViewProjectionMatrix );
	
	// TODO: Set up light frustum from matrices
	memset(&shadowMap->lightFrustum, 0, sizeof(shadowMap->lightFrustum));
	
	Log::Debug("Set up light matrix for %s light at (%.1f, %.1f, %.1f)", 
	          (light->rlType == refLightType_t::RL_DIRECTIONAL) ? "directional" :
	          (light->rlType == refLightType_t::RL_OMNI) ? "point" : "spot",
	          light->origin[0], light->origin[1], light->origin[2]);
}

void ShadowMapManager::AddShadowLight( const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags ) {
	if ( numShadowOnlyLights >= MAX_SHADOW_LIGHTS ) {
		return;
	}
	
	refLight_t* shadowLight = &shadowOnlyLights[numShadowOnlyLights];
	
	// Set up shadow light properties
	VectorCopy( org, shadowLight->origin );
	shadowLight->radius = radius;
	shadowLight->scale = intensity;
	shadowLight->color[0] = r;
	shadowLight->color[1] = g; 
	shadowLight->color[2] = b;
	
	// Determine light type (default to omni for now)
	shadowLight->rlType = refLightType_t::RL_OMNI;
	
	Log::Debug("Added shadow-only light %d at (%.1f, %.1f, %.1f) radius=%.1f", 
	          numShadowOnlyLights, org[0], org[1], org[2], radius);
	
	numShadowOnlyLights++;
}

void ShadowMapManager::UpdateShadowMaps() {
	// Set up shadow maps for all collected shadow-only lights
	numShadowLights = 0; // Reset before setting up
	
	for ( int i = 0; i < numShadowOnlyLights && i < r_shadowLights.Get(); i++ ) {
		refLight_t* light = &shadowOnlyLights[i];
		SetupLightShadows( light, numShadowLights );
		numShadowLights++; // Increment after each successful setup
	}
	
	Log::Debug("Updated shadow maps for %d shadow lights (%d shadow-only lights collected)", 
	          numShadowLights, numShadowOnlyLights);
}

void ShadowMapManager::RenderShadowMaps() {
	if ( numShadowLights == 0 ) {
		return;
	}
	
	if ( !shadowAtlas.fbo ) {
		Log::Warn("Shadow atlas FBO not initialized");
		return;
	}
	
	// Bind shadow atlas FBO for rendering
	R_BindFBO( shadowAtlas.fbo );
	
	// Clear the entire shadow atlas
	glClearColor( 1.0f, 1.0f, 1.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	
	// Save current OpenGL state
	int prevViewport[4];
	glGetIntegerv( GL_VIEWPORT, prevViewport );
	
	// Render shadow map for each shadow light
	for ( int lightIndex = 0; lightIndex < numShadowLights; lightIndex++ ) {
		lightShadowInfo_t* lightShadow = &lightShadows[lightIndex];
		
		if ( !lightShadow->castsShadows ) {
			continue;
		}
		
		// Render each cascade for this light
		for ( int cascade = 0; cascade < lightShadow->numCascades; cascade++ ) {
			shadowMap_t* shadowMap = &lightShadow->cascades[cascade];
			
			// Set up rendering for this shadow map
			SetupShadowMapRendering( shadowMap );
			
			// Render shadow casters for this light/cascade
			RenderShadowCasters( shadowMap );
			
			Log::Debug("Rendered shadow map for light %d cascade %d at atlas (%d, %d)", 
			          lightIndex, cascade, shadowMap->atlasOffset[0], shadowMap->atlasOffset[1]);
		}
	}
	
	// Restore viewport and FBO
	glViewport( prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3] );
	R_BindNullFBO();
	
	Log::Debug("Shadow atlas rendering complete for %d lights", numShadowLights);
}

void ShadowMapManager::SetupShadowMapRendering(shadowMap_t* shadowMap) {
	// Set viewport to this shadow map's atlas region
	glViewport( shadowMap->atlasOffset[0], shadowMap->atlasOffset[1], 
	           shadowMap->size[0], shadowMap->size[1] );
	
	// Set up depth testing for shadow map generation
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDepthMask( GL_TRUE );
	
	// Disable color writes for depth-only rendering (except for VSM/EVSM)
	shadowingMode_t technique = shadowMap->technique;
	if ( technique == shadowingMode_t::SHADOWING_ESM16 || technique == shadowingMode_t::SHADOWING_ESM32 ) {
		// ESM needs color output for exponential depth
		glColorMask( GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE );
	} else if ( technique == shadowingMode_t::SHADOWING_VSM16 || technique == shadowingMode_t::SHADOWING_VSM32 || 
	           technique == shadowingMode_t::SHADOWING_EVSM32 ) {
		// VSM/EVSM need RG output for moments
		glColorMask( GL_TRUE, GL_TRUE, GL_FALSE, GL_FALSE );
	} else {
		// Depth only
		glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	}
	
	// Clear this shadow map region
	glClearDepth( 1.0 );
	if ( technique >= shadowingMode_t::SHADOWING_ESM16 ) {
		glClearColor( 1.0f, 1.0f, 1.0f, 1.0f ); // Clear to white for shadow techniques
	}
	glClear( GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT );
	
	// Set up matrices for light perspective
	GL_LoadProjectionMatrix( shadowMap->lightProjectionMatrix );
	GL_LoadModelViewMatrix( shadowMap->lightViewMatrix );
	
	Log::Debug("Set up shadow map rendering: viewport=(%d,%d,%d,%d) technique=%d", 
	          shadowMap->atlasOffset[0], shadowMap->atlasOffset[1], 
	          shadowMap->size[0], shadowMap->size[1], static_cast<int>(technique));
}

void ShadowMapManager::RenderShadowCasters(shadowMap_t* shadowMap) {
	// TODO: Render shadow casting geometry using shadowDepth shader
	// For now, just a placeholder that clears to specific colors for debugging
	
	shadowingMode_t technique = shadowMap->technique;
	
	// Clear with technique-specific debug color
	switch ( technique ) {
		case shadowingMode_t::SHADOWING_ESM16:
			glClearColor( 0.8f, 0.0f, 0.0f, 1.0f ); // Red tint for ESM16
			break;
		case shadowingMode_t::SHADOWING_ESM32:
			glClearColor( 0.0f, 0.8f, 0.0f, 1.0f ); // Green tint for ESM32
			break;
		case shadowingMode_t::SHADOWING_VSM16:
			glClearColor( 0.0f, 0.0f, 0.8f, 1.0f ); // Blue tint for VSM16
			break;
		case shadowingMode_t::SHADOWING_VSM32:
			glClearColor( 0.8f, 0.8f, 0.0f, 1.0f ); // Yellow tint for VSM32
			break;
		case shadowingMode_t::SHADOWING_EVSM32:
			glClearColor( 0.8f, 0.0f, 0.8f, 1.0f ); // Magenta tint for EVSM32
			break;
		default:
			glClearColor( 0.5f, 0.5f, 0.5f, 1.0f ); // Gray for unknown
			break;
	}
	
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	
	Log::Debug("Shadow caster rendering placeholder for technique %d", static_cast<int>(technique));
}

// Light matrix calculation functions
void ShadowMapManager::SetupDirectionalLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For directional lights, we need an orthographic projection
	// Light direction comes from projTarget
	vec3_t lightDir;
	VectorCopy( light->projTarget, lightDir );
	VectorNormalize( lightDir );
	
	// Create view matrix looking down the light direction
	vec3_t lightPos, up, right;
	
	// Position the light far away in the opposite direction
	VectorScale( lightDir, -1000.0f, lightPos );
	VectorAdd( lightPos, light->origin, lightPos );
	
	// Create orthonormal basis
	PerpendicularVector( up, lightDir );
	CrossProduct( lightDir, up, right );
	
	// Build view matrix
	MatrixLookAtRH( viewMatrix, lightPos, lightDir, up );
	
	// Create orthographic projection matrix
	// TODO: Calculate proper bounds based on scene geometry
	float size = 512.0f; // Temporary fixed size
	MatrixOrthogonalProjection( projectionMatrix, -size, size, -size, size, 1.0f, 2000.0f );
	
	Log::Debug("Set up directional light matrix, direction=(%.2f, %.2f, %.2f)", lightDir[0], lightDir[1], lightDir[2]);
}

void ShadowMapManager::SetupPointLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For point lights, we'll use a perspective projection
	// For now, implement as a single face (will expand to cube map later)
	
	vec3_t lightPos, lookDir, up;
	VectorCopy( light->origin, lightPos );
	
	// Look down -Z axis for now (will implement 6-face cube mapping later)
	VectorSet( lookDir, 0.0f, 0.0f, -1.0f );
	VectorSet( up, 0.0f, 1.0f, 0.0f );
	
	// Build view matrix
	MatrixLookAtRH( viewMatrix, lightPos, lookDir, up );
	
	// Create perspective projection 
	float fov = 90.0f; // 90 degree FOV for cube face
	float aspect = 1.0f;
	float nearPlane = 1.0f;
	float farPlane = light->radius;
	
	// Convert FOV to projection bounds
	float top = nearPlane * tanf( DEG2RAD( fov * 0.5f ) );
	float bottom = -top;
	float right = top * aspect;
	float left = -right;
	
	MatrixPerspectiveProjection( projectionMatrix, left, right, bottom, top, nearPlane, farPlane );
	
	Log::Debug("Set up point light matrix, position=(%.1f, %.1f, %.1f) radius=%.1f", 
	          lightPos[0], lightPos[1], lightPos[2], light->radius);
}

void ShadowMapManager::SetupSpotLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix) {
	// For spot lights, use perspective projection with light direction
	vec3_t lightPos, lightDir, up;
	VectorCopy( light->origin, lightPos );
	VectorCopy( light->projTarget, lightDir );
	VectorNormalize( lightDir );
	
	// Create up vector
	PerpendicularVector( up, lightDir );
	
	// Build view matrix
	MatrixLookAtRH( viewMatrix, lightPos, lightDir, up );
	
	// Create perspective projection based on spot light cone
	float fov = 60.0f; // Default spot light cone angle
	float aspect = 1.0f;
	float nearPlane = 1.0f;
	float farPlane = light->radius;
	
	// Convert FOV to projection bounds
	float top = nearPlane * tanf( DEG2RAD( fov * 0.5f ) );
	float bottom = -top;
	float right = top * aspect;
	float left = -right;
	
	MatrixPerspectiveProjection( projectionMatrix, left, right, bottom, top, nearPlane, farPlane );
	
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

void R_AddShadowLight( const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags ) {
	shadowMapManager.AddShadowLight( org, radius, intensity, r, g, b, hShader, flags );
}
