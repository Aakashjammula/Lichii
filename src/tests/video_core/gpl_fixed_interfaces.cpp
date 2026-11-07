// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/renderer_vulkan/pipeline_shader_emission.h"

namespace {
constexpr size_t VERTEX_SLOT = 0;
constexpr size_t FRAGMENT_SLOT = 4;

using Shader::IR::Attribute;

Attribute Generic(u32 index, u32 element = 0) {
    return Attribute::Generic0X + index * 4 + element;
}

struct Programs {
    Shader::ObjectPool<Shader::IR::Inst> instructions;
    Shader::ObjectPool<Shader::IR::Block> blocks;

    Shader::IR::Program Begin(Shader::Stage stage, Shader::IR::Block*& block) {
        Shader::IR::Program program;
        block = blocks.Create(instructions);
        program.stage = stage;
        program.blocks = {block};
        program.post_order_blocks = {block};
        program.syntax_list = {
            {{.block = block}, Shader::IR::AbstractSyntaxNode::Type::Block},
            {{}, Shader::IR::AbstractSyntaxNode::Type::Return},
        };
        return program;
    }

    // A vertex stage writing x of each listed generic (or legacy attribute).
    Shader::IR::Program Vertex(std::initializer_list<Attribute> stores) {
        Shader::IR::Block* block;
        auto program = Begin(Shader::Stage::VertexB, block);
        Shader::IR::IREmitter ir{*block};
        for (const Attribute attribute : stores) {
            ir.SetAttribute(attribute, ir.Imm32(0.5f), ir.Imm32(0));
            program.info.stores.Set(attribute, true);
        }
        return program;
    }

    // A fragment stage loading x of each listed attribute into colour 0.
    Shader::IR::Program Fragment(
        std::initializer_list<std::pair<Attribute, Shader::Interpolation>> loads) {
        Shader::IR::Block* block;
        auto program = Begin(Shader::Stage::Fragment, block);
        Shader::IR::IREmitter ir{*block};
        u32 component = 0;
        for (const auto& [attribute, interpolation] : loads) {
            ir.SetFragColor(0, component++ % 4, ir.GetAttribute(attribute));
            program.info.loads.Set(attribute, true);
            if (Shader::IR::IsGeneric(attribute)) {
                program.info.interpolation[Shader::IR::GenericAttributeIndex(attribute)] =
                    interpolation;
            }
        }
        program.info.stores_frag_color[0] = true;
        return program;
    }
};

Shader::Profile MakeProfile() {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    return profile;
}

// Emits vertex then fragment the way CreateGraphicsPipeline does, with stage bindings on.
std::array<std::vector<u32>, 2> EmitPipeline(Shader::IR::Program& vertex,
                                             Shader::IR::Program& fragment, bool fixed) {
    const Shader::Profile profile = MakeProfile();
    Shader::Backend::Bindings binding;
    std::array<std::vector<u32>, 2> result;

    Shader::RuntimeInfo vertex_info;
    vertex_info.previous_stage_stores.mask.set();
    if (fixed) {
        REQUIRE(Shader::Maxwell::UseFixedStageInterface(vertex, vertex_info));
    }
    Shader::Maxwell::ConvertLegacyToGeneric(vertex, vertex_info);
    Vulkan::BeginGraphicsStageBindings(binding, VERTEX_SLOT);
    result[0] = Vulkan::EmitPipelineShader(profile, vertex_info, vertex, binding, 0);

    Shader::RuntimeInfo fragment_info;
    fragment_info.previous_stage_stores = vertex.info.stores;
    fragment_info.previous_stage_legacy_stores_mapping = vertex.info.legacy_stores_mapping;
    if (fixed) {
        Shader::Maxwell::UseFixedStageInterface(fragment, fragment_info);
    }
    Shader::Maxwell::ConvertLegacyToGeneric(fragment, fragment_info);
    Vulkan::BeginGraphicsStageBindings(binding, FRAGMENT_SLOT);
    result[1] = Vulkan::EmitPipelineShader(profile, fragment_info, fragment, binding, 1);
    return result;
}

struct Interface {
    // Location -> interpolation decorations (Flat 14, NoPerspective 13) of user varyings.
    std::map<u32, std::set<u32>> inputs;
    std::map<u32, std::set<u32>> outputs;
};

Interface ReadInterface(const std::vector<u32>& spirv) {
    constexpr u32 OP_DECORATE = 71, OP_VARIABLE = 59;
    constexpr u32 DECORATION_LOCATION = 30, DECORATION_FLAT = 14, DECORATION_NO_PERSPECTIVE = 13;
    constexpr u32 STORAGE_INPUT = 1, STORAGE_OUTPUT = 3;
    std::map<u32, u32> storage, location;
    std::map<u32, std::set<u32>> interpolation;
    for (size_t word = 5; word < spirv.size();) {
        const u32 count = spirv[word] >> 16;
        const u32 opcode = spirv[word] & 0xffff;
        if (opcode == OP_DECORATE && count >= 3) {
            const u32 target = spirv[word + 1], decoration = spirv[word + 2];
            if (decoration == DECORATION_LOCATION) {
                location[target] = spirv[word + 3];
            } else if (decoration == DECORATION_FLAT || decoration == DECORATION_NO_PERSPECTIVE) {
                interpolation[target].insert(decoration);
            }
        } else if (opcode == OP_VARIABLE && count >= 4) {
            storage[spirv[word + 2]] = spirv[word + 3];
        }
        word += std::max(count, 1u);
    }
    Interface result;
    for (const auto& [id, loc] : location) {
        if (storage[id] == STORAGE_INPUT) {
            result.inputs[loc] = interpolation[id];
        } else if (storage[id] == STORAGE_OUTPUT) {
            result.outputs[loc] = interpolation[id];
        }
    }
    return result;
}
} // namespace

