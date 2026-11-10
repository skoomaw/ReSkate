#include "world_cover.h"
#include "world_cover_hlsl.h" // generated from world_cover.hlsl (cmake/UI.cmake)
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Plugins/plugin_camera.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// world_cover.h says what this does and how it finds its moment in the game's frame.
namespace dingosdk::world_cover {
namespace {
using Microsoft::WRL::ComPtr;

// Slots of the D3D12 interfaces' tables (d3d12.h's declaration order).
constexpr std::size_t device_create_rtv = 20, device_create_dsv = 21, device_copy_descriptors = 23, device_copy_simple = 24;
constexpr std::size_t list_set_targets = 46, list_begin_render_pass = 68, queue_execute = 10;
// world_cover.hlsl's cbuffer: seven float4s of camera and look, then the places kept clear (one
// float4 with their count, four with their feet and radii, one with their heights).
constexpr UINT constant_count = 52;

using CreateViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const void*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                                   const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopySimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using SetTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

std::atomic<CreateViewFn> original_rtv{}, original_dsv{};
std::atomic<CopyDescriptorsFn> original_copy{};
std::atomic<CopySimpleFn> original_copy_simple{};
std::atomic<SetTargetsFn> original_targets{};
using RenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                              const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
std::atomic<RenderPassFn> original_render_pass{};
std::atomic<ExecuteFn> original_execute{};

// What a plugin asked for (client thread).
struct Asked {
    std::mutex mutex;
    Settings settings;
    std::vector<Clear> clear;
    std::uint64_t clear_until{}; // keep_clear holds for 300 ms
};
Asked& asked() { static auto* value = new Asked; return *value; }
std::atomic<bool> wanted{};
std::atomic<bool> allowed{};     // the world has been in play for a moment (allow)
std::atomic<bool> retired{};     // the game's frame is not one the cover can safely go into
std::uint64_t playing_since{};   // client thread: when play began, 0 out of play

// A view the game made: what it is of, and in which format.
struct View {
    ID3D12Resource* resource{};
    DXGI_FORMAT format{};
};
bool hdr_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// What one of the game's command lists did while it was recorded, as far as the cover cares.
struct Notes {
    bool world{};          // drew with the world's depth and an HDR colour target
    ID3D12Resource* colour{};
    bool colour_state_known{};
    D3D12_RESOURCE_STATES colour_state{};
    bool depth_state_known{};
    D3D12_RESOURCE_STATES depth_state{}; // the main depth's state when the list ends
    bool unsafe{};         // a split or aliasing barrier on the colour or depth: no cover this frame
    std::uint64_t last_written{}; // the kind (format, flags, compute or drawn) of the last full-size float picture written on it
    std::vector<std::string> events; // while tracing: what happened on it to the depth and the big float pictures
    // The frame's last list: the screen goes from PRESENT to a render target on it, and the float
    // picture of the finished scene it puts there (with the UI, from a layer of its own) is moved from
    // being written to being read: that picture, its format, and its state before this list.
    bool composite{};
    ID3D12Resource* final_picture{};
    D3D12_RESOURCE_STATES final_before{};
    DXGI_FORMAT final_format{};
};

struct Shared {
    std::mutex mutex;
    // The game's render target and depth views, by CPU handle (every heap of the device: one address space).
    std::unordered_map<SIZE_T, View> rtvs, dsvs;
    std::unordered_map<ID3D12GraphicsCommandList*, Notes> notes;
    ID3D12CommandQueue* queue{};
    ID3D12Device* device{};
    // The frame: the screen's size, the main depth buffer (last seen), the world colour.
    std::uint32_t width{}, height{};
    ID3D12Resource* main_depth{};
    std::uint32_t depth_width{}, depth_height{};
    ID3D12Resource* colour{};
    DXGI_FORMAT colour_format{};
    bool colour_state_known{};
    D3D12_RESOURCE_STATES colour_state{};
    bool depth_state_known{};
    D3D12_RESOURCE_STATES depth_state{}; // the main depth's state after the lists submitted so far
    bool depth_elsewhere{}; // a list for another queue changed the depth's state this frame
    bool main_seen{};      // the main depth went through a barrier since the last present
    unsigned main_missed{}; // presents in a row it did not: after 30 another may take its place
    bool unsafe{};         // this frame's colour or depth went through a split or aliasing barrier
    bool pending{};        // the world was drawn in a submitted batch, the cover not yet
    bool drawn{};          // the cover is drawn this frame
    std::uint64_t frame{};
    // Ours: the picture's copy, the target's view, our views, our lists.
    ComPtr<ID3D12Resource> picture;
    D3D12_RESOURCE_DESC picture_desc{};
    ComPtr<ID3D12DescriptorHeap> views, targets;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    DXGI_FORMAT pipeline_format{DXGI_FORMAT_UNKNOWN};
    // Eight: the GPU runs frames behind, and with three every other frame found none free (2026-11-10).
    std::array<ComPtr<ID3D12CommandAllocator>, 8> allocators;
    std::array<UINT64, 8> allocator_fence{};
    std::size_t next_allocator{};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value{};
    bool failed{}, said_drawn{}, said_late{}, said_depth{}, said_colour{}, said_waiting{}, said_target{};
    unsigned presents_wanted{}; // presents while the cover was wanted and allowed (for the log)
    std::set<std::string> bindings_said; // what the game binds with the world's depth, said once each
    std::set<ID3D12Resource*> pictures_said; // full-size float pictures seen written, said once each
    // The game writes more than one full-size float picture a frame. Drawing into whichever came last
    // before the cover's moment made it flicker (2026-11-10), so it locks onto one: the picture written
    // last, after the world's depth is done, in most of 30 frames (the game's final float picture,
    // before its tone mapping and UI).
    struct Vote {
        unsigned count{};
        DXGI_FORMAT format{};
    };
    // By kind, not by picture: the game keeps two of each and takes turns with them a frame each (its
    // anti-aliasing reads last frame's), so one picture locked onto had the cover every other frame.
    std::map<std::uint64_t, Vote> votes;
    std::uint64_t frame_last{}; // the kind of this frame's last picture written after the depth
    std::uint64_t locked{};     // the kind the cover goes into, 0 before one is chosen
    bool locked_seen{};
    unsigned frames_voted{}, locked_missed{};
    // Why frames went without the cover, counted and said in the log now and then.
    const char* why{};          // the reason the last try this frame did not draw
    bool tried{};               // the cover's moment came this frame
    std::map<std::string, unsigned> outcomes;
    unsigned outcome_frames{}, outcome_reports{};
    // A trace of six frames, once, about two seconds after the picture is chosen: every batch the game
    // submits, in order, with what happened in it to the depth and to every big float picture, and
    // where the cover went in. Said in the log, to see why the cover shows on some frames only.
    unsigned frames_since_lock{}, trace_frames_left{}, quiet_batches{};
    bool traced{};
    std::vector<std::string> trace_lines;
    std::map<ID3D12Resource*, int> trace_ids;
    std::vector<std::string> trace_names; // each traced picture as first seen, by id
};
Shared& shared() { static auto* value = new Shared; return *value; }

// The main depth buffer's handles, readable without the lock by the OMSetRenderTargets hook, which
// runs thousands of times a frame: the common case (another depth, or none) leaves at once.
std::array<std::atomic<SIZE_T>, 8> depth_handles{};
std::atomic<std::size_t> next_depth_handle{};
std::atomic<ID3D12Resource*> main_depth_now{};
std::atomic<ID3D12Resource*> colour_now{};
std::atomic<bool> active{}; // wanted, and installed
std::atomic<bool> tracing{}; // the six traced frames are being recorded
// Our own command list: its barriers go through the same hook as the game's and are not the game's.
std::atomic<ID3D12GraphicsCommandList*> our_list{};

void say_failed(const std::string& why) {
    auto& s = shared();
    if (s.failed) return;
    s.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics, "World cover unavailable: " + why);
}

void remember_depth_handle(SIZE_T handle) {
    for (const auto& known : depth_handles)
        if (known.load(std::memory_order_relaxed) == handle) return;
    depth_handles[next_depth_handle.fetch_add(1) % depth_handles.size()].store(handle);
}
bool is_depth_handle(SIZE_T handle) {
    for (const auto& known : depth_handles)
        if (known.load(std::memory_order_relaxed) == handle) return true;
    return false;
}

// ---- hooks ------------------------------------------------------------------------------------------
void STDMETHODCALLTYPE hooked_rtv(ID3D12Device* device, ID3D12Resource* resource, const void* description, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    original_rtv.load()(device, resource, description, at);
    try {
        View view{resource, DXGI_FORMAT_UNKNOWN};
        if (description) view.format = static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(description)->Format;
        else if (resource) view.format = resource->GetDesc().Format;
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.rtvs[at.ptr] = view;
    } catch (...) {}
}
void STDMETHODCALLTYPE hooked_dsv(ID3D12Device* device, ID3D12Resource* resource, const void* description, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    original_dsv.load()(device, resource, description, at);
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        s.dsvs[at.ptr] = {resource, DXGI_FORMAT_UNKNOWN};
        if (resource && resource == main_depth_now.load()) remember_depth_handle(at.ptr);
    } catch (...) {}
}
// Views copied from one heap to another keep what they are of.
void copy_views(D3D12_DESCRIPTOR_HEAP_TYPE type, SIZE_T to, SIZE_T from, UINT count, UINT increment) {
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    auto& map = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? s.rtvs : s.dsvs;
    for (UINT i = 0; i < count; ++i) {
        const auto found = map.find(from + SIZE_T{i} * increment);
        if (found == map.end()) map.erase(to + SIZE_T{i} * increment);
        else {
            map[to + SIZE_T{i} * increment] = found->second;
            if (type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV && found->second.resource && found->second.resource == main_depth_now.load())
                remember_depth_handle(to + SIZE_T{i} * increment);
        }
    }
}
void STDMETHODCALLTYPE hooked_copy_simple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE to, D3D12_CPU_DESCRIPTOR_HANDLE from,
                                          D3D12_DESCRIPTOR_HEAP_TYPE type) {
    original_copy_simple.load()(device, count, to, from, type);
    try {
        copy_views(type, to.ptr, from.ptr, count, device->GetDescriptorHandleIncrementSize(type));
    } catch (...) {}
}
void STDMETHODCALLTYPE hooked_copy(ID3D12Device* device, UINT to_count, const D3D12_CPU_DESCRIPTOR_HANDLE* to_starts, const UINT* to_sizes, UINT from_count,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* from_starts, const UINT* from_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    original_copy.load()(device, to_count, to_starts, to_sizes, from_count, from_starts, from_sizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    try {
        // Both sides are runs of descriptors; walk them together, one descriptor at a time.
        const UINT increment = device->GetDescriptorHandleIncrementSize(type);
        UINT ti = 0, fi = 0, to_at = 0, from_at = 0;
        while (ti < to_count && fi < from_count) {
            const UINT to_size = to_sizes ? to_sizes[ti] : 1, from_size = from_sizes ? from_sizes[fi] : 1;
            copy_views(type, to_starts[ti].ptr + SIZE_T{to_at} * increment, from_starts[fi].ptr + SIZE_T{from_at} * increment, 1, increment);
            if (++to_at >= to_size) ++ti, to_at = 0;
            if (++from_at >= from_size) ++fi, from_at = 0;
        }
    } catch (...) {}
}

// A floating-point colour format a view of this format reads as, or UNKNOWN: a typeless view takes
// its family's float format.
DXGI_FORMAT hdr_view_format(DXGI_FORMAT f) {
    if (hdr_format(f)) return f;
    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (f == DXGI_FORMAT_R32G32B32A32_TYPELESS) return DXGI_FORMAT_R32G32B32A32_FLOAT;
    return DXGI_FORMAT_UNKNOWN;
}

// Targets bound with the world's depth (OMSetRenderTargets or BeginRenderPass): the first colour target,
// if it is a floating-point picture the depth's size, is the world's picture.
void note_targets(ID3D12GraphicsCommandList* list, UINT count, SIZE_T first_colour, const char* how) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    const View* view{};
    if (count) {
        const auto found = s.rtvs.find(first_colour);
        if (found != s.rtvs.end() && found->second.resource) view = &found->second;
    }
    D3D12_RESOURCE_DESC desc{};
    if (view) desc = view->resource->GetDesc();
    const DXGI_FORMAT format = view ? hdr_view_format(view->format != DXGI_FORMAT_UNKNOWN ? view->format : desc.Format) : DXGI_FORMAT_UNKNOWN;
    const bool fits = view && format != DXGI_FORMAT_UNKNOWN && desc.Width == s.depth_width && desc.Height == s.depth_height &&
                      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1;
    // What the game binds with the world's depth, each kind said once (at most 12), while no picture is found.
    if (!s.said_colour && s.bindings_said.size() < 12) {
        const std::string kind = !count ? std::format("{}: depth only", how)
                                 : !view ? std::format("{}: {} target(s), the first a view not known", how, count)
                                         : std::format("{}: {} target(s), the first {}x{} in view format {} (resource format {})", how, count, desc.Width,
                                                       desc.Height, static_cast<unsigned>(view->format), static_cast<unsigned>(desc.Format));
        if (s.bindings_said.insert(kind).second)
            logging::write(logging::Level::info, logging::Channel::graphics, "World cover: with the world's depth, " + kind + ".");
    }
    if (!fits || s.locked) return;
    auto& n = s.notes[list];
    n.world = true;
    n.colour = view->resource;
    n.colour_state_known = true; // bound as a target: in RENDER_TARGET now
    n.colour_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s.colour_format = format;
    if (!s.said_colour) {
        s.said_colour = true;
        logging::printf(logging::Level::info, logging::Channel::graphics, "World cover: the world's picture is %llux%u, format %u, %u mip level(s) (%s).",
                        static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(format), static_cast<unsigned>(desc.MipLevels), how);
    }
    colour_now.store(n.colour);
}

