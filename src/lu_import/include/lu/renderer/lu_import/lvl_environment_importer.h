#pragma once

#include "lu/renderer/render_types.h"

#include <filesystem>
#include <string>

namespace lu::renderer::lu_import {

struct LvlEnvironmentImportResult {
    EnvironmentState environment;
    bool has_environment = false;
    std::string error;
};

LvlEnvironmentImportResult importLvlEnvironment(const std::filesystem::path& lvl_path);

} // namespace lu::renderer::lu_import
