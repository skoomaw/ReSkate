#pragma once
#include <cstdint>

// Research (the footstep-research branch, not for merging): where the game's footsteps get their surface.
// The skater sound's surface (reskate.skater.surface_sound) does not reach them. Every 15 seconds, for a
// second and a half, the two physics-material lookups the skater sound hands its surfaces to (0x7111c0,
// 0x45e120, native_audio_layout.h) are watched with a breakpoint: each call's caller and first arguments are
// kept, and said in the log by caller, with whether the skater was on the board or on foot.
namespace dingosdk::footstep_research {
// Client update thread, every tick.
void tick(std::uint64_t now) noexcept;
}
