/*
===========================================================================

Daemon BSD Source Code
Copyright (c) 2024 Daemon Developers
All rights reserved.

This file is part of the Daemon BSD Source Code (Daemon Source Code).

===========================================================================
*/

/* shadowMapping.glsl - Core shadow mapping functions */

#ifndef SHADOW_MAPPING_GLSL
#define SHADOW_MAPPING_GLSL

// Shadow mapping uniforms (only declared when shadow mapping is enabled)
#if defined(USE_SHADOW_MAPPING)

#ifndef MAX_SHADOW_LIGHTS
#define MAX_SHADOW_LIGHTS 8
#endif
#define SHADOW_SLICES_PER_LIGHT 6
#define MAX_SHADOW_SLICES (MAX_SHADOW_LIGHTS * SHADOW_SLICES_PER_LIGHT)
#define POINT_SHADOW_FACE_COUNT 6

// Shadow atlas and parameters
uniform sampler2D u_ShadowAtlas;
uniform vec4 u_ShadowParams; // x: bias, y: ESM exponent, z: PCF filter size, w: unused

// Shadow matrices (one 4x4 transform per light slice)
uniform mat4 u_ShadowMatrices[MAX_SHADOW_SLICES]; // lightIndex * SHADOW_SLICES_PER_LIGHT + sliceIndex

// Per-slice atlas transforms (offset.xy, scale.xy)
uniform vec4 u_ShadowTileInfo[MAX_SHADOW_SLICES];

// Cascade splits for directional lights
uniform vec4 u_CascadeSplits[MAX_SHADOW_LIGHTS]; // Per light cascade splits

// Light shadow info per shadow slot (vec4 for portability)
// x: lightIndex in u_Lights (scene/UBO index), y: numSlices, z: baseSliceIndex, w: REF_* flags
uniform vec4 u_ShadowLightInfo[MAX_SHADOW_LIGHTS];

// Current technique being used
uniform int u_ShadowTechnique;

//
// Shadow sampling functions
//

// ESM16 shadow sampling
float SampleShadowESM16(sampler2D shadowMap, vec3 shadowCoord, float exponent, bool inverseLight) {
	float stored = texture(shadowMap, shadowCoord.xy).r;
	float esm;
	if (inverseLight) {
		float receiverExp = exp(exponent * (1.0 - shadowCoord.z));
		float denom = max(stored, 1e-6);
		esm = receiverExp / denom;
	} else {
		float receiverExp = exp(exponent * shadowCoord.z);
		receiverExp = max(receiverExp, 1e-6);
		esm = stored / receiverExp;
	}
	return clamp(esm, 0.0, 1.0);
}

// ESM32 shadow sampling (same as ESM16 but with higher precision)
float SampleShadowESM32(sampler2D shadowMap, vec3 shadowCoord, float exponent, bool inverseLight) {
	return SampleShadowESM16(shadowMap, shadowCoord, exponent, inverseLight);
}

// VSM16/32 shadow sampling
float SampleShadowVSM(sampler2D shadowMap, vec3 shadowCoord) {
	vec2 moments = texture(shadowMap, shadowCoord.xy).rg;
	float depth = shadowCoord.z;

	// Basic VSM
	if (depth <= moments.x) {
		return 1.0;
	}

	float variance = moments.y - (moments.x * moments.x);
	variance = max(variance, 0.0001); // Prevent division by zero

	float d = depth - moments.x;
	float pMax = variance / (variance + d * d);

	return pMax;
}

// EVSM32 shadow sampling
float SampleShadowEVSM32(sampler2D shadowMap, vec3 shadowCoord, float exponent, bool inverseLight) {
	vec2 texExp = texture(shadowMap, shadowCoord.xy).rg; // [exp(+k*z_occ), exp(-k*z_occ)]
	float z = inverseLight ? (1.0 - shadowCoord.z) : shadowCoord.z;
	float posR = exp(exponent * z);
	float negR = exp(-exponent * z);
	// For the negative warp, the function is monotonically decreasing,
	// so lit requires negR >= exp(-k*z_occ).
	float litPos = posR <= texExp.x ? 1.0 : 0.0;
	float litNeg = negR >= texExp.y ? 1.0 : 0.0;
	return min(litPos, litNeg);
}

