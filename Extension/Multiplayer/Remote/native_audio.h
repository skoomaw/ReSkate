#pragma once
#include "native_skater.h"
#include <span>
namespace dingosdk::multiplayer {
void prepare_audio_capture(std::uintptr_t base, const NativeFrame &) noexcept;
// Installs the skater sound hook now (client thread) instead of when a session starts.
void prepare_remote_audio(std::uintptr_t base) noexcept;
std::vector<AudioSample> drain_audio_capture(std::uint64_t now);
void update_remote_audio(std::uintptr_t base, const NativeFrame &, const Pose &, const AudioState &) noexcept;
void stop_remote_audio() noexcept;
void reset_audio() noexcept;
std::string native_audio_status();
std::uint64_t captured_audio_frames() noexcept;
std::uint64_t played_audio_frames() noexcept;
// The local skater's surface sound: material words in its sound updates take this word's material
// and property slots, keeping their own flags (0: each surface its own sound); only those naming one
// of `from` (by slots), or every one when `from` is empty. Others in a session hear it too: they hear
// what the skater's sound updates say.
void set_surface_sound(std::uint32_t word, std::span<const std::uint32_t> from = {}) noexcept;
// The surface the local skater's sounds are of now, as the game gave it (before any surface
// sound): the material word most of its sound's material fields name; 0 when none is known.
std::uint32_t local_surface() noexcept;
// Whether the local skater's sound updates are wanted outside a session: a surface sound is set,
// or the surface was read in the last few seconds.
bool surface_wanted() noexcept;
} // namespace dingosdk::multiplayer
