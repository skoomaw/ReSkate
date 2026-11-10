#include "world_model.h"
#include "world_model_hlsl.h" // generated from world_model.hlsl (cmake/UI.cmake)
#include "viewmodel_model.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/clock.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>

// world_model.h says what this does.
namespace dingosdk::world_model {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::uintmax_t most_file_bytes = 64ull * 1024 * 1024;
constexpr UINT frame_constants = 32, material_constants = 4;
constexpr std::size_t counted_bytes = near_bins * 2 * 4; // counts, then sums
constexpr std::size_t readbacks = 3;

struct Shared {
    std::mutex mutex;
    std::uint64_t serial{};
    Status status{Status::none};
    std::string why;
    std::shared_ptr<const viewmodel::Model> loaded;
    Look look;
    double until{};
    std::optional<float> near_distance;
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
std::atomic<AnchorFn> board_anchor{};

void read_in_background(std::filesystem::path file, std::uint64_t serial) {
    std::string why, bytes;
    std::optional<viewmodel::Model> model;
    std::error_code code;
    const auto size = std::filesystem::file_size(file, code);
    if (code) why = "\"" + file.filename().string() + "\" cannot be read";
    else if (size > most_file_bytes) why = "\"" + file.filename().string() + "\" is larger than 64 MB";
    else {
        std::ifstream in(file, std::ios::binary);
        bytes.resize(static_cast<std::size_t>(size));
        if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) why = "\"" + file.filename().string() + "\" cannot be read";
        else model = viewmodel::parse(bytes, why);
    }
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (s.serial != serial) return;
    if (!model) {
        s.status = Status::failed;
        s.why = why;
        logging::write(logging::Level::warning, logging::Channel::graphics, "World model " + file.filename().string() + " not loaded: " + why);
        return;
    }
    s.status = Status::ready;
    s.why.clear();
    s.loaded = std::make_shared<const viewmodel::Model>(std::move(*model));
}

// ---- the GPU's side ---------------------------------------------------------------------------------
struct Retired {
    ComPtr<ID3D12Resource> resource;
    std::uint64_t fence{};
};
struct Readback {
    ComPtr<ID3D12Resource> buffer;
    std::uint64_t fence{};
    bool pending{};
};
struct Gpu {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> draw, measure;
    DXGI_FORMAT draw_format{DXGI_FORMAT_UNKNOWN};
    std::shared_ptr<const viewmodel::Model> model;
    ComPtr<ID3D12Resource> vertices, indices;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    ComPtr<ID3D12DescriptorHeap> views, depth_view;
    ComPtr<ID3D12Resource> own_depth;
    std::uint32_t depth_width{}, depth_height{};
    ComPtr<ID3D12Resource> counted, zeros;
    std::array<Readback, readbacks> back;
    NearEstimate estimate;
    std::optional<float> said_near;
    std::vector<Retired> retired;
    bool failed{}, said_drawn{};
};
Gpu& gpu() {
    static auto* value = new Gpu;
    return *value;
}

void say_failed(const std::string& why) {
    auto& g = gpu();
    if (g.failed) return;
    g.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics, "World models unavailable: " + why);
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, std::size_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&made)))) return {};
    made->SetName(name);
    return made;
}
bool fill(ID3D12Resource* target, const void* data, std::size_t size) {
    void* mapped{};
    const D3D12_RANGE none{};
    if (FAILED(target->Map(0, &none, &mapped)) || !mapped) return false;
    std::memcpy(mapped, data, size);
    target->Unmap(0, nullptr);
    return true;
}
void retire(ComPtr<ID3D12Resource>& resource, std::uint64_t fence) {
    if (resource) gpu().retired.push_back({std::move(resource), fence});
    resource.Reset();
}

bool build_root(ID3D12Device* device) {
    auto& g = gpu();
    if (g.root) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2; // t0 the picture's copy, t1 the game's depth
    std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = frame_constants;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = material_constants;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[3].Descriptor.ShaderRegister = 0;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        say_failed("its root signature could not be made.");
        return false;
    }
    return true;
}

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, error;
    if (FAILED(D3DCompile(world_model_hlsl, std::strlen(world_model_hlsl), "world_model", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error)) ||
        !code) {
        say_failed(std::string("its shader ") + entry + " did not compile" +
                   (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : std::string(".")));
        return {};
    }
    return code;
}

