// SPDX-License-Identifier: GPL-3.0-or-later

// Times vkCreateGraphicsPipelines for real vertex+fragment SPIR-V pairs with and without
// VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT. Each <dir>/NNNN holds vs.spv and fs.spv:
//
//   python3 tools/nx2/pipeline_compile_bench_extract.py <cache>/vulkan.fork-vNN.bin <dir>
//   for each <dir>/NNNN, with N = the stage file:
//     shader-sweep --environment N.env --cache-version 52 --pipeline-key key.bin --output N.spv
//
// Descriptor layouts, vertex inputs and colour formats come from a minimal reflection of the
// modules, and the SPIR-V comes from shader-sweep's offline profile, so the shader code matches
// the emulator's pipelines but the fixed state does not.
//
//   c++ -O2 -std=c++20 pipeline_compile_bench.cpp -lvulkan -o pipeline_compile_bench
//   __GL_SHADER_DISK_CACHE=0 ./pipeline_compile_bench <dir> [--order opt-first|fast-first]
//       [--only opt|fast] [--salt N]
//
// Each build salts the SPIR-V header so no driver cache can serve it; --salt N fixes the salts
// (opt and fast then share one) to test whether the driver's disk cache keys on the flag.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        const VkResult r_ = (expr);                                                                \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "%s failed: %d\n", #expr, r_);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

