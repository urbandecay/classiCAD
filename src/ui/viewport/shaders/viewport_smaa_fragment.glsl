/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from Blender 5.2.2 workbench_effect_smaa_frag.glsl. */
in vec2 uvs;
in vec2 pixcoord;
in vec4 offset0;
in vec4 offset1;
in vec4 offset2;
#if SMAA_STAGE == 0
uniform sampler2D color_tx;
layout(location = 0) out vec2 out_edges;
#elif SMAA_STAGE == 1
uniform sampler2D edges_tx;
uniform sampler2D area_tx;
uniform sampler2D search_tx;
layout(location = 0) out vec4 out_weights;
#elif SMAA_STAGE == 2
uniform sampler2D color_tx;
uniform sampler2D blend_tx;
uniform float mix_factor;
uniform float taa_accumulated_weight;
layout(location = 0) out vec4 out_color;
#endif
void main()
{
  vec4 offset[3];
  offset[0] = offset0;
  offset[1] = offset1;
  offset[2] = offset2;
#if SMAA_STAGE == 0
  out_edges = SMAALumaEdgeDetectionPS(uvs, offset, color_tx);
#elif SMAA_STAGE == 1
  out_weights = SMAABlendingWeightCalculationPS(
      uvs, pixcoord, offset, edges_tx, area_tx, search_tx, vec4(0.0f));
#elif SMAA_STAGE == 2
  out_color = vec4(0.0f);
  if (mix_factor > 0.0f) {
    out_color += SMAANeighborhoodBlendingPS(uvs, offset[0], color_tx, blend_tx) * mix_factor;
  }
  if (mix_factor < 1.0f) {
    out_color += texture(color_tx, uvs) * (1.0f - mix_factor);
  }
  out_color /= max(taa_accumulated_weight, 1.0e-6f);
  out_color.rgb = exp2(out_color.rgb) - 1.0f;
  if (out_color.a > 0.999f) {
    out_color.a = 1.0f;
  }
#endif
}