bool build_pipelines(ID3D12Device* device, DXGI_FORMAT format) {
    auto& g = gpu();
    if (g.draw && g.measure && g.draw_format == format) return true;
    g.draw.Reset();
    if (!build_root(device)) return false;
    const auto vertex = compile("vs_main", "vs_5_0"), pixel = compile("ps_main", "ps_5_0"), counting = compile("ps_measure", "ps_5_0");
    if (!vertex || !pixel || !counting) return false;
    const std::array<D3D12_INPUT_ELEMENT_DESC, 2> layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(viewmodel::Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(viewmodel::Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Both sides (the exporter's change of axes is a reflection); our own depth keeps the nearest.
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.SampleDesc.Count = 1;
    // Measuring: nothing written but the counts.
    if (!g.measure) {
        auto measuring = description;
        measuring.PS = {counting->GetBufferPointer(), counting->GetBufferSize()};
        measuring.NumRenderTargets = 0;
        if (FAILED(device->CreateGraphicsPipelineState(&measuring, IID_PPV_ARGS(&g.measure)))) {
            say_failed("its measuring pipeline was refused.");
            return false;
        }
    }
    // Drawing: blended over the picture by its opacity; the picture's own alpha is left as it is.
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.draw)))) {
        say_failed("its pipeline was refused for the world's picture's format.");
        return false;
    }
    g.draw_format = format;
    return true;
}

// The model's geometry, read by the GPU straight from upload memory (it is small).
bool take_model(const Frame& f, std::shared_ptr<const viewmodel::Model> model) {
    auto& g = gpu();
    retire(g.vertices, f.fence_this);
    retire(g.indices, f.fence_this);
    g.model = std::move(model);
    if (!g.model) return false;
    const std::size_t vertex_bytes = g.model->vertices.size() * sizeof(viewmodel::Vertex), index_bytes = g.model->indices.size() * 4;
    g.vertices = buffer(f.device, vertex_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"ReSkate world model vertices");
    g.indices = buffer(f.device, index_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"ReSkate world model indices");
    if (!g.vertices || !g.indices || !fill(g.vertices.Get(), g.model->vertices.data(), vertex_bytes) ||
        !fill(g.indices.Get(), g.model->indices.data(), index_bytes)) {
        g.model.reset();
        g.vertices.Reset();
        g.indices.Reset();
        say_failed("its model could not be put on the GPU.");
        return false;
    }
    g.vertex_view = {g.vertices->GetGPUVirtualAddress(), static_cast<UINT>(vertex_bytes), sizeof(viewmodel::Vertex)};
    g.index_view = {g.indices->GetGPUVirtualAddress(), static_cast<UINT>(index_bytes), DXGI_FORMAT_R32_UINT};
    return true;
}

// Our heaps, the counts and their readbacks, and our own depth the size of the picture.
bool build_resources(const Frame& f) {
    auto& g = gpu();
    if (!g.views) {
        D3D12_DESCRIPTOR_HEAP_DESC views{};
        views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        views.NumDescriptors = 2;
        views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_HEAP_DESC depth{};
        depth.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        depth.NumDescriptors = 1;
        if (FAILED(f.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&g.views))) || FAILED(f.device->CreateDescriptorHeap(&depth, IID_PPV_ARGS(&g.depth_view)))) {
            g.views.Reset();
            say_failed("no descriptor heaps.");
            return false;
        }
    }
    if (!g.counted) {
        g.counted = buffer(f.device, counted_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"ReSkate world model near counts");
        g.zeros = buffer(f.device, counted_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                         L"ReSkate world model zeros");
        const std::vector<std::uint8_t> nothing(counted_bytes, 0);
        if (!g.counted || !g.zeros || !fill(g.zeros.Get(), nothing.data(), nothing.size())) {
            g.counted.Reset();
            say_failed("no buffer for measuring the game's depth.");
            return false;
        }
        for (auto& back : g.back) {
            back.buffer = buffer(f.device, counted_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
                                 L"ReSkate world model near readback");
            back.pending = false;
            if (!back.buffer) {
                g.counted.Reset();
                say_failed("no readback for measuring the game's depth.");
                return false;
            }
        }
    }
    if (!g.own_depth || g.depth_width != f.width || g.depth_height != f.height) {
        retire(g.own_depth, f.fence_this);
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = f.width;
        description.Height = f.height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_D32_FLOAT;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        if (FAILED(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                     IID_PPV_ARGS(&g.own_depth)))) {
            say_failed("its depth buffer could not be made.");
            return false;
        }
        g.own_depth->SetName(L"ReSkate world model depth");
        f.device->CreateDepthStencilView(g.own_depth.Get(), nullptr, g.depth_view->GetCPUDescriptorHandleForHeapStart());
        g.depth_width = f.width;
        g.depth_height = f.height;
    }
    return true;
}

