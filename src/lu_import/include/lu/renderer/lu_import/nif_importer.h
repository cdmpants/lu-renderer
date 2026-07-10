#pragma once

#include "lu/renderer/render_types.h"
#include "lu/renderer/lu_import/shader_database.h"

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

// Resolves technique-owned state with the inherited authored NIF candidates
// already stored on material. Exposed so render-state regressions can be tested
// without depending on a machine-local NIF fixture.
void applyEffectiveNifRenderState(MaterialAsset& material, const LuShaderPolicy& policy);

} // namespace lu::renderer::lu_import