void STDMETHODCALLTYPE hooked_targets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* colours, BOOL single_range,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    original_targets.load()(list, count, colours, single_range, depth);
    if (!active.load(std::memory_order_relaxed) || !depth || !is_depth_handle(depth->ptr)) return;
    try {
        note_targets(list, colours ? count : 0, count && colours ? colours[0].ptr : 0, "OMSetRenderTargets");
    } catch (...) {}
}

void STDMETHODCALLTYPE hooked_render_pass(ID3D12GraphicsCommandList4* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* colours,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags) {
    original_render_pass.load()(list, count, colours, depth, flags);
    if (!active.load(std::memory_order_relaxed) || !depth || !is_depth_handle(depth->cpuDescriptor.ptr)) return;
    try {
        note_targets(list, colours ? count : 0, count && colours ? colours[0].cpuDescriptor.ptr : 0, "BeginRenderPass");
    } catch (...) {}
}

bool record_cover(Shared& s);

// Under the lock. A short name for a picture in the trace: #1, #2, ...
// `d`, when given, describes it the first time (the picture may be gone when the trace ends).
int trace_id(Shared& s, ID3D12Resource* r, const D3D12_RESOURCE_DESC* d = nullptr) {
    const auto at = s.trace_ids.find(r);
    if (at != s.trace_ids.end()) return at->second;
    const int id = static_cast<int>(s.trace_ids.size()) + 1;
    s.trace_ids.emplace(r, id);
    s.trace_names.push_back(d ? std::format("#{} {}x{} format {} flags 0x{:x}{}", id, d->Width, d->Height, static_cast<unsigned>(d->Format),
                                            static_cast<unsigned>(d->Flags), r == main_depth_now.load() ? " (the world's depth)" : "")
                              : std::format("#{}", id));
    return id;
}
// Under the lock: a batch's events as one trace line (or counted as quiet).
void trace_batch(Shared& s, bool ours_queue, UINT count, ID3D12CommandList* const* lists) {
    std::string line;
    for (UINT i = 0; i < count; ++i) {
        const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (found == s.notes.end() || found->second.events.empty()) continue;
        line += std::format(" [list {}:", i);
        for (const auto& e : found->second.events) line += " " + e + ";";
        line += "]";
        found->second.events.clear();
    }
    if (line.empty()) {
        ++s.quiet_batches;
        return;
    }
    if (s.quiet_batches) s.trace_lines.push_back(std::format("  ({} batch(es) with nothing traced)", s.quiet_batches));
    s.quiet_batches = 0;
    s.trace_lines.push_back(std::format("  batch of {} on {}:{}", count, ours_queue ? "the direct queue" : "ANOTHER queue", line));
}