// What the GPU has counted in earlier frames, into the estimate.
void read_counts(const Frame& f) {
    auto& g = gpu();
    for (auto& back : g.back) {
        if (!back.pending || back.fence > f.fence_done) continue;
        back.pending = false;
        void* mapped{};
        const D3D12_RANGE all{0, counted_bytes};
        if (FAILED(back.buffer->Map(0, &all, &mapped)) || !mapped) continue;
        NearSample sample;
        std::memcpy(sample.counts.data(), mapped, near_bins * 4);
        std::memcpy(sample.sums.data(), static_cast<const std::uint8_t*>(mapped) + near_bins * 4, near_bins * 4);
        const D3D12_RANGE none{};
        back.buffer->Unmap(0, &none);
        if (const auto found = near_of(sample)) g.estimate.add(*found);
    }
    const auto now = g.estimate.value();
    if (now && (!g.said_near || std::abs(*now - *g.said_near) > 0.05f * *g.said_near)) {
        g.said_near = now;
        logging::write(logging::Level::info, logging::Channel::graphics, std::format("World models: the game's near distance measures {:.4f} m.", *now));
    }
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.near_distance = now;
}
} // namespace

void load(const std::filesystem::path& file) {
    auto& s = shared();
    std::uint64_t serial{};
    {
        std::lock_guard lock(s.mutex);
        serial = ++s.serial;
        s.status = Status::loading;
        s.why.clear();
        s.loaded.reset();
    }
    std::thread(read_in_background, file, serial).detach();
}
void unload() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    ++s.serial;
    s.status = Status::none;
    s.why.clear();
    s.loaded.reset();
    s.until = 0;
}
Info info() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    Info out;
    out.status = s.status;
    out.why = s.why;
    out.near_distance = s.near_distance;
    if (s.loaded)
        for (const auto& material : s.loaded->materials) out.materials.push_back(material.name);
    return out;
}
void show(const Look& look) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.look = look;
        s.until = steady_seconds() + 0.3;
    } catch (...) {}
}
void hide() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.until = 0;
}
void set_board_anchor(AnchorFn anchor) noexcept { board_anchor.store(anchor); }

bool wanted() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    return s.loaded && steady_seconds() < s.until && !gpu().failed;
}

