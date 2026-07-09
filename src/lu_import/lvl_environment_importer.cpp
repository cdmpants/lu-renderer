#include "lu/renderer/lu_import/lvl_environment_importer.h"

#include "netdevil/zone/lvl/lvl_reader.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <vector>

namespace lu::renderer::lu_import {

namespace {

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0) return {};
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

Vec3 float3(const float values[3]) {
    return {values[0], values[1], values[2]};
}

float maxComponent(Vec3 value) {
    return std::max({value.x, value.y, value.z});
}

EnvironmentState environmentFromLvlLighting(const assets::LvlLightingInfo& lighting) {
    EnvironmentState environment;
    environment.ambient = float3(lighting.ambient);
    environment.specular = float3(lighting.specular);
    environment.upper_hemi = float3(lighting.upper_hemi);

    // LVL exposes g_upperHemiLight directly. The client shader has a lower-hemi
    // global too; until its scene source is identified, use the ambient color as
    // the least surprising lower hemisphere input instead of a renderer color.
    environment.lower_hemi = environment.ambient;

    environment.sun.position = {lighting.position.x, lighting.position.y, lighting.position.z};
    environment.sun.direction = normalize(environment.sun.position);
    environment.sun.color = float3(lighting.dir_light);
    environment.sun.intensity = 1.0f;

    environment.fog_color = float3(lighting.fog_color);
    if (lighting.has_draw_distances) {
        environment.fog_near = lighting.min_draw.fog_near;
        environment.fog_far = lighting.max_draw.fog_far;
        environment.post_fog_solid = lighting.max_draw.post_fog_solid;
        environment.post_fog_fade = lighting.max_draw.post_fog_fade;
    } else {
        environment.fog_near = lighting.fog_near;
        environment.fog_far = lighting.fog_far;
    }
    environment.fog_enabled =
        environment.fog_far > environment.fog_near &&
        maxComponent(environment.fog_color) > 0.0f;

    return environment;
}

} // namespace

LvlEnvironmentImportResult importLvlEnvironment(const std::filesystem::path& lvl_path) {
    LvlEnvironmentImportResult result;

    std::vector<uint8_t> data = readFile(lvl_path);
    if (data.empty()) {
        result.error = "Could not read LVL file: " + lvl_path.string();
        return result;
    }

    try {
        assets::LvlFile lvl = assets::lvl_parse(std::span<const uint8_t>(data.data(), data.size()));
        result.has_environment = lvl.has_environment;
        if (lvl.has_environment) {
            result.environment = environmentFromLvlLighting(lvl.environment.lighting);
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    return result;
}

} // namespace lu::renderer::lu_import