void STDMETHODCALLTYPE hooked_execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    const auto original = original_execute.load();
    if (!active.load(std::memory_order_relaxed) || !lists || !count) return original(queue, count, lists);
    try {
        auto& s = shared();
        std::unique_lock lock(s.mutex);
        if (queue != s.queue) {
            if (tracing.load()) {
                trace_batch(s, false, count, lists);
                for (UINT i = 0; i < count; ++i) s.notes.erase(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            }
            lock.unlock();
            return original(queue, count, lists);
        }
        bool world_here = false;
        for (UINT i = 0; i < count; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found != s.notes.end() && found->second.world) world_here = true;
        }
        (void)world_here;
        // The frame's last batch, which puts the finished scene on the screen with the UI (a layer of its
        // own): the cover goes into the scene's picture just before it, in a list of ours. The game draws
        // almost a whole frame in one command list, so this is the first place after the scene is done
        // (traced 2026-11-10: the screen PRESENT to render target, the 1920x1080 R11G11B10 scene and the
        // RGBA8 UI layer read, the screen back to PRESENT, all in one batch of one list).
        const Notes* composite{};
        for (UINT i = 0; i < count && !composite; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found != s.notes.end() && found->second.composite && found->second.final_picture) composite = &found->second;
        }
        if (composite && !s.drawn) {
            s.tried = true;
            s.colour = composite->final_picture;
            s.colour_state = composite->final_before;
            s.colour_state_known = true;
            s.colour_format = composite->final_format;
            s.why = s.unsafe ? "a split or aliasing barrier, or another queue" : s.depth_elsewhere ? "the depth moved on another queue" : nullptr;
            const bool drew = !s.why && record_cover(s);
            if (tracing.load()) {
                if (s.quiet_batches) s.trace_lines.push_back(std::format("  ({} batch(es) with nothing traced)", s.quiet_batches));
                s.quiet_batches = 0;
                s.trace_lines.push_back(drew ? std::format("  >> COVER drawn into #{} (it was in state 0x{:x})", trace_id(s, s.colour), static_cast<unsigned>(s.colour_state))
                                             : std::format("  >> cover not drawn: {}", s.why ? s.why : "?"));
            }
            if (drew) {
                s.drawn = true;
                ID3D12CommandList* ours[] = {s.list.Get()};
                original(queue, 1, ours);
                queue->Signal(s.fence.Get(), ++s.fence_value);
                s.allocator_fence[(s.next_allocator + s.allocators.size() - 1) % s.allocators.size()] = s.fence_value;
                if (!s.said_drawn) {
                    s.said_drawn = true;
                    logging::printf(logging::Level::info, logging::Channel::graphics,
                                    "World cover: drawn into the finished scene (format %u) just before the game puts it on the screen with its UI.",
                                    static_cast<unsigned>(s.colour_format));
                }
            }
        }
        if (tracing.load()) trace_batch(s, true, count, lists);
        for (UINT i = 0; i < count; ++i) {
            const auto found = s.notes.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (found == s.notes.end()) continue;
            const auto& n = found->second;
            if (n.depth_state_known) {
                s.depth_state_known = true;
                s.depth_state = n.depth_state;
            }
            if (n.unsafe) s.unsafe = true;
            // A picture counts only once the world's depth is done and readable: one written before
            // that is not the lit world (the game writes one then, and the cover could never read the
            // depth at its moment, 2026-11-10). The last one written after it is kept for the vote.
            const bool depth_done = s.depth_state_known && (s.depth_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                    !(s.depth_state & D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (n.last_written && depth_done) s.frame_last = n.last_written;
            if (s.locked && n.last_written == s.locked) s.locked_seen = true;
            if (n.world) {
                s.colour = n.colour;
                s.pending = !s.drawn;
            }
            if (n.colour_state_known && n.colour && n.colour == s.colour) {
                s.colour_state_known = true;
                s.colour_state = n.colour_state;
            }
            s.notes.erase(found);
        }
        lock.unlock();
        original(queue, count, lists);
    } catch (...) {
        original(queue, count, lists);
    }
}

