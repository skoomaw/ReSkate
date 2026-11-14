#include "footstep_research.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Skater/no_bail.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace dingosdk::footstep_research {
namespace {
// The lookups, as offsets from the game's image (native_audio_layout.h).
constexpr std::array<std::uintptr_t, 2> lookups{0x7111c0, 0x45e120};
constexpr std::uint64_t every_ms = 15000, watch_ms = 1500, first_after_ms = 20000;

struct Call {
    std::uint8_t lookup{};
    std::uintptr_t caller{};
    std::uint64_t rcx{}, rdx{}, r8{};
};
constexpr std::size_t calls_kept = 16384;
std::array<Call, calls_kept> calls{};
std::atomic<std::uint64_t> call_count{};

std::uintptr_t base{};
std::array<std::uint8_t, 2> opcodes{};      // each lookup's own first byte
std::array<bool, 2> usable{};               // its start checked: a function's (padding before it)
std::atomic<bool> watching{};
void* handler_handle{};
thread_local int stepping = -1;              // the lookup this thread is stepping past, to put its int3 back

// A byte written over code without allocating (the handler's re-arm).
void write_code_byte(std::uintptr_t address, std::uint8_t value) noexcept {
    DWORD old{};
    if (!VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *reinterpret_cast<volatile std::uint8_t*>(address) = value;
    VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 1);
}

LONG CALLBACK handler(EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord || !base) return EXCEPTION_CONTINUE_SEARCH;
    auto& context = *pointers->ContextRecord;
    const auto code = pointers->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && stepping >= 0) {
        // Past the lookup's first instruction: its int3 back, while still watching.
        const auto which = static_cast<std::size_t>(stepping);
        stepping = -1;
        if (watching.load(std::memory_order_acquire)) write_code_byte(base + lookups[which], 0xcc);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    for (std::size_t i = 0; i < lookups.size(); ++i) {
        if (!usable[i] || address != base + lookups[i]) continue;
        const auto n = call_count.fetch_add(1, std::memory_order_relaxed);
        auto& call = calls[n % calls_kept];
        std::uintptr_t caller{};
        std::memcpy(&caller, reinterpret_cast<const void*>(context.Rsp), sizeof(caller)); // at the entry: the return address
        call = {static_cast<std::uint8_t>(i), caller, context.Rcx, context.Rdx, context.R8};
        // Its own byte back, the one instruction run, then (single step) the int3 again.
        (void)patch_breakpoint_restore(address, opcodes[i]);
        context.Rip = address;
        if (watching.load(std::memory_order_acquire)) {
            stepping = static_cast<int>(i);
            context.EFlags |= 0x100;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string state_name(std::uint32_t state) {
    if (state >= 300 && state < 400) return std::format("{} (wipeout)", state);
    if (state >= 200 && state < 300) return std::format("{} (on the board)", state);
    return std::format("{}", state);
}

void report(std::uint64_t from, std::uint64_t to, std::uint32_t state_from, std::uint32_t state_to) {
    struct Seen {
        std::size_t count{};
        std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> arguments; // a few distinct
    };
    std::map<std::pair<std::uint8_t, std::uintptr_t>, Seen> by_caller;
    const auto first = to > calls_kept ? std::max(from, to - calls_kept) : from;
    for (auto n = first; n < to; ++n) {
        const auto& call = calls[n % calls_kept];
        auto& seen = by_caller[{call.lookup, call.caller}];
        ++seen.count;
        const auto arguments = std::tuple{call.rcx & 0xffffffffu, call.rdx & 0xffffffffu, call.r8 & 0xffffffffu};
        if (seen.arguments.size() < 4 && std::find(seen.arguments.begin(), seen.arguments.end(), arguments) == seen.arguments.end())
            seen.arguments.push_back(arguments);
    }
    std::vector<std::pair<std::pair<std::uint8_t, std::uintptr_t>, Seen>> ranked(by_caller.begin(), by_caller.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Footstep search: {} lookups in {:.1f} s, the skater {} then {}; {} callers.", to - from,
                 static_cast<double>(watch_ms) / 1000.0, state_name(state_from), state_name(state_to), ranked.size());
    for (std::size_t r = 0; r < ranked.size() && r < 20; ++r) {
        const auto& [key, seen] = ranked[r];
        std::string line = std::format("Footstep search:   lookup {:#x} from Skate+{:#x}: {} time(s); arguments (low 32 bits: rcx rdx r8)",
                                       lookups[key.first], key.second - base, seen.count);
        for (const auto& [rcx, rdx, r8] : seen.arguments) line += std::format(" [{:#x} {:#x} {:#x}]", rcx, rdx, r8);
        logging::write(logging::Level::info, logging::Channel::skater, line);
    }
}
}

void tick(std::uint64_t now) noexcept {
    try {
        static std::uint64_t started{}, next_at{}, window_from{};
        static std::uint32_t state_from{};
        static bool ready{}, said{};
        if (!started) {
            started = now;
            next_at = now + first_after_ms;
        }
        if (!ready) {
            if (now < next_at) return;
            ready = true;
            base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            // Each lookup's start: logged, and watched only where it is a function's (the byte before it padding
            // or a return), so a wrong offset is not patched.
            for (std::size_t i = 0; i < lookups.size(); ++i) {
                const auto* at = reinterpret_cast<const std::uint8_t*>(base + lookups[i]);
                std::string bytes;
                for (int k = -1; k < 16; ++k) bytes += std::format("{:02x}{}", at[k], k == -1 ? " | " : " ");
                const auto before = at[-1];
                usable[i] = (before == 0xcc || before == 0x90 || before == 0xc3) && at[0] != 0xcc;
                opcodes[i] = at[0];
                logging::log(logging::Level::info, logging::Channel::skater, "Footstep search: lookup {:#x}: {}{}", lookups[i], bytes,
                             usable[i] ? "" : " (not a function's start: not watched)");
            }
            handler_handle = patch_breakpoint_handler("footstep search", handler);
            if (!handler_handle) {
                logging::write(logging::Level::warning, logging::Channel::skater, "Footstep search: no exception handler; off.");
                usable = {};
            }
            next_at = now;
        }
        if (!handler_handle) return;
        if (watching.load()) {
            if (now < next_at) return;
            watching.store(false, std::memory_order_release);
            for (std::size_t i = 0; i < lookups.size(); ++i)
                if (usable[i] && *reinterpret_cast<const std::uint8_t*>(base + lookups[i]) == 0xcc)
                    (void)patch_breakpoint_restore(base + lookups[i], opcodes[i]);
            report(window_from, call_count.load(), state_from, watched_physics_state().state);
            next_at = now + every_ms;
            return;
        }
        if (now < next_at) return;
        window_from = call_count.load();
        state_from = watched_physics_state().state;
        watching.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < lookups.size(); ++i)
            if (usable[i] && patch_breakpoint("footstep search", base + lookups[i], opcodes[i]) != HookOk) usable[i] = false;
        if (!said) {
            said = true;
            logging::write(logging::Level::info, logging::Channel::skater,
                           "Footstep search: on (every 15 s for 1.5 s; walk on foot for some of them, skate for others).");
        }
        next_at = now + watch_ms;
    } catch (...) {}
}
}
