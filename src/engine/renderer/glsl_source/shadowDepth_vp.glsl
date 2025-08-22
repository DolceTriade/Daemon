/*
===========================================================================

Daemon BSD Source Code
Copyright (c) 2024 Daemon Developers
All rights reserved.

This file is part of the Daemon BSD Source Code (Daemon Source Code).

===========================================================================
*/

/* shadowDepth_vp.glsl - Shadow map depth generation vertex shader */

#insert common
#insert vertexSimple_vp
#insert vertexSkinning_vp
#insert vertexAnimation_vp

uniform mat4 u_ModelMatrix;
uniform mat4 u_ModelViewProjectionMatrix;

void DeformVertex( inout vec4 pos, inout vec3 normal, inout vec2 st, inout vec4 color, in float time );

void main()
{
	localBasis LB;
	vec4 position, color;
	vec2 texCoord, lmCoord;

	VertexFetch(position, LB, color, texCoord, lmCoord);

	// Apply deformations if needed
	DeformVertex( position, LB.normal, texCoord, color, 0.0 );

	// Transform vertex position into homogenous clip-space
	gl_Position = u_ModelViewProjectionMatrix * position;
}