// PCF filtering for shadow maps
float SampleShadowPCF(sampler2D shadowMap, vec3 shadowCoord, int filterType, vec2 shadowMapSize,
	float exponent, bool inverseLight, vec2 tileMin, vec2 tileMax) {
	vec2 texelSize = 1.0 / shadowMapSize;
	float shadow = 0.0;
	int samples = 0;
	vec2 clampedBase = clamp(shadowCoord.xy, tileMin, tileMax);

	if (filterType == 0) {
		// No PCF - single sample
		vec3 singleCoord = vec3(clampedBase, shadowCoord.z);
		switch (u_ShadowTechnique) {
			case 2: // SHADOWING_ESM16
				return SampleShadowESM16(shadowMap, singleCoord, exponent, inverseLight);
			case 3: // SHADOWING_ESM32
				return SampleShadowESM32(shadowMap, singleCoord, exponent, inverseLight);
			case 4: // SHADOWING_VSM16
			case 5: // SHADOWING_VSM32
				return SampleShadowVSM(shadowMap, singleCoord);
			case 6: // SHADOWING_EVSM32
				return SampleShadowEVSM32(shadowMap, singleCoord, exponent, inverseLight);
			default:
				return 1.0;
		}
	}
	else if (filterType == 1) {
		// 2x2 PCF
		for (int x = -1; x <= 0; x++) {
			for (int y = -1; y <= 0; y++) {
				vec3 sampleCoord = shadowCoord + vec3(float(x) * texelSize.x, float(y) * texelSize.y, 0.0);
				sampleCoord.xy = clamp(sampleCoord.xy, tileMin, tileMax);

				switch (u_ShadowTechnique) {
					case 2: shadow += SampleShadowESM16(shadowMap, sampleCoord, exponent, inverseLight); break;
					case 3: shadow += SampleShadowESM32(shadowMap, sampleCoord, exponent, inverseLight); break;
					case 4:
					case 5: shadow += SampleShadowVSM(shadowMap, sampleCoord); break;
					case 6: shadow += SampleShadowEVSM32(shadowMap, sampleCoord, exponent, inverseLight); break;
					default: shadow += 1.0; break;
				}
				samples++;
			}
		}
	}
	else if (filterType == 2) {
		// 4x4 PCF
		for (int x = -2; x <= 1; x++) {
			for (int y = -2; y <= 1; y++) {
				vec3 sampleCoord = shadowCoord + vec3(float(x) * texelSize.x, float(y) * texelSize.y, 0.0);
				sampleCoord.xy = clamp(sampleCoord.xy, tileMin, tileMax);

				switch (u_ShadowTechnique) {
					case 2: shadow += SampleShadowESM16(shadowMap, sampleCoord, exponent, inverseLight); break;
					case 3: shadow += SampleShadowESM32(shadowMap, sampleCoord, exponent, inverseLight); break;
					case 4:
					case 5: shadow += SampleShadowVSM(shadowMap, sampleCoord); break;
					case 6: shadow += SampleShadowEVSM32(shadowMap, sampleCoord, exponent, inverseLight); break;
					default: shadow += 1.0; break;
				}
				samples++;
			}
		}
	}
	else if (filterType == 3) {
		// Poisson disk sampling
		vec2 poissonDisk[16] = vec2[](
			vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
			vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
			vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
			vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
			vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
			vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
			vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
			vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
		);

		for (int i = 0; i < 16; i++) {
			vec3 sampleCoord = shadowCoord + vec3(poissonDisk[i] * texelSize, 0.0);
			sampleCoord.xy = clamp(sampleCoord.xy, tileMin, tileMax);

			switch (u_ShadowTechnique) {
				case 2: shadow += SampleShadowESM16(shadowMap, sampleCoord, exponent, inverseLight); break;
				case 3: shadow += SampleShadowESM32(shadowMap, sampleCoord, exponent, inverseLight); break;
				case 4:
				case 5: shadow += SampleShadowVSM(shadowMap, sampleCoord); break;
				case 6: shadow += SampleShadowEVSM32(shadowMap, sampleCoord, exponent, inverseLight); break;
				default: shadow += 1.0; break;
			}
			samples++;
		}
	}

	return (samples > 0) ? shadow / float(samples) : 1.0;
}

// Cascade selection for directional lights
int SelectShadowCascade(float viewDepth, int lightIndex) {
	vec4 splits = u_CascadeSplits[lightIndex];

	if (viewDepth < splits.x) return 0;
	if (viewDepth < splits.y) return 1;
	if (viewDepth < splits.z) return 2;
	return 3;
}

