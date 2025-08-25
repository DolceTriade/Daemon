/*
===========================================================================

Daemon BSD Source Code
Copyright (c) 2024 Daemon Developers
All rights reserved.

This file is part of the Daemon BSD Source Code (Daemon Source Code).

===========================================================================
*/

/* shadowDepth_fp.glsl - Shadow map depth generation fragment shader */

#insert common

// Shadow technique uniforms
uniform int u_ShadowTechnique;
uniform vec4 u_ShadowParams;

DECLARE_OUTPUT(vec4)

void main()
{
	// Get fragment depth in light space
	float depth = gl_FragCoord.z;
	float ESMExponent = u_ShadowParams.y;

	// Output depends on shadow technique
	if (u_ShadowTechnique == 2) { // SHADOWING_ESM16
		// ESM16: Store exp(exponent * depth)
		float esmDepth = exp(ESMExponent * depth);
		outputColor = vec4(esmDepth, 0.0, 0.0, 1.0);
	}
	else if (u_ShadowTechnique == 3) { // SHADOWING_ESM32
		// ESM32: Store exp(exponent * depth)
		float esmDepth = exp(ESMExponent * depth);
		outputColor = vec4(esmDepth, 0.0, 0.0, 1.0);
	}
	else if (u_ShadowTechnique == 4 || u_ShadowTechnique == 5) { // SHADOWING_VSM16 or VSM32
		// VSM: Store first and second moments (depth, depth^2)
		float moment1 = depth;
		float moment2 = depth * depth;
		outputColor = vec4(moment1, moment2, 0.0, 1.0);
	}
	else if (u_ShadowTechnique == 6) { // SHADOWING_EVSM32
		// EVSM: Store positive and negative exponential moments
		float posExp = exp(ESMExponent * depth);
		float negExp = exp(-ESMExponent * depth);
		outputColor = vec4(posExp, negExp, 0.0, 1.0);
	}
	else {
		// Default: Just store depth (shouldn't happen in normal operation)
		outputColor = vec4(depth, 0.0, 0.0, 1.0);
	}
}