bool record(const Frame& f) noexcept {
    try {
        auto& g = gpu();
        if (g.failed || !f.device || !f.list || !f.picture || !f.depth || f.width < 16 || f.height < 16 || !(f.focal > 0)) return false;
        std::shared_ptr<const viewmodel::Model> loaded;
        Look look;
        {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            if (steady_seconds() >= s.until) return false;
            loaded = s.loaded;
            look = s.look;
        }
        std::erase_if(g.retired, [&](const Retired& r) { return r.fence <= f.fence_done; });
        if (g.model != loaded && !take_model(f, loaded)) return false;
        if (!g.model || g.model->parts.empty()) return false;
        if (!build_pipelines(f.device, f.format) || !build_resources(f)) return false;
        read_counts(f);
        const auto near_now = g.estimate.value();

        // Views: the picture's copy as the target's format reads it, and the game's depth as it is.
        const UINT step = f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto view = g.views->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC picture{};
        picture.Format = f.format;
        picture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        picture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        picture.Texture2D.MipLevels = 1;
        f.device->CreateShaderResourceView(f.picture, &picture, view);
        view.ptr += step;
        D3D12_SHADER_RESOURCE_VIEW_DESC depth = picture;
        depth.Format = f.depth_format;
        f.device->CreateShaderResourceView(f.depth, &depth, view);

        // On the board: within the deck as the game draws it now (the anchor), else not at all.
        auto at = look.position;
        auto turn = look.rotation;
        if (look.on_board) {
            std::optional<Anchor> anchor;
            if (const auto anchor_fn = board_anchor.load()) anchor = anchor_fn();
            if (!anchor) return false;
            std::tie(at, turn) = within(anchor->position, anchor->rotation, look.position, look.rotation);
        }
        const auto place = placement(at, turn, std::clamp(look.scale, 0.01f, 100.0f));
        const auto& m = f.camera;
        const std::array<float, frame_constants> constants{
            place[0], place[1], place[2], place[3], place[4], place[5], place[6], place[7], place[8], place[9], place[10], place[11],
            m[0], m[1], m[2], f.focal,
            m[4], m[5], m[6], static_cast<float>(f.width),
            m[8], m[9], m[10], static_cast<float>(f.height),
            m[12], m[13], m[14], near_now.value_or(0.0f),
            0.015f, 0.01f, std::clamp(look.glance, 0.0f, 1.0f), 0};

        auto* list = f.list;
        ID3D12DescriptorHeap* heaps[] = {g.views.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(g.root.Get());
        list->SetGraphicsRoot32BitConstants(0, frame_constants, constants.data(), 0);
        list->SetGraphicsRootDescriptorTable(2, g.views->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootUnorderedAccessView(3, g.counted->GetGPUVirtualAddress());
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(f.width), static_cast<float>(f.height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(f.width), static_cast<LONG>(f.height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &g.vertex_view);
        list->IASetIndexBuffer(&g.index_view);
        const auto draw_parts = [&](bool coloured) {
            for (const auto& part : g.model->parts) {
                if (coloured) {
                    const auto& name = g.model->materials[part.material].name;
                    const auto found = look.materials.find(name);
                    const auto& material = found == look.materials.end() ? look.standard : found->second;
                    const std::array<float, material_constants> values{material.colour[0], material.colour[1], material.colour[2],
                                                                       std::clamp(material.opacity, 0.0f, 1.0f)};
                    list->SetGraphicsRoot32BitConstants(1, material_constants, values.data(), 0);
                }
                list->DrawIndexedInstanced(part.count, 1, part.first, 0, 0);
            }
        };

        // Measuring the game's near distance, into a readback the GPU has finished with.
        const auto free_back = std::find_if(g.back.begin(), g.back.end(), [](const Readback& b) { return !b.pending; });
        if (free_back != g.back.end()) {
            transition(list, g.counted.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(g.counted.Get(), 0, g.zeros.Get(), 0, counted_bytes);
            transition(list, g.counted.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetPipelineState(g.measure.Get());
            list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            draw_parts(false);
            D3D12_RESOURCE_BARRIER done{};
            done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            done.UAV.pResource = g.counted.Get();
            list->ResourceBarrier(1, &done);
            transition(list, g.counted.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(free_back->buffer.Get(), 0, g.counted.Get(), 0, counted_bytes);
            transition(list, g.counted.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            free_back->fence = f.fence_this;
            free_back->pending = true;
        }
        // Drawing, once the near distance is known.
        const auto own = g.depth_view->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &f.target, FALSE, &own);
        if (!near_now) return true;
        list->ClearDepthStencilView(own, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->SetPipelineState(g.draw.Get());
        draw_parts(true);
        if (!g.said_drawn) {
            g.said_drawn = true;
            logging::write(logging::Level::info, logging::Channel::graphics, "World models: drawn into the world's picture.");
        }
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing one.");
        return false;
    }
}

void release() noexcept {
    auto& g = gpu();
    // The overlay waits for the GPU before it tears down, so nothing here is still in use.
    g.retired.clear();
    g.root.Reset();
    g.draw.Reset();
    g.measure.Reset();
    g.draw_format = DXGI_FORMAT_UNKNOWN;
    g.model.reset();
    g.vertices.Reset();
    g.indices.Reset();
    g.views.Reset();
    g.depth_view.Reset();
    g.own_depth.Reset();
    g.depth_width = g.depth_height = 0;
    g.counted.Reset();
    g.zeros.Reset();
    for (auto& back : g.back) back = {};
    g.failed = false;
    g.said_drawn = false;
}
}