TEST_CASE("Fixed interfaces make the fragment SPIR-V independent of the vertex shader",
          "[video_core][spirv][gpl]") {
    using Shader::Interpolation;
    Programs pool;
    const auto fragment = [&pool] {
        return pool.Fragment({{Generic(0), Interpolation::Smooth},
                              {Generic(1), Interpolation::Flat},
                              {Generic(5), Interpolation::NoPerspective}});
    };
    auto vertex_a = pool.Vertex({Attribute::PositionX, Generic(0), Generic(1)});
    auto vertex_b = pool.Vertex({Attribute::PositionX, Generic(0), Generic(5), Generic(7)});

    auto fragment_a = fragment();
    auto fragment_b = fragment();
    const auto linked_a = EmitPipeline(vertex_a, fragment_a, false);
    const auto linked_b = EmitPipeline(vertex_b, fragment_b, false);
    // The linked interface depends on what the vertex shader writes.
    REQUIRE(linked_a[1] != linked_b[1]);

    auto vertex_c = pool.Vertex({Attribute::PositionX, Generic(0), Generic(1)});
    auto vertex_d = pool.Vertex({Attribute::PositionX, Generic(0), Generic(5), Generic(7)});
    auto fragment_c = fragment();
    auto fragment_d = fragment();
    const auto fixed_c = EmitPipeline(vertex_c, fragment_c, true);
    const auto fixed_d = EmitPipeline(vertex_d, fragment_d, true);
    REQUIRE(fixed_c[1] == fixed_d[1]);
}

TEST_CASE("Fixed interfaces keep fragment locations and interpolation",
          "[video_core][spirv][gpl]") {
    using Shader::Interpolation;
    Programs pool;
    const auto fragment = [&pool] {
        return pool.Fragment({{Generic(0), Interpolation::Smooth},
                              {Generic(1), Interpolation::Flat},
                              {Generic(5), Interpolation::NoPerspective}});
    };
    // With a vertex shader writing every input the linked and fixed fragment interfaces agree.
    auto vertex_linked = pool.Vertex({Generic(0), Generic(1), Generic(5)});
    auto vertex_fixed = pool.Vertex({Generic(0), Generic(1), Generic(5)});
    auto fragment_linked = fragment();
    auto fragment_fixed = fragment();
    const auto linked = EmitPipeline(vertex_linked, fragment_linked, false);
    const auto fixed = EmitPipeline(vertex_fixed, fragment_fixed, true);
    REQUIRE(linked[1] == fixed[1]);

    const Interface fragment_interface = ReadInterface(fixed[1]);
    const std::map<u32, std::set<u32>> expected{{0, {}}, {1, {14}}, {5, {13}}};
    REQUIRE(fragment_interface.inputs == expected);

    // The linked vertex stage declares what it writes; the fixed one every generic location,
    // all without interpolation decorations, so each fragment input has a matching output.
    const Interface linked_vertex = ReadInterface(linked[0]);
    const Interface fixed_vertex = ReadInterface(fixed[0]);
    REQUIRE(linked_vertex.outputs.size() == 3);
    REQUIRE(fixed_vertex.outputs.size() == Shader::IR::NUM_GENERICS);
    for (const auto& [location, decorations] : fixed_vertex.outputs) {
        REQUIRE(location < Shader::IR::NUM_GENERICS);
        REQUIRE(decorations.empty());
    }
    for (const auto& [location, decorations] : fragment_interface.inputs) {
        REQUIRE(fixed_vertex.outputs.contains(location));
    }
}

TEST_CASE("The fixed vertex interface does not depend on the fragment shader",
          "[video_core][spirv][gpl]") {
    using Shader::Interpolation;
    Programs pool;
    auto vertex_a = pool.Vertex({Attribute::PositionX, Generic(2)});
    auto vertex_b = pool.Vertex({Attribute::PositionX, Generic(2)});
    auto fragment_a = pool.Fragment({{Generic(2), Interpolation::Smooth}});
    auto fragment_b = pool.Fragment({{Generic(9), Interpolation::Flat}});
    const auto fixed_a = EmitPipeline(vertex_a, fragment_a, true);
    const auto fixed_b = EmitPipeline(vertex_b, fragment_b, true);
    REQUIRE(fixed_a[0] == fixed_b[0]);
}

TEST_CASE("Fragment stages loading legacy or indexed varyings keep the linked interface",
          "[video_core][gpl]") {
    using Shader::Interpolation;
    Programs pool;
    auto legacy = pool.Fragment({{Attribute::ColorFrontDiffuseR, Interpolation::Smooth}});
    Shader::RuntimeInfo info;
    info.previous_stage_stores.Set(Generic(3), true);
    info.previous_stage_legacy_stores_mapping[Attribute::ColorFrontDiffuseR] = Generic(3);
    REQUIRE_FALSE(Shader::Maxwell::UseFixedStageInterface(legacy, info));
    REQUIRE(info.previous_stage_stores.Generic(3));
    REQUIRE_FALSE(info.previous_stage_stores.Generic(4));
    REQUIRE(info.previous_stage_legacy_stores_mapping.size() == 1);

    auto indexed = pool.Fragment({{Generic(0), Interpolation::Smooth}});
    indexed.info.loads_indexed_attributes = true;
    REQUIRE_FALSE(Shader::Maxwell::UseFixedStageInterface(indexed, info));

    auto plain = pool.Fragment({{Generic(0), Interpolation::Smooth}});
    REQUIRE(Shader::Maxwell::UseFixedStageInterface(plain, info));
    REQUIRE(info.previous_stage_stores.Generic(4));
    REQUIRE(info.previous_stage_legacy_stores_mapping.empty());
}
