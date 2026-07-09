#pragma once

#include "lu/renderer/render_types.h"

#include <filesystem>
#include <string>

namespace lu::renderer::lu_import {

struct NifImportOptions {
    std::filesystem::path client_root;
    std::filesystem::path nif_path;
};

struct NifImportResult {
    RenderWorld world;
    std::string error;
};

NifImportResult importNif(const NifImportOptions& options);

} // namespace lu::renderer::lu_import