std::vector<uint32_t> ReadWords(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> words(size / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return words;
}

enum class Base { Float, Int, Uint };

struct Reflection {
    struct Binding {
        uint32_t set, binding, count;
        VkDescriptorType type;
    };
    std::vector<Binding> bindings;
    std::map<uint32_t, Base> inputs;  ///< location -> component type
    std::map<uint32_t, Base> outputs; ///< location -> component type
    bool has_push_constants{};
    bool unsupported{};
};

Reflection Reflect(const std::vector<uint32_t>& code) {
    Reflection result;
    struct Type {
        uint32_t op{};
        std::vector<uint32_t> operands;
    };
    std::map<uint32_t, Type> types;
    std::map<uint32_t, uint32_t> constants;
    std::map<uint32_t, uint32_t> set, binding, location;
    std::map<uint32_t, bool> block, buffer_block, builtin;
    struct Variable {
        uint32_t id, pointer_type, storage;
    };
    std::vector<Variable> variables;
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xffff;
        const uint32_t count = code[i] >> 16;
        if (count == 0) {
            break;
        }
        const uint32_t* w = &code[i + 1];
        switch (op) {
        case 71: // OpDecorate
            if (w[1] == 34) set[w[0]] = w[2];
            if (w[1] == 33) binding[w[0]] = w[2];
            if (w[1] == 30) location[w[0]] = w[2];
            if (w[1] == 2) block[w[0]] = true;
            if (w[1] == 3) buffer_block[w[0]] = true;
            if (w[1] == 11) builtin[w[0]] = true;
            break;
        case 20: case 21: case 22: case 23: case 25: case 26: case 27: case 28: case 29: case 30:
        case 32:
            types[w[0]] = Type{op, std::vector<uint32_t>(w + 1, w + count - 1)};
            break;
        case 43: // OpConstant
            constants[w[1]] = w[2];
            break;
        case 59: // OpVariable
            variables.push_back({w[1], w[0], w[2]});
            break;
        default:
            break;
        }
        i += count;
    }
    const auto base_of = [&](uint32_t id) {
        while (types[id].op == 23 || types[id].op == 28) {
            id = types[id].operands[0];
        }
        const Type& t = types[id];
        if (t.op == 22) return Base::Float;
        if (t.op == 21) return t.operands[1] ? Base::Int : Base::Uint;
        return Base::Float;
    };
    for (const Variable& var : variables) {
        const Type& ptr = types[var.pointer_type];
        uint32_t pointee = ptr.operands.size() > 1 ? ptr.operands[1] : 0;
        if (var.storage == 9) {
            result.has_push_constants = true;
            continue;
        }
        if (var.storage == 1 || var.storage == 3) {
            if (builtin[var.id] || !location.contains(var.id)) {
                continue;
            }
            uint32_t slots = 1;
            uint32_t elem = pointee;
            if (types[elem].op == 28) {
                slots = constants[types[elem].operands[1]];
                elem = types[elem].operands[0];
            }
            for (uint32_t s = 0; s < slots; ++s) {
                (var.storage == 1 ? result.inputs : result.outputs)[location[var.id] + s] =
                    base_of(elem);
            }
            continue;
        }
        if (var.storage != 0 && var.storage != 2 && var.storage != 12) {
            continue;
        }
        uint32_t count = 1;
        while (types[pointee].op == 28 || types[pointee].op == 29) {
            if (types[pointee].op == 29) {
                result.unsupported = true;
                break;
            }
            count *= constants[types[pointee].operands[1]];
            pointee = types[pointee].operands[0];
        }
        VkDescriptorType type{VK_DESCRIPTOR_TYPE_MAX_ENUM};
        if (var.storage == 12 || (var.storage == 2 && buffer_block[pointee])) {
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else if (var.storage == 2) {
            type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else {
            const Type& t = types[pointee];
            if (t.op == 27) {
                type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else if (t.op == 26) {
                type = VK_DESCRIPTOR_TYPE_SAMPLER;
            } else if (t.op == 25) {
                const uint32_t dim = t.operands[1];
                const uint32_t sampled = t.operands[5];
                if (dim == 6) {
                    result.unsupported = true;
                } else if (dim == 5) {
                    type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                        : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                } else {
                    type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                        : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                }
            }
        }
        if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            continue;
        }
        result.bindings.push_back({set[var.id], binding[var.id], count, type});
    }
    return result;
}

VkFormat AttributeFormat(Base base) {
    switch (base) {
    case Base::Int: return VK_FORMAT_R32G32B32A32_SINT;
    case Base::Uint: return VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

VkFormat ColorFormat(Base base) {
    switch (base) {
    case Base::Int: return VK_FORMAT_R32G32B32A32_SINT;
    case Base::Uint: return VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
}

struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    bool stats{};
};

bool print_stats = false;

Context CreateContext() {
    Context ctx;
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "pipeline_compile_bench",
                                1, nullptr, 0, VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0,
                                   nullptr, 0, nullptr};
    CHECK(vkCreateInstance(&ici, nullptr, &ctx.instance));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(ctx.instance, &count, devices.data());
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.vendorID == 0x10de || !ctx.physical) {
            ctx.physical = pd;
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx.physical, &props);
    std::printf("device: %s, driver %u.%u\n", props.deviceName, props.driverVersion >> 22,
                (props.driverVersion >> 14) & 0xff);

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available(ext_count);
    vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &ext_count, available.data());
    const char* wanted[] = {
        "VK_EXT_shader_viewport_index_layer", "VK_EXT_shader_stencil_export",
        "VK_NV_viewport_array2",              "VK_NV_geometry_shader_passthrough",
        "VK_EXT_transform_feedback",          "VK_KHR_fragment_shader_barycentric",
        "VK_EXT_fragment_shader_interlock",   "VK_EXT_shader_atomic_float",
        "VK_EXT_robustness2",                 "VK_KHR_pipeline_executable_properties",
    };
    std::vector<const char*> enabled;
    for (const char* name : wanted) {
        for (const auto& ext : available) {
            if (std::strcmp(ext.extensionName, name) == 0) {
                enabled.push_back(name);
            }
        }
    }
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &executable_props};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         &robustness2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(ctx.physical, &features);
    // As the emulator: no robustBufferAccess2/robustImageAccess2, only null descriptors.
    robustness2.robustBufferAccess2 = VK_FALSE;
    robustness2.robustImageAccess2 = VK_FALSE;
    if (std::find_if(enabled.begin(), enabled.end(), [](const char* n) {
            return std::strcmp(n, "VK_EXT_robustness2") == 0;
        }) == enabled.end()) {
        f13.pNext = nullptr;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1,
                                      &priority};
    const VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 &features,
                                 0,
                                 1,
                                 &qci,
                                 0,
                                 nullptr,
                                 static_cast<uint32_t>(enabled.size()),
                                 enabled.data(),
                                 nullptr};
    CHECK(vkCreateDevice(ctx.physical, &dci, nullptr, &ctx.device));
    ctx.stats = executable_props.pipelineExecutableInfo == VK_TRUE;
    return ctx;
}

struct Pipeline {
    std::string name;
    std::vector<uint32_t> vs, fs;
    Reflection vr, fr;
};

