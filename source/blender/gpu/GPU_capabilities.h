/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * GPU Capabilities & workarounds
 * This module expose the reported implementation limits & enabled
 * workaround for drivers that needs specific code-paths.
 */

#pragma once

#include "BLI_sys_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int GPU_max_texture_size(void);
int GPU_max_texture_3d_size(void);
int GPU_max_texture_layers(void);
int GPU_max_textures(void);
int GPU_max_textures_vert(void);
int GPU_max_textures_geom(void);
int GPU_max_textures_frag(void);
int GPU_max_work_group_count(int index);
int GPU_max_work_group_size(int index);
int GPU_max_uniforms_vert(void);
int GPU_max_uniforms_frag(void);
int GPU_max_batch_indices(void);
int GPU_max_batch_vertices(void);
int GPU_max_vertex_attribs(void);
int GPU_max_varying_floats(void);
int GPU_max_shader_storage_buffer_bindings(void);
int GPU_max_compute_shader_storage_blocks(void);
int GPU_max_samplers(void);
size_t GPU_max_storage_buffer_size(void);

int GPU_extensions_len(void);
const char *GPU_extension_get(int i);

int GPU_texture_size_with_limit(int res);

bool GPU_mip_render_workaround(void);
bool GPU_depth_blitting_workaround(void);
bool GPU_use_main_context_workaround(void);
bool GPU_use_hq_normals_workaround(void);
bool GPU_clear_viewport_workaround(void);
bool GPU_crappy_amd_driver(void);

bool GPU_geometry_shader_support(void);
bool GPU_compute_shader_support(void);
bool GPU_shader_draw_parameters_support(void);
bool GPU_hdr_support(void);
bool GPU_texture_view_support();
bool GPU_stencil_export_support();

bool GPU_mem_stats_supported(void);
void GPU_mem_stats_get(int *totalmem, int *freemem);

/**
 * Return support for the active context + window.
 */
bool GPU_stereo_quadbuffer_support(void);

int GPU_minimum_per_vertex_stride(void);
bool GPU_transform_feedback_support(void);

#ifdef __cplusplus
}
#endif
