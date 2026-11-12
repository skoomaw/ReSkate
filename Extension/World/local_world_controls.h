#pragma once
#include "Engine/Core/Log/logging.h"
#include "Extension/Profile/runtime_internal.h"

namespace dingosdk::profile_runtime {
struct EnvironmentChange { std::uint32_t id{}, padding{}; const void* value{}; };

static_assert(sizeof(EnvironmentChange) == 16);

struct EnvironmentField {
    unsigned kind; std::uint32_t hash; unsigned cache;
    int mask_offset; std::uint32_t mask_bit; unsigned mask_group;
    bool boolean;
};

inline constexpr std::array<EnvironmentField, 14> environment_fields{{
    {0,0x8ba423e4,0x26b,0x198,1,0,true},       // Fog.Enable
    {0,0x7f3a9c6d,0x1d4,0x198,0x80000000,0,false}, // HeightFogVisibilityRange
    {1,0x9aff5c27,0x190,0x1f0,0x40,0,false},  // LuminanceScale
    {1,0x717edc66,0x200,0x290,0x200,1,false},  // CloudLayer1AlphaMul
    {1,0x6e2608e5,0x2a4,0x290,0x800000,1,false},
    {2,0xe0a01ad4,0x5c,0x62,4,0,false},       // WindStrength
    {2,0xbb9fa0d4,0x40,0x62,1,0,false},        // WindDirection
    {2,0x5543330b,0x58,0x62,8,0,false},        // WindVariationMultiplier
    {0,0x9d5b5d74,0x188,0x1dc,1,1,false},   // HeightFogStart
    {0,0x87c8f1fb,0x18c,0x1dc,2,1,false},   // HeightFogEnd
    {0,0x0dc0efa5,0x1bc,0x198,8,0,false},   // Gradient Start
    {0,0x0b87a32a,0x1d0,0x198,0x10,0,false},// Gradient End
    {1,0xe5a3a4c5,0x210,0x1e0,0x20000,2,false}, // Physical sky Light1Intensity
    {1,0x3004c626,0x1a8,0x1e0,0x1000000,2,false},// Physical sky Light2Intensity
}};

struct AtmosphereOwnership {
    bool original_flag{};
    std::array<std::byte,80> original{}, applied{};
    std::shared_ptr<std::uintptr_t> texture;
};
struct EnvironmentNode {
    std::uintptr_t component{}, data{}, parent{};
    unsigned kind{};
    std::map<unsigned, AtmosphereOwnership> atmosphere;
};

struct WorldControlRuntime {
    using Construct = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
    using Destroy = std::uintptr_t (*)(std::uintptr_t, unsigned);
    using Property = void (*)(std::uintptr_t, const EnvironmentChange*);
    std::array<Construct, 3> construct{};
    std::array<Destroy, 3> destroy{};
    std::array<Property, 3> property{};
    std::atomic<bool> environment_active{}, population_active{};
    std::recursive_mutex mutex;
    WorldControlsModel model;
    std::map<std::uintptr_t, EnvironmentNode> nodes;
    WorldMap map{WorldMap::none};
    DWORD thread{};
    std::uint64_t last_update{};
    std::uint64_t diagnostic_due{};
    std::uint64_t fog_report_due{}; // the fog test: when the fog components are next said in the log
    unsigned fog_reports{};
    std::map<unsigned, std::map<std::string, std::shared_ptr<std::uintptr_t>, std::less<>>> textures;
};

WorldControlRuntime& world_control_runtime();

bool environment_identity(const EnvironmentNode& n);

template<unsigned Kind> std::uintptr_t environment_construct(std::uintptr_t factory, std::uintptr_t info) {
    auto& r = world_control_runtime();
    const auto result = r.construct[Kind](factory, info);
    if (r.environment_active.load(std::memory_order_acquire)) {
        PreserveError preserve;
        try {
            EnvironmentNode n; n.component = result; n.kind = Kind;
            std::uintptr_t collection{};
            if (read(result + 8, n.data) && read(result + 0x18, collection) && read(collection, n.parent) && environment_identity(n)) {
                std::lock_guard lock(r.mutex);
                if (r.nodes.size() < 4096) r.nodes[result] = n;
            }
        } catch (...) { dingosdk::logging::event(dingosdk::logging::Channel::world, "{\"event\":\"world_environment_capture_failed\"}"); }
    }
    return result;
}

template<unsigned Kind> std::uintptr_t environment_destroy(std::uintptr_t component, unsigned flags) {
    auto& r = world_control_runtime();
    { PreserveError preserve; std::lock_guard lock(r.mutex); r.nodes.erase(component); }
    return r.destroy[Kind](component, flags);
}

float environment_choice(unsigned field, const WorldControls& c, float original, const EnvironmentNode& n);

void update_environment_controls(WorldMap map);

void start_world_controls() noexcept;
}
