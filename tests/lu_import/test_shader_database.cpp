#include "lu/renderer/lu_import/lvl_environment_importer.h"
#include "lu/renderer/lu_import/nif_importer.h"
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
    expectPrefix("mesh_S66__lod", 66);
    expectPrefix("mesh-s66_lod", 66);
    expectPrefix("mesh S66_lod", 66);
    expectPrefix("mesh_without_prefix", std::nullopt);
    expectPrefix("S_name", std::nullopt);
    expectPrefix("S39name", std::nullopt);
    expectPrefix("meshS66_lod", std::nullopt);
    expectPrefix("subscene38", std::nullopt);
    expectPrefix("G12S38_1", std::nullopt);
    expectPrefix("S78__Pillar3_G12S38_1", 78);
    expectPrefix("S78__Paradox_Teleporter_G19S39_1", 78);
    expectPrefix("mesh_S_name", std::nullopt);
    expect(parseNiMultiShaderGameValue("NiMultiShader5") == std::optional<int32_t>{5}, "NiMultiShader5 parses gameValue 5");
    expect(parseNiMultiShaderGameValue("nimultishader75") == std::optional<int32_t>{75}, "lowercase nimultishader parses");
    expect(parseNiMultiShaderGameValue("NiMultiShader") == std::nullopt, "NiMultiShader without value does not parse");
    expect(inferShaderIdFromFxShaderMetadata("FXshader_LEGOPPLightingOK_Glow_IgnoreVertAlpha") == std::optional<int32_t>{40}, "FXshader Glow IgnoreVertAlpha resolves to shader 40");
    expect(inferShaderIdFromFxShaderMetadata("FXshader_LEGOPPLightingVertColor_Darkling_Specular") == std::optional<int32_t>{66}, "FXshader Darkling Specular resolves to shader 66");
    expect(inferShaderIdFromFxShaderMetadata("FXshader_LEGOPPLightingVertColorTextured_Emissive") == std::optional<int32_t>{58}, "FXshader VC textured emissive resolves to shader 58");
    expect(inferShaderIdFromFxShaderMetadata("FXshader_LEGOPPLightingOK_ShinyGlint") == std::optional<int32_t>{63}, "FXshader plain ShinyGlint resolves to shader 63");
    expect(inferShaderIdFromFxShaderMetadata("FXshader_LEGOPPLightingVertColor_3Lights") == std::optional<int32_t>{1}, "FXshader BBB 3Lights resolves to LEGO identity");
    expect(inferShaderIdFromFxShaderMetadata("FXshader_LEGOPPLightingOK") == std::optional<int32_t>{1}, "FXshader base LEGOPP resolves to LEGO");
    expect(inferShaderIdFromFxShaderMetadata("LEGOPPLightingOK_Glow") == std::nullopt, "FXshader inference requires FXshader marker");
    LuShaderPolicy fx_noenv = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({1, 5, "LEGO"}),
        "FXshader_LEGOPPLightingTextured_noenv");
    expect(fx_noenv.source_file == "LEGOPPLighting_noenv.fx", "FXshader noenv selects noenv source file");
    expect(fx_noenv.source_technique == "Technique_LEGOPPLightingTextured_noenv", "FXshader noenv keeps exact source technique");
    expect(!fx_noenv.uses_vertex_color, "FXshader Textured noenv does not use vertex color");
    expect(fx_noenv.uses_texture, "FXshader Textured noenv samples the base texture");
    expect(!fx_noenv.uses_material_diffuse, "FXshader Textured noenv does not use material diffuse as color");
    expect(!fx_noenv.uses_reflection, "FXshader noenv disables env reflection");
    expect(fx_noenv.uses_specular, "FXshader noenv keeps specular");
    expect(!fx_noenv.uses_shadow_terrain, "FXshader noenv disables high-end terrain shadow multiplier");
    expect(fx_noenv.reflection_map.empty(), "FXshader noenv clears reflection map");

    LuShaderPolicy fx_noenv_nospec = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({1, 5, "LEGO"}),
        "FXshader_LEGOPPLightingVertColor_noenv_nospec");
    expect(fx_noenv_nospec.source_file == "LEGOPPLighting_noenv_nospec.fx", "FXshader noenv_nospec selects noenv_nospec source file");
    expect(fx_noenv_nospec.source_technique == "Technique_LEGOPPLightingVertColor_noenv_nospec", "FXshader noenv_nospec keeps exact source technique");
    expect(fx_noenv_nospec.uses_vertex_color, "FXshader VertColor noenv_nospec uses vertex color");
    expect(!fx_noenv_nospec.uses_texture, "FXshader VertColor noenv_nospec does not sample base texture");
    expect(!fx_noenv_nospec.uses_material_diffuse, "FXshader VertColor noenv_nospec does not use material diffuse as color");
    expect(!fx_noenv_nospec.uses_reflection, "FXshader noenv_nospec disables reflection");
    expect(!fx_noenv_nospec.uses_specular, "FXshader noenv_nospec disables specular");
    expect(!fx_noenv_nospec.uses_shadow_terrain, "FXshader noenv_nospec disables high-end terrain shadow multiplier");

    LuShaderPolicy fx_nondecal_noenv = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({14, 14, "LEGO Masked NonDecal"}),
        "FXshader_LEGOPPLightingVertColorTextured_NonDecal_noenv");
    expect(fx_nondecal_noenv.legopp_variant == LegoppShaderVariant::MaskedNonDecal, "FXshader NonDecal noenv keeps MaskedNonDecal variant");
    expect(fx_nondecal_noenv.source_file == "LEGOPPLighting_noenv.fx", "FXshader NonDecal noenv selects noenv source file");
    expect(fx_nondecal_noenv.source_technique == "Technique_LEGOPPLightingVertColorTextured_NonDecal_noenv", "FXshader NonDecal noenv keeps exact source technique");
    expect(fx_nondecal_noenv.uses_vertex_color, "FXshader NonDecal noenv uses vertex color");
    expect(fx_nondecal_noenv.uses_texture, "FXshader NonDecal noenv samples base texture");
    expect(!fx_nondecal_noenv.uses_material_diffuse, "FXshader NonDecal noenv multiplies texture by vertex color");
    expect(!fx_nondecal_noenv.uses_reflection, "FXshader NonDecal noenv disables reflection");
    expect(fx_nondecal_noenv.uses_specular, "FXshader NonDecal noenv keeps specular");
    expect(!fx_nondecal_noenv.uses_shadow_terrain, "FXshader NonDecal noenv disables high-end terrain shadow multiplier");

    LuShaderPolicy fx_nondecal_noenv_nospec = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({14, 14, "LEGO Masked NonDecal"}),
        "FXshader_LEGOPPLightingVertColorTextured_NonDecal_noenv_nospec");
    expect(fx_nondecal_noenv_nospec.legopp_variant == LegoppShaderVariant::MaskedNonDecal, "FXshader NonDecal noenv_nospec keeps MaskedNonDecal variant");
    expect(fx_nondecal_noenv_nospec.source_file == "LEGOPPLighting_noenv_nospec.fx", "FXshader NonDecal noenv_nospec selects noenv_nospec source file");
    expect(fx_nondecal_noenv_nospec.source_technique == "Technique_LEGOPPLightingVertColorTextured_NonDecal_noenv_nospec", "FXshader NonDecal noenv_nospec keeps exact source technique");
    expect(fx_nondecal_noenv_nospec.uses_vertex_color, "FXshader NonDecal noenv_nospec uses vertex color");
    expect(fx_nondecal_noenv_nospec.uses_texture, "FXshader NonDecal noenv_nospec samples base texture");
    expect(!fx_nondecal_noenv_nospec.uses_material_diffuse, "FXshader NonDecal noenv_nospec multiplies texture by vertex color");
    expect(!fx_nondecal_noenv_nospec.uses_reflection, "FXshader NonDecal noenv_nospec disables reflection");
    expect(!fx_nondecal_noenv_nospec.uses_specular, "FXshader NonDecal noenv_nospec disables specular");
    expect(!fx_nondecal_noenv_nospec.uses_shadow_terrain, "FXshader NonDecal noenv_nospec disables high-end terrain shadow multiplier");

    LuShaderPolicy fx_low = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({38, 27, "LEGO-Glow"}),
        "FXshader_LEGOPPLightingOK_low_Glow");
    expect(fx_low.source_file == "LEGOPPLighting_low.fx", "FXshader low selects low source file");
    expect(fx_low.source_technique == "Technique_LEGOPPLightingOK_low_Glow", "FXshader low keeps exact source technique");
    expect(!fx_low.uses_texture, "FXshader OK low glow does not sample base texture");
    expect(fx_low.uses_material_diffuse, "FXshader OK low glow uses material diffuse");
    expect(!fx_low.uses_reflection, "FXshader low disables reflection");
    expect(!fx_low.uses_specular, "FXshader low disables specular");
    expect(!fx_low.uses_shadow_terrain, "FXshader low disables high-end terrain shadow multiplier");
    expect(fx_low.alpha_semantic == ShaderAlphaSemantic::ControlGlow, "FXshader low glow keeps glow-control alpha");

    LuShaderPolicy fx_item_noenv = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({41, 48, "LEGO-ItemGlow"}),
        "FXshader_LEGOPPLighting_Item_Glow_noenv");
    expect(fx_item_noenv.source_file == "LEGOPPLighting_Item_noenv.fx", "FXshader item noenv selects item noenv source file");
    expect(fx_item_noenv.source_technique == "Technique_LEGOPPLighting_Item_Glow_noenv", "FXshader item noenv keeps exact source technique");
    expect(fx_item_noenv.uses_material_diffuse, "FXshader item noenv uses item/material color path");
    expect(!fx_item_noenv.uses_shadow_terrain, "FXshader item noenv keeps item terrain-shadow policy disabled");
    expect(!fx_item_noenv.uses_reflection, "FXshader item noenv disables reflection");

    LuShaderPolicy fx_darkling_noenv = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({66, 76, "Darkling /w Specular"}),
        "FXshader_LEGOPPLightingVertColor_noenv_Darkling_Specular");
    expect(fx_darkling_noenv.source_file == "LEGOPPLighting_noenv.fx", "FXshader darkling specular noenv selects noenv source file");
    expect(fx_darkling_noenv.source_technique == "Technique_LEGOPPLightingVertColor_noenv_Darkling_Specular", "FXshader darkling specular noenv keeps exact source technique");
    expect(fx_darkling_noenv.uses_vertex_color, "FXshader darkling specular noenv uses vertex color");
    expect(fx_darkling_noenv.uses_texture, "FXshader darkling specular noenv still samples darkling texture control data");
    expect(!fx_darkling_noenv.uses_material_diffuse, "FXshader darkling specular noenv does not use material diffuse as color");
    expect(!fx_darkling_noenv.uses_reflection, "FXshader darkling specular noenv disables reflection");
    expect(fx_darkling_noenv.uses_specular, "FXshader darkling specular noenv keeps specular");

    LuShaderPolicy fx_darkling_low = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({65, 75, "Darkling"}),
        "FXshader_LEGOPPLightingVertColor_low_Darkling");
    expect(fx_darkling_low.source_file == "LEGOPPLighting.fx", "FXshader darkling low is not invented without a source technique");

    LuShaderPolicy fx_bbb = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({1, 5, "LEGO"}),
        "FXshader_LEGOPPLightingVertColor_3Lights");
    expect(fx_bbb.shader_family == LegacyShaderFamily::LegoppEffect, "FXshader BBB uses LEGOPP effect dispatch");
    expect(fx_bbb.legopp_variant == LegoppShaderVariant::ThreeLight, "FXshader BBB variant");
    expect(static_cast<int>(LegoppShaderVariant::ThreeLight) == 24, "FXshader BBB variant id matches shader constant");
    expect(fx_bbb.source_file == "LEGOPPLighting_BBB.fx", "FXshader BBB source file");
    expect(fx_bbb.source_technique == "Technique_LEGOPPLightingVertColor_3Lights", "FXshader BBB source technique");
    expect(fx_bbb.uses_vertex_color, "FXshader BBB uses vertex color");
    expect(!fx_bbb.uses_texture, "FXshader BBB does not sample base texture in source pixel shader");
    expect(!fx_bbb.uses_material_diffuse, "FXshader BBB uses vertex color, not material diffuse");
    expect(fx_bbb.reflection_semantic == "dark", "FXshader BBB records source dark env sampler semantic");
    expect(fx_bbb.reflection_map == "default_reflection.dds", "FXshader BBB keeps current cubemap until a dedicated dark map exists");
    expect(fx_bbb.source_status_note.find("NTM=dark") != std::string::npos, "FXshader BBB records missing dedicated dark cubemap");
    expect(fx_bbb.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "FXshader BBB alpha is output alpha");
    expect(fx_bbb.port_status == ShaderPortStatus::Verified, "FXshader BBB source math verified");

    LuShaderPolicy fx_noenv_fade_up = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({43, 32, "LEGO-FadeUp"}),
        "FXshader_LEGOPPLightingOK_noenv_FadeUp");
    expect(fx_noenv_fade_up.source_file == "LEGOPPLighting_noenv.fx", "FXshader noenv FadeUp selects noenv source file");
    expect(fx_noenv_fade_up.source_technique == "Technique_LEGOPPLightingOK_noenv_FadeUp", "FXshader noenv FadeUp keeps exact source technique");
    expect(!fx_noenv_fade_up.uses_reflection, "FXshader noenv FadeUp disables reflection and uses vertex-alpha fade source");
    expect(fx_noenv_fade_up.uses_specular, "FXshader noenv FadeUp keeps specular source path");

    LuShaderPolicy fx_vct_emissive = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({58, 65, "VC_Texture_Emissive"}),
        "FXshader_LEGOPPLightingVertColorTextured_Emissive");
    expect(fx_vct_emissive.source_technique == "Technique_LEGOPPLightingVertColorTextured_Emissive", "FXshader VCT emissive keeps exact source technique");
    expect(fx_vct_emissive.uses_vertex_color, "FXshader VCT emissive uses vertex color");
    expect(fx_vct_emissive.uses_texture, "FXshader VCT emissive samples texture");
    expect(!fx_vct_emissive.uses_material_diffuse, "FXshader VCT emissive multiplies texture by vertex color, not material diffuse");
    expect(fx_vct_emissive.uses_uv_animation, "FXshader VCT emissive uses AnimUV source vertex path");
    expect(fx_vct_emissive.alpha_semantic == ShaderAlphaSemantic::ControlEmissive, "FXshader VCT emissive alpha remains control data");

    LuShaderPolicy fx_vct_noenv_nospec_glow = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({38, 27, "LEGO-Glow"}),
        "FXshader_LEGOPPLightingVertColorTextured_noenv_nospec_Glow");
    expect(fx_vct_noenv_nospec_glow.source_file == "LEGOPPLighting_noenv_nospec.fx", "FXshader VCT noenv_nospec glow source file");
    expect(fx_vct_noenv_nospec_glow.source_technique == "Technique_LEGOPPLightingVertColorTextured_noenv_nospec_Glow", "FXshader VCT noenv_nospec glow exact source technique");
    expect(fx_vct_noenv_nospec_glow.uses_vertex_color, "FXshader VCT noenv_nospec glow uses vertex color");
    expect(fx_vct_noenv_nospec_glow.uses_texture, "FXshader VCT noenv_nospec glow samples texture");
    expect(!fx_vct_noenv_nospec_glow.uses_material_diffuse, "FXshader VCT noenv_nospec glow does not use material diffuse");
    expect(!fx_vct_noenv_nospec_glow.uses_reflection, "FXshader VCT noenv_nospec glow disables reflection");
    expect(!fx_vct_noenv_nospec_glow.uses_specular, "FXshader VCT noenv_nospec glow disables specular");
    expect(fx_vct_noenv_nospec_glow.alpha_semantic == ShaderAlphaSemantic::ControlGlow, "FXshader VCT noenv_nospec glow alpha remains glow control");

    LuShaderPolicy fx_low_super = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({19, 22, "LEGO-SuperEmissive"}),
        "FXshader_LEGOPPLightingOK_low_SuperEmissive");
    expect(fx_low_super.source_file == "LEGOPPLighting_low.fx", "FXshader low superemissive source file");
    expect(fx_low_super.source_technique == "Technique_LEGOPPLightingOK_low_SuperEmissive", "FXshader low superemissive exact source technique");
    expect(!fx_low_super.uses_texture, "FXshader low superemissive OK path does not sample texture");
    expect(fx_low_super.uses_material_diffuse, "FXshader low superemissive OK path uses material diffuse");
    expect(!fx_low_super.uses_reflection, "FXshader low superemissive disables reflection");
    expect(!fx_low_super.uses_specular, "FXshader low superemissive disables specular");
    expect(fx_low_super.uses_uv_animation, "FXshader low superemissive uses AnimUV source vertex path");
    expect(fx_low_super.alpha_semantic == ShaderAlphaSemantic::ControlEmissive, "FXshader low superemissive alpha remains emissive control");

    LuShaderPolicy fx_textured_nl = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({45, 52, "LEGO-No Light"}),
        "FXshader_LEGOPPLightingTextured_NL");
    expect(fx_textured_nl.source_technique == "Technique_LEGOPPLightingTextured_NL", "FXshader Textured_NL exact source technique");
    expect(fx_textured_nl.alpha_mode == RenderAlphaMode::AlphaBlend, "FXshader Textured_NL source explicitly enables alpha blend");
    expect(fx_textured_nl.uses_texture, "FXshader Textured_NL samples texture");
    expect(fx_textured_nl.uses_uv_animation, "FXshader Textured_NL uses no-light texture motion path");
    expect(!fx_textured_nl.uses_lighting, "FXshader Textured_NL disables lighting");

    LuShaderPolicy fx_vc_skinned_nl = applyFxShaderMetadataPolicyOverrides(
        shaderPolicyFromInfo({45, 52, "LEGO-No Light"}),
        "FXshader_LEGOPPLightingVertColorSkinned_NL");
    expect(fx_vc_skinned_nl.source_technique == "Technique_LEGOPPLightingVertColorSkinned_NL", "FXshader VertColorSkinned_NL exact source technique");
    expect(fx_vc_skinned_nl.alpha_mode == RenderAlphaMode::Opaque, "FXshader VertColorSkinned_NL source does not force alpha blend");
    expect(fx_vc_skinned_nl.uses_vertex_color, "FXshader VertColorSkinned_NL uses vertex color");
    expect(!fx_vc_skinned_nl.uses_texture, "FXshader VertColorSkinned_NL does not sample texture");
    expect(!fx_vc_skinned_nl.uses_uv_animation, "FXshader VertColorSkinned_NL does not use texture motion");
    expect(!fx_vc_skinned_nl.uses_lighting, "FXshader VertColorSkinned_NL disables lighting");

    ShaderDatabase fixture_database = ShaderDatabase::fromRecords({
        {1, 5, "LEGO"},
        {19, 22, "LEGO-SuperEmissive"},
        {38, 27, "LEGO-Glow"},
        {65, 75, "Darkling"},
        {66, 76, "Darkling /w Specular"},
        {100, 100, "MultiShader"},
    }, {
        {"mesh\\minifig\\accessories\\minifig_accessory_knight_valiant.nif", 19},
        {"mesh\\factionkit2\\minifig_accessory_spacerangerkit3_armor.nif", 19},
        {"mesh\\environment\\env_test_multishader.nif", 100},
    });

    ResolvedLuShader direct_asset = fixture_database.resolveAssetMeshShader(
        "mesh/minifig/accessories/minifig_accessory_knight_valiant.nif",
        "valiant_sword");
    expect(direct_asset.resolved, "CDClient direct asset shader resolves");
    expect(direct_asset.resolution_source == ShaderResolutionSource::CdClientAsset, "CDClient direct asset resolution source");
    expect(direct_asset.shader.id == 19, "CDClient direct asset resolves shader id 19");
    expect(direct_asset.policy.legopp_variant == LegoppShaderVariant::SuperEmissive, "CDClient direct asset uses SuperEmissive policy");
    expect(direct_asset.policy.alpha_semantic == ShaderAlphaSemantic::ControlEmissive, "CDClient SuperEmissive alpha is control data");

    ResolvedLuShader armor_asset = fixture_database.resolveAssetMeshShader(
        "mesh/factionkit2/minifig_accessory_spacerangerkit3_armor.nif",
        "LOD_Shape0");
    expect(armor_asset.resolved, "spaceranger armor resolves from CDClient");
    expect(armor_asset.resolution_source == ShaderResolutionSource::CdClientAsset,
        "spaceranger armor records authoritative CDClient source");
    expect(armor_asset.shader.id == 19, "spaceranger armor uses CDClient SuperEmissive shader");
    expect(armor_asset.policy.alpha_semantic == ShaderAlphaSemantic::ControlEmissive,
        "spaceranger armor preserves control-alpha semantics");

    ResolvedLuShader multishader_asset = fixture_database.resolveAssetMeshShader(
        "mesh/environment/env_test_multishader.nif",
        "S38_lamp_glow");
    expect(multishader_asset.resolved, "CDClient multishader prefix resolves");
    expect(multishader_asset.asset_is_multishader, "CDClient multishader marks asset as multishader");
    expect(multishader_asset.multishader_prefix_id == std::optional<int32_t>{38}, "CDClient multishader captures S prefix");
    expect(multishader_asset.resolution_source == ShaderResolutionSource::CdClientMultishaderPrefix, "CDClient multishader source");
    expect(multishader_asset.shader.id == 38, "CDClient multishader resolves prefix shader id");
    expect(multishader_asset.policy.alpha_semantic == ShaderAlphaSemantic::ControlGlow, "CDClient glow prefix alpha is control data");

    ResolvedLuShader parent_prefixed_multishader = fixture_database.resolveAssetMeshShader(
        "mesh/environment/env_test_multishader.nif",
        "unprefixed_child",
        "S19_armor_parent");
    expect(parent_prefixed_multishader.resolved, "CDClient multishader resolves parent prefix");
    expect(parent_prefixed_multishader.multishader_prefix_id == std::optional<int32_t>{19},
        "CDClient multishader captures parent S prefix");
    expect(parent_prefixed_multishader.shader.id == 19,
        "CDClient multishader parent prefix selects submesh shader");

    ResolvedLuShader child_prefixed_multishader = fixture_database.resolveAssetMeshShader(
        "mesh/environment/env_test_multishader.nif",
        "S38_child",
        "S19_parent");
    expect(child_prefixed_multishader.shader.id == 38,
        "CDClient multishader child prefix is authoritative before parent prefix");

    ResolvedLuShader missing_prefix = fixture_database.resolveAssetMeshShader(
        "mesh/environment/env_test_multishader.nif",
        "lamp_without_prefix");
    expect(!missing_prefix.resolved, "CDClient multishader without prefix remains unresolved");
    expect(missing_prefix.asset_is_multishader, "CDClient multishader without prefix still marks multishader asset");
    expect(missing_prefix.shader.id == 1, "CDClient multishader without prefix keeps visible LEGO diagnostic fallback");

    ResolvedLuShader nimultishader = fixture_database.resolveNifMaterialShader("NiMultiShader5");
    expect(nimultishader.resolved, "NiMultiShader material resolves through gameValue");
    expect(nimultishader.resolution_source == ShaderResolutionSource::NifMultiShaderGameValue, "NiMultiShader resolution source");
    expect(nimultishader.metadata == "NiMultiShader5", "NiMultiShader preserves metadata");
    expect(nimultishader.shader.id == 1, "NiMultiShader5 resolves gameValue 5 to LEGO shader id 1");
    expect(nimultishader.policy.legopp_variant == LegoppShaderVariant::Base, "NiMultiShader5 uses LEGO base policy");

    ResolvedLuShader nims_darkling = fixture_database.resolveNifMaterialShader("DARKLING_PIRATE_GRUNT_NIMS");
    expect(nims_darkling.resolved, "Darkling NIMS material resolves");
    expect(nims_darkling.resolution_source == ShaderResolutionSource::NifMaterialName, "Darkling NIMS resolution source");
    expect(nims_darkling.shader.id == 65, "Darkling NIMS resolves shader id 65");
    expect(nims_darkling.policy.alpha_mode == RenderAlphaMode::Opaque, "Darkling NIMS stays opaque");
    expect(nims_darkling.policy.alpha_semantic == ShaderAlphaSemantic::ControlDarkling, "Darkling NIMS alpha is control data");

    ResolvedLuShader fx_darkling_spec = fixture_database.resolveNifMaterialShader(
        "FXshader_LEGOPPLightingVertColor_noenv_Darkling_Specular");
    expect(fx_darkling_spec.resolved, "FXshader darkling specular material resolves");
    expect(fx_darkling_spec.resolution_source == ShaderResolutionSource::NifFxShaderName, "FXshader darkling specular resolution source");
    expect(fx_darkling_spec.shader.id == 66, "FXshader darkling specular resolves shader id 66");
    expect(fx_darkling_spec.policy.source_file == "LEGOPPLighting_noenv.fx", "FXshader darkling specular noenv source file");
    expect(!fx_darkling_spec.policy.uses_reflection, "FXshader darkling specular noenv disables reflection");
    expect(fx_darkling_spec.policy.alpha_semantic == ShaderAlphaSemantic::ControlDarkling, "FXshader darkling specular alpha is control data");
    expectLvlEnvironmentImport();

    LuShaderInfo lego{1, 5, "LEGO"};
    LuShaderPolicy lego_policy = shaderPolicyFromInfo(lego);
    expect(lego_policy.shader_family == LegacyShaderFamily::LegoppLighting, "LEGO uses LEGOPP lighting program");
    expect(lego_policy.alpha_mode == RenderAlphaMode::Opaque, "LEGO defaults opaque");
    expect(lego_policy.depth_write, "LEGO keeps depth writes enabled for NiAlphaProperty render state");
    expect(lego_policy.uses_ni_render_state, "LEGO technique consumes Ni render state");
    expect(lego_policy.source_file == "LEGOPPLighting.fx", "LEGO source file");
    expect(lego_policy.uses_material_diffuse, "LEGO uses material diffuse as brick color");
    expect(lego_policy.uses_reflection, "LEGO uses env reflection");
    expect(lego_policy.reflection_semantic == "glow", "LEGO reflection semantic");
    expect(lego_policy.reflection_map == "default_reflection.dds", "LEGO reflection map");
    expect(lego_policy.port_status == ShaderPortStatus::Verified, "LEGO verified against LEGOPP source");

    LuShaderInfo lego_super{19, 22, "LEGO-SuperEmissive"};
    LuShaderPolicy lego_super_policy = shaderPolicyFromInfo(lego_super);
    expect(lego_super_policy.shader_family == LegacyShaderFamily::LegoppEmissive, "LEGO SuperEmissive dispatches through emissive program family for now");
    expect(lego_super_policy.legopp_variant == LegoppShaderVariant::SuperEmissive, "LEGO SuperEmissive variant");
    expect(lego_super_policy.source_technique == "Technique_LEGOPPLightingOK_SuperEmissive", "LEGO SuperEmissive source technique");
    expect(lego_super_policy.alpha_semantic == ShaderAlphaSemantic::ControlEmissive, "LEGO SuperEmissive alpha is emissive control data");
    expect(lego_super_policy.uses_ni_render_state, "LEGO SuperEmissive technique consumes Ni render state");
    expect(lego_super_policy.uses_uv_animation, "LEGO SuperEmissive uses AnimUV vertex shader");
    expect(lego_super_policy.port_status == ShaderPortStatus::Verified, "LEGO SuperEmissive verified");

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

    LuShaderInfo basic_nl{26, 34, "Basic NL"};
    LuShaderPolicy basic_nl_policy = shaderPolicyFromInfo(basic_nl);
    expect(basic_nl_policy.shader_family == LegacyShaderFamily::Basic, "Basic NL uses Basic program");
    expect(basic_nl_policy.source_file == "BasicShaders.fx", "Basic NL source file");
    expect(basic_nl_policy.source_technique == "Technique_Basic_NoLighting", "Basic NL source technique");
    expect(!basic_nl_policy.uses_vertex_color, "Basic NL does not use vertex color");
    expect(basic_nl_policy.uses_texture, "Basic NL samples texture");
    expect(!basic_nl_policy.uses_lighting, "Basic NL is no-light");
    expect(!basic_nl_policy.uses_specular, "Basic NL ignores specular");
    expect(!basic_nl_policy.uses_reflection, "Basic NL ignores reflection");
    expect(basic_nl_policy.port_status == ShaderPortStatus::Verified, "Basic NL verified");

    LuShaderInfo basic_nl_uvanim{28, 36, "Basic NL UVAnim"};
    LuShaderPolicy basic_nl_uvanim_policy = shaderPolicyFromInfo(basic_nl_uvanim);
    expect(basic_nl_uvanim_policy.shader_family == LegacyShaderFamily::Basic, "Basic NL UVAnim uses Basic program");
    expect(basic_nl_uvanim_policy.source_file == "BasicShaders.fx", "Basic NL UVAnim source file");
    expect(basic_nl_uvanim_policy.source_technique == "Technique_Basic_NoLighting_VertColor_UVScrolling_SimpleV", "Basic NL UVAnim closest source technique");
    expect(basic_nl_uvanim_policy.source_status_note.find("No non-vertex-color") != std::string::npos,
        "Basic NL UVAnim records source mismatch");
    expect(basic_nl_uvanim_policy.uses_vertex_color, "Basic NL UVAnim uses available source vertex-color path");
    expect(basic_nl_uvanim_policy.uses_uv_animation, "Basic NL UVAnim scrolls UVs");
    expect(!basic_nl_uvanim_policy.uses_lighting, "Basic NL UVAnim is no-light");
    expect(basic_nl_uvanim_policy.port_status == ShaderPortStatus::Inferred, "Basic NL UVAnim is explicitly inferred");

    LuShaderInfo basic_vc_uvanim{31, 39, "Basic VC UVAnim"};
    LuShaderPolicy basic_vc_uvanim_policy = shaderPolicyFromInfo(basic_vc_uvanim);
    expect(basic_vc_uvanim_policy.shader_family == LegacyShaderFamily::Basic, "Basic VC UVAnim uses Basic program");
    expect(basic_vc_uvanim_policy.source_file == "BasicShaders.fx", "Basic VC UVAnim source file");
    expect(basic_vc_uvanim_policy.source_technique == "Technique_Basic_NoLighting_VertColor_UVScrolling_SimpleV", "Basic VC UVAnim source technique");
    expect(basic_vc_uvanim_policy.uses_vertex_color, "Basic VC UVAnim uses vertex color");
    expect(basic_vc_uvanim_policy.uses_uv_animation, "Basic VC UVAnim scrolls UVs");
    expect(!basic_vc_uvanim_policy.uses_lighting, "Basic VC UVAnim is no-light");
    expect(basic_vc_uvanim_policy.port_status == ShaderPortStatus::Verified, "Basic VC UVAnim verified");

    LuShaderInfo basic_vc_no_texture{29, 37, "Basic VC NT"};
    LuShaderPolicy basic_vc_no_texture_policy = shaderPolicyFromInfo(basic_vc_no_texture);
    expect(basic_vc_no_texture_policy.shader_family == LegacyShaderFamily::Basic, "Basic VC NT uses Basic program");
    expect(basic_vc_no_texture_policy.source_technique == "Technique_Basic_NoLighting_VertColor_NoTexture", "Basic VC NT source technique");
    expect(basic_vc_no_texture_policy.uses_vertex_color, "Basic VC NT uses vertex color");
    expect(!basic_vc_no_texture_policy.uses_texture, "Basic VC NT ignores texture");
    expect(basic_vc_no_texture_policy.port_status == ShaderPortStatus::Verified, "Basic VC NT verified");

    LuShaderInfo basic_nl_nt{70, 80, "Basic NL NT"};
    LuShaderPolicy basic_nl_nt_policy = shaderPolicyFromInfo(basic_nl_nt);
    expect(basic_nl_nt_policy.shader_family == LegacyShaderFamily::Basic, "Basic NL NT uses Basic program");
    expect(basic_nl_nt_policy.source_file == "BasicShaders.fx", "Basic NL NT source file");
    expect(basic_nl_nt_policy.source_technique == "Technique_Basic_Material_NoLighting_NoTexture", "Basic NL NT source technique");
    expect(!basic_nl_nt_policy.uses_texture, "Basic NL NT ignores texture");
    expect(basic_nl_nt_policy.uses_material_diffuse, "Basic NL NT uses material diffuse");
    expect(!basic_nl_nt_policy.uses_lighting, "Basic NL NT is no-light");
    expect(!basic_nl_nt_policy.uses_specular, "Basic NL NT ignores specular");
    expect(!basic_nl_nt_policy.uses_reflection, "Basic NL NT ignores reflection");
    expect(basic_nl_nt_policy.port_status == ShaderPortStatus::Verified, "Basic NL NT verified");

    LuShaderInfo scrolling_no_fog{71, 81, "ScrollingUV NL AnimAlpha NoFog"};
    LuShaderPolicy scrolling_no_fog_policy = shaderPolicyFromInfo(scrolling_no_fog);
    expect(scrolling_no_fog_policy.shader_family == LegacyShaderFamily::AlphaUvScroll, "Scrolling NoFog uses alpha uv scroll program");
    expect(scrolling_no_fog_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Scrolling NoFog anim alpha blends");
    expect(scrolling_no_fog_policy.source_file == "inferred", "Scrolling NoFog inferred source marker");
    expect(scrolling_no_fog_policy.source_technique == "ScrollingUV NL AnimAlpha NoFog", "Scrolling NoFog inferred source technique");
    expect(!scrolling_no_fog_policy.uses_lighting, "Scrolling NoFog is no-light");
    expect(!scrolling_no_fog_policy.uses_fog, "Scrolling NoFog ignores fog");
    expect(!scrolling_no_fog_policy.uses_specular, "Scrolling NoFog ignores specular");
    expect(!scrolling_no_fog_policy.uses_reflection, "Scrolling NoFog ignores reflection");
    expect(scrolling_no_fog_policy.uses_uv_animation, "Scrolling NoFog uses UV animation");
    expect(scrolling_no_fog_policy.uses_alpha_animation, "Scrolling NoFog uses alpha animation");
    expect(scrolling_no_fog_policy.port_status == ShaderPortStatus::Inferred, "Scrolling NoFog is inferred from label tokens");

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

    LuShaderInfo opaque_nl_no_fog{73, 83, "Opaque NL NoFog"};
    LuShaderPolicy opaque_nl_no_fog_policy = shaderPolicyFromInfo(opaque_nl_no_fog);
    expect(opaque_nl_no_fog_policy.shader_family == LegacyShaderFamily::Basic, "NoFog NL uses Basic program");
    expect(opaque_nl_no_fog_policy.source_file == "inferred", "NoFog NL inferred source marker");
    expect(opaque_nl_no_fog_policy.source_technique == "Opaque NL NoFog", "NoFog NL inferred source technique");
    expect(!opaque_nl_no_fog_policy.uses_lighting, "NoFog NL is no-light");
    expect(!opaque_nl_no_fog_policy.uses_fog, "NoFog NL ignores fog");
    expect(!opaque_nl_no_fog_policy.uses_specular, "NoFog NL ignores specular");
    expect(!opaque_nl_no_fog_policy.uses_reflection, "NoFog NL ignores reflection");
    expect(opaque_nl_no_fog_policy.port_status == ShaderPortStatus::Inferred, "NoFog NL is inferred from label tokens");

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

    LuShaderInfo opaque_vc_nt_lit_no_fog{75, 85, "Opaque VC NT NoFog"};
    LuShaderPolicy opaque_vc_nt_lit_no_fog_policy = shaderPolicyFromInfo(opaque_vc_nt_lit_no_fog);
    expect(opaque_vc_nt_lit_no_fog_policy.shader_family == LegacyShaderFamily::BasicLit, "NoFog VC NT lit uses BasicLit program");
    expect(opaque_vc_nt_lit_no_fog_policy.source_file == "inferred", "NoFog VC NT lit inferred source marker");
    expect(opaque_vc_nt_lit_no_fog_policy.source_technique == "Opaque VC NT NoFog", "NoFog VC NT lit inferred source technique");
    expect(opaque_vc_nt_lit_no_fog_policy.uses_vertex_color, "NoFog VC NT lit uses vertex color");
    expect(!opaque_vc_nt_lit_no_fog_policy.uses_texture, "NoFog VC NT lit ignores texture");
    expect(opaque_vc_nt_lit_no_fog_policy.uses_lighting, "NoFog VC NT lit keeps lighting");
    expect(!opaque_vc_nt_lit_no_fog_policy.uses_fog, "NoFog VC NT lit ignores fog");
    expect(!opaque_vc_nt_lit_no_fog_policy.uses_specular, "NoFog VC NT lit ignores specular");
    expect(!opaque_vc_nt_lit_no_fog_policy.uses_reflection, "NoFog VC NT lit ignores reflection");
    expect(opaque_vc_nt_lit_no_fog_policy.port_status == ShaderPortStatus::Inferred, "NoFog VC NT lit is inferred from label tokens");

    LuShaderInfo opaque_vc_lit_no_fog{76, 86, "Opaque VC NoFog"};
    LuShaderPolicy opaque_vc_lit_no_fog_policy = shaderPolicyFromInfo(opaque_vc_lit_no_fog);
    expect(opaque_vc_lit_no_fog_policy.shader_family == LegacyShaderFamily::BasicLit, "NoFog VC lit uses BasicLit program");
    expect(opaque_vc_lit_no_fog_policy.source_file == "inferred", "NoFog VC lit inferred source marker");
    expect(opaque_vc_lit_no_fog_policy.source_technique == "Opaque VC NoFog", "NoFog VC lit inferred source technique");
    expect(opaque_vc_lit_no_fog_policy.uses_vertex_color, "NoFog VC lit uses vertex color");
    expect(opaque_vc_lit_no_fog_policy.uses_texture, "NoFog VC lit uses texture");
    expect(opaque_vc_lit_no_fog_policy.uses_lighting, "NoFog VC lit keeps lighting");
    expect(!opaque_vc_lit_no_fog_policy.uses_fog, "NoFog VC lit ignores fog");
    expect(!opaque_vc_lit_no_fog_policy.uses_specular, "NoFog VC lit ignores specular");
    expect(!opaque_vc_lit_no_fog_policy.uses_reflection, "NoFog VC lit ignores reflection");
    expect(opaque_vc_lit_no_fog_policy.port_status == ShaderPortStatus::Inferred, "NoFog VC lit is inferred from label tokens");

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
    expect(!clear_policy.depth_write, "Clear Plastic disables depth writes per source FX");
    expect(clear_policy.cull_mode == RenderCullMode::Clockwise, "Clear Plastic matches source CW cull");
    expect(clear_policy.source_technique == "Technique_ClearPlastic", "Clear Plastic source technique");
    expect(clear_policy.reflection_semantic == "glow", "Clear Plastic reflection semantic");
    expect(clear_policy.reflection_map == "default_reflection.dds", "Clear Plastic reflection map");
    expect(clear_policy.port_status == ShaderPortStatus::Verified, "Clear Plastic verified");

    LuShaderInfo alpha_blend{22, 8, "VertColor_NoLighting_Alpha"};
    LuShaderPolicy alpha_blend_policy = shaderPolicyFromInfo(alpha_blend);
    expect(alpha_blend_policy.shader_family == LegacyShaderFamily::AlphaAsAlpha, "VertColor NoLighting Alpha uses AlphaAsAlpha program");
    expect(alpha_blend_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "VertColor NoLighting Alpha blends");
    expect(!alpha_blend_policy.depth_write, "VertColor NoLighting Alpha disables depth writes per source FX");
    expect(alpha_blend_policy.cull_mode == RenderCullMode::TwoSided, "VertColor NoLighting Alpha source is two-sided");
    expect(alpha_blend_policy.source_file == "AlphaAsAlpha.fx", "VertColor NoLighting Alpha source file");
    expect(alpha_blend_policy.source_technique == "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaBlend", "VertColor NoLighting Alpha source technique");
    expect(alpha_blend_policy.uses_vertex_color, "VertColor NoLighting Alpha uses vertex color");
    expect(alpha_blend_policy.uses_texture, "VertColor NoLighting Alpha samples base texture");
    expect(!alpha_blend_policy.uses_lighting, "VertColor NoLighting Alpha is unlit");
    expect(!alpha_blend_policy.uses_fog, "VertColor NoLighting Alpha has no source fog path");
    expect(!alpha_blend_policy.uses_specular, "VertColor NoLighting Alpha ignores specular");
    expect(!alpha_blend_policy.uses_reflection, "VertColor NoLighting Alpha ignores reflection");
    expect(alpha_blend_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "VertColor NoLighting Alpha outputs alpha");
    expect(alpha_blend_policy.validation_status_note.find("S22__") != std::string::npos,
        "VertColor NoLighting Alpha records real S22 validation asset");
    expect(alpha_blend_policy.port_status == ShaderPortStatus::Verified, "VertColor NoLighting Alpha verified");

    LuShaderInfo one_sided_alpha_nl_vc{50, 57, "OneSidedAlpha NL VC"};
    LuShaderPolicy one_sided_alpha_nl_vc_policy = shaderPolicyFromInfo(one_sided_alpha_nl_vc);
    expect(one_sided_alpha_nl_vc_policy.shader_family == LegacyShaderFamily::AlphaAsAlpha, "OneSidedAlpha NL VC uses AlphaAsAlpha program");
    expect(one_sided_alpha_nl_vc_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "OneSidedAlpha NL VC blends");
    expect(!one_sided_alpha_nl_vc_policy.depth_write, "OneSidedAlpha NL VC disables depth writes per source FX");
    expect(one_sided_alpha_nl_vc_policy.cull_mode == RenderCullMode::Backface, "OneSidedAlpha NL VC is one-sided");
    expect(one_sided_alpha_nl_vc_policy.source_file == "AlphaAsAlpha.fx", "OneSidedAlpha NL VC source file");
    expect(one_sided_alpha_nl_vc_policy.source_technique == "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaBlend", "OneSidedAlpha NL VC source technique");
    expect(one_sided_alpha_nl_vc_policy.uses_vertex_color, "OneSidedAlpha NL VC uses vertex color");
    expect(one_sided_alpha_nl_vc_policy.uses_texture, "OneSidedAlpha NL VC samples texture when present");
    expect(!one_sided_alpha_nl_vc_policy.uses_lighting, "OneSidedAlpha NL VC is no-light");
    expect(!one_sided_alpha_nl_vc_policy.uses_fog, "OneSidedAlpha NL VC has no source fog path");
    expect(one_sided_alpha_nl_vc_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "OneSidedAlpha NL VC outputs alpha");
    expect(one_sided_alpha_nl_vc_policy.port_status == ShaderPortStatus::Verified, "OneSidedAlpha NL VC verified");

    LuShaderInfo alpha_test{47, 54, "VertColorTex_NoLight_AlphaTest"};
    LuShaderPolicy alpha_test_policy = shaderPolicyFromInfo(alpha_test);
    expect(alpha_test_policy.shader_family == LegacyShaderFamily::AlphaAsAlpha, "AlphaTest uses AlphaAsAlpha program");
    expect(alpha_test_policy.alpha_mode == RenderAlphaMode::Opaque,
        "AlphaTest-labelled technique leaves test enable to NiAlphaProperty");
    expect(alpha_test_policy.depth_write, "AlphaTest keeps depth writes enabled");
    expect(alpha_test_policy.source_technique == "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaTest", "AlphaTest source technique");
    expect(!alpha_test_policy.uses_fog, "AlphaTest has no source fog path");
    expect(alpha_test_policy.alpha_semantic == ShaderAlphaSemantic::AlphaTest, "AlphaTest alpha semantic");
    expect(alpha_test_policy.port_status == ShaderPortStatus::Verified, "AlphaTest verified");

    LuShaderInfo lego_masked{14, 14, "LEGO Masked NonDecal"};
    LuShaderPolicy lego_masked_policy = shaderPolicyFromInfo(lego_masked);
    expect(lego_masked_policy.legopp_variant == LegoppShaderVariant::MaskedNonDecal, "LEGO Masked NonDecal variant");
    expect(lego_masked_policy.source_file == "LEGOPPLighting.fx", "LEGO Masked NonDecal source file");
    expect(lego_masked_policy.source_technique == "Technique_LEGOPPLightingOK_Masked_NonDecal", "LEGO Masked NonDecal source technique");
    expect(lego_masked_policy.port_status == ShaderPortStatus::Verified, "LEGO Masked NonDecal verified");

    LuShaderInfo lego_reveal{6, 12, "LEGO-Reveal"};
    LuShaderPolicy lego_reveal_policy = shaderPolicyFromInfo(lego_reveal);
    expect(lego_reveal_policy.legopp_variant == LegoppShaderVariant::Reveal, "LEGO Reveal variant");
    expect(lego_reveal_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "LEGO Reveal blends output alpha");
    expect(lego_reveal_policy.source_file == "LEGOPPLighting.fx", "LEGO Reveal source file");
    expect(lego_reveal_policy.source_technique == "Technique_LEGOPPLightingOK_Reveal", "LEGO Reveal source technique");
    expect(lego_reveal_policy.uses_uv_animation, "LEGO Reveal uses AnimUV vertex shader");
    expect(lego_reveal_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "LEGO Reveal alpha is output alpha");
    expect(!lego_reveal_policy.depth_write, "LEGO Reveal disables depth writes while blending reveal alpha");
    expect(lego_reveal_policy.port_status == ShaderPortStatus::Verified, "LEGO Reveal source math verified");

    LuShaderInfo lego_frontend{11, 25, "LEGO_FrontEnd"};
    LuShaderPolicy lego_frontend_policy = shaderPolicyFromInfo(lego_frontend);
    expect(lego_frontend_policy.legopp_variant == LegoppShaderVariant::FrontEnd, "LEGO FrontEnd variant");
    expect(lego_frontend_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "LEGO FrontEnd source blends");
    expect(lego_frontend_policy.depth_write, "LEGO FrontEnd source keeps depth writes");
    expect(lego_frontend_policy.cull_mode == RenderCullMode::Clockwise, "LEGO FrontEnd source culls clockwise");
    expect(lego_frontend_policy.source_file == "LEGOPPLighting_FrontEnd.fx", "LEGO FrontEnd source file");
    expect(lego_frontend_policy.source_technique == "Technique_LEGOPPLightingOK_FrontEnd", "LEGO FrontEnd source technique");
    expect(!isLegoppFrontendAlphaTestTechnique(lego_frontend_policy.source_technique), "LEGO FrontEnd default technique blends");
    expect(isLegoppFrontendAlphaTestTechnique("Technique_LEGOPPLightingVertColorTexturedSkinned_FrontEnd"), "LEGO FrontEnd VCT skinned source technique alpha-tests");
    expect(lego_frontend_policy.port_status == ShaderPortStatus::Verified, "LEGO FrontEnd source washout math verified");

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
    expect(lego_emissive_policy.legopp_variant == LegoppShaderVariant::Emissive, "LEGO emissive variant");
    expect(lego_emissive_policy.source_file == "LEGOPPLighting.fx", "LEGO emissive source file");
    expect(lego_emissive_policy.source_technique == "Technique_LEGOPPLightingOK_Emissive", "LEGO emissive source technique");
    expect(!lego_emissive_policy.uses_texture, "LEGO emissive OK variant uses brick color");
    expect(lego_emissive_policy.uses_material_diffuse, "LEGO emissive uses material diffuse as brick color");
    expect(lego_emissive_policy.uses_uv_animation, "LEGO emissive uses AnimUV vertex shader");
    expect(lego_emissive_policy.port_status == ShaderPortStatus::Verified, "LEGO emissive verified");

    LuShaderInfo vc_texture_emissive{58, 65, "VC_Texture_Emissive"};
    LuShaderPolicy vc_texture_emissive_policy = shaderPolicyFromInfo(vc_texture_emissive);
    expect(vc_texture_emissive_policy.shader_family == LegacyShaderFamily::LegoppEmissive, "VC Texture Emissive program");
    expect(vc_texture_emissive_policy.legopp_variant == LegoppShaderVariant::Emissive, "VC Texture Emissive variant");
    expect(vc_texture_emissive_policy.source_technique == "Technique_LEGOPPLightingVertColorTextured_Emissive", "VC Texture Emissive source technique");
    expect(vc_texture_emissive_policy.uses_vertex_color, "VC Texture Emissive uses vertex color");
    expect(vc_texture_emissive_policy.uses_texture, "VC Texture Emissive uses texture");
    expect(!vc_texture_emissive_policy.uses_material_diffuse, "VC Texture Emissive multiplies texture by vertex color, not material diffuse");
    expect(vc_texture_emissive_policy.uses_uv_animation, "VC Texture Emissive uses AnimUV vertex shader");
    expect(vc_texture_emissive_policy.alpha_semantic == ShaderAlphaSemantic::ControlEmissive, "VC Texture Emissive alpha is emissive control data");
    expect(vc_texture_emissive_policy.port_status == ShaderPortStatus::Verified, "VC Texture Emissive verified");

    LuShaderInfo lego_glow{38, 27, "LEGO-Glow"};
    LuShaderPolicy lego_glow_policy = shaderPolicyFromInfo(lego_glow);
    expect(lego_glow_policy.legopp_variant == LegoppShaderVariant::Glow, "LEGO Glow variant");
    expect(lego_glow_policy.source_file == "LEGOPPLighting.fx", "LEGO Glow source file");
    expect(lego_glow_policy.source_technique == "Technique_LEGOPPLightingOK_Glow", "LEGO Glow source technique");
    expect(lego_glow_policy.alpha_semantic == ShaderAlphaSemantic::ControlGlow, "LEGO Glow alpha is glow control data");
    expect(lego_glow_policy.glow_color.x == 0.0f &&
           lego_glow_policy.glow_color.y == 1.0f &&
           lego_glow_policy.glow_color.z == 1.0f &&
           lego_glow_policy.glow_color.w == 1.0f, "LEGO Glow uses LEGOPP default cyan glow color");
    expect(lego_glow_policy.glow_lightness == 1.0f, "LEGO Glow uses LEGOPP default glow lightness");
    expect(lego_glow_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "LEGO Glow records missing validation assets");
    expect(lego_glow_policy.validation_status_note.find("metadata candidates import with no shader-38 mesh") != std::string::npos,
        "LEGO Glow records metadata-only candidates");
    expect(lego_glow_policy.port_status == ShaderPortStatus::Verified, "LEGO Glow source math verified");

    LuShaderInfo lego_grayscale{39, 28, "LEGO-Grayscale"};
    LuShaderPolicy lego_grayscale_policy = shaderPolicyFromInfo(lego_grayscale);
    expect(lego_grayscale_policy.legopp_variant == LegoppShaderVariant::Grayscale, "LEGO Grayscale variant");
    expect(lego_grayscale_policy.source_file == "LEGOPPLighting.fx", "LEGO Grayscale source file");
    expect(lego_grayscale_policy.source_technique == "Technique_LEGOPPLightingOK_Grayscale", "LEGO Grayscale source technique");
    expect(lego_grayscale_policy.grayscale_lerp == 1.0f, "LEGO Grayscale uses LEGOPP default lerp");
    expect(lego_grayscale_policy.grayscale_lightness == 0.2f, "LEGO Grayscale uses LEGOPP default lightness");
    expect(lego_grayscale_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "LEGO Grayscale keeps output alpha");
    expect(lego_grayscale_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "LEGO Grayscale records missing validation assets");
    expect(lego_grayscale_policy.validation_status_note.find("no valid S39_ prefix") != std::string::npos,
        "LEGO Grayscale rejects scene-label S39 strings as shader prefixes");
    expect(lego_grayscale_policy.port_status == ShaderPortStatus::Verified, "LEGO Grayscale source math verified");

    LuShaderInfo lego_glow_ignore{40, 29, "LEGO-Glow-IgnoreVertAlpha"};
    LuShaderPolicy lego_glow_ignore_policy = shaderPolicyFromInfo(lego_glow_ignore);
    expect(lego_glow_ignore_policy.legopp_variant == LegoppShaderVariant::GlowIgnoreVertAlpha, "LEGO Glow IgnoreVertAlpha variant");
    expect(lego_glow_ignore_policy.source_technique == "Technique_LEGOPPLightingOK_Glow_IgnoreVertAlpha", "LEGO Glow IgnoreVertAlpha source technique");
    expect(lego_glow_ignore_policy.alpha_semantic == ShaderAlphaSemantic::ControlGlow, "LEGO Glow IgnoreVertAlpha alpha is glow control data");
    expect(lego_glow_ignore_policy.glow_lightness == 1.0f, "LEGO Glow IgnoreVertAlpha uses LEGOPP default glow lightness");
    expect(lego_glow_ignore_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "LEGO Glow IgnoreVertAlpha records missing validation assets");
    expect(lego_glow_ignore_policy.validation_status_note.find("metadata candidates import with no shader-40 mesh") != std::string::npos,
        "LEGO Glow IgnoreVertAlpha records metadata-only candidates");
    expect(lego_glow_ignore_policy.port_status == ShaderPortStatus::Verified, "LEGO Glow IgnoreVertAlpha source math verified");

    LuShaderInfo lego_item_glow{41, 48, "LEGO-ItemGlow"};
    LuShaderPolicy lego_item_glow_policy = shaderPolicyFromInfo(lego_item_glow);
    expect(lego_item_glow_policy.legopp_variant == LegoppShaderVariant::ItemGlow, "LEGO Item Glow variant");
    expect(lego_item_glow_policy.source_file == "LEGOPPLighting_Item.fx", "LEGO Item Glow source file");
    expect(lego_item_glow_policy.source_technique == "Technique_LEGOPPLighting_Item_Glow", "LEGO Item Glow source technique");
    expect(!lego_item_glow_policy.uses_shadow_terrain, "LEGO Item Glow uses NoTerrainShadow item path");
    expect(lego_item_glow_policy.alpha_semantic == ShaderAlphaSemantic::ControlGlow, "LEGO Item Glow alpha is glow control data");
    expect(lego_item_glow_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "LEGO Item Glow records missing validation assets");
    expect(lego_item_glow_policy.validation_status_note.find("no valid S41_ prefix") != std::string::npos,
        "LEGO Item Glow records no valid prefix candidates");
    expect(lego_item_glow_policy.port_status == ShaderPortStatus::Verified, "LEGO Item Glow source math verified");

    LuShaderInfo lego_fade_up{43, 32, "LEGO-FadeUp"};
    LuShaderPolicy lego_fade_up_policy = shaderPolicyFromInfo(lego_fade_up);
    expect(lego_fade_up_policy.legopp_variant == LegoppShaderVariant::FadeUp, "LEGO FadeUp variant");
    expect(lego_fade_up_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "LEGO FadeUp blends source-computed alpha");
    expect(!lego_fade_up_policy.depth_write, "LEGO FadeUp disables depth writes while fading");
    expect(lego_fade_up_policy.source_file == "LEGOPPLighting.fx", "LEGO FadeUp source file");
    expect(lego_fade_up_policy.source_technique == "Technique_LEGOPPLightingOK_FadeUp", "LEGO FadeUp source technique");
    expect(lego_fade_up_policy.fade_up_height == 0.0f, "LEGO FadeUp uses LEGOPP default fade height");
    expect(lego_fade_up_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "LEGO FadeUp records no local resolved users");
    expect(lego_fade_up_policy.port_status == ShaderPortStatus::Verified, "LEGO FadeUp source math verified");

    LuShaderInfo lego_face_create{37, 26, "LEGO_FaceCreate"};
    LuShaderPolicy lego_face_create_policy = shaderPolicyFromInfo(lego_face_create);
    expect(lego_face_create_policy.legopp_variant == LegoppShaderVariant::FaceCreate, "LEGO FaceCreate variant");
    expect(lego_face_create_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "LEGO FaceCreate source enables alpha blend");
    expect(lego_face_create_policy.force_alpha_blend, "LEGO FaceCreate forces alpha blend");
    expect(lego_face_create_policy.force_alpha_test, "LEGO FaceCreate source also enables alpha test");
    expect(lego_face_create_policy.alpha_threshold == 127, "LEGO FaceCreate source alpha ref");
    expect(lego_face_create_policy.cull_mode == RenderCullMode::Clockwise, "LEGO FaceCreate source culls clockwise");
    expect(lego_face_create_policy.alpha_semantic == ShaderAlphaSemantic::AlphaTest, "LEGO FaceCreate alpha semantic");
    expect(lego_face_create_policy.port_status == ShaderPortStatus::Verified, "LEGO FaceCreate source pixel path verified");

    LuShaderInfo lego_item{23, 31, "LEGO-Item"};
    LuShaderPolicy lego_item_policy = shaderPolicyFromInfo(lego_item);
    expect(lego_item_policy.legopp_variant == LegoppShaderVariant::Item, "LEGO Item variant");
    expect(lego_item_policy.source_file == "LEGOPPLighting_Item.fx", "LEGO Item source file");
    expect(lego_item_policy.source_technique == "Technique_LEGOPPLighting_Item", "LEGO Item source technique");
    expect(lego_item_policy.uses_material_diffuse, "LEGO Item uses attr_itemColor/material diffuse");
    expect(!lego_item_policy.uses_shadow_terrain, "LEGO Item uses NoTerrainShadow pixel path");
    expect(lego_item_policy.port_status == ShaderPortStatus::Verified, "LEGO Item verified");

    LuShaderInfo lego_no_light{45, 52, "LEGO-No Light"};
    LuShaderPolicy lego_no_light_policy = shaderPolicyFromInfo(lego_no_light);
    expect(lego_no_light_policy.legopp_variant == LegoppShaderVariant::NoLight, "LEGO NoLight variant");
    expect(!lego_no_light_policy.uses_material_diffuse, "LEGO NoLight uses material diffuse alpha only, not diffuse color");
    expect(!lego_no_light_policy.uses_lighting, "LEGO NoLight disables lighting");
    expect(!lego_no_light_policy.uses_fog, "LEGO NoLight disables fog");
    expect(!lego_no_light_policy.uses_specular, "LEGO NoLight disables specular");
    expect(!lego_no_light_policy.uses_reflection, "LEGO NoLight disables reflection");
    expect(lego_no_light_policy.uses_texture, "LEGO NoLight defaults to Textured_NL source path");
    expect(lego_no_light_policy.uses_uv_animation, "LEGO NoLight Textured_NL uses texture motion");
    expect(lego_no_light_policy.reflection_map.empty(), "LEGO NoLight has no reflection map");
    expect(lego_no_light_policy.port_status == ShaderPortStatus::Verified, "LEGO NoLight verified");

    LuShaderInfo lego_animuv{21, 30, "LEGO-AnimUV"};
    LuShaderPolicy lego_animuv_policy = shaderPolicyFromInfo(lego_animuv);
    expect(lego_animuv_policy.shader_family == LegacyShaderFamily::LegoppEffect, "LEGO AnimUV uses LEGOPP effect program");
    expect(lego_animuv_policy.legopp_variant == LegoppShaderVariant::AnimUv, "LEGO AnimUV variant");
    expect(lego_animuv_policy.source_file == "LEGOPPLighting.fx", "LEGO AnimUV source file");
    expect(lego_animuv_policy.source_technique == "Technique_LEGOPPLightingOK_AnimUV", "LEGO AnimUV source technique");
    expect(lego_animuv_policy.uses_uv_animation, "LEGO AnimUV uses source AnimUV vertex path");
    expect(lego_animuv_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "LEGO AnimUV keeps output alpha");
    expect(lego_animuv_policy.port_status == ShaderPortStatus::Verified, "LEGO AnimUV verified against source and a real asset");

    LuShaderInfo darkling{65, 75, "Darkling"};
    LuShaderPolicy darkling_policy = shaderPolicyFromInfo(darkling);
    expect(darkling_policy.legopp_variant == LegoppShaderVariant::Darkling, "Darkling variant");
    expect(darkling_policy.alpha_mode == RenderAlphaMode::Opaque, "Darkling does not globally blend by shader policy");
    expect(darkling_policy.alpha_semantic == ShaderAlphaSemantic::ControlDarkling, "Darkling alpha is effect control data");
    expect(darkling_policy.source_technique == "Technique_LEGOPPLightingVertColor_Darkling", "Darkling source technique");
    expect(!darkling_policy.uses_material_diffuse, "Darkling uses vertex color/texture, not material diffuse color");
    expect(!darkling_policy.uses_specular, "Darkling base variant has no specular common path");
    expect(!darkling_policy.uses_reflection, "Darkling base variant has no reflection common path");
    expect(darkling_policy.port_status == ShaderPortStatus::Verified, "Darkling verified");

    LuShaderInfo darkling_spec{66, 76, "Darkling /w Specular"};
    LuShaderPolicy darkling_spec_policy = shaderPolicyFromInfo(darkling_spec);
    expect(darkling_spec_policy.legopp_variant == LegoppShaderVariant::DarklingSpecular, "Darkling specular variant");
    expect(darkling_spec_policy.source_technique == "Technique_LEGOPPLightingVertColor_Darkling_Specular", "Darkling specular source technique");
    expect(darkling_spec_policy.uses_specular, "Darkling specular uses LEGOPP specular common path");
    expect(darkling_spec_policy.uses_reflection, "Darkling specular uses LEGOPP reflection common path");
    expect(!darkling_spec_policy.uses_material_diffuse, "Darkling specular uses vertex color/texture, not material diffuse color");
    expect(darkling_spec_policy.port_status == ShaderPortStatus::Verified, "Darkling specular verified");

    LuShaderInfo darkling_structure{67, 77, "Darkling Structure"};
    LuShaderPolicy darkling_structure_policy = shaderPolicyFromInfo(darkling_structure);
    expect(darkling_structure_policy.legopp_variant == LegoppShaderVariant::DarklingStructure, "Darkling structure variant");
    expect(darkling_structure_policy.source_technique == "Technique_LEGOPPLightingVertColor_Darkling_NonDecal", "Darkling structure source technique");
    expect(!darkling_structure_policy.uses_specular, "Darkling structure has no specular common path");
    expect(!darkling_structure_policy.uses_reflection, "Darkling structure has no reflection common path");
    expect(!darkling_structure_policy.uses_material_diffuse, "Darkling structure uses vertex color/texture, not material diffuse color");
    expect(darkling_structure_policy.port_status == ShaderPortStatus::Verified, "Darkling structure verified");

    LuShaderInfo shiny_glint{63, 72, "ShinyGlint"};
    LuShaderPolicy shiny_glint_policy = shaderPolicyFromInfo(shiny_glint);
    expect(shiny_glint_policy.legopp_variant == LegoppShaderVariant::ShinyGlint, "Plain ShinyGlint variant");
    expect(static_cast<int>(LegoppShaderVariant::ShinyGlint) == 25, "Plain ShinyGlint variant id matches shader constant");
    expect(shiny_glint_policy.source_file == "LEGOPPLighting.fx", "Plain ShinyGlint source file");
    expect(shiny_glint_policy.source_technique == "Technique_LEGOPPLightingOK_ShinyGlint", "Plain ShinyGlint source technique");
    expect(shiny_glint_policy.uses_material_diffuse, "Plain ShinyGlint uses regular LEGOPP brick color path");
    expect(shiny_glint_policy.uses_specular, "Plain ShinyGlint uses regular LEGOPP specular path");
    expect(shiny_glint_policy.uses_reflection, "Plain ShinyGlint uses regular LEGOPP reflection path");
    expect(shiny_glint_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Plain ShinyGlint keeps normal output alpha");
    expect(shiny_glint_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "Plain ShinyGlint records missing validation assets");
    expect(shiny_glint_policy.validation_status_note.find("no valid S63_ prefix") != std::string::npos,
        "Plain ShinyGlint records no valid prefix candidates");
    expect(shiny_glint_policy.port_status == ShaderPortStatus::Verified, "Plain ShinyGlint source math verified");

    LuShaderInfo darkling_glint{92, 102, "Darking Shiny Glint"};
    LuShaderPolicy darkling_glint_policy = shaderPolicyFromInfo(darkling_glint);
    expect(darkling_glint_policy.legopp_variant == LegoppShaderVariant::DarklingShinyGlint, "Shiny glint variant");
    expect(darkling_glint_policy.source_technique == "Technique_LEGOPPLightingOK_ShinyGlint", "Shiny glint source technique");
    expect(darkling_glint_policy.uses_material_diffuse, "Shiny glint uses regular LEGOPP brick color path");
    expect(darkling_glint_policy.uses_specular, "Shiny glint uses regular LEGOPP specular path");
    expect(darkling_glint_policy.uses_reflection, "Shiny glint uses regular LEGOPP reflection path");
    expect(darkling_glint_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Shiny glint keeps normal output alpha");
    expect(darkling_glint_policy.shiny_glint_height == 0.0f, "Shiny glint uses source default height");
    expect(darkling_glint_policy.shiny_glint_size_power == 0.0f, "Shiny glint uses source default size power");
    expect(darkling_glint_policy.shiny_glint_color.x == 1.0f &&
           darkling_glint_policy.shiny_glint_color.y == 1.0f &&
           darkling_glint_policy.shiny_glint_color.z == 1.0f &&
           darkling_glint_policy.shiny_glint_color.w == 1.0f, "Shiny glint uses source default color");
    expect(darkling_glint_policy.source_status_note.find("no distinct Darkling Shiny Glint technique") != std::string::npos,
        "Shiny glint records missing darkling-specific source technique");
    expect(darkling_glint_policy.port_status == ShaderPortStatus::Verified, "Shiny glint source math verified");

    LuShaderInfo darkling_spec_glint{93, 103, "Darkling /w Specular Shiny Glint"};
    LuShaderPolicy darkling_spec_glint_policy = shaderPolicyFromInfo(darkling_spec_glint);
    expect(darkling_spec_glint_policy.source_technique == "Technique_LEGOPPLightingOK_ShinyGlint", "Spec shiny glint maps to source glint family");
    expect(darkling_spec_glint_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Spec shiny glint alpha is not darkling control data");
    expect(darkling_spec_glint_policy.source_status_note.find("no distinct Darkling Specular Shiny Glint technique") != std::string::npos,
        "Spec shiny glint records missing darkling-specific source technique");
    expect(darkling_spec_glint_policy.port_status == ShaderPortStatus::Verified, "Spec shiny glint source math verified");

    LuShaderInfo darkling_structure_glint{94, 104, "Darkling Structure Shiny Glint"};
    LuShaderPolicy darkling_structure_glint_policy = shaderPolicyFromInfo(darkling_structure_glint);
    expect(darkling_structure_glint_policy.source_technique == "Technique_LEGOPPLightingOK_ShinyGlint", "Structure shiny glint maps to source glint family");
    expect(darkling_structure_glint_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Structure shiny glint alpha is not darkling control data");
    expect(darkling_structure_glint_policy.source_status_note.find("no distinct Darkling Structure Shiny Glint technique") != std::string::npos,
        "Structure shiny glint records missing darkling-specific source technique");
    expect(darkling_structure_glint_policy.port_status == ShaderPortStatus::Verified, "Structure shiny glint source math verified");

    LuShaderInfo vert_color_alpha_fade{33, 9, "VertColor_Alpha_Fade"};
    LuShaderPolicy vert_color_alpha_fade_policy = shaderPolicyFromInfo(vert_color_alpha_fade);
    expect(vert_color_alpha_fade_policy.shader_family == LegacyShaderFamily::Basic, "VertColor Alpha Fade uses Basic program");
    expect(vert_color_alpha_fade_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "VertColor Alpha Fade blends");
    expect(!vert_color_alpha_fade_policy.depth_write, "VertColor Alpha Fade disables depth writes");
    expect(vert_color_alpha_fade_policy.uses_vertex_color, "VertColor Alpha Fade uses vertex color");
    expect(vert_color_alpha_fade_policy.source_status_note.find("No VertColor_Alpha_Fade technique") != std::string::npos,
        "VertColor Alpha Fade records why source is deferred");
    expect(vert_color_alpha_fade_policy.port_status == ShaderPortStatus::Inferred, "VertColor Alpha Fade is inferred");

    LuShaderInfo one_sided_alpha_skinned{52, 59, "OneSidedAlpha AnimUV V Skinned"};
    LuShaderPolicy one_sided_alpha_skinned_policy = shaderPolicyFromInfo(one_sided_alpha_skinned);
    expect(one_sided_alpha_skinned_policy.shader_family == LegacyShaderFamily::AlphaUvScroll, "OneSidedAlpha skinned uses UV scroll alpha program");
    expect(one_sided_alpha_skinned_policy.source_technique == "Technique_AlphaAsAlphaSkinned_UVScrolling_SimpleV_NoLighting_AlphaAnim", "OneSidedAlpha skinned source technique");
    expect(one_sided_alpha_skinned_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "OneSidedAlpha skinned blends");
    expect(one_sided_alpha_skinned_policy.cull_mode == RenderCullMode::Backface, "OneSidedAlpha skinned remains one-sided");
    expect(!one_sided_alpha_skinned_policy.uses_lighting, "OneSidedAlpha skinned is no-light");
    expect(one_sided_alpha_skinned_policy.uses_uv_animation, "OneSidedAlpha skinned scrolls UVs");
    expect(one_sided_alpha_skinned_policy.uses_alpha_animation, "OneSidedAlpha skinned uses fade alpha");
    expect(one_sided_alpha_skinned_policy.port_status == ShaderPortStatus::Verified, "OneSidedAlpha skinned verified");

    LuShaderInfo one_sided_alpha{57, 64, "OneSidedAlpha AnimUV V"};
    LuShaderPolicy one_sided_alpha_policy = shaderPolicyFromInfo(one_sided_alpha);
    expect(one_sided_alpha_policy.shader_family == LegacyShaderFamily::AlphaUvScroll, "OneSidedAlpha uses UV scroll alpha program");
    expect(one_sided_alpha_policy.source_technique == "Technique_AlphaAsAlpha_UVScrolling_SimpleV_NoLighting_AlphaAnim", "OneSidedAlpha source technique");
    expect(one_sided_alpha_policy.cull_mode == RenderCullMode::Backface, "OneSidedAlpha remains one-sided");
    expect(one_sided_alpha_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "OneSidedAlpha records no local resolved users");

    LuShaderInfo two_textures_added{83, 93, "Two Textures Added NL VC AnimUV"};
    LuShaderPolicy two_textures_added_policy = shaderPolicyFromInfo(two_textures_added);
    expect(two_textures_added_policy.shader_family == LegacyShaderFamily::BasicTwoLayer, "Two Textures Added uses Basic two-layer program");
    expect(two_textures_added_policy.source_file == "BasicShaders.fx", "Two Textures Added source file");
    expect(two_textures_added_policy.source_technique == "Technique_TwoLayersAdded_NoLighting_VertColor_UVScrolling", "Two Textures Added source technique");
    expect(two_textures_added_policy.uses_vertex_color, "Two Textures Added uses vertex color");
    expect(two_textures_added_policy.uses_material_diffuse, "Two Textures Added uses material diffuse r/g layer weights");
    expect(!two_textures_added_policy.uses_lighting, "Two Textures Added is no-light");
    expect(!two_textures_added_policy.uses_specular, "Two Textures Added ignores specular");
    expect(!two_textures_added_policy.uses_reflection, "Two Textures Added ignores reflection");
    expect(two_textures_added_policy.uses_uv_animation, "Two Textures Added uses two UV motion matrices");
    expect(two_textures_added_policy.port_status == ShaderPortStatus::Verified, "Two Textures Added verified");

    LuShaderInfo polished_metal{88, 98, "Polished Metal"};
    LuShaderPolicy polished_metal_policy = shaderPolicyFromInfo(polished_metal);
    expect(polished_metal_policy.shader_family == LegacyShaderFamily::Metallic, "Polished Metal is Metallic");
    expect(polished_metal_policy.source_file == "Metallic.fx", "Polished Metal source file");
    expect(polished_metal_policy.source_technique == "Technique_Lighting_PolishedMetal", "Polished Metal source technique");
    expect(!polished_metal_policy.uses_texture, "Polished Metal default fixture is no-texture");
    expect(polished_metal_policy.uses_material_diffuse, "Polished Metal uses material diffuse by default");
    expect(polished_metal_policy.uses_specular, "Polished Metal uses metallic specular");
    expect(polished_metal_policy.uses_reflection, "Polished Metal uses env reflection");
    expect(polished_metal_policy.reflection_map == "metal_reflection_polished.dds", "Polished Metal reflection map");
    expect(polished_metal_policy.port_status == ShaderPortStatus::Verified, "Polished Metal source math verified");

    LuShaderInfo brushed_steel{89, 99, "Brushed Steel"};
    LuShaderPolicy brushed_steel_policy = shaderPolicyFromInfo(brushed_steel);
    expect(brushed_steel_policy.shader_family == LegacyShaderFamily::Metallic, "Brushed Steel is Metallic");
    expect(brushed_steel_policy.source_file == "Metallic.fx", "Brushed Steel source file");
    expect(brushed_steel_policy.source_technique == "Technique_Lighting_BrushedSteel", "Brushed Steel source technique");
    expect(!brushed_steel_policy.uses_texture, "Brushed Steel default fixture is no-texture");
    expect(brushed_steel_policy.uses_material_diffuse, "Brushed Steel uses material diffuse by default");
    expect(brushed_steel_policy.uses_specular, "Brushed Steel uses metallic specular");
    expect(brushed_steel_policy.uses_reflection, "Brushed Steel uses env reflection");
    expect(brushed_steel_policy.reflection_map == "metal_reflection_brushed.dds", "Brushed Steel reflection map");
    expect(brushed_steel_policy.port_status == ShaderPortStatus::Verified, "Brushed Steel source math verified");

    LuShaderInfo brushed_steel_item{90, 100, "Brushed Steel Item"};
    LuShaderPolicy brushed_steel_item_policy = shaderPolicyFromInfo(brushed_steel_item);
    expect(brushed_steel_item_policy.shader_family == LegacyShaderFamily::Metallic, "Brushed Steel Item is Metallic, not LEGOPP");
    expect(brushed_steel_item_policy.source_file == "Metallic.fx", "Brushed Steel Item source file");
    expect(brushed_steel_item_policy.source_technique == "Technique_Lighting_BrushedSteel_VertColor_Item", "Brushed Steel Item source technique");
    expect(brushed_steel_item_policy.uses_vertex_color, "Brushed Steel Item uses vertex color item path");
    expect(!brushed_steel_item_policy.uses_texture, "Brushed Steel Item fixture is the no-texture item variant");
    expect(brushed_steel_item_policy.uses_specular, "Brushed Steel Item uses metallic specular");
    expect(brushed_steel_item_policy.uses_reflection, "Brushed Steel Item uses env reflection");
    expect(brushed_steel_item_policy.reflection_map == "metal_reflection_brushed.dds", "Brushed Steel Item uses brushed metal reflection map");
    expect(brushed_steel_item_policy.port_status == ShaderPortStatus::Verified, "Brushed Steel Item source math verified");

    LuShaderInfo two_layers_blended_nl{95, 105, "Two Layers Blended NL VC AnimUV"};
    LuShaderPolicy two_layers_blended_nl_policy = shaderPolicyFromInfo(two_layers_blended_nl);
    expect(two_layers_blended_nl_policy.shader_family == LegacyShaderFamily::BasicTwoLayer, "Two Layers Blended NL uses Basic two-layer identity");
    expect(two_layers_blended_nl_policy.source_file == "inferred", "Two Layers Blended NL source missing from FX package");
    expect(two_layers_blended_nl_policy.source_status_note.find("No TwoLayersBlended technique") != std::string::npos,
        "Two Layers Blended NL records why source is deferred");
    expect(two_layers_blended_nl_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Two Layers Blended NL blends");
    expect(!two_layers_blended_nl_policy.uses_lighting, "Two Layers Blended NL is no-light");
    expect(two_layers_blended_nl_policy.port_status == ShaderPortStatus::Inferred, "Two Layers Blended NL is inferred");

    LuShaderInfo two_layers_blended{96, 106, "Two Layers Blended VC AnimUV"};
    LuShaderPolicy two_layers_blended_policy = shaderPolicyFromInfo(two_layers_blended);
    expect(two_layers_blended_policy.shader_family == LegacyShaderFamily::BasicTwoLayer, "Two Layers Blended uses Basic two-layer identity");
    expect(two_layers_blended_policy.source_file == "inferred", "Two Layers Blended source missing from FX package");
    expect(two_layers_blended_policy.source_status_note.find("No TwoLayersBlended technique") != std::string::npos,
        "Two Layers Blended records why source is deferred");
    expect(two_layers_blended_policy.uses_lighting, "Two Layers Blended without NL keeps lighting flag");
    expect(two_layers_blended_policy.port_status == ShaderPortStatus::Inferred, "Two Layers Blended is inferred");

    LuShaderInfo two_layers_added{97, 107, "Two Layers Added VC AnimUV"};
    LuShaderPolicy two_layers_added_policy = shaderPolicyFromInfo(two_layers_added);
    expect(two_layers_added_policy.shader_family == LegacyShaderFamily::BasicTwoLayer, "Two Layers Added uses Basic two-layer identity");
    expect(two_layers_added_policy.source_technique == "Technique_TwoLayersAdded_NoLighting_VertColor_UVScrolling", "Two Layers Added maps to available source technique");
    expect(two_layers_added_policy.validation_status_note.find("No resolved CDClient") != std::string::npos,
        "Two Layers Added records no local resolved users");
    expect(two_layers_added_policy.port_status == ShaderPortStatus::Verified, "Two Layers Added source math verified");

    LuShaderInfo additive{77, 87, "Additive NoLight VertColor"};
    LuShaderPolicy additive_policy = shaderPolicyFromInfo(additive);
    expect(additive_policy.alpha_mode == RenderAlphaMode::Additive, "Additive shader policy");
    expect(!additive_policy.depth_write, "Additive disables depth writes");
    expect(additive_policy.uses_vertex_color, "Additive uses vertex color");
    expect(additive_policy.source_status_note.find("No additive static-mesh") != std::string::npos,
        "Additive records why source is provisional");
    expect(additive_policy.port_status == ShaderPortStatus::Placeholder, "Additive source technique is provisional");

    LuShaderInfo pet_cloud{82, 92, "Pet Taming LEGO In Cloud"};
    LuShaderPolicy pet_cloud_policy = shaderPolicyFromInfo(pet_cloud);
    expect(pet_cloud_policy.shader_family == LegacyShaderFamily::Basic, "Pet cloud uses PostProcessing Basic program path");
    expect(pet_cloud_policy.legopp_variant == LegoppShaderVariant::PetTamingCloud, "Pet cloud keeps LEGO-family diagnostic variant");
    expect(pet_cloud_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Pet cloud source blends");
    expect(!pet_cloud_policy.depth_write, "Pet cloud source disables depth writes");
    expect(pet_cloud_policy.source_file == "PostProcessingShaders.fx", "Pet cloud source file");
    expect(pet_cloud_policy.source_technique == "Technique_ImaginationCloud", "Pet cloud source technique");
    expect(!pet_cloud_policy.uses_ni_render_state,
        "Pet cloud technique ignores inherited Ni render state per FX annotation");
    expect(pet_cloud_policy.uses_vertex_color, "Pet cloud source Basic_VS uses vertex color");
    expect(!pet_cloud_policy.uses_lighting, "Pet cloud source Basic_VS is unlit");
    expect(!pet_cloud_policy.uses_fog, "Pet cloud source has no fog");
    expect(!pet_cloud_policy.uses_specular, "Pet cloud source has no specular");
    expect(!pet_cloud_policy.uses_reflection, "Pet cloud source has no reflection");
    expect(pet_cloud_policy.port_status == ShaderPortStatus::Verified, "Pet cloud verified against PostProcessing source and real assets");

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
    expect(!ocean_distort_policy.depth_write, "Ocean distortion disables depth writes");
    expect(ocean_distort_policy.source_file == "Ocean.fx", "Ocean distortion source file");
    expect(ocean_distort_policy.source_technique == "Technique_Ocean_Distort_2Layers", "Ocean distortion technique");
    expect(ocean_distort_policy.uses_vertex_color, "Ocean distortion uses vertex color");
    expect(ocean_distort_policy.uses_lighting, "Ocean distortion uses source light/ambient");
    expect(!ocean_distort_policy.uses_specular, "Ocean distortion has no specular");
    expect(!ocean_distort_policy.uses_reflection, "Ocean distortion has no reflection");
    expect(ocean_distort_policy.uses_uv_animation, "Ocean distortion uses UV animation");
    expect(ocean_distort_policy.uses_alpha_animation, "Ocean distortion uses fade alpha");
    expect(ocean_distort_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Ocean distortion outputs alpha");
    expect(ocean_distort_policy.port_status == ShaderPortStatus::Verified, "Ocean distortion verified");

    LuShaderInfo alpha_uv_scroll{61, 69, "ScrollingUV_NoLight_AnimAlpha"};
    LuShaderPolicy alpha_uv_scroll_policy = shaderPolicyFromInfo(alpha_uv_scroll);
    expect(alpha_uv_scroll_policy.shader_family == LegacyShaderFamily::AlphaUvScroll, "UV scroll alpha program");
    expect(alpha_uv_scroll_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "UV scroll alpha blends");
    expect(!alpha_uv_scroll_policy.depth_write, "UV scroll alpha disables depth writes");
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
    expect(ocean_distort_directional_policy.uses_lighting, "Ocean directional distortion uses source light/ambient");
    expect(!ocean_distort_directional_policy.uses_specular, "Ocean directional distortion has no specular");
    expect(!ocean_distort_directional_policy.uses_reflection, "Ocean directional distortion has no reflection");
    expect(ocean_distort_directional_policy.uses_uv_animation, "Ocean directional distortion uses UV animation");
    expect(ocean_distort_directional_policy.uses_alpha_animation, "Ocean directional distortion uses fade alpha");
    expect(ocean_distort_directional_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Ocean directional distortion outputs alpha");
    expect(ocean_distort_directional_policy.port_status == ShaderPortStatus::Verified, "Ocean directional distortion verified");

    LuShaderInfo ocean_distort_fx{80, 90, "Distortion FX (Ocean)"};
    LuShaderPolicy ocean_distort_fx_policy = shaderPolicyFromInfo(ocean_distort_fx);
    expect(ocean_distort_fx_policy.shader_family == LegacyShaderFamily::OceanDistortFx, "Ocean FX distortion program");
    expect(ocean_distort_fx_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Ocean FX distortion blends");
    expect(!ocean_distort_fx_policy.depth_write, "Ocean FX distortion disables depth writes");
    expect(ocean_distort_fx_policy.source_file == "Ocean.fx", "Ocean FX distortion source file");
    expect(ocean_distort_fx_policy.source_technique == "Technique_Ocean_Distort_FX_2Layers", "Ocean FX distortion technique");
    expect(ocean_distort_fx_policy.uses_vertex_color, "Ocean FX distortion uses vertex color");
    expect(!ocean_distort_fx_policy.uses_lighting, "Ocean FX distortion vertex source is unlit");
    expect(!ocean_distort_fx_policy.uses_specular, "Ocean FX distortion has no specular");
    expect(!ocean_distort_fx_policy.uses_reflection, "Ocean FX distortion has no reflection");
    expect(ocean_distort_fx_policy.uses_uv_animation, "Ocean FX distortion uses UV animation");
    expect(ocean_distort_fx_policy.uses_alpha_animation, "Ocean FX distortion uses fade/control alpha");
    expect(ocean_distort_fx_policy.alpha_semantic == ShaderAlphaSemantic::OutputAlpha, "Ocean FX distortion outputs alpha after source FX window");
    expect(ocean_distort_fx_policy.port_status == ShaderPortStatus::Verified, "Ocean FX distortion verified");

    LuShaderInfo ocean_distort_nodepth{85, 95, "Distortion NoDepth (Ocean) (Alpha)"};
    LuShaderPolicy ocean_distort_nodepth_policy = shaderPolicyFromInfo(ocean_distort_nodepth);
    expect(ocean_distort_nodepth_policy.shader_family == LegacyShaderFamily::OceanDistort, "Ocean NoDepth uses regular ocean identity");
    expect(ocean_distort_nodepth_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Ocean NoDepth blends");
    expect(!ocean_distort_nodepth_policy.depth_write, "Ocean NoDepth disables depth writes");
    expect(ocean_distort_nodepth_policy.source_file == "Ocean.fx", "Ocean NoDepth source file");
    expect(ocean_distort_nodepth_policy.source_technique == "Technique_Ocean_Distort_2Layers", "Ocean NoDepth closest technique");
    expect(ocean_distort_nodepth_policy.source_status_note.find("No Ocean Distort NoDepth technique") != std::string::npos,
        "Ocean NoDepth records why source is inferred");
    expect(ocean_distort_nodepth_policy.uses_vertex_color, "Ocean NoDepth uses vertex color");
    expect(!ocean_distort_nodepth_policy.uses_specular, "Ocean NoDepth has no specular");
    expect(!ocean_distort_nodepth_policy.uses_reflection, "Ocean NoDepth has no reflection");
    expect(ocean_distort_nodepth_policy.uses_uv_animation, "Ocean NoDepth uses UV animation");
    expect(ocean_distort_nodepth_policy.uses_alpha_animation, "Ocean NoDepth uses fade alpha");
    expect(ocean_distort_nodepth_policy.port_status == ShaderPortStatus::Inferred, "Ocean NoDepth is explicitly inferred");

    LuShaderInfo ocean_distort_unlit{91, 101, "Distortion (Ocean) Unlit"};
    LuShaderPolicy ocean_distort_unlit_policy = shaderPolicyFromInfo(ocean_distort_unlit);
    expect(ocean_distort_unlit_policy.shader_family == LegacyShaderFamily::OceanDistortUnlit, "Ocean unlit distortion program");
    expect(ocean_distort_unlit_policy.alpha_mode == RenderAlphaMode::AlphaBlend, "Ocean unlit distortion blends");
    expect(!ocean_distort_unlit_policy.depth_write, "Ocean unlit distortion disables depth writes");
    expect(ocean_distort_unlit_policy.source_file == "Ocean.fx", "Ocean unlit distortion source file");
    expect(ocean_distort_unlit_policy.source_technique == "Technique_Ocean_Distort_Unlit_2Layers", "Ocean unlit distortion technique");
    expect(ocean_distort_unlit_policy.uses_vertex_color, "Ocean unlit distortion uses vertex color");
    expect(!ocean_distort_unlit_policy.uses_lighting, "Ocean unlit distortion is no-light");
    expect(!ocean_distort_unlit_policy.uses_specular, "Ocean unlit distortion has no specular");
    expect(!ocean_distort_unlit_policy.uses_reflection, "Ocean unlit distortion has no reflection");
    expect(ocean_distort_unlit_policy.uses_uv_animation, "Ocean unlit distortion uses UV animation");
    expect(ocean_distort_unlit_policy.uses_alpha_animation, "Ocean unlit distortion uses fade alpha");
    expect(ocean_distort_unlit_policy.port_status == ShaderPortStatus::Verified, "Ocean unlit distortion verified");

    MaterialAsset authored_state_material;
    authored_state_material.diffuse.w = 0.25f;
    authored_state_material.nif_resolved_state.alpha.present = true;
    authored_state_material.nif_resolved_state.alpha.raw_flags = 0x2609u;
    authored_state_material.nif_resolved_state.alpha.blend_enabled = true;
    authored_state_material.nif_resolved_state.alpha.source_blend = 4;
    authored_state_material.nif_resolved_state.alpha.destination_blend = 9;
    authored_state_material.nif_resolved_state.alpha.test_enabled = true;
    authored_state_material.nif_resolved_state.alpha.test_function = 7;
    authored_state_material.nif_resolved_state.alpha.threshold = 0;
    authored_state_material.nif_resolved_state.alpha.no_sorter = true;
    authored_state_material.nif_resolved_state.z_buffer.present = true;
    authored_state_material.nif_resolved_state.z_buffer.test_enabled = true;
    authored_state_material.nif_resolved_state.z_buffer.write_enabled = true;
    authored_state_material.nif_resolved_state.z_buffer.test_function = 3;
    LuShaderPolicy ni_state_policy;
    ni_state_policy.uses_ni_render_state = true;
    ni_state_policy.alpha_semantic = ShaderAlphaSemantic::ControlEmissive;
    applyEffectiveNifRenderState(authored_state_material, ni_state_policy);
    expect(authored_state_material.alpha_blend,
        "explicit NiAlpha blending is honored even when alpha is emissive control data");
    expect(authored_state_material.alpha_test && authored_state_material.alpha_threshold == 0,
        "NiAlpha test-enable preserves a zero reference");
    expect(authored_state_material.source_blend == 4 && authored_state_material.destination_blend == 9,
        "NiAlpha blend factors survive effective-state resolution");
    expect(authored_state_material.alpha_test_function == 7,
        "NiAlpha comparison function survives effective-state resolution");
    expect(authored_state_material.depth_write && authored_state_material.depth_test_function == 3,
        "blended Ni state preserves depth writes and LessEqual testing");
    expect(authored_state_material.disable_transparent_sort,
        "NiAlpha no-sort survives effective-state resolution");

    MaterialAsset ignored_ni_state_material = authored_state_material;
    LuShaderPolicy technique_owned_policy;
    technique_owned_policy.uses_ni_render_state = false;
    technique_owned_policy.alpha_mode = RenderAlphaMode::AlphaBlend;
    technique_owned_policy.depth_write = false;
    applyEffectiveNifRenderState(ignored_ni_state_material, technique_owned_policy);
    expect(ignored_ni_state_material.alpha_blend && !ignored_ni_state_material.depth_write,
        "UsesNiRenderState=false keeps technique blend and depth state");
    expect(!ignored_ni_state_material.alpha_test &&
           ignored_ni_state_material.source_blend == 6 &&
           ignored_ni_state_material.destination_blend == 7,
        "UsesNiRenderState=false ignores authored NiAlpha details");

    MaterialAsset alpha_value_only_material;
    alpha_value_only_material.diffuse.w = 0.2f;
    applyEffectiveNifRenderState(alpha_value_only_material, ni_state_policy);
    expect(!alpha_value_only_material.alpha_blend,
        "material alpha alone never promotes an opaque technique to blending");

    MaterialAsset vertex_color_material;
    vertex_color_material.mesh_has_vertex_colors = true;
    vertex_color_material.nif_resolved_state.vertex_color.present = true;
    vertex_color_material.nif_resolved_state.vertex_color.source_vertex_mode = 0;
    expect(!nifVertexColorsAreEffective(vertex_color_material),
        "NiVertexColor source-ignore rejects an otherwise present vertex-color stream");
    vertex_color_material.nif_resolved_state.vertex_color.source_vertex_mode = 2;
    expect(nifVertexColorsAreEffective(vertex_color_material),
        "NiVertexColor ambient/diffuse mode enables the authored vertex-color stream");
    vertex_color_material.nif_resolved_state.vertex_color.present = false;
    expect(nifVertexColorsAreEffective(vertex_color_material),
        "a stream remains usable when no NiVertexColor override is authored");
    vertex_color_material.mesh_has_vertex_colors = false;
    expect(!nifVertexColorsAreEffective(vertex_color_material),
        "NiVertexColor mode cannot invent a missing vertex-color stream");

    MaterialAsset property_state_material;
    property_state_material.nif_resolved_state.has_specular = true;
    property_state_material.nif_resolved_state.specular_enabled = false;
    property_state_material.nif_resolved_state.stencil.present = true;
    property_state_material.nif_resolved_state.stencil.enabled = true;
    property_state_material.nif_resolved_state.stencil.fail_action = 2;
    property_state_material.nif_resolved_state.stencil.z_fail_action = 3;
    property_state_material.nif_resolved_state.stencil.pass_action = 5;
    property_state_material.nif_resolved_state.stencil.test_function = 4;
    property_state_material.nif_resolved_state.stencil.reference = 0x123u;
    property_state_material.nif_resolved_state.stencil.mask = 0x1ffu;
    property_state_material.nif_resolved_state.stencil.draw_mode = 3;
    ni_state_policy.uses_specular = true;
    applyEffectiveNifRenderState(property_state_material, ni_state_policy);
    expect(!property_state_material.lu_shader_uses_specular,
        "disabled NiSpecularProperty gates a specular-capable technique");
    expect(property_state_material.stencil_enabled &&
           property_state_material.stencil_fail_action == 2 &&
           property_state_material.stencil_z_fail_action == 3 &&
           property_state_material.stencil_pass_action == 5,
        "enabled NiStencil actions survive effective-state resolution");
    expect(property_state_material.stencil_test_function == 4 &&
           property_state_material.stencil_reference == 0x23u &&
           property_state_material.stencil_read_mask == 0xffu,
        "NiStencil comparison state is narrowed to the GPU's eight-bit stencil buffer");
    expect(property_state_material.cull_mode == RenderCullMode::TwoSided,
        "NiStencil DRAW_BOTH overrides technique backface culling");
    property_state_material.nif_resolved_state.specular_enabled = true;
    applyEffectiveNifRenderState(property_state_material, ni_state_policy);
    expect(property_state_material.lu_shader_uses_specular,
        "enabled NiSpecularProperty preserves a specular-capable technique");

    MaterialAsset ignored_property_state;
    ignored_property_state.nif_resolved_state.stencil = property_state_material.nif_resolved_state.stencil;
    technique_owned_policy.uses_specular = true;
    applyEffectiveNifRenderState(ignored_property_state, technique_owned_policy);
    expect(!ignored_property_state.stencil_enabled &&
           ignored_property_state.cull_mode == RenderCullMode::Backface,
        "UsesNiRenderState=false ignores NiStencil render state");

    MaterialAsset requested_blended_depth_write;
    requested_blended_depth_write.alpha_mode = RenderAlphaMode::AlphaBlend;
    requested_blended_depth_write.alpha_blend = true;
    requested_blended_depth_write.depth_write = true;
    const CurrentRenderStateDiagnostic current_state =
        currentRenderStateDiagnostic(requested_blended_depth_write);
    expect(current_state.transparent_classification,
        "current state diagnostic classifies blended material as transparent");
    expect(current_state.requested_depth_write,
        "current state diagnostic preserves requested depth write");
    expect(current_state.submitted_depth_write,
        "blending preserves an independently requested depth write");
    expect(current_state.submitted_alpha_blend,
        "current state diagnostic exposes submitted alpha blending");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
