/*
===========================================================================
Copyright (C) 2009-2011 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of XreaL source code.

XreaL source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

XreaL source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XreaL source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// computeLight_fp.glsl - Light computing helper functions

#define COMPUTELIGHT_GLSL

#ifndef MAX_SHADOW_LIGHTS
#define MAX_SHADOW_LIGHTS 8
#endif

const int REF_INVERSE_DLIGHT = 1;
const int REF_RESTRICT_DLIGHT = 2;

#if !defined(USE_BSP_SURFACE)
	#define USE_MODEL_SURFACE
#endif

#if !defined(USE_GRID_LIGHTING)
	#define USE_LIGHT_MAPPING
#endif

#if defined(USE_REFLECTIVE_SPECULAR)
uniform samplerCube u_EnvironmentMap0;
uniform samplerCube u_EnvironmentMap1;
uniform float u_EnvironmentInterpolation;

// Only the RGB components are meaningful
// FIXME: using reflective specular will always globally decrease the scene brightness
// because we're multiplying with something that can only be less than 1.
vec4 EnvironmentalSpecularFactor( vec3 viewDir, vec3 normal )
{
	vec4 envColor0 = textureCube(u_EnvironmentMap0, reflect( -viewDir, normal ) );
	vec4 envColor1 = textureCube(u_EnvironmentMap1, reflect( -viewDir, normal ) );
	return mix( envColor0, envColor1, u_EnvironmentInterpolation );
}
#endif // USE_REFLECTIVE_SPECULAR

// lighting helper functions

#if defined(USE_GRID_LIGHTING) || defined(USE_GRID_DELUXE_MAPPING)
	void ReadLightGrid( in vec4 texel, in float lightFactor, out vec3 ambientColor, out vec3 lightColor ) {
		float ambientScale = 2.0 * texel.a;
		float directedScale = 2.0 - ambientScale;
		ambientColor = ambientScale * texel.rgb;
		lightColor = directedScale * texel.rgb;
		ambientColor *= lightFactor;
		lightColor *= lightFactor;
	}
#endif

#if defined(USE_DELUXE_MAPPING) || defined(USE_GRID_DELUXE_MAPPING) || defined(r_realtimeLighting)
	#if !defined(USE_PHYSICAL_MAPPING) && defined(r_specularMapping)
		uniform vec2 u_SpecularExponent;

		vec3 computeSpecularity( vec3 lightColor, vec4 materialColor, float NdotH ) {
			return lightColor * materialColor.rgb * pow(NdotH, u_SpecularExponent.x * materialColor.a + u_SpecularExponent.y) * r_SpecularScale;
		}
	#endif
#endif

#if defined(USE_DELUXE_MAPPING) || defined(USE_GRID_DELUXE_MAPPING) || defined(r_realtimeLighting)
void computeDeluxeLight( vec3 lightDir, vec3 normal, vec3 viewDir, vec3 lightColor,
	vec4 diffuseColor, vec4 materialColor,
	inout vec4 color )
{
	vec3 H = normalize( lightDir + viewDir );

	#if defined(USE_PHYSICAL_MAPPING) || defined(r_specularMapping)
		float NdotH = clamp( dot( normal, H ), 0.0, 1.0 );
	#endif // USE_PHYSICAL_MAPPING || r_specularMapping

	// clamp( NdotL, 0.0, 1.0 ) is done below
	float NdotL = dot( normal, lightDir );

	#if !defined(USE_BSP_SURFACE) && defined(r_halfLambertLighting)
		// http://developer.valvesoftware.com/wiki/Half_Lambert
		NdotL = NdotL * 0.5 + 0.5;
		NdotL *= NdotL;
	#endif

	NdotL = clamp( NdotL, 0.0, 1.0 );

	#if defined(USE_PHYSICAL_MAPPING)
		// Daemon PBR packing defaults to ORM like glTF 2.0 defines
		// https://www.khronos.org/blog/art-pipeline-for-gltf
		// > ORM texture for Occlusion, Roughness, and Metallic
		// https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/schema/material.pbrMetallicRoughness.schema.json
		// > The metalness values are sampled from the B channel. The roughness values are sampled from the G channel.
		// > These values are linear. If other channels are present (R or A), they are ignored for metallic-roughness calculations.
		// https://docs.blender.org/manual/en/2.80/addons/io_scene_gltf2.html
		// > glTF stores occlusion in the red (R) channel, allowing it to optionally share the same image
		// > with the roughness and metallic channels.
		float roughness = materialColor.g;
		float metalness = materialColor.b;

		float NdotV = clamp( dot( normal, viewDir ), 0.0, 1.0);
		float VdotH = clamp( dot( viewDir, H ), 0.0, 1.0);

		float alpha = roughness * roughness;
		float k = 0.125 * ( roughness + 1.0 ) * ( roughness + 1.0 );

		float D = alpha / ( ( NdotH * NdotH ) * (alpha * alpha - 1.0 ) + 1.0 );
		D *= D;

		float FexpNH = pow( 1.0 - NdotH, 5.0 );
		float FexpNV = pow( 1.0 - NdotV, 5.0 );
		vec3 F = mix( vec3( 0.04 ), diffuseColor.rgb, metalness );
		F += ( 1.0 - F ) * FexpNH;

		float G = NdotL / (NdotL * ( 1.0 - k ) + k );
		G *= NdotV / ( NdotV * ( 1.0 - k ) + k );

		vec3 diffuseBRDF = NdotL * diffuseColor.rgb * ( 1.0 - metalness );
		vec3 specularBRDF = vec3( ( D * F * G ) / max( 4.0 * NdotL * NdotV, 0.0001f ) );
		color.rgb += ( diffuseBRDF + specularBRDF ) * lightColor.rgb * NdotL;
		color.a = mix( diffuseColor.a, 1.0, FexpNV );

	#else // !USE_PHYSICAL_MAPPING
		color.rgb += lightColor.rgb * NdotL * diffuseColor.rgb;
		#if defined(r_specularMapping)
			color.rgb += computeSpecularity(lightColor.rgb, materialColor, NdotH);
		#endif // r_specularMapping
	#endif // !USE_PHYSICAL_MAPPING
}
#endif // defined(USE_DELUXE_MAPPING) || defined(USE_GRID_DELUXE_MAPPING) defined(r_realtimeLighting)

#if !defined(USE_DELUXE_MAPPING) && !defined(USE_GRID_DELUXE_MAPPING)
	void computeLight( in vec3 lightColor, vec4 diffuseColor, inout vec4 color ) {
		color.rgb += lightColor.rgb * diffuseColor.rgb;
	}
#endif // !defined(USE_DELUXE_MAPPING) && !defined(USE_GRID_DELUXE_MAPPING)

#if defined(r_realtimeLighting)

struct Light {
	vec3 center;
	float radius;
	vec3 color;
	float type;
	vec3 direction;
	float angle;
};

#if defined( HAVE_ARB_shading_language_420pack )
layout(std140, binding = BIND_LIGHTS) uniform u_Lights {
#else
layout(std140) uniform u_Lights {
#endif
	Light lights[MAX_REF_LIGHTS];
};

#define GetLight( idx ) lights[idx]

uniform int u_numLights;
uniform float u_RealtimeLightNormalScale;
uniform float u_RealtimeLightSpecularScale;
uniform float u_ShadowInverseOcclusionThreshold;

void computeDynamicLight( uint idx, int shadowSlot, vec3 P, vec3 normal, vec3 geometricNormal, vec3 viewOrigin, vec3 viewDir, vec4 diffuse,
	vec4 material, inout vec4 color )
{
	Light light = GetLight( idx );
	int typeBits = int(light.type + 0.5);
	int flagMask = typeBits >> 2;
	int lightType = typeBits & 3;
	float lightTypeFloat = float(lightType);

	vec3 L;
	float attenuation;

	if( lightType == 0 ) {
		// point light
		L = light.center.xyz - P;
		// 2.57 ~= 8.0 ^ ( 1.0 / 2.2 ), adjusted after overbright changes
		float t = 1.0 + 2.57 * length( L ) / light.radius;
		// Quadratic attenuation function instead of linear because of overbright changes
		attenuation = 1.0 / ( t * t );
		L = normalize( L );
	} else if( lightType == 1 ) {
		// spot light
		L = light.center - P;
		// 2.57 ~= 8.0 ^ ( 1.0 / 2.2 ), adjusted after overbright changes
		float t = 1.0 + 2.57 * length( L ) / light.radius;
		// Quadratic attenuation function instead of linear because of overbright changes
		attenuation = 1.0 / ( t * t );
		L = normalize( L );

		if( dot( -L, light.direction ) <= light.angle ) {
			attenuation = 0.0;
		}
	} else if( lightType == 2 ) {
        // sun (directional) light: use vector from surface towards light
        L = normalize( -light.direction );
        attenuation = 1.0;
    }

#if defined(USE_SHADOW_MAPPING)
	int shadowFlags = 0;
#endif
	float normalScale = clamp(u_RealtimeLightNormalScale, 0.0, 1.0);
	vec3 dynamicNormal = normalize(mix(geometricNormal, normal, normalScale));
	vec3 shadowNormal = dynamicNormal;
	float shadowFactor = 1.0;
#if defined(USE_SHADOW_MAPPING)
	if ((flagMask & REF_INVERSE_DLIGHT) != 0) {
		shadowNormal = normalize(geometricNormal);
	}
	if (shadowSlot >= 0) {
		shadowFactor = clamp(CalculateShadowFactor(P, viewOrigin, shadowNormal, L, shadowSlot, lightTypeFloat, light.center.xyz), 0.0, 1.0);
		shadowFlags = int(u_ShadowLightInfo[shadowSlot].w + 0.5);
	}
    int combinedFlags = shadowFlags | flagMask;
    bool inverseLight = (combinedFlags & REF_INVERSE_DLIGHT) != 0;
    if (inverseLight && shadowSlot < 0) {
        return;
    }
#else
    bool inverseLight = (flagMask & REF_INVERSE_DLIGHT) != 0;
#endif
	vec3 baseLightRGB = attenuation * attenuation * light.color;

#if defined(USE_SHADOW_MAPPING)
	if (inverseLight) {
		if (shadowSlot >= 0) {
			float occlusion = clamp(1.0 - shadowFactor, 0.0, 1.0);
			float occlusionThreshold = clamp(u_ShadowInverseOcclusionThreshold, 0.0, 0.99);
			occlusion = max(occlusion - occlusionThreshold, 0.0) / (1.0 - occlusionThreshold);
			if (occlusion > 0.0) {
				// Apply inverse shadows as direct darkening so they are less sensitive
				// to normal-map BRDF modulation on the receiver.
				vec3 darkness = baseLightRGB * occlusion * diffuse.rgb;
				color.rgb = max(color.rgb - darkness, vec3(0.0));
			}
		}
		return;
	}
#endif

	vec3 lightRGB = baseLightRGB * shadowFactor;
	vec4 dynamicMaterial = material;
#if !defined(USE_PHYSICAL_MAPPING) && defined(r_specularMapping)
	dynamicMaterial.rgb *= clamp(u_RealtimeLightSpecularScale, 0.0, 1.0);
#endif
    computeDeluxeLight(
        L, dynamicNormal, viewDir, lightRGB,
        diffuse, dynamicMaterial, color );
}

const uint lightsPerLayer = 16u;

#define idxs_t uvec4

uniform usampler3D u_LightTiles;

const vec3 tileScale = vec3( r_tileStep, 1.0 / float( NUM_LIGHT_LAYERS ) );

idxs_t fetchIdxs( in vec3 coords, in usampler3D u_LightTiles ) {
	return texture3D( u_LightTiles, coords );
}

// 8 bits per light ID
uint nextIdx( in uint count, in idxs_t idxs ) {
	return ( idxs[count / 4u] >> ( 8u * ( count % 4u ) ) ) & 0xFFu;
}

void computeDynamicLights( vec3 P, vec3 normal, vec3 geometricNormal, vec3 viewOrigin, vec3 viewDir, vec4 diffuse, vec4 material,
	inout vec4 color, in usampler3D u_LightTiles )
{
	if( u_numLights == 0 ) {
		return;
	}

	#if defined(r_showLightTiles)
		uint totalLights = 0u;
	#endif

	vec2 tile = floor( gl_FragCoord.xy * ( 1.0 / float( TILE_SIZE ) ) ) + 0.5;

	// Count active shadow slots from u_ShadowLightInfo (sorted by light index)
	int numShadowSlots = 0;
#if defined(USE_SHADOW_MAPPING)
	if (u_ShadowTechnique > 1) {
		for (int s = 0; s < MAX_SHADOW_LIGHTS; s++) {
			if (u_ShadowLightInfo[s].y > 0.5) numShadowSlots++;
		}
	}
#endif

	for( uint layer = 0u; layer < uint( NUM_LIGHT_LAYERS ); layer++ ) {
		uint lightCount = 0u;
		idxs_t idxs = fetchIdxs( tileScale * vec3( tile, float( layer ) + 0.5 ), u_LightTiles );

		// Merge this layer's ascending light indices with ascending shadow slots
		int slot = 0;

		for( uint i = 0u; i < lightsPerLayer; i++ ) {
			uint idx = nextIdx( lightCount, idxs );

			if( idx == 0u ) {
				break;
			}

			/* Light IDs are stored relative to the layer
			Subtract 1 because 0 means there's no light */
			idx = ( idx - 1u ) * uint( NUM_LIGHT_LAYERS ) + layer;

			// Advance slot cursor to match or exceed this idx
			int matchedSlot = -1;
#if defined(USE_SHADOW_MAPPING)
			while (slot < numShadowSlots && uint(u_ShadowLightInfo[slot].x) < idx) {
				slot++;
			}
			if (slot < numShadowSlots && uint(u_ShadowLightInfo[slot].x) == idx) {
				matchedSlot = slot;
			}
#endif

			computeDynamicLight( idx, matchedSlot, P, normal, geometricNormal, viewOrigin, viewDir, diffuse, material, color );
			lightCount++;
		}
		#if defined(r_showLightTiles)
			totalLights += lightCount;
		#endif
	}

	#if defined(r_showLightTiles)
		if ( totalLights > 0 ) {
			color = vec4( float( totalLights ) / u_numLights, float( totalLights ) / u_numLights,
				float( totalLights ) / u_numLights, 1.0 );
		}
	#endif
}

#endif // defined(r_realtimeLighting)
