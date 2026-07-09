#include "lu/renderer/lu_import/animation_importer.h"

#include "gamebryo/kfm/kfm_reader.h"
#include "gamebryo/nif/nif_reader.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <unordered_map>

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

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::filesystem::path normalizeRelativePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.rfind("./", 0) == 0) value.erase(0, 2);
    return std::filesystem::path(value);
}

const lu::assets::NifTextKeyExtraData* findTextKeys(
    const lu::assets::NifFile& kf,
    int32_t block_ref) {

    if (block_ref < 0) return nullptr;
    for (const auto& text_keys : kf.text_key_data) {
        if (static_cast<int32_t>(text_keys.block_index) == block_ref) return &text_keys;
    }
    return nullptr;
}

AnimationClip clipFromSequence(
    const lu::assets::NifFile& kf,
    const lu::assets::NifControllerSequence& sequence,
    const std::filesystem::path& source_path,
    uint32_t sequence_id,
    uint32_t anim_index) {

    AnimationClip clip;
    clip.name = sequence.name;
    clip.source_path = source_path.string();
    clip.sequence_id = sequence_id;
    clip.anim_index = anim_index;
    clip.start_time = sequence.start_time;
    clip.stop_time = sequence.stop_time;
    clip.frequency = sequence.frequency;
    clip.cycle_type = sequence.cycle_type;

    clip.controlled_blocks.reserve(sequence.controlled_blocks.size());
    for (const auto& block : sequence.controlled_blocks) {
        AnimationControlledBlock out;
        out.node_name = block.node_name;
        out.controller_type = block.controller_type;
        out.property_type = block.property_type;
        out.controller_id = block.controller_id;
        out.interpolator_id = block.interpolator_id;
        out.interpolator_ref = block.interpolator_ref;
        out.controller_ref = block.controller_ref;
        out.priority = block.priority;
        clip.controlled_blocks.push_back(std::move(out));
    }

    if (const auto* text_keys = findTextKeys(kf, sequence.text_keys_ref)) {
        clip.text_keys.reserve(text_keys->keys.size());
        for (const auto& key : text_keys->keys) {
            clip.text_keys.push_back({key.time, key.text});
        }
    }
    return clip;
}

AnimationImportResult importKf(const std::filesystem::path& path) {
    AnimationImportResult result;
    std::vector<uint8_t> data = readFile(path);
    if (data.empty()) {
        result.error = "Could not read KF file: " + path.string();
        return result;
    }

    auto kf = lu::assets::nif_parse(std::span<const uint8_t>(data.data(), data.size()));
    result.animation.source_path = path.string();
    result.animation.clips.reserve(kf.sequences.size());
    for (size_t i = 0; i < kf.sequences.size(); ++i) {
        result.animation.clips.push_back(
            clipFromSequence(kf, kf.sequences[i], path, static_cast<uint32_t>(i), static_cast<uint32_t>(i)));
    }
    return result;
}

AnimationImportResult importKfm(const std::filesystem::path& path) {
    AnimationImportResult result;
    std::vector<uint8_t> data = readFile(path);
    if (data.empty()) {
        result.error = "Could not read KFM file: " + path.string();
        return result;
    }

    auto kfm = lu::assets::kfm_parse(std::span<const uint8_t>(data.data(), data.size()));
    result.animation.source_path = path.string();
    result.animation.model_path = kfm.model_path_normalized();
    result.animation.model_root = kfm.model_root;

    std::unordered_map<std::string, lu::assets::NifFile> kf_cache;
    std::unordered_map<std::string, std::filesystem::path> kf_paths;
    for (const auto& sequence : kfm.sequences) {
        std::filesystem::path kf_path = path.parent_path() / normalizeRelativePath(sequence.kf_filename);
        const std::string cache_key = kf_path.lexically_normal().string();
        auto kf_it = kf_cache.find(cache_key);
        if (kf_it == kf_cache.end()) {
            std::vector<uint8_t> kf_data = readFile(kf_path);
            if (kf_data.empty()) {
                result.error = "Could not read referenced KF file: " + kf_path.string();
                return result;
            }
            auto parsed = lu::assets::nif_parse(std::span<const uint8_t>(kf_data.data(), kf_data.size()));
            kf_paths[cache_key] = kf_path;
            kf_it = kf_cache.emplace(cache_key, std::move(parsed)).first;
        }

        const auto& kf = kf_it->second;
        if (sequence.anim_index >= kf.sequences.size()) {
            result.error = "KFM sequence anim_index is out of range for KF: " + kf_path.string();
            return result;
        }
        result.animation.clips.push_back(clipFromSequence(
            kf,
            kf.sequences[sequence.anim_index],
            kf_paths[cache_key],
            sequence.id,
            sequence.anim_index));
    }

    return result;
}

} // namespace

AnimationImportResult importAnimation(const AnimationImportOptions& options) {
    AnimationImportResult result;
    const std::string ext = lowerCopy(options.path.extension().string());
    try {
        if (ext == ".kfm") return importKfm(options.path);
        if (ext == ".kf") return importKf(options.path);
        result.error = "Unsupported animation file extension: " + options.path.string();
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

} // namespace lu::renderer::lu_import