// ---- drawing ----------------------------------------------------------------------------------------
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier); // our own list: the hook passes it on (it is not the game's)
}

bool build_pipeline(Shared& s, DXGI_FORMAT format) {
    if (s.pipeline && s.pipeline_format == format) return true;
    s.pipeline.Reset();
    if (!s.root) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2; // t0 the picture, t1 the game's depth
        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = constant_count;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob ||
            FAILED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.root)))) {
            say_failed("its root signature could not be made.");
            return false;
        }
    }
    const auto length = std::strlen(world_cover_hlsl);
    ComPtr<ID3DBlob> vertex, pixel, error;
    if (FAILED(D3DCompile(world_cover_hlsl, length, "world_cover", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vertex, &error)) || !vertex ||
        FAILED(D3DCompile(world_cover_hlsl, length, "world_cover", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &pixel, &error)) || !pixel) {
        say_failed(std::string("its shader did not compile") + (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : "."));
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = s.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    if (FAILED(s.device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&s.pipeline)))) {
        say_failed("its pipeline was refused for the world's picture's format.");
        return false;
    }
    s.pipeline_format = format;
    return true;
}

// Our own list, heaps, the picture's copy: made the first time, and the copy again when the
// picture changes size or format.
bool build_resources(Shared& s, const D3D12_RESOURCE_DESC& colour) {
    if (!s.list) {
        for (auto& allocator : s.allocators)
            if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
                say_failed("no command allocator.");
                return false;
            }
        if (FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0].Get(), nullptr, IID_PPV_ARGS(&s.list))) ||
            FAILED(s.list->Close()) || FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) {
            say_failed("no command list.");
            return false;
        }
        s.list->SetName(L"ReSkate world cover");
        our_list.store(s.list.Get());
        D3D12_DESCRIPTOR_HEAP_DESC views{};
        views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        views.NumDescriptors = 2;
        views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_HEAP_DESC targets{};
        targets.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targets.NumDescriptors = 1;
        if (FAILED(s.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&s.views))) || FAILED(s.device->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&s.targets)))) {
            say_failed("no descriptor heaps.");
            return false;
        }
    }
    if (!s.picture || s.picture_desc.Width != colour.Width || s.picture_desc.Height != colour.Height || s.picture_desc.Format != colour.Format) {
        s.picture.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = colour.Width;
        desc.Height = colour.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = colour.Format;
        desc.SampleDesc.Count = 1;
        if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.picture)))) {
            say_failed("no copy of the world's picture could be made.");
            return false;
        }
        s.picture_desc = desc;
    }
    return true;
}

// Whether this device lets a view read a typed resource in another format of its family.
bool casting_typed(ID3D12Device* device) {
    static const bool supported = [device] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
        return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) &&
               options.CastingFullyTypedFormatSupported;
    }();
    return supported;
}

