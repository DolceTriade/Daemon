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

// Shadow atlas and parameters
uniform sampler2D u_ShadowAtlas;
uniform vec4 u_ShadowParams; // x: bias, y: ESM exponent, z: PCF filter size, w: unused

// Shadow matrices (up to 4 cascades per light, up to 4 lights)
uniform mat4 u_ShadowMatrices[16]; // lightIndex * 4 + cascadeIndex

// Cascade splits for directional lights
uniform vec4 u_CascadeSplits[4]; // Per light cascade splits

// Light shadow info (using vec4 instead of ivec4 for compatibility)
uniform vec4 u_ShadowLightInfo[4]; // x: technique, y: numCascades, z: atlasOffset.x, w: atlasOffset.y

// Current technique being used
uniform int u_ShadowTechnique;

//
// Shadow sampling functions
//

// ESM16 shadow sampling
float SampleShadowESM16(sampler2D shadowMap, vec3 shadowCoord, float exponent) {
	// Atlas stores exp(k * z_occluder). Receiver term is exp(k * z_receiver).
	// Lit when z_receiver <= z_occluder ⇒ exp(k*z_rcv) <= exp(k*z_occ).
	// A soft test is stored / receiverExp, clamped to [0,1].
	float stored = texture(shadowMap, shadowCoord.xy).r; // exp(k*z_occ)
	float receiverExp = exp(exponent * shadowCoord.z);
	receiverExp = max(receiverExp, 1e-6);
	float esm = stored / receiverExp;
	return clamp(esm, 0.0, 1.0);
}

// ESM32 shadow sampling (same as ESM16 but with higher precision)
float SampleShadowESM32(sampler2D shadowMap, vec3 shadowCoord, float exponent) {
	return SampleShadowESM16(shadowMap, shadowCoord, exponent);
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
float SampleShadowEVSM32(sampler2D shadowMap, vec3 shadowCoord, float exponent) {
	vec2 texExp = texture(shadowMap, shadowCoord.xy).rg; // [exp(+k*z_occ), exp(-k*z_occ)]
	float z = shadowCoord.z;
	float posR = exp(exponent * z);
	float negR = exp(-exponent * z);
	float litPos = posR <= texExp.x ? 1.0 : 0.0;
	float litNeg = negR <= texExp.y ? 1.0 : 0.0;
	return min(litPos, litNeg);
}

// PCF filtering for shadow maps
float SampleShadowPCF(sampler2D shadowMap, vec3 shadowCoord, int filterType, vec2 shadowMapSize, float exponent) {
	vec2 texelSize = 1.0 / shadowMapSize;
	float shadow = 0.0;
	int samples = 0;

	if (filterType == 0) {
		// No PCF - single sample
		switch (u_ShadowTechnique) {
			case 2: // SHADOWING_ESM16
				return SampleShadowESM16(shadowMap, shadowCoord, exponent);
			case 3: // SHADOWING_ESM32
				return SampleShadowESM32(shadowMap, shadowCoord, exponent);
			case 4: // SHADOWING_VSM16
			case 5: // SHADOWING_VSM32
				return SampleShadowVSM(shadowMap, shadowCoord);
			case 6: // SHADOWING_EVSM32
				return SampleShadowEVSM32(shadowMap, shadowCoord, exponent);
			default:
				return 1.0;
		}
	}
	else if (filterType == 1) {
		// 2x2 PCF
		for (int x = -1; x <= 0; x++) {
			for (int y = -1; y <= 0; y++) {
				vec3 sampleCoord = shadowCoord + vec3(float(x) * texelSize.x, float(y) * texelSize.y, 0.0);

				switch (u_ShadowTechnique) {
					case 2: shadow += SampleShadowESM16(shadowMap, sampleCoord, exponent); break;
					case 3: shadow += SampleShadowESM32(shadowMap, sampleCoord, exponent); break;
					case 4:
					case 5: shadow += SampleShadowVSM(shadowMap, sampleCoord); break;
					case 6: shadow += SampleShadowEVSM32(shadowMap, sampleCoord, exponent); break;
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

				switch (u_ShadowTechnique) {
					case 2: shadow += SampleShadowESM16(shadowMap, sampleCoord, exponent); break;
					case 3: shadow += SampleShadowESM32(shadowMap, sampleCoord, exponent); break;
					case 4:
					case 5: shadow += SampleShadowVSM(shadowMap, sampleCoord); break;
					case 6: shadow += SampleShadowEVSM32(shadowMap, sampleCoord, exponent); break;
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

			switch (u_ShadowTechnique) {
				case 2: shadow += SampleShadowESM16(shadowMap, sampleCoord, exponent); break;
				case 3: shadow += SampleShadowESM32(shadowMap, sampleCoord, exponent); break;
				case 4:
				case 5: shadow += SampleShadowVSM(shadowMap, sampleCoord); break;
				case 6: shadow += SampleShadowEVSM32(shadowMap, sampleCoord, exponent); break;
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
float CalculateShadowFactor(vec3 worldPos, vec3 viewOrigin, vec3 normal, int lightIndex) {
	if (lightIndex < 0 || lightIndex >= 4) {
		return 1.0; // No shadow
	}

	vec4 lightInfo = u_ShadowLightInfo[lightIndex];
	int technique = u_ShadowTechnique;
	int numCascades = int(lightInfo.y);

	if (technique == 0 || technique == 1) { // NONE or BLOB
		return 1.0; // No shadow mapping
	}

	// Calculate view-space depth for cascade selection
	float viewDepth = length(worldPos - viewOrigin);
	int cascadeIndex = (numCascades > 1) ? SelectShadowCascade(viewDepth, lightIndex) : 0;

	// Get shadow matrix
	int matrixIndex = lightIndex * 4 + cascadeIndex;
	mat4 shadowMatrix = u_ShadowMatrices[matrixIndex];

	// Transform world position to light space
	vec4 lightSpacePos = shadowMatrix * vec4(worldPos, 1.0);
	vec3 shadowCoord = lightSpacePos.xyz / lightSpacePos.w;

	// Transform to [0,1] range
	shadowCoord = shadowCoord * 0.5 + 0.5;

	// Apply shadow bias based on surface normal and light direction
	float bias = u_ShadowParams.x;
	shadowCoord.z -= bias;

	// Check if we're outside the shadow map
	if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
	    shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
	    shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
		return 1.0; // Outside shadow map, fully lit
	}

	// Offset into atlas based on light's atlas region
	vec2 atlasOffset = vec2(lightInfo.z, lightInfo.w) / vec2(r_shadowAtlasSize);
	vec2 atlasScale = vec2(r_shadowMapSize) / vec2(r_shadowAtlasSize);
	shadowCoord.xy = shadowCoord.xy * atlasScale + atlasOffset;

	// Sample shadow map with PCF
	float exponent = u_ShadowParams.y;
	int pcfFilter = int(u_ShadowParams.z);
	vec2 shadowMapSize = vec2(r_shadowMapSize);

	return SampleShadowPCF(u_ShadowAtlas, shadowCoord, pcfFilter, shadowMapSize, exponent);
}

#endif // USE_SHADOW_MAPPING

#endif // SHADOW_MAPPING_GLSL
