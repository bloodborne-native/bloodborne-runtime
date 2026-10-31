// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <unordered_set>
#include "common/serdes.h"

#include <cstdio>
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 23u; // Metal: no buffer offset table in the shaders
static constexpr u32 ShaderMetaVersion = 9u; // fixed quad local output map in InfoPersistent
static constexpr u32 PipelineKeyVersion = 6u; // fixed quad vertex pipeline key
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }

    // Only the vertex stage has one: a later stage without one (geometry) must not erase it, or
    // the pipeline is built with an empty fetch shader (ported from bbport 0.6-pre1, 72e5c8c).
    if (spec.fetch_shader_data) {
        fetch_shader_data = spec.fetch_shader_data;
    }
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    // bbport: built before it is inserted: a pipeline the driver rejects while preloading throws
    // (Serialization::CorruptData) and leaves no empty entry behind.
    auto pipeline = std::make_unique<ComputePipeline>(device, profile, compute_key, *sel.infos[0],
                                                      sel.modules[0], sdata, true);
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill({});

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    // Supermedo/bloodborne_pc f33e7617 (João Pedro Valadares): rebuild motion pipelines
    // at first use. Cached metadata can describe a plain shader permutation, bypassing
    // LoadShaderMeta's motion check; motion shaders also contain session-local addresses.
    if (sel.graphics_key.motion_vectors) {
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    // An entry left half loaded must not lend its stages to this one (bbport 0.6-pre1).
    sel.infos.fill(nullptr);
    sel.modules.fill({});
    sel.fetch_shader.reset();
    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    auto pipeline = std::make_unique<GraphicsPipeline>(
        device, profile, sel.graphics_key, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill({});
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    // bbport: a SPIR-V binary starts with its magic number and a 5-word header; anything else is
    // a damaged file (crash or power loss while it was written) the driver must not see.
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        if (!spv.empty()) {
            throw Serialization::CorruptData{"damaged SPIR-V in the shader cache"};
        }
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    VideoCore::Backend::ShaderHandle module{};
    // bbport: a module the driver rejects is a damaged entry (WarmUp rebuilds the cache), not a
    // fatal error as in CompileSPV.
    const auto compile = [&] {
        const auto shader = device.CreateShader(spv, {});
        if (!shader.handle) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        return shader.handle;
    };

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = compile();
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = compile();
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Shader metadata and SPIR-V filenames share permutation indices. After a backend version
    // change, retaining old blobs alongside new ones can associate a valid metadata entry with
    // a different binary at the same index. Migrate the entire cache before loading any modules.
    constexpr std::array<u32, 4> cache_versions{0x42425043u, Serialization::ShaderBinaryVersion,
                                               Serialization::ShaderMetaVersion,
                                               Serialization::PipelineKeyVersion};
    constexpr size_t header_size = sizeof(cache_versions);
    const auto save_profile = [&] {
        std::vector<u8> current(header_size + sizeof(profile));
        std::memcpy(current.data(), cache_versions.data(), header_size);
        std::memcpy(current.data() + header_size, &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(current));
    };
    // Check both compiler/cache versions and the device profile.
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        // A missing profile also means any remaining blobs have unknown compatibility.
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();

        save_profile();
        return;
    }
    const bool versions_match = profile_data.size() == header_size + sizeof(Shader::Profile) &&
        std::memcmp(profile_data.data(), cache_versions.data(), header_size) == 0;
    Shader::Profile cached_profile{};
    if (versions_match) {
        std::memcpy(&cached_profile, profile_data.data() + header_size, sizeof(cached_profile));
    }
    if (!versions_match || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current compiler/system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        save_profile();
        return;
    }

    // The index of every cached permutation, loaded below or not (NewPermutationIndex).
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::ShaderMeta, [&](std::vector<u8>&& data) {
            try {
                Serialization::Archive ar{std::move(data)};
                Program program{};
                Shader::StageSpecialization spec{};
                spec.info = &program.info;
                std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
                size_t perm_idx = SIZE_MAX;
                LoadShaderMeta(ar, program.info, fetch_shader, spec, perm_idx);
                if (perm_idx != SIZE_MAX) {
                    auto& next = cached_permutations[program.info.pgm_hash];
                    next = std::max(next, perm_idx + 1);
                }
            } catch (const std::exception&) {
                // A damaged entry is found and handled with the pipeline keys below.
            }
        });

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_damaged{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            // bbport: a damaged entry (cut short by a crash or a power loss, or rejected by the
            // driver) used to stop the game at every start until the cache was deleted by hand
            // (issues #28, #38, #39). It is counted here and the cache is rebuilt below.
            try {
                Serialization::Archive ar{std::move(data)};
                Serialization::Reader pldata{ar};

                u32 version{};
                pldata.Read(version);
                if (version != Serialization::PipelineKeyVersion) {
                    return;
                }

                u32 is_compute{};
                pldata.Read(is_compute);

                bool result{};
                if (is_compute) {
                    result = LoadComputePipeline(ar);
                } else {
                    result = LoadGraphicsPipeline(ar);
                }

                if (result) {
                    ++num_pipelines;
                }
            } catch (const std::exception& e) {
                if (num_damaged++ == 0) {
                    LOG_WARNING(Render, "Pipeline cache: damaged entry ({})", e.what());
                }
                sel.infos.fill(nullptr);
                sel.modules.fill({});
                sel.fetch_shader.reset();
            }
        });

    if (num_damaged) {
        // Nothing preloaded is trusted: modules of a damaged entry may sit in programs that later
        // lookups would reuse. Start as with no cache and write a fresh one.
        LOG_WARNING(Render, "Pipeline cache: {} damaged entries, rebuilding it", num_damaged);
        graphics_pipelines.clear();
        compute_pipelines.clear();
        std::unordered_set<VideoCore::Backend::ShaderHandle> modules;
        for (const auto& [_, program] : program_cache) {
            if (!program) {
                continue;
            }
            for (const auto& permutation : program->modules) {
                if (permutation.module) {
                    modules.insert(permutation.module);
                }
            }
        }
        for (const auto module : modules) {
            ReleaseShader(module);
        }
        program_cache.clear();
        cached_permutations.clear();
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        save_profile();
        return;
    }

    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        // Bloodborne-Android bf1e6cf: the cached walker may be an SRT program.
        srt.Write(static_cast<const u8*>(SrtWalkerCode(walker_func)), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        walker_func = RegisterWalkerCode(code, walker_func_size);
        if (!walker_func) return false;
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
