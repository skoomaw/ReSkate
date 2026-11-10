#pragma once
#include "world_model_math.h"
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A world model (reskate.world_model): a plugin's model (a .rsvm, viewmodel_model.h, at rest) drawn into
// the world: into the game's lit picture before its post-processing, at the moment world_cover.h finds
// (world_cover.cpp draws it in the same list as the cover), so the game's grading, bloom and anti-aliasing
// treat it as part of the world. Placed in the world by the plugin each tick; hidden where the game's own
// surfaces are in front of it, by the game's depth; each of its materials a colour and an opacity, so what
// is behind it shows through (a board's frosted tread over its deck's graphic).
//
// The game's near distance, which comparing depths needs, is measured from what the model is drawn over
// (world_model_math.h) and said in the log; until it is known only the measuring is done and nothing is
// seen. The measure takes the model to lie on a surface of the game's, as a tread lies on its deck: a model
// that floats with the world far behind it would be measured short. The model is lit from the picture around it: its colour times the brightest of the game's pixels
// near it, brighter at a glancing angle, so it follows the game's light without knowing it.
//
// One at a time; the plugin host decides whose.
namespace dingosdk::world_model {
struct MaterialLook {
    std::array<float, 3> colour{0.92f, 0.95f, 1.0f}; // 0..1 each
    float opacity{0.5f};                              // 0 unseen, 1 nothing shows through
};
struct Look {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
    float scale{1};
    MaterialLook standard;                          // a material not named below
    std::map<std::string, MaterialLook> materials;  // by the model's material names
    float glance{0.5f};                             // 0..1: how much brighter and more opaque it is edge-on
};

// ---- client thread ----------------------------------------------------------------------------------
// Starts reading the model in the background, replacing any other. Its pictures are not read: a world
// model's materials are their colour and opacity.
void load(const std::filesystem::path& file);
void unload();
enum class Status { none, loading, ready, failed };
struct Info {
    Status status{Status::none};
    std::string why;
    std::vector<std::string> materials;
    std::optional<float> near; // the game's near distance as measured, once it is
};
Info info();
// Drawn for the next 300 ms (set it every tick) with this look, once it is loaded.
void show(const Look& look) noexcept;
void hide() noexcept;

// ---- render thread (world_cover.cpp) -----------------------------------------------------------------
// Whether there is something to draw this frame: world_cover goes into the game's frame for it.
bool wanted() noexcept;
struct Frame {
    ID3D12Device* device{};
    ID3D12GraphicsCommandList* list{};        // world_cover's own list, recording
    D3D12_CPU_DESCRIPTOR_HANDLE target{};     // the world's picture, as a render target
    DXGI_FORMAT format{};                     // its format
    ID3D12Resource* picture{};                // a copy of it, readable by pixel shaders
    ID3D12Resource* depth{};                  // the game's depth, readable by pixel shaders as it is
    DXGI_FORMAT depth_format{};               // how to read it
    std::uint32_t width{}, height{};
    std::array<float, 16> camera{};           // plugin_camera::Projector::m: right, up, back, eye rows
    float focal{};                            // pixels
    std::uint64_t fence_this{};               // the value the queue signals once this list is done
    std::uint64_t fence_done{};               // the value it has reached
};
// Records the model into the list, the world's picture bound as the target when it returns. False when
// nothing was recorded.
bool record(const Frame& frame) noexcept;
// The overlay is tearing its graphics down: our resources go.
void release() noexcept;
}
