/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from Blender 5.2.2 workbench_effect_smaa_vert.glsl. */
out vec2 uvs;
out vec2 pixcoord;
out vec4 offset0;
out vec4 offset1;
out vec4 offset2;
void main()
{
  int v = gl_VertexID % 3;
  float x = -1.0f + float((v & 1) << 2);
  float y = -1.0f + float((v & 2) << 1);
  gl_Position = vec4(x, y, 1.0f, 1.0f);
  uvs = (gl_Position.xy + 1.0f) * 0.5f;

  vec4 offset[3];
#if SMAA_STAGE == 0
  SMAAEdgeDetectionVS(uvs, offset);
#elif SMAA_STAGE == 1
  SMAABlendingWeightCalculationVS(uvs, pixcoord, offset);
#elif SMAA_STAGE == 2
  SMAANeighborhoodBlendingVS(uvs, offset[0]);
#endif
  offset0 = offset[0];
  offset1 = offset[1];
  offset2 = offset[2];
}