// Records our list: the picture copied, then drawn back with the cover over it. Under the lock.
bool record_cover(Shared& s) {
    s.why = "nothing to draw into";
    if (s.failed || !s.colour || !s.main_depth) return false;
    // The game's depth is read where it is, by our list, never copied or moved: only while the game
    // itself has left it readable by pixel shaders, and only through a view its format allows.
    const auto depth_desc = s.main_depth->GetDesc();
    DXGI_FORMAT depth_read = DXGI_FORMAT_UNKNOWN;
    if (depth_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS) depth_read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    else if (depth_desc.Format == DXGI_FORMAT_R32_TYPELESS) depth_read = DXGI_FORMAT_R32_FLOAT;
    else if (casting_typed(s.device)) {
        // A depth made in a typed depth format (the game's is D32_FLOAT_S8X24_UINT, and it reads it in
        // shaders itself), read through the same family's float format.
        if (depth_desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) depth_read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        else if (depth_desc.Format == DXGI_FORMAT_D32_FLOAT) depth_read = DXGI_FORMAT_R32_FLOAT;
    }
    if (depth_read == DXGI_FORMAT_UNKNOWN || !s.depth_state_known || !(s.depth_state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ||
        (s.depth_state & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) {
        if (!s.said_depth) {
            s.said_depth = true;
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "World cover: not drawn; the game's depth (format %u) is in state 0x%x%s at the cover's moment, not one it can be read in as it is.",
                            static_cast<unsigned>(depth_desc.Format), static_cast<unsigned>(s.depth_state), s.depth_state_known ? "" : " (not known)");
        }
        s.why = "the depth not readable";
        return false;
    }
    const auto colour_desc = s.colour->GetDesc();
    s.why = "the picture and depth differ in size, shape or state";
    if (colour_desc.Width != s.depth_width || colour_desc.Height != s.depth_height) return false;
    // Ours draws into it: a picture made only for compute (no render target views) is left alone.
    if (!(colour_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        if (!s.said_target) {
            s.said_target = true;
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "World cover: not drawn; the world's picture (format %u, flags 0x%x) takes no render target.",
                            static_cast<unsigned>(colour_desc.Format), static_cast<unsigned>(colour_desc.Flags));
        }
        return false;
    }
    // One plain 2D picture, which CopyResource can copy into ours as it is.
    if (colour_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colour_desc.MipLevels != 1 || colour_desc.DepthOrArraySize != 1 ||
        colour_desc.SampleDesc.Count != 1)
        return false;
    // Only from states a graphics queue's own work leaves it in: never while compute writes it.
    constexpr auto usable = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s.colour_state == D3D12_RESOURCE_STATE_COMMON || (s.colour_state & ~usable) != 0) return false;
    const auto projector = plugin_camera::projector(static_cast<float>(colour_desc.Width), static_cast<float>(colour_desc.Height));
    if (!projector) {
        s.why = "no camera";
        return false;
    }
    if (!build_resources(s, colour_desc) || !build_pipeline(s, s.colour_format)) return false;
    // An allocator the GPU has finished with, or none this frame.
    const std::size_t at = s.next_allocator;
    if (s.fence->GetCompletedValue() < s.allocator_fence[at]) {
        s.why = "no command allocator free";
        return false;
    }
    s.why = "recording failed";
    if (FAILED(s.allocators[at]->Reset()) || FAILED(s.list->Reset(s.allocators[at].Get(), nullptr))) return false;
    s.next_allocator = (at + 1) % s.allocators.size();

    Settings settings;
    std::vector<Clear> clear;
    {
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        settings = a.settings;
        if (GetTickCount64() < a.clear_until) clear = a.clear;
    }
    // Views: the picture's copy as the target's format reads it, and the game's depth as it is.
    const UINT view_step = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto view = s.views->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC picture_view{};
    picture_view.Format = s.colour_format;
    picture_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    picture_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    picture_view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(s.picture.Get(), &picture_view, view);
    view.ptr += view_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_view{};
    depth_view.Format = depth_read;
    depth_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(s.main_depth, &depth_view, view);
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = s.colour_format;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto target = s.targets->GetCPUDescriptorHandleForHeapStart();
    original_rtv.load()(s.device, s.colour, &target_view, target); // not through our hook: it takes the lock held here

    auto* list = s.list.Get();
    const auto before = s.colour_state;
    transition(list, s.colour, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(s.picture.Get(), s.colour);
    transition(list, s.picture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, s.colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const auto& m = projector->m;
    const float width = static_cast<float>(colour_desc.Width), height = static_cast<float>(colour_desc.Height);
    std::array<float, constant_count> constants{
        m[0], m[1], m[2], projector->focal,
        m[4], m[5], m[6], width,
        m[8], m[9], m[10], height,
        m[12], m[13], m[14], 0.1f, // the near distance: it scales distances, not directions; only the noise's size depends on it
        settings.colour[0], settings.colour[1], settings.colour[2], settings.amount,
        settings.facing, settings.softness, settings.brightness, settings.breakup,
        settings.sparkle, 0, 0, 0,
        static_cast<float>(clear.size()), 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // the four places' feet and radii, below
        0, 0, 0, 0};
    for (std::size_t i = 0; i < clear.size() && i < 4; ++i) {
        constants[32 + i * 4] = clear[i].foot[0];
        constants[33 + i * 4] = clear[i].foot[1];
        constants[34 + i * 4] = clear[i].foot[2];
        constants[35 + i * 4] = clear[i].radius;
        constants[48 + i] = clear[i].height;
    }
    ID3D12DescriptorHeap* heaps[] = {s.views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(s.root.Get());
    list->SetPipelineState(s.pipeline.Get());
    list->SetGraphicsRoot32BitConstants(0, constant_count, constants.data(), 0);
    list->SetGraphicsRootDescriptorTable(1, s.views->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(colour_desc.Width), static_cast<LONG>(colour_desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    transition(list, s.colour, D3D12_RESOURCE_STATE_RENDER_TARGET, before);
    transition(list, s.picture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (FAILED(list->Close())) return false;
    s.why = nullptr;
    return true;
}

bool depth_shaped(const D3D12_RESOURCE_DESC& d, std::uint32_t width, std::uint32_t height) {
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || d.SampleDesc.Count != 1 ||
        d.DepthOrArraySize != 1 || !width || !height)
        return false;
    if (d.Format != DXGI_FORMAT_R32G8X24_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT && d.Format != DXGI_FORMAT_R32_TYPELESS &&
        d.Format != DXGI_FORMAT_D32_FLOAT)
        return false;
    // The screen's shape (a render scale below 100% gives a smaller one), at least half its width:
    // the shadow maps are square.
    const double screen = static_cast<double>(width) / height, ours = static_cast<double>(d.Width) / d.Height;
    return std::abs(ours - screen) < 0.02 * screen && d.Width * 2 >= width;
}

} // namespace

void set(const Settings& settings) noexcept {
    auto& a = asked();
    {
        std::lock_guard lock(a.mutex);
        a.settings = settings;
    }
    wanted.store(true);
}
void clear() noexcept { wanted.store(false); }
void keep_clear(std::vector<Clear> places) noexcept {
    try {
        if (places.size() > 4) places.resize(4);
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        a.clear = std::move(places);
        a.clear_until = a.clear.empty() ? 0 : GetTickCount64() + 300;
    } catch (...) {}
}
void allow(bool playing) noexcept {
    const std::uint64_t now = GetTickCount64();
    if (!playing) {
        playing_since = 0;
        allowed.store(false);
        return;
    }
    if (!playing_since) playing_since = now;
    allowed.store(now - playing_since >= 2000);
}
bool on() noexcept { return wanted.load(); }

void install(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) noexcept {
    if (!device || !queue || !list) return;
    auto& s = shared();
    {
        std::lock_guard lock(s.mutex);
        s.device = device;
        s.queue = queue;
    }
    static std::atomic<bool> hooked{};
    if (hooked.exchange(true)) return;
    const auto hook = [](void* target, void* detour, auto& original, const char* name) {
        void* trampoline{};
        if (hook_prepare(target, detour, &trampoline) != HookOk) {
            logging::printf(logging::Level::warning, logging::Channel::graphics, "World cover: %s hook could not be prepared.", name);
            return false;
        }
        original.store(reinterpret_cast<std::remove_reference_t<decltype(original.load())>>(trampoline));
        if (hook_enable(target) != HookOk) {
            original.store(nullptr);
            logging::printf(logging::Level::warning, logging::Channel::graphics, "World cover: %s hook could not be enabled.", name);
            return false;
        }
        return true;
    };
    auto* const device_table = *reinterpret_cast<void***>(device);
    auto* const list_table = *reinterpret_cast<void***>(list);
    auto* const queue_table = *reinterpret_cast<void***>(queue);
    const bool ok = hook(device_table[device_create_rtv], reinterpret_cast<void*>(&hooked_rtv), original_rtv, "CreateRenderTargetView") &&
                    hook(device_table[device_create_dsv], reinterpret_cast<void*>(&hooked_dsv), original_dsv, "CreateDepthStencilView") &&
                    hook(device_table[device_copy_descriptors], reinterpret_cast<void*>(&hooked_copy), original_copy, "CopyDescriptors") &&
                    hook(device_table[device_copy_simple], reinterpret_cast<void*>(&hooked_copy_simple), original_copy_simple, "CopyDescriptorsSimple") &&
                    hook(list_table[list_set_targets], reinterpret_cast<void*>(&hooked_targets), original_targets, "OMSetRenderTargets") &&
                    hook(queue_table[queue_execute], reinterpret_cast<void*>(&hooked_execute), original_execute, "ExecuteCommandLists");
    // Render passes (ID3D12GraphicsCommandList4) bind targets too; only when the runtime's lists have them.
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (ok && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))) && list4) {
        auto* const table4 = *reinterpret_cast<void***>(list4.Get());
        (void)hook(table4[list_begin_render_pass], reinterpret_cast<void*>(&hooked_render_pass), original_render_pass, "BeginRenderPass");
    }
    if (!ok) say_failed("its D3D12 hooks are not all in place.");
    else logging::write(logging::Level::info, logging::Channel::graphics, "World cover: hooks installed.");
}

void present(std::uint32_t width, std::uint32_t height) noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        const bool ready = original_execute.load() && original_targets.load() && !s.failed && s.queue && !retired.load();
        active.store(ready && wanted.load() && allowed.load());
        if (!active.load()) {
            s.notes.clear();
            return;
        }
        if (s.pending && !s.drawn && !s.said_late) {
            s.said_late = true;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           "World cover: the world's last drawing was in the frame's last batch, so the cover had no place before post-processing.");
        }
        s.width = width, s.height = height;
        s.main_missed = s.main_seen ? 0 : s.main_missed + 1;
        // How this frame went, counted; every 600 frames (the first eight times) said in the log.
        if (s.main_depth) {
            ++s.outcomes[s.drawn ? "drawn" : !s.tried ? "the frame's last batch (to the screen) not seen" : (s.why ? s.why : "not drawn")];
            if (++s.outcome_frames >= 600) {
                if (s.outcome_reports < 8) {
                    ++s.outcome_reports;
                    std::string line = "World cover: the last 600 frames:";
                    for (const auto& [what, count] : s.outcomes) line += std::format(" {} {};", what, count);
                    logging::write(logging::Level::info, logging::Channel::graphics, line);
                }
                s.outcomes.clear();
                s.outcome_frames = 0;
            }
        }
        // The trace: started 120 frames after the picture is chosen, said frame by frame, six frames.
        if (s.main_depth && !s.traced && !tracing.load() && ++s.frames_since_lock == 120) {
            s.trace_frames_left = 6;
            s.trace_lines.clear();
            s.trace_ids.clear();
            s.trace_names.clear();
            s.quiet_batches = 0;
            tracing.store(true);
            logging::write(logging::Level::info, logging::Channel::graphics,
                           "World cover: trace of 6 frames follows (#n are the big float pictures and the depth, by first sight; states in hex).");
        } else if (tracing.load()) {
            if (s.quiet_batches) s.trace_lines.push_back(std::format("  ({} batch(es) with nothing traced)", s.quiet_batches));
            s.quiet_batches = 0;
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("World cover trace, frame {} of 6 ({}), then the present:", 7 - s.trace_frames_left, s.drawn ? "cover drawn" : "cover NOT drawn"));
            for (const auto& line : s.trace_lines)
                for (std::size_t at = 0; at < line.size(); at += 700)
                    logging::write(logging::Level::info, logging::Channel::graphics, (at ? "World cover trace   ..." : "World cover trace") + line.substr(at, 700));
            s.trace_lines.clear();
            if (--s.trace_frames_left == 0) {
                tracing.store(false);
                s.traced = true;
                std::string names = "World cover trace: pictures:";
                for (const auto& name : s.trace_names) names += " " + name + ";";
                logging::write(logging::Level::info, logging::Channel::graphics, names);
            }
        }
        s.tried = false;
        s.why = nullptr;
        // The picture the cover goes into: voted for over 30 frames, then kept while it is still written.
        if (s.frame_last) {
            ++s.votes[s.frame_last].count;
            ++s.frames_voted;
        }
        if (s.locked) {
            s.locked_missed = s.locked_seen ? 0 : s.locked_missed + 1;
            if (s.locked_missed > 120) {
                logging::write(logging::Level::info, logging::Channel::graphics, "World cover: the world's picture is no longer written; choosing again.");
                s.locked = 0;
                colour_now.store(nullptr);
                s.votes.clear();
                s.frames_voted = 0;
            }
        } else if (s.frames_voted >= 30) {
            const auto best = std::max_element(s.votes.begin(), s.votes.end(),
                                               [](const auto& a, const auto& b) { return a.second.count < b.second.count; });
            if (best != s.votes.end() && best->second.count >= 20 && best->second.format != DXGI_FORMAT_UNKNOWN) {
                s.locked = best->first;
                s.colour_format = best->second.format;
                s.locked_missed = 0;
                s.said_colour = true;
                logging::printf(logging::Level::info, logging::Channel::graphics,
                                "World cover: the world's picture is the one written last after the depth in %u of %u frames (format %u).", best->second.count,
                                s.frames_voted, static_cast<unsigned>(best->second.format));
            }
            s.votes.clear();
            s.frames_voted = 0;
        }
        s.frame_last = 0;
        s.locked_seen = false;
        s.pending = s.drawn = s.depth_elsewhere = s.unsafe = s.main_seen = false;
        s.colour_state_known = false;
        ++s.frame;
        // Notes left from lists recorded but never submitted would otherwise grow without end.
        if (s.notes.size() > 4096) s.notes.clear();
        // Wanted for ten seconds or so and never drawn: say what of the frame was found.
        if (!s.said_drawn && !s.said_waiting && ++s.presents_wanted == 600) {
            s.said_waiting = true;
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "World cover: not drawn yet; world depth %s, world picture %s, depth views known %u, render target views known %zu.",
                            s.main_depth ? "found" : "not found", s.said_colour ? "found" : "not found",
                            static_cast<unsigned>(std::count_if(depth_handles.begin(), depth_handles.end(), [](const auto& h) { return h.load() != 0; })),
                            s.rtvs.size());
        }
    } catch (...) {}
}

