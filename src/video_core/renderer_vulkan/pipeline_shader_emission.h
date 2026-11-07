// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <span>
#include <vector>

#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace Vulkan {

// EDEN_NX2_GPL_STAGE_BINDINGS=1: each graphics stage numbers its descriptor bindings from
// stage slot * GRAPHICS_STAGE_BINDING_STRIDE instead of continuing after the previous stage, so a
// stage's SPIR-V does not depend on the other stages of its pipeline (a prerequisite for graphics
// pipeline libraries, see docs/nx2/gpl-feasibility.md). All stages stay in set 0.
inline constexpr u32 GRAPHICS_STAGE_BINDING_STRIDE = 128;

[[nodiscard]] inline bool GraphicsStageBindingsRequested() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_GPL_STAGE_BINDINGS");
        return value && value[0] == '1';
    }();
    return enabled;
}

// Binding numbers a stage takes with unified numbering: an upper bound of what both the SPIR-V
// emitter and the descriptor layout advance by.
[[nodiscard]] inline u32 NumStageBindings(const Shader::Info& info) {
    u32 count = static_cast<u32>(
        info.texture_buffer_descriptors.size() + info.image_buffer_descriptors.size() +
        info.texture_descriptors.size() + info.image_descriptors.size());
    for (const auto& desc : info.constant_buffer_descriptors) {
        count += std::max(desc.count, 1u);
    }
    for (const auto& desc : info.storage_buffers_descriptors) {
        count += std::max(desc.count, 1u);
    }
    return count;
}

// A pipeline with a stage larger than its range keeps the carried-over numbering.
[[nodiscard]] inline bool GraphicsStageBindingsFit(std::span<const Shader::Info* const> infos) {
    return std::ranges::all_of(infos, [](const Shader::Info* info) {
        return info == nullptr || NumStageBindings(*info) <= GRAPHICS_STAGE_BINDING_STRIDE;
    });
}

[[nodiscard]] inline u32 GraphicsStageFirstBinding(size_t stage_index) {
    return static_cast<u32>(stage_index) * GRAPHICS_STAGE_BINDING_STRIDE;
}

// Rescaling indices keep accumulating: they index the shared push-constant words, not bindings.
inline void BeginGraphicsStageBindings(Shader::Backend::Bindings& binding, size_t stage_index) {
    const u32 base = GraphicsStageFirstBinding(stage_index);
    binding.unified = base;
    binding.uniform_buffer = base;
    binding.storage_buffer = base;
    binding.texture = base;
    binding.image = base;
}

inline std::vector<u32> EmitPipelineShader(const Shader::Profile& profile,
                                           const Shader::RuntimeInfo& runtime_info,
                                           Shader::IR::Program& program,
                                           Shader::Backend::Bindings& binding, u64 shader_hash,
                                           std::span<const u32> reference = {}) {
    std::vector<u32> code{
        Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding)};
    if (!reference.empty() && !std::ranges::equal(code, reference)) {
        LOG_ERROR(Render_Vulkan,
                  "Nx2Perf async_pipelines: shader {:016x} worker SPIR-V differs; "
                  "using GPU-thread SPIR-V",
                  shader_hash);
        code.assign(reference.begin(), reference.end());
    }
    return code;
}

} // namespace Vulkan
