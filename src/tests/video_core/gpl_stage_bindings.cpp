// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_shader_emission.h"

namespace {
constexpr size_t VERTEX_SLOT = 0;
constexpr size_t FRAGMENT_SLOT = 4;

Shader::TextureDescriptor Texture() {
    return {.type = Shader::TextureType::Color2D,
            .is_depth = false,
            .is_multisample = false,
            .is_integer = false,
            .has_secondary = false,
            .cbuf_index = 0,
            .cbuf_offset = 0,
            .shift_left = 0,
            .secondary_cbuf_index = 0,
            .secondary_cbuf_offset = 0,
            .secondary_shift_left = 0,
            .count = 1,
            .size_shift = 0};
}

struct Programs {
    Shader::ObjectPool<Shader::IR::Inst> instructions;
    Shader::ObjectPool<Shader::IR::Block> blocks;

    // A stage writing one storage word, with the given numbers of constant buffers, storage
    // buffers and textures.
    Shader::IR::Program Make(Shader::Stage stage, u32 cbufs, u32 ssbos, u32 textures) {
        Shader::IR::Program program;
        auto* block = blocks.Create(instructions);
        program.stage = stage;
        program.blocks = {block};
        program.post_order_blocks = {block};
        program.syntax_list = {
            {{.block = block}, Shader::IR::AbstractSyntaxNode::Type::Block},
            {{}, Shader::IR::AbstractSyntaxNode::Type::Return},
        };
        for (u32 index = 0; index < cbufs; ++index) {
            program.info.constant_buffer_descriptors.push_back({index, 1});
        }
        for (u32 index = 0; index < ssbos; ++index) {
            program.info.storage_buffers_descriptors.push_back({0, 0x110 + index * 8, 1, true});
        }
        for (u32 index = 0; index < textures; ++index) {
            program.info.texture_descriptors.push_back(Texture());
        }
        if (ssbos > 0) {
            program.info.used_storage_buffer_types = Shader::IR::Type::U32;
            Shader::IR::IREmitter ir{*block};
            block->AppendNewInst(Shader::IR::Opcode::WriteStorage32,
                                 {ir.Imm32(0), ir.Imm32(0), ir.Imm32(0x12345678)});
        }
        return program;
    }
};

// Emits vertex then fragment the way CreateGraphicsPipeline does.
std::array<std::vector<u32>, 2> EmitPipeline(Shader::IR::Program& vertex,
                                             Shader::IR::Program& fragment, bool stage_bindings) {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    profile.support_vertex_instance_id = false;
    Shader::Backend::Bindings binding;
    std::array<std::vector<u32>, 2> result;
    std::array<Shader::IR::Program*, 2> stages{&vertex, &fragment};
    std::array<size_t, 2> slots{VERTEX_SLOT, FRAGMENT_SLOT};
    for (size_t stage = 0; stage < stages.size(); ++stage) {
        if (stage_bindings) {
            Vulkan::BeginGraphicsStageBindings(binding, slots[stage]);
        }
        result[stage] = Vulkan::EmitPipelineShader(profile, {}, *stages[stage], binding, stage);
    }
    return result;
}

// Values of the Binding decorations in a module.
std::set<u32> DecoratedBindings(const std::vector<u32>& spirv) {
    constexpr u32 OP_DECORATE = 71;
    constexpr u32 DECORATION_BINDING = 33;
    std::set<u32> bindings;
    for (size_t word = 5; word < spirv.size();) {
        const u32 count = spirv[word] >> 16;
        const u32 opcode = spirv[word] & 0xffff;
        if (opcode == OP_DECORATE && count >= 4 && spirv[word + 2] == DECORATION_BINDING) {
            bindings.insert(spirv[word + 3]);
        }
        word += std::max(count, 1u);
    }
    return bindings;
}

std::array<std::set<u32>, 2> LayoutBindings(const Shader::Info& vertex,
                                            const Shader::Info& fragment, bool stage_bindings) {
    Vulkan::DescriptorLayoutBuilder builder;
    std::array<std::set<u32>, 2> result;
    const std::array infos{&vertex, &fragment};
    const std::array flags{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    const std::array slots{VERTEX_SLOT, FRAGMENT_SLOT};
    for (size_t stage = 0; stage < infos.size(); ++stage) {
        if (stage_bindings) {
            builder.StartGraphicsStage(slots[stage]);
        }
        builder.Add(*infos[stage], flags[stage]);
    }
    for (const VkDescriptorSetLayoutBinding& entry : builder.LayoutBindings()) {
        result[entry.stageFlags == VK_SHADER_STAGE_VERTEX_BIT ? 0 : 1].insert(entry.binding);
    }
    for (const VkDescriptorUpdateTemplateEntry& entry : builder.TemplateEntries()) {
        REQUIRE((result[0].contains(entry.dstBinding) || result[1].contains(entry.dstBinding)));
    }
    return result;
}
} // namespace

TEST_CASE("Per-stage bindings make the fragment SPIR-V independent of the vertex shader",
          "[video_core][spirv][gpl]") {
    Programs pool;
    auto small_vertex = pool.Make(Shader::Stage::VertexB, 1, 0, 0);
    auto large_vertex = pool.Make(Shader::Stage::VertexB, 3, 2, 2);
    auto fragment_a = pool.Make(Shader::Stage::Fragment, 2, 1, 3);
    auto fragment_b = pool.Make(Shader::Stage::Fragment, 2, 1, 3);

    const auto carried_small = EmitPipeline(small_vertex, fragment_a, false);
    const auto carried_large = EmitPipeline(large_vertex, fragment_b, false);
    // The old numbering depends on the vertex shader, which is what this switch removes.
    REQUIRE(carried_small[1] != carried_large[1]);

    auto fragment_c = pool.Make(Shader::Stage::Fragment, 2, 1, 3);
    auto fragment_d = pool.Make(Shader::Stage::Fragment, 2, 1, 3);
    const auto staged_small = EmitPipeline(small_vertex, fragment_c, true);
    const auto staged_large = EmitPipeline(large_vertex, fragment_d, true);
    REQUIRE(staged_small[1] == staged_large[1]);
}

TEST_CASE("Per-stage bindings stay in their stage range and match the descriptor layout",
          "[video_core][spirv][gpl]") {
    for (const bool stage_bindings : {false, true}) {
        Programs pool;
        auto vertex = pool.Make(Shader::Stage::VertexB, 3, 2, 2);
        auto fragment = pool.Make(Shader::Stage::Fragment, 2, 1, 3);
        const auto spirv = EmitPipeline(vertex, fragment, stage_bindings);
        const std::array decorated{DecoratedBindings(spirv[0]), DecoratedBindings(spirv[1])};
        const auto layout = LayoutBindings(vertex.info, fragment.info, stage_bindings);

        REQUIRE(decorated[0] == layout[0]);
        REQUIRE(decorated[1] == layout[1]);
        for (const u32 binding : decorated[0]) {
            REQUIRE_FALSE(decorated[1].contains(binding));
        }
        if (stage_bindings) {
            const u32 fragment_base = Vulkan::GraphicsStageFirstBinding(FRAGMENT_SLOT);
            REQUIRE(*decorated[0].begin() == 0);
            REQUIRE(*decorated[0].rbegin() < Vulkan::GRAPHICS_STAGE_BINDING_STRIDE);
            REQUIRE(*decorated[1].begin() == fragment_base);
            REQUIRE(*decorated[1].rbegin() <
                    fragment_base + Vulkan::GRAPHICS_STAGE_BINDING_STRIDE);
        } else {
            REQUIRE(*decorated[1].begin() == *decorated[0].rbegin() + 1);
        }
    }
}

TEST_CASE("A stage with more bindings than its range keeps the carried-over numbering",
          "[video_core][gpl]") {
    Programs pool;
    auto vertex = pool.Make(Shader::Stage::VertexB, 1, 0, 0);
    auto fragment = pool.Make(Shader::Stage::Fragment, 18, 8, 0);
    std::array<const Shader::Info*, 5> infos{&vertex.info, nullptr, nullptr, nullptr,
                                             &fragment.info};
    REQUIRE(Vulkan::NumStageBindings(fragment.info) == 26);
    REQUIRE(Vulkan::GraphicsStageBindingsFit(infos));
    while (Vulkan::NumStageBindings(fragment.info) <= Vulkan::GRAPHICS_STAGE_BINDING_STRIDE) {
        fragment.info.texture_descriptors.push_back(Texture());
    }
    REQUIRE_FALSE(Vulkan::GraphicsStageBindingsFit(infos));
}