void release() noexcept {
    try {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        active.store(false);
        // The overlay waits for the GPU before it tears down; ours went on the same queue.
        our_list.store(nullptr);
        s.list.Reset();
        for (auto& allocator : s.allocators) allocator.Reset();
        s.allocator_fence = {};
        s.fence.Reset();
        s.fence_value = 0;
        s.picture.Reset();
        s.views.Reset();
        s.targets.Reset();
        s.pipeline.Reset();
        s.root.Reset();
        s.pipeline_format = DXGI_FORMAT_UNKNOWN;
        s.notes.clear();
        s.rtvs.clear();
        s.dsvs.clear();
        s.device = nullptr;
        s.queue = nullptr;
        s.colour = nullptr;
        s.main_depth = nullptr;
        main_depth_now.store(nullptr);
        s.depth_state_known = false;
        s.failed = false;
    } catch (...) {}
}

void on_barrier(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b, BarrierFn /*original*/) noexcept {
    if (!active.load(std::memory_order_relaxed) || list == our_list.load(std::memory_order_relaxed)) return;
    try {
        // While tracing: every transition of a picture at least half the screen's size, of any format
        // (the depth, the float pictures, and the tone-mapped and UI pictures after them), on any list.
        if (tracing.load(std::memory_order_relaxed) && b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
            const auto d = b.Transition.pResource->GetDesc();
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                auto& s = shared();
                std::lock_guard lock(s.mutex);
                if (d.Width * 2 >= s.width && d.Height * 2 >= s.height) {
                    auto& n = s.notes[list];
                    if (n.events.size() < 400)
                        n.events.push_back(std::format("{}#{} 0x{:x}>0x{:x}{}", list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ? "" : "(compute) ",
                                                       trace_id(s, b.Transition.pResource, &d), static_cast<unsigned>(b.Transition.StateBefore),
                                                       static_cast<unsigned>(b.Transition.StateAfter),
                                                       b.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY ? " begin" : b.Flags == D3D12_RESOURCE_BARRIER_FLAG_END_ONLY ? " end" : ""));
                }
            }
        }
        auto* const colour = colour_now.load(std::memory_order_relaxed);
        // Memory aliased onto (or off) the world's colour: it is not ours to touch this frame.
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if (colour && (b.Aliasing.pResourceBefore == colour || b.Aliasing.pResourceAfter == colour)) {
                auto& s = shared();
                std::lock_guard lock(s.mutex);
                s.notes[list].unsafe = true;
            }
            return;
        }
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) return;
        auto* const resource = b.Transition.pResource;
        // The frame's last list (Notes::composite): the screen made a render target, and the finished
        // scene's float picture moved from being written to being read on the same list.
        if (list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE) {
            const bool to_screen = b.Transition.StateBefore == D3D12_RESOURCE_STATE_PRESENT && (b.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET);
            const bool finished = (b.Transition.StateBefore & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) &&
                                  (b.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                                  !(b.Transition.StateAfter & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            if (to_screen || finished) {
                const auto d = resource->GetDesc();
                auto& s = shared();
                std::lock_guard lock(s.mutex);
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1) {
                    auto& n = s.notes[list];
                    if (to_screen && d.Width == s.width && d.Height == s.height) n.composite = true;
                    const DXGI_FORMAT f = hdr_view_format(d.Format);
                    if (finished && !n.final_picture && f != DXGI_FORMAT_UNKNOWN && d.Width == s.depth_width && d.Height == s.depth_height &&
                        d.MipLevels == 1 && d.DepthOrArraySize == 1) {
                        n.final_picture = resource;
                        n.final_before = b.Transition.StateBefore;
                        n.final_format = f;
                    }
                }
            }
        }
        constexpr auto writes = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr auto reads = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_COPY_SOURCE;
        const bool written = (b.Transition.StateBefore & writes) && (b.Transition.StateAfter & reads) && !(b.Transition.StateAfter & writes);
        // The world's colour changing state on this list: where it ends up when the list is submitted.
        if (resource == colour) {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                // Another queue works on the picture this frame, unsynchronised with a list of ours.
                s.unsafe = true;
                return;
            }
            auto& n = s.notes[list];
            if (!n.colour) n.colour = resource;
            // Half-way through a split barrier the picture is in no state at all.
            if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) n.unsafe = true;
            if (written) {
                n.world = true; // the picture was drawn into on this list
                n.last_written = s.locked;
            }
            n.colour_state_known = true;
            n.colour_state = b.Transition.StateAfter;
            return;
        }
        // The world's lit picture as the game finishes writing it, drawn into or by compute (the game
        // lights the world in compute: no float target is ever bound with the world's depth, 2026-11-10):
        // a full-size, floating-point picture going from being written to being read. The last one
        // written before a batch with none is the one the cover goes into: still before the game's
        // tone mapping (its output is not floating point) and its UI.
        if (written && b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE && resource != main_depth_now.load(std::memory_order_relaxed) &&
            list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            const auto desc = resource->GetDesc();
            const DXGI_FORMAT format = hdr_view_format(desc.Format);
            if (format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
                desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
                return;
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            if (!s.depth_width || desc.Width != s.depth_width || desc.Height != s.depth_height) return;
            // Its kind: format, flags, and whether it was written by compute or drawn into.
            const std::uint64_t kind = std::uint64_t{static_cast<std::uint32_t>(format)} | (std::uint64_t{static_cast<std::uint32_t>(desc.Flags)} << 8) |
                                       ((b.Transition.StateBefore & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? (std::uint64_t{1} << 40) : 0);
            auto& n = s.notes[list];
            n.last_written = kind;
            if (s.votes.size() < 16 || s.votes.contains(kind)) s.votes[kind].format = format;
            // One of the kind the cover goes into, just written: this frame's picture.
            if (s.locked && kind == s.locked) {
                n.world = true;
                n.colour = resource;
                n.colour_state_known = true;
                n.colour_state = b.Transition.StateAfter;
                colour_now.store(resource);
            }
            if (s.pictures_said.size() < 8 && s.pictures_said.insert(resource).second)
                logging::printf(logging::Level::info, logging::Channel::graphics,
                                "World cover: a full-size float picture written (%llux%u, format %u, flags 0x%x, state 0x%x to 0x%x).",
                                static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.Flags),
                                static_cast<unsigned>(b.Transition.StateBefore), static_cast<unsigned>(b.Transition.StateAfter));
            return;
        }
        // The main depth buffer: where it goes, so our list knows whether it can read it as it is. Nothing
        // is ever recorded on the game's lists (a copy of the depth put on them hung the GPU).
        if (resource == main_depth_now.load(std::memory_order_relaxed)) {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            s.main_seen = true;
            if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                s.depth_elsewhere = true; // another queue moves it: not this frame
                return;
            }
            auto& n = s.notes[list];
            if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) n.unsafe = true;
            n.depth_state_known = true;
            n.depth_state = b.Transition.StateAfter;
            return;
        }
        // Which depth buffer is the world's: the first screen-shaped one each frame that goes from being
        // written to being read, as the depth tap finds it.
        if (!(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE)) return;
        constexpr auto read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (!(b.Transition.StateAfter & read) || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) return;
        // The whole buffer, or its depth plane alone (subresource 0): the game moves the planes apart.
        if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0) return;
        if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        if (!s.device) return;
        const auto desc = resource->GetDesc();
        if (!depth_shaped(desc, s.width, s.height)) return;
        // The largest such buffer is the world's: the game keeps smaller, screen-shaped ones too (a
        // 960x540 one at 1080p, 2026-11-10). A smaller one takes over only once the one known has
        // not been seen for 30 presents (the screen got smaller).
        if (s.main_depth && desc.Width * desc.Height <= UINT64{s.depth_width} * s.depth_height && s.main_missed < 30) return;
        // A new main depth buffer (a resize, a level): its views are looked for afresh.
        s.main_depth = resource;
        main_depth_now.store(resource);
        s.depth_state_known = false;
        s.main_seen = true;
        s.main_missed = 0;
        for (auto& handle : depth_handles) handle.store(0);
        for (const auto& [handle, view] : s.dsvs)
            if (view.resource == resource) remember_depth_handle(handle);
        s.depth_width = static_cast<std::uint32_t>(desc.Width);
        s.depth_height = desc.Height;
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "World cover: the world's depth is %llux%u, format %u, %u mip level(s), barrier on subresource %s, now in state 0x%x.",
                        static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.MipLevels),
                        b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? "all" : "0",
                        static_cast<unsigned>(b.Transition.StateAfter));
        auto& n = s.notes[list];
        n.depth_state_known = true;
        n.depth_state = b.Transition.StateAfter;
    } catch (...) {}
}
} // namespace dingosdk::world_cover
