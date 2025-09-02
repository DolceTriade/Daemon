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
// image_t and FBO_t are now in tr_local.h
#include <vector> // For std::vector

// Include tr_local.h for common renderer structures and shadow mapping structs
#include "tr_local.h"

class ShadowMapManager {
public:
	// Initialization
	void Init(shadowData_t *sd);
	void Shutdown(shadowData_t *sd);
	void BeginFrame();
	void EndFrame();

	// Shadow light management
	void AddShadowLight(const vec3_t org, float radius, float intensity, float r, float g, float b, qhandle_t hShader, int flags );

	// Shadow map management
	// AllocateShadowMap and FreeShadowMap are removed as per previous discussion
	void UpdateShadowMaps();

	// Atlas management
	bool AllocateAtlasRegion(shadowAtlas_t* atlas, int width, int height, vec2_t offset, int lightIndex, int cascade);

	// Rendering
    void RenderShadowMaps();

    // Debug
    void DebugRenderShadowAtlas();

	// Light management
	bool SetupLightShadows(refLight_t* light);

    // Utility
    bool IsShadowMappingEnabled() const;
    shadowingMode_t GetShadowTechnique() const;

	// Atlas access
	image_t* GetShadowAtlas(const shadowAtlas_t* atlas) const;

	// Shadow data access for shader uniforms
	void GetShadowMatrices(matrix_t* matrices, int maxMatrices) const;
	void GetShadowLightInfo(vec4_t* lightInfo, int maxLights) const;
	void GetCascadeSplits(vec4_t* splits, int maxLights) const;
    int GetNumShadowLights() const;

    // Frontend: build per-cascade views (drawSurfs + sort) from precomputed light matrices
    void BuildShadowViews();

private:
	// These members are now part of shadowData_t
	// shadowAtlas_t shadowAtlas;
	// lightShadowInfo_t lightShadows[MAX_SHADOW_LIGHTS];
	// int numShadowLights;
	// refLight_t shadowOnlyLights[MAX_SHADOW_LIGHTS];
	// int numShadowOnlyLights;

	// Internal methods
	void InitAtlas(shadowAtlas_t* atlas);
	void ShutdownAtlas(shadowAtlas_t* atlas);
	void CreateShadowMapFBO(shadowAtlas_t* atlas);
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