// Main shadow calculation function
int SelectOmniShadowFace(vec3 L) {
    vec3 absDir = abs(L);

    int faceIndex = 0;
    if (absDir.x >= absDir.y && absDir.x >= absDir.z) {
        faceIndex = (L.x >= 0.0) ? 0 : 1;
    } else if (absDir.y >= absDir.x && absDir.y >= absDir.z) {
        faceIndex = (L.y >= 0.0) ? 2 : 3;
    } else {
        faceIndex = (L.z >= 0.0) ? 4 : 5;
    }
    return faceIndex;
}

float CalculateShadowFactor(vec3 worldPos, vec3 viewOrigin, vec3 normal, int lightIndex, float lightType, vec3 lightPosition) {
    if (lightIndex < 0 || lightIndex >= MAX_SHADOW_LIGHTS) {
        return 1.0; // No shadow
    }

    vec4 lightInfo = u_ShadowLightInfo[lightIndex];
    int numSlices = int(lightInfo.y + 0.5);
    int baseSlice = int(lightInfo.z + 0.5);

    // Early out if shadow mapping is disabled at runtime
    if (u_ShadowTechnique <= 1) {
        return 1.0;
    }

	// Calculate view-space depth for cascade selection
	float viewDepth = length(worldPos - viewOrigin);
	int sliceIndex = 0;

	if (numSlices <= 0) {
		return 1.0;
	}

	if (lightType == 0.0 && numSlices == POINT_SHADOW_FACE_COUNT) {
		vec3 toFragment = worldPos - lightPosition;
		if (length(toFragment) < 1e-4) {
			sliceIndex = 0;
		} else {
			sliceIndex = SelectOmniShadowFace(toFragment);
		}
	} else if (numSlices > 1) {
		int cascadeIndex = SelectShadowCascade(viewDepth, lightIndex);
		sliceIndex = clamp(cascadeIndex, 0, numSlices - 1);
	}

	int matrixIndex = baseSlice + sliceIndex;
	if (matrixIndex < 0 || matrixIndex >= MAX_SHADOW_SLICES) {
		return 1.0;
	}

	mat4 shadowMatrix = u_ShadowMatrices[matrixIndex];
    vec4 tile = u_ShadowTileInfo[matrixIndex];
    if (tile.z <= 0.0 || tile.w <= 0.0) {
        return 1.0;
    }

	// Transform world position to light space
	vec4 lightSpacePos = shadowMatrix * vec4(worldPos, 1.0);
	vec3 shadowCoord = lightSpacePos.xyz / lightSpacePos.w;

	// Transform to [0,1] range
	shadowCoord = shadowCoord * 0.5 + 0.5;

	// Apply receiver bias and keep depth inside clip range so near-zero samples
	// (common with point lights placed close to the caster) are not discarded.
	float bias = u_ShadowParams.x;
	shadowCoord.z = clamp(shadowCoord.z - bias, 0.0, 1.0);

	// Allow a tiny epsilon around the border to avoid precision flicker at tile edges.
	const float borderEps = 1e-1;
	if (shadowCoord.x < -borderEps || shadowCoord.x > 1.0 + borderEps ||
	    shadowCoord.y < -borderEps || shadowCoord.y > 1.0 + borderEps) {
		return 1.0; // Outside shadow map, fully lit
	}
	shadowCoord.xy = clamp(shadowCoord.xy, 0.0, 1.0);

	// Offset into atlas based on slice transform
	vec2 atlasOffset = tile.xy;
	vec2 atlasScale = tile.zw;
	shadowCoord.xy = shadowCoord.xy * atlasScale + atlasOffset;

	// Sample shadow map with PCF
	float exponent = u_ShadowParams.y;
	int lightFlags = int(u_ShadowLightInfo[lightIndex].w + 0.5);
	bool inverseLight = (lightFlags & 1) != 0;
	if (inverseLight) {
		exponent *= u_ShadowParams.w;
	}
	int pcfFilter = int(u_ShadowParams.z);
    // shadowCoord.xy is already transformed into atlas space, so a single texel
    // step is 1 / r_shadowAtlasSize in each dimension, not 1 / r_shadowMapSize.
    vec2 shadowMapSize = vec2(r_shadowAtlasSize);

	vec2 atlasTexel = 1.0 / shadowMapSize;
	vec2 tileMin = atlasOffset + atlasTexel;
	vec2 tileMax = atlasOffset + atlasScale - atlasTexel;
	tileMax = max(tileMax, tileMin);

	return SampleShadowPCF(u_ShadowAtlas, shadowCoord, pcfFilter, shadowMapSize, exponent, inverseLight, tileMin, tileMax);
}

#endif // USE_SHADOW_MAPPING

#endif // SHADOW_MAPPING_GLSL
