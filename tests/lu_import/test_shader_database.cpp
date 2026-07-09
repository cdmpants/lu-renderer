#include "lu/renderer/lu_import/lvl_environment_importer.h"
#include "lu/renderer/lu_import/shader_database.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <optional>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void expectNear(float actual, float expected, const char* message) {
    float delta = actual - expected;
    if (delta < 0.0f) delta = -delta;
    expect(delta < 0.0001f, message);
}

void expectPrefix(const char* value, std::optional<int32_t> expected) {
    std::optional<int32_t> actual = lu::renderer::lu_import::parseMultishaderPrefix(value);
    expect(actual == expected, value);
}

void writeU16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xffu));
    data.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void writeU32(std::vector<uint8_t>& data, uint32_t value) {
    writeU16(data, static_cast<uint16_t>(value & 0xffffu));
    writeU16(data, static_cast<uint16_t>((value >> 16u) & 0xffffu));
}

void writeF32(std::vector<uint8_t>& data, float value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(float));
}

void patchU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset + 0] = static_cast<uint8_t>(value & 0xffu);
    data[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    data[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
    data[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
}

void writeChunkHeader(std::vector<uint8_t>& data, uint32_t chunk_id,
                      size_t& chunk_start, size_t& total_size_pos, size_t& data_offset_pos) {
    chunk_start = data.size();
    writeU32(data, 0x4B4E4843u);
    writeU32(data, chunk_id);
    writeU16(data, 1);
    writeU16(data, 0);
    total_size_pos = data.size();
    writeU32(data, 0);
    data_offset_pos = data.size();
    writeU32(data, 0);
    patchU32(data, data_offset_pos, static_cast<uint32_t>(data.size()));
}

std::vector<uint8_t> buildLvlWithEnvironment() {
    std::vector<uint8_t> data;

    size_t chunk_start = 0;
    size_t total_size_pos = 0;
    size_t data_offset_pos = 0;
    writeChunkHeader(data, 1000, chunk_start, total_size_pos, data_offset_pos);
    writeU32(data, 40);
    patchU32(data, total_size_pos, static_cast<uint32_t>(data.size() - chunk_start));

    writeChunkHeader(data, 2000, chunk_start, total_size_pos, data_offset_pos);
    const size_t lighting_offset_pos = data.size();
    writeU32(data, 0);
    writeU32(data, 0);
    writeU32(data, 0);

    patchU32(data, lighting_offset_pos, static_cast<uint32_t>(data.size()));
    writeF32(data, 0.11f); writeF32(data, 0.12f); writeF32(data, 0.13f); // ambient
    writeF32(data, 0.21f); writeF32(data, 0.22f); writeF32(data, 0.23f); // specular
    writeF32(data, 0.31f); writeF32(data, 0.32f); writeF32(data, 0.33f); // upper hemi
    writeF32(data, 0.0f); writeF32(data, 10.0f); writeF32(data, 0.0f);   // light position

    writeF32(data, 12.0f); writeF32(data, 30.0f); writeF32(data, 0.0f);
    writeF32(data, 0.0f); writeF32(data, 100.0f); writeF32(data, 100.0f);
    writeF32(data, 18.0f); writeF32(data, 120.0f); writeF32(data, 0.0f);
    writeF32(data, 0.0f); writeF32(data, 200.0f); writeF32(data, 200.0f);

    writeU32(data, 0); // cull values
    writeF32(data, 0.41f); writeF32(data, 0.42f); writeF32(data, 0.43f); // fog color
    writeF32(data, 0.51f); writeF32(data, 0.52f); writeF32(data, 0.53f); // directional light color
    writeF32(data, 0.0f); writeF32(data, 0.0f); writeF32(data, 0.0f);    // start position
    writeF32(data, 1.0f); writeF32(data, 0.0f); writeF32(data, 0.0f); writeF32(data, 0.0f);

    patchU32(data, total_size_pos, static_cast<uint32_t>(data.size() - chunk_start));
    return data;
}

void expectLvlEnvironmentImport() {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "lu_renderer_env_test.lvl";
    std::vector<uint8_t> data = buildLvlWithEnvironment();
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    auto imported = lu::renderer::lu_import::importLvlEnvironment(path);
    std::filesystem::remove(path);
    expect(imported.error.empty(), "LVL environment import has no error");
    expect(imported.has_environment, "LVL environment import found environment");
    expectNear(imported.environment.ambient.x, 0.11f, "LVL ambient red");
    expectNear(imported.environment.specular.y, 0.22f, "LVL specular green");
    expectNear(imported.environment.upper_hemi.z, 0.33f, "LVL upper hemi blue");
    expectNear(imported.environment.lower_hemi.x, 0.11f, "LVL lower hemi falls back to ambient");
    expectNear(imported.environment.sun.position.y, 10.0f, "LVL light position y");
    expectNear(imported.environment.sun.direction.y, 1.0f, "LVL light direction derived from position");
    expectNear(imported.environment.sun.color.z, 0.53f, "LVL directional light blue");
    expect(imported.environment.fog_enabled, "LVL fog enabled");
    expectNear(imported.environment.fog_near, 12.0f, "LVL fog near from min draw");
    expectNear(imported.environment.fog_far, 120.0f, "LVL fog far from max draw");
    expectNear(imported.environment.fog_color.y, 0.42f, "LVL fog color green");
}

} // namespace

int main() {
    using namespace lu::renderer;
    using namespace lu::renderer::lu_import;

    expectPrefix("S39_name", 39);
    expectPrefix("s39_name", 39);
    expectPrefix("mesh_without_prefix", std::nullopt);
    expectPrefix("S_name", std::nullopt);
    expectPrefix("S39name", std::nullopt);
    expectLvlEnvironmentImport();

    LuShaderInfo lego{1, 5, "LEGO"};
    LuShaderPolicy lego_policy = shaderPolicyFromInfo(lego);
    expect(lego_policy.shader_family == LegacyShaderFamily::LegoppLighting, "LEGO uses LEGOPP lighting program");
    expect(lego_policy.alpha_mode == RenderAlphaMode::Opaque, "LEGO defaults opaque");
    expect(lego_policy.source_file == "LEGOPPLighting.fx", "LEGO source file");
    expect(lego_policy.uses_material_diffuse, "LEGO uses material diffuse as brick color");
    expect(lego_policy.uses_reflection, "LEGO uses env reflection");
    expect(lego_policy.reflection_semantic == "glow", "LEGO reflection semantic");
    expect(lego_policy.reflection_map == "default_reflection.dds", "LEGO reflection map");
    expect(lego_policy.port_status == ShaderPortStatus::Verified, "LEGO verified against LEGOPP source");

    LuShaderInfo basic_vc{30, 38, "Basic VC"};
    LuShaderPolicy basic_vc_policy = shaderPolicyFromInfo(basic_vc);
    expect(basic_vc_policy.shader_family == LegacyShaderFamily::Basic, "Basic VC uses Basic program");
    expect(basic_vc_policy.source_technique == "Technique_Basic_NoLighting_VertColor", "Basic VC source technique");
    expect(basic_vc_policy.uses_vertex_color, "Basic VC uses vertex color");
    expect(basic_vc_policy.port_status == ShaderPortStatus::Verified, "Basic VC verified");

    LuShaderInfo basic_nl_vc_nt{25, 33, "Basic NL VC NT"};
    LuShaderPolicy basic_nl_vc_nt_policy = shaderPolicyFromInfo(basic_nl_vc_nt);
    expect(basic_nl_vc_nt_policy.shader_family == LegacyShaderFamily::Basic, "Basic NL VC NT uses Basic program");
    expect(basic_nl_vc_nt_policy.source_file == "BasicShaders.fx", "Basic NL VC NT source file");
    expect(basic_nl_vc_nt_policy.source_technique == "Technique_Basic_NoLighting_VertColor_NoTexture", "Basic NL VC NT source technique");
    expect(basic_nl_vc_nt_policy.uses_vertex_color, "Basic NL VC NT uses vertex color");
    expect(!basic_nl_vc_nt_policy.uses_texture, "Basic NL VC NT ignores texture");
    expect(!basic_nl_vc_nt_policy.uses_lighting, "Basic NL VC NT is no-light");
    expect(basic_nl_vc_nt_policy.port_status == ShaderPortStatus::Verified, "Basic NL VC NT verified");

    LuShaderInfo basic_nl_vc{27, 35, "Basic NL VC"};
    LuShaderPolicy basic_nl_vc_policy = shaderPolicyFromInfo(basic_nl_vc);
    expect(basic_nl_vc_policy.shader_family == LegacyShaderFamily::Basic, "Basic NL VC uses Basic program");
    expect(basic_nl_vc_policy.source_technique == "Technique_Basic_NoLighting_VertColor", "Basic NL VC source technique");
    expect(basic_nl_vc_policy.uses_vertex_color, "Basic NL VC uses vertex color");
    expect(basic_nl_vc_policy.uses_texture, "Basic NL VC uses texture");
    expect(!basic_nl_vc_policy.uses_lighting, "Basic NL VC is no-light");
    expect(basic_nl_vc_policy.port_status == ShaderPortStatus::Verified, "Basic NL VC verified");

    LuShaderInfo basic_vc_no_texture{29, 37, "Basic VC NT"};
    LuShaderPolicy basic_vc_no_texture_policy = shaderPolicyFromInfo(basic_vc_no_texture);
    expect(basic_vc_no_texture_policy.shader_family == LegacyShaderFamily::Basic, "Basic VC NT uses Basic program");
    expect(basic_vc_no_texture_policy.source_technique == "Technique_Basic_NoLighting_VertColor_NoTexture", "Basic VC NT source technique");
    expect(basic_vc_no_texture_policy.uses_vertex_color, "Basic VC NT uses vertex color");
    expect(!basic_vc_no_texture_policy.uses_texture, "Basic VC NT ignores texture");
    expect(basic_vc_no_texture_policy.port_status == ShaderPortStatus::Verified, "Basic VC NT verified");

    LuShaderInfo opaque_no_fog{72, 82, "Opaque NL VC NT NoFog"};
    LuShaderPolicy opaque_no_fog_policy = shaderPolicyFromInfo(opaque_no_fog);
    expect(opaque_no_fog_policy.shader_family == LegacyShaderFamily::Basic, "NoFog VC NT uses Basic program");
    expect(opaque_no_fog_policy.source_file == "inferred", "NoFog VC NT inferred source marker");
    expect(opaque_no_fog_policy.source_technique == "Opaque NL VC NT NoFog", "NoFog VC NT inferred source technique");
    expect(opaque_no_fog_policy.uses_vertex_color, "NoFog VC NT uses vertex color");
    expect(!opaque_no_fog_policy.uses_texture, "NoFog VC NT ignores texture");
    expect(!opaque_no_fog_policy.uses_lighting, "NoFog VC NT is no-light");
    expect(!opaque_no_fog_policy.uses_fog, "NoFog VC NT ignores fog");
    expect(!opaque_no_fog_policy.uses_specular, "NoFog VC NT ignores specular");
    expect(!opaque_no_fog_policy.uses_reflection, "NoFog VC NT ignores reflection");
    expect(opaque_no_fog_policy.reflection_map.empty(), "NoFog VC NT has no reflection map");
    expect(opaque_no_fog_policy.port_status == ShaderPortStatus::Inferred, "NoFog VC NT is inferred from label tokens");

    LuShaderInfo opaque_vc_no_fog{74, 84, "Opaque NL VC NoFog"};
    LuShaderPolicy opaque_vc_no_fog_policy = shaderPolicyFromInfo(opaque_vc_no_fog);
    expect(opaque_vc_no_fog_policy.shader_family == LegacyShaderFamily::Basic, "NoFog VC uses Basic program");
    expect(opaque_vc_no_fog_policy.source_file == "inferred", "NoFog VC inferred source marker");
    expect(opaque_vc_no_fog_policy.source_technique == "Opaque NL VC NoFog", "NoFog VC inferred source technique");
    expect(opaque_vc_no_fog_policy.uses_vertex_color, "NoFog VC uses vertex color");
    expect(opaque_vc_no_fog_policy.uses_texture, "NoFog VC uses texture");
    expect(!opaque_vc_no_fog_policy.uses_lighting, "NoFog VC is no-light");
    expect(!opaque_vc_no_fog_policy.uses_fog, "NoFog VC ignores fog");
    expect(!opaque_vc_no_fog_policy.uses_specular, "NoFog VC ignores specular");
    expect(!opaque_vc_no_fog_policy.uses_reflection, "NoFog VC ignores reflection");
    expect(opaque_vc_no_fog_policy.port_status == ShaderPortStatus::Inferred, "NoFog VC is inferred from label tokens");

    LuShaderInfo basic{84, 94, "Basic"};
    LuShaderPolicy basic_policy = shaderPolicyFromInfo(basic);
    expect(basic_policy.shader_family == LegacyShaderFamily::Basic, "Basic uses Basic program");
    expect(basic_policy.source_technique == "Technique_Basic_NoLighting", "Basic source technique");
    expect(!basic_policy.uses_vertex_color, "Basic does not use vertex color");
    expect(basic_policy.port_status == ShaderPortStatus::Verified, "Basic verified");

    LuShaderInfo clear_plastic{3, 6, "Clear Plastic"};
    LuShaderPolicy clear_policy = shaderPolicyFromInfo(clear_plastic);
    expect(clear_policy.shader_family == LegacyShaderFamily::ClearPlastic, "Clear Plastic family");
    expect(clear_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Clear Plastic blends");
    expect(clear_policy.cull_mode == RenderCullMode::Clockwise, "Clear Plastic matches source CW cull");
    expect(clear_policy.source_technique == "Technique_ClearPlastic", "Clear Plastic source technique");
    expect(clear_policy.reflection_semantic == "glow", "Clear Plastic reflection semantic");
    expect(clear_policy.reflection_map == "default_reflection.dds", "Clear Plastic reflection map");
    expect(clear_policy.port_status == ShaderPortStatus::Verified, "Clear Plastic verified");

    LuShaderInfo alpha_test{47, 54, "VertColorTex_NoLight_AlphaTest"};
    LuShaderPolicy alpha_test_policy = shaderPolicyFromInfo(alpha_test);
    expect(alpha_test_policy.shader_family == LegacyShaderFamily::AlphaAsAlpha, "AlphaTest uses AlphaAsAlpha program");
    expect(alpha_test_policy.alpha_mode == RenderAlphaMode::AlphaTest, "AlphaTest shader policy");
    expect(alpha_test_policy.source_technique == "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaTest", "AlphaTest source technique");
    expect(alpha_test_policy.port_status == ShaderPortStatus::Verified, "AlphaTest verified");

    LuShaderInfo terrain_rim{17, 3, "Terrain Mesh Rim Light"};
    LuShaderPolicy terrain_rim_policy = shaderPolicyFromInfo(terrain_rim);
    expect(terrain_rim_policy.shader_family == LegacyShaderFamily::TerrainRim, "Terrain rim program");
    expect(terrain_rim_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Terrain rim blends fade alpha");
    expect(terrain_rim_policy.source_file == "TerrainDiffuse.fx", "Terrain rim source file");
    expect(terrain_rim_policy.source_technique == "Technique_TerrainMeshLighting_Rim", "Terrain rim source technique");
    expect(terrain_rim_policy.uses_vertex_color, "Terrain rim uses vertex color");
    expect(terrain_rim_policy.port_status == ShaderPortStatus::Verified, "Terrain rim verified");

    LuShaderInfo lego_emissive{46, 53, "LEGO-Emissive"};
    LuShaderPolicy lego_emissive_policy = shaderPolicyFromInfo(lego_emissive);
    expect(lego_emissive_policy.shader_family == LegacyShaderFamily::LegoppEmissive, "LEGO emissive program");
    expect(lego_emissive_policy.source_file == "LEGOPPLighting.fx", "LEGO emissive source file");
    expect(lego_emissive_policy.source_technique == "Technique_LEGOPPLightingOK_Emissive", "LEGO emissive source technique");
    expect(!lego_emissive_policy.uses_texture, "LEGO emissive OK variant uses brick color");
    expect(lego_emissive_policy.uses_material_diffuse, "LEGO emissive uses material diffuse as brick color");
    expect(lego_emissive_policy.port_status == ShaderPortStatus::Verified, "LEGO emissive verified");

    LuShaderInfo additive{77, 87, "Additive NoLight VertColor"};
    LuShaderPolicy additive_policy = shaderPolicyFromInfo(additive);
    expect(additive_policy.alpha_mode == RenderAlphaMode::Additive, "Additive shader policy");
    expect(additive_policy.uses_vertex_color, "Additive uses vertex color");
    expect(additive_policy.port_status == ShaderPortStatus::Placeholder, "Additive source technique is provisional");

    LuShaderInfo lego_no_ambient{78, 88, "LEGO NoAmbient"};
    LuShaderPolicy lego_no_ambient_policy = shaderPolicyFromInfo(lego_no_ambient);
    expect(lego_no_ambient_policy.shader_family == LegacyShaderFamily::LegoppNoAmbient, "LEGO NoAmbient uses NoAmbient LEGOPP program");
    expect(lego_no_ambient_policy.source_file == "LEGOPPLighting.fx", "LEGO NoAmbient source file");
    expect(lego_no_ambient_policy.source_technique == "Technique_LEGOPPLightingOK_NoAmbient", "LEGO NoAmbient source technique");
    expect(lego_no_ambient_policy.uses_material_diffuse, "LEGO NoAmbient uses material diffuse as brick color");
    expect(lego_no_ambient_policy.port_status == ShaderPortStatus::Verified, "LEGO NoAmbient verified");

    LuShaderInfo ocean_distort{60, 68, "Distortion (Ocean)"};
    LuShaderPolicy ocean_distort_policy = shaderPolicyFromInfo(ocean_distort);
    expect(ocean_distort_policy.shader_family == LegacyShaderFamily::OceanDistort, "Ocean distortion program");
    expect(ocean_distort_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Ocean distortion blends");
    expect(ocean_distort_policy.source_file == "Ocean.fx", "Ocean distortion source file");
    expect(ocean_distort_policy.source_technique == "Technique_Ocean_Distort_2Layers", "Ocean distortion technique");
    expect(ocean_distort_policy.uses_vertex_color, "Ocean distortion uses vertex color");
    expect(ocean_distort_policy.uses_uv_animation, "Ocean distortion uses UV animation");
    expect(ocean_distort_policy.uses_alpha_animation, "Ocean distortion uses fade alpha");
    expect(ocean_distort_policy.port_status == ShaderPortStatus::Verified, "Ocean distortion verified");

    LuShaderInfo alpha_uv_scroll{61, 69, "ScrollingUV_NoLight_AnimAlpha"};
    LuShaderPolicy alpha_uv_scroll_policy = shaderPolicyFromInfo(alpha_uv_scroll);
    expect(alpha_uv_scroll_policy.shader_family == LegacyShaderFamily::AlphaUvScroll, "UV scroll alpha program");
    expect(alpha_uv_scroll_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "UV scroll alpha blends");
    expect(alpha_uv_scroll_policy.cull_mode == RenderCullMode::TwoSided, "UV scroll alpha two-sided");
    expect(alpha_uv_scroll_policy.source_file == "AlphaAsAlpha.fx", "UV scroll alpha source file");
    expect(alpha_uv_scroll_policy.source_technique == "Technique_AlphaAsAlpha_UVScrolling_SimpleV_NoLighting_AlphaAnim", "UV scroll alpha technique");
    expect(alpha_uv_scroll_policy.uses_vertex_color, "UV scroll alpha uses vertex color");
    expect(!alpha_uv_scroll_policy.uses_lighting, "UV scroll alpha is no-light");
    expect(alpha_uv_scroll_policy.uses_uv_animation, "UV scroll alpha uses UV animation");
    expect(alpha_uv_scroll_policy.uses_alpha_animation, "UV scroll alpha uses fade alpha");
    expect(alpha_uv_scroll_policy.port_status == ShaderPortStatus::Verified, "UV scroll alpha verified");

    LuShaderInfo alpha_uv_scroll_post{64, 73, "ScrollingUV_NoLight_AimAlpha_Post"};
    LuShaderPolicy alpha_uv_scroll_post_policy = shaderPolicyFromInfo(alpha_uv_scroll_post);
    expect(alpha_uv_scroll_post_policy.shader_family == LegacyShaderFamily::AlphaUvScroll, "UV scroll alpha post program");
    expect(alpha_uv_scroll_post_policy.source_technique == "Technique_AlphaAsAlpha_UVScrolling_SimpleV_NoLighting_AlphaAnim", "UV scroll alpha post technique");
    expect(alpha_uv_scroll_post_policy.uses_uv_animation, "UV scroll alpha post uses UV animation");
    expect(alpha_uv_scroll_post_policy.uses_alpha_animation, "UV scroll alpha post uses fade alpha");
    expect(alpha_uv_scroll_post_policy.port_status == ShaderPortStatus::Verified, "UV scroll alpha post verified");

    LuShaderInfo ocean_distort_directional{79, 89, "Distortion Directional (Ocean)"};
    LuShaderPolicy ocean_distort_directional_policy = shaderPolicyFromInfo(ocean_distort_directional);
    expect(ocean_distort_directional_policy.shader_family == LegacyShaderFamily::OceanDistortDirectional, "Ocean directional distortion program");
    expect(ocean_distort_directional_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Ocean directional distortion blends");
    expect(ocean_distort_directional_policy.source_file == "Ocean.fx", "Ocean directional distortion source file");
    expect(ocean_distort_directional_policy.source_technique == "Technique_Ocean_Distort_Directional_2Layers", "Ocean directional distortion technique");
    expect(ocean_distort_directional_policy.uses_uv_animation, "Ocean directional distortion uses UV animation");
    expect(ocean_distort_directional_policy.uses_alpha_animation, "Ocean directional distortion uses fade alpha");
    expect(ocean_distort_directional_policy.port_status == ShaderPortStatus::Verified, "Ocean directional distortion verified");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
