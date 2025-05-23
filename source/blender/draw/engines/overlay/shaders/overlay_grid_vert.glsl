/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_info.hh"

VERTEX_SHADER_CREATE_INFO(overlay_grid_next)

/**
 * Infinite grid:
 * Draw anti-aliased grid and axes of different sizes with smooth blending between Level of
 * details. We draw multiple triangles to avoid float precision issues due to perspective
 * interpolation.
 */

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  vec3 vert_pos;

  /* Project camera pos to the needed plane */
  if (flag_test(grid_flag, PLANE_XY)) {
    vert_pos = vec3(pos.x, pos.y, 0.0);
  }
  else if (flag_test(grid_flag, PLANE_XZ)) {
    vert_pos = vec3(pos.x, 0.0, pos.y);
  }
  else if (flag_test(grid_flag, PLANE_YZ)) {
    vert_pos = vec3(0.0, pos.x, pos.y);
  }
  else /* PLANE_IMAGE */ {
    vert_pos = vec3(pos.xy * 0.5 + 0.5, 0.0);
  }

  local_pos = vert_pos;

  vec3 real_pos = drw_view_position() * plane_axes + vert_pos * grid_buf.size.xyz;

  /* Used for additional Z axis */
  if (flag_test(grid_flag, CLIP_ZPOS)) {
    real_pos.z = clamp(real_pos.z, 0.0, 1e30);
    local_pos.z = clamp(local_pos.z, 0.0, 1.0);
  }
  if (flag_test(grid_flag, CLIP_ZNEG)) {
    real_pos.z = clamp(real_pos.z, -1e30, 0.0);
    local_pos.z = clamp(local_pos.z, -1.0, 0.0);
  }

  gl_Position = drw_view().winmat * (drw_view().viewmat * vec4(real_pos, 1.0));
}
