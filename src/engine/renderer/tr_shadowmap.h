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

In addition, the Daemon Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following the
terms and conditions of the GNU General Public License which accompanied the Daemon
Source Code.  If not, please request a copy in writing from id Software at the address
below.

===========================================================================
*/

// tr_shadowmap.h - Shadow mapping system

#ifndef TR_SHADOWMAP_H
#define TR_SHADOWMAP_H

// Forward declarations
struct refLight_t;
struct viewParms_t;
struct image_t;
struct FBO_t;

// Maximum number of shadow-casting lights
#define MAX_SHADOW_LIGHTS 8
#define MAX_SHADOW_CASCADES 4

// Shadow map atlas region allocation
struct atlasRegion_t {
	int offset[2];
	int size[2];
	bool allocated;
	int lightIndex;
	int cascadeIndex;
};

// Shadow map data for a single light/cascade
struct shadowMap_t {
	shadowingMode_t technique;
	int atlasOffset[2];
	int size[2];
	int cascadeIndex;
	int lightIndex;
	
	// Technique-specific parameters
	union {
		struct { 
			float exponent; 
		} esm;
		struct { 
			float minVariance; 
			float blurRadius;
		} vsm;  
		struct { 
			float exponent; 
			float minVariance; 
		} evsm;
	} params;
	
	// Light space matrices
	matrix_t lightViewMatrix;
	matrix_t lightProjectionMatrix;
	matrix_t lightViewProjectionMatrix;
	frustum_t lightFrustum;
	
	// Cascade data (for directional lights)
	float cascadeSplit;
	vec3_t cascadeBounds[2];
};

// Shadow atlas containing all shadow maps
struct shadowAtlas_t {
	// Atlas textures
	image_t* colorImage;      // For VSM/EVSM (RG format)
	image_t* depthImage;      // Depth buffer
	FBO_t* fbo;
	
	// Atlas properties
	int size;
	int maxRegions;
	
	// Region allocation
	std::vector<atlasRegion_t> regions;
	int allocatedRegions;
};

// Per-light shadow information
struct lightShadowInfo_t {
	bool castsShadows;
	int numCascades;
	shadowMap_t cascades[MAX_SHADOW_CASCADES];
	float cascadeSplits[MAX_SHADOW_CASCADES];
};

class ShadowMapManager {
public:
	// Initialization
	void Init();
	void Shutdown();
	void BeginFrame();
	void EndFrame();
	
	// Shadow light management
	void AddShadowLight( const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags );
	
	// Shadow map management
	shadowMap_t* AllocateShadowMap(refLight_t* light, int cascade = -1);
	void FreeShadowMap(shadowMap_t* shadowMap);
	void UpdateShadowMaps();
	
	// Atlas management
	void ResizeAtlas(int newSize);
	bool AllocateAtlasRegion(int width, int height, int* offset, int lightIndex, int cascade);
	void FreeAtlasRegion(const int* offset);
	
	// Rendering
	void RenderShadowMaps();
	void SetupShadowMapRendering(shadowMap_t* shadowMap);
	void RenderShadowCasters(shadowMap_t* shadowMap);
	void FinishShadowMapRendering();
	
	// Light management
	void SetupLightShadows(refLight_t* light, int lightIndex);
	void UpdateCascadeSplits(lightShadowInfo_t* lightShadow, const viewParms_t* viewParms);
	
	// Utility
	bool IsShadowMappingEnabled() const;
	shadowingMode_t GetShadowTechnique() const;
	
private:
	shadowAtlas_t shadowAtlas;
	lightShadowInfo_t lightShadows[MAX_SHADOW_LIGHTS];
	int numShadowLights;
	
	// Shadow-only lights from cgame (REF_INVERSE_DLIGHT)
	refLight_t shadowOnlyLights[MAX_SHADOW_LIGHTS];
	int numShadowOnlyLights;
	
	// Internal methods
	void InitAtlas();
	void ShutdownAtlas();
	void CreateShadowMapFBO();
	void SetupLightMatrix(refLight_t* light, shadowMap_t* shadowMap, const vec3_t* bounds = nullptr);
	void CalculateCascadeBounds(const viewParms_t* viewParms, float nearSplit, float farSplit, vec3_t bounds[2]);
	
	// Light matrix calculation for different light types
	void SetupDirectionalLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix);
	void SetupPointLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix);
	void SetupSpotLightMatrix(refLight_t* light, shadowMap_t* shadowMap, matrix_t viewMatrix, matrix_t projectionMatrix);
	
	// Technique-specific setup
	void SetupESMParams(shadowMap_t* shadowMap);
	void SetupVSMParams(shadowMap_t* shadowMap);
	void SetupEVSMParams(shadowMap_t* shadowMap);
};

// Global shadow map manager
extern ShadowMapManager shadowMapManager;

// Shadow mapping utility functions
bool R_ShadowMappingEnabled();
void R_InitShadowMapping();
void R_ShutdownShadowMapping();
void R_BeginShadowMapping();
void R_EndShadowMapping();

// Shadow light management
void R_AddShadowLight( const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags );

#endif // TR_SHADOWMAP_H
