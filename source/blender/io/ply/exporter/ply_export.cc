/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup ply
 */

#include <cstdio>

#include "BKE_layer.hh"
#include "BKE_report.hh"

#include "DNA_collection_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BLI_memory_utils.hh"
#include "IO_ply.hh"

#include "ply_data.hh"
#include "ply_export.hh"
#include "ply_export_data.hh"
#include "ply_export_header.hh"
#include "ply_export_load_plydata.hh"
#include "ply_file_buffer_ascii.hh"
#include "ply_file_buffer_binary.hh"

namespace blender::io::ply {

void exporter_main(bContext *C, const PLYExportParams &export_params)
{
  std::unique_ptr<blender::io::ply::PlyData> plyData = std::make_unique<PlyData>();
  load_plydata(*plyData, CTX_data_ensure_evaluated_depsgraph(C), export_params);

  std::unique_ptr<FileBuffer> buffer;

  try {
    if (export_params.ascii_format) {
      buffer = std::make_unique<FileBufferAscii>(export_params.filepath);
    }
    else {
      buffer = std::make_unique<FileBufferBinary>(export_params.filepath);
    }
  }
  catch (const std::system_error &ex) {
    fprintf(stderr, "%s\n", ex.what());
    BKE_reportf(export_params.reports,
                RPT_ERROR,
                "PLY Export: Cannot open file '%s'",
                export_params.filepath);
    return;
  }

  write_header(*buffer.get(), *plyData.get(), export_params);

  write_vertices(*buffer.get(), *plyData.get());

  write_faces(*buffer.get(), *plyData.get());

  write_edges(*buffer.get(), *plyData.get());

  buffer->close_file();
}
}  // namespace blender::io::ply
