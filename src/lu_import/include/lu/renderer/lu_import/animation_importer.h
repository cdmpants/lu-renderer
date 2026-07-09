#pragma once

#include "lu/renderer/render_types.h"

#include <filesystem>
#include <string>

namespace lu::renderer::lu_import {

struct AnimationImportOptions {
    std::filesystem::path path;
};

struct AnimationImportResult {
    AnimationAsset animation;
    std::string error;
};

AnimationImportResult importAnimation(const AnimationImportOptions& options);

} // namespace lu::renderer::lu_import