/// Builds one pipeline from salted copies of the modules (a distinct word in the SPIR-V
/// header's generator field), so no in-process or on-disk driver cache can serve it.
double Build(const Context& ctx, const Pipeline& p, VkPipelineCreateFlags flags, uint32_t salt,
             bool* ok) {
    VkDevice dev = ctx.device;
    std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> sets;
    const auto add = [&](const Reflection& r, VkShaderStageFlags stage) {
        for (const auto& b : r.bindings) {
            auto [it, inserted] = sets[b.set].try_emplace(
                b.binding, VkDescriptorSetLayoutBinding{b.binding, b.type, b.count, stage, nullptr});
            if (!inserted) {
                it->second.stageFlags |= stage;
            }
        }
    };
    add(p.vr, VK_SHADER_STAGE_VERTEX_BIT);
    add(p.fr, VK_SHADER_STAGE_FRAGMENT_BIT);
    uint32_t num_sets = sets.empty() ? 0 : sets.rbegin()->first + 1;
    std::vector<VkDescriptorSetLayout> layouts(num_sets);
    for (uint32_t s = 0; s < num_sets; ++s) {
        std::vector<VkDescriptorSetLayoutBinding> list;
        for (auto& [_, b] : sets[s]) {
            list.push_back(b);
        }
        const VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 nullptr, 0, static_cast<uint32_t>(list.size()),
                                                 list.data()};
        CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &layouts[s]));
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
    const VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
                                         num_sets, layouts.data(), 1, &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &layout));

    std::vector<uint32_t> vs = p.vs, fs = p.fs;
    vs[2] = fs[2] = 0x7fff0000u ^ salt;
    VkShaderModule modules[2];
    const VkShaderModuleCreateInfo vci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                       vs.size() * 4, vs.data()};
    const VkShaderModuleCreateInfo fci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                       fs.size() * 4, fs.data()};
    CHECK(vkCreateShaderModule(dev, &vci, nullptr, &modules[0]));
    CHECK(vkCreateShaderModule(dev, &fci, nullptr, &modules[1]));
    const VkPipelineShaderStageCreateInfo stages[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         modules[0], "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", nullptr},
    };
    std::vector<VkVertexInputBindingDescription> vbind;
    std::vector<VkVertexInputAttributeDescription> vattr;
    for (const auto& [loc, base] : p.vr.inputs) {
        const uint32_t index = static_cast<uint32_t>(vbind.size());
        vbind.push_back({index, 16, VK_VERTEX_INPUT_RATE_VERTEX});
        vattr.push_back({loc, index, AttributeFormat(base), 0});
    }
    const VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,
        static_cast<uint32_t>(vbind.size()),
        vbind.data(),
        static_cast<uint32_t>(vattr.size()),
        vattr.data()};
    const VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    const VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                               nullptr, 0, 1, nullptr, 1, nullptr};
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    const uint32_t num_colors = p.fr.outputs.empty() ? 0 : p.fr.outputs.rbegin()->first + 1;
    std::vector<VkFormat> formats(num_colors, VK_FORMAT_UNDEFINED);
    for (const auto& [loc, base] : p.fr.outputs) {
        formats[loc] = ColorFormat(base);
    }
    std::vector<VkPipelineColorBlendAttachmentState> blend(num_colors);
    for (auto& b : blend) {
        b.colorWriteMask = 0xf;
    }
    const VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE,
        VK_LOGIC_OP_COPY, num_colors, blend.data(), {}};
    const VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               nullptr, 0, 2, dyn_states};
    const VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                  nullptr,
                                                  0,
                                                  num_colors,
                                                  formats.data(),
                                                  VK_FORMAT_D32_SFLOAT,
                                                  VK_FORMAT_UNDEFINED};
    if (print_stats && ctx.stats) {
        flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    }
    const VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                           &rendering,
                                           flags,
                                           2,
                                           stages,
                                           &vertex,
                                           &ia,
                                           nullptr,
                                           &vp,
                                           &rs,
                                           &ms,
                                           &ds,
                                           &cb,
                                           &dyn,
                                           layout,
                                           VK_NULL_HANDLE,
                                           0,
                                           VK_NULL_HANDLE,
                                           0};
    VkPipeline pipeline{};
    const auto begin = std::chrono::steady_clock::now();
    const VkResult result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, nullptr, &pipeline);
    const double ms_taken =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    *ok = result == VK_SUCCESS;
    if (pipeline && print_stats && ctx.stats) {
        const auto get_props = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
            vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR"));
        const auto get_stats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
            vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR"));
        const VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pipeline};
        uint32_t count = 0;
        get_props(dev, &info, &count, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> props(
            count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        get_props(dev, &info, &count, props.data());
        for (uint32_t e = 0; e < count; ++e) {
            const VkPipelineExecutableInfoKHR exe{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
                                                  nullptr, pipeline, e};
            uint32_t n = 0;
            get_stats(dev, &exe, &n, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> st(
                n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            get_stats(dev, &exe, &n, st.data());
            std::printf("  [%s] %s:", flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT ? "fast" : "opt",
                        props[e].name);
            for (const auto& s : st) {
                if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) {
                    std::printf(" %s=%llu", s.name, static_cast<unsigned long long>(s.value.u64));
                }
            }
            std::printf("\n");
        }
    }
    if (pipeline) {
        vkDestroyPipeline(dev, pipeline, nullptr);
    }
    vkDestroyShaderModule(dev, modules[0], nullptr);
    vkDestroyShaderModule(dev, modules[1], nullptr);
    vkDestroyPipelineLayout(dev, layout, nullptr);
    for (VkDescriptorSetLayout l : layouts) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    return ms_taken;
}

void Summary(const char* label, std::vector<double> times) {
    if (times.empty()) {
        return;
    }
    std::sort(times.begin(), times.end());
    double total = 0;
    for (double t : times) {
        total += t;
    }
    const auto pct = [&](double p) { return times[static_cast<size_t>(p * (times.size() - 1))]; };
    std::printf("%-6s n=%zu total %.0f ms  p50 %.1f  p90 %.1f  p99 %.1f  max %.1f ms\n", label,
                times.size(), total, pct(0.5), pct(0.9), pct(0.99), times.back());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dir> [--order opt-first|fast-first] [--only opt|fast] "
                             "[--salt N]\n", argv[0]);
        return 2;
    }
    bool fast_first = false;
    std::string only;
    uint32_t salt_base = static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    bool fixed_salt = false;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--order") fast_first = std::string(argv[i + 1]) == "fast-first";
        if (arg == "--only") only = argv[i + 1];
        if (arg == "--stats") print_stats = std::string(argv[i + 1]) == "1";
        if (arg == "--salt") {
            salt_base = static_cast<uint32_t>(std::stoul(argv[i + 1]));
            fixed_salt = true;
        }
    }
    std::vector<Pipeline> pipelines;
    std::vector<std::filesystem::path> dirs;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
        dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        Pipeline p{dir.filename().string(), ReadWords(dir / "vs.spv"), ReadWords(dir / "fs.spv")};
        if (p.vs.size() < 5 || p.fs.size() < 5) {
            continue;
        }
        p.vr = Reflect(p.vs);
        p.fr = Reflect(p.fs);
        if (p.vr.unsupported || p.fr.unsupported) {
            continue;
        }
        pipelines.push_back(std::move(p));
    }
    const Context ctx = CreateContext();
    std::vector<double> opt, fast;
    size_t failed = 0;
    for (size_t i = 0; i < pipelines.size(); ++i) {
        const uint32_t salt = salt_base + static_cast<uint32_t>(i) * 2;
        double t_opt = -1, t_fast = -1;
        bool ok_opt = true, ok_fast = true;
        const auto run_opt = [&] {
            t_opt = Build(ctx, pipelines[i], 0, fixed_salt ? salt : salt, &ok_opt);
        };
        const auto run_fast = [&] {
            t_fast = Build(ctx, pipelines[i], VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT,
                           fixed_salt ? salt : salt + 1, &ok_fast);
        };
        if (only != "fast" && !fast_first) run_opt();
        if (only != "opt") run_fast();
        if (only != "fast" && fast_first) run_opt();
        if (!ok_opt || !ok_fast) {
            ++failed;
            continue;
        }
        if (t_opt >= 0) opt.push_back(t_opt);
        if (t_fast >= 0) fast.push_back(t_fast);
        std::printf("%s opt %.1f fast %.1f\n", pipelines[i].name.c_str(), t_opt, t_fast);
    }
    std::printf("%zu pipelines, %zu failed to build\n", pipelines.size(), failed);
    Summary("opt", opt);
    Summary("fast", fast);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
    return 0;
}
