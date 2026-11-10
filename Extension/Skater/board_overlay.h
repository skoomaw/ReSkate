#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Items shown on the local skater's board over the board the player chose, for plugins
// (reskate.outfit.board_set): what outfit_overlay.h does for the skater's own slots, for the board's
// parts. An owner puts an item in a part ("deck", "grip", "grip_cutout", "trucks", "wheels"); the
// board is handed it through the game's own recipe copy (the call the multiplayer code makes for
// other players' boards), and given back to the game while one of its menus is up (the menus change
// the board), when nothing is wanted, and when the plugin stops. Nothing of the player's saved board
// changes: this is what is drawn, and what the other players are sent while it is. Never a paid store
// item. Client thread throughout.
namespace dingosdk::board_overlay {
// The board recipe's slot for a part, by the slot hashes worn_items.cpp reads the board with; 0 for
// a name that is not a part.
std::uint32_t part_slot(std::string_view part) noexcept;
// Empty when the part is the owner's now, otherwise why not.
std::string set(std::string owner, std::string part, std::string item);
// The owner's item on that part (every part, when empty) taken off.
void clear(std::string_view owner, std::string_view part = {});
// Each client tick.
void tick() noexcept;
std::string status();
}
