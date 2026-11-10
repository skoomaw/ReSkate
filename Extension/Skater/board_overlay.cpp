#include "board_overlay.h"
#include "Extension/Skater/skater_items.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/content_catalogs.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>

// board_overlay.h says what this does. It follows outfit_overlay.cpp's way with the skater's recipe:
// the board's item component is the same kind of component (the multiplayer code captures the local
// board and hands other players' boards their recipes through the same copy), so the same capture,
// copy, feed and hand back apply to it.
namespace dingosdk::board_overlay {
namespace {
using skater_items::SkaterItems;

// The board recipe's slots (worn_items.cpp: read from the game's presets with Fit Check's
// `turnaround parts`, 2026-10-20).
struct Part {
    std::string_view name;
    std::uint32_t slot;
};
constexpr std::array<Part, 5> parts{{
    {"deck", 0x66071628}, {"grip", 0x755d5ee0}, {"grip_cutout", 0x976f18c4}, {"trucks", 0x0add98be}, {"wheels", 0xc83bb94e}}};

struct Wanted {
    std::string owner, part, item;
};
struct State {
    std::map<std::uint32_t, Wanted> wanted; // by slot
    bool own{}, dirty{};
    std::uintptr_t board{}, component{};
    std::optional<skater_items::Feed> feed;   // how the game fed the board before a recipe of ours
    std::map<std::uint32_t, multiplayer::CosmeticSlot> original; // the player's own items in the slots we change
    std::map<std::uint32_t, std::string> shown;                  // what we put in each slot
    std::uint64_t given_back{}, menu_seen{}, checked{}, applied{};
    std::string status{"nothing shown"}, logged;
};
State& state() {
    static auto* s = new State;
    return *s;
}

void say(State& s, const std::string& line) {
    if (line == s.logged) return;
    s.logged = line;
    logging::log(logging::Level::info, logging::Channel::skater, "Board items: {}.", line);
}

void give_back(State& s, const SkaterItems& m, std::uint64_t now) {
    if (s.board && s.own) {
        const auto component = m.component(s.board);
        if (const auto fed = skater_items::feed(m, component); fed.follows_outfit() || s.feed) {
            skater_items::hand_back(m, component, fed.follows_outfit() ? fed : *s.feed);
            s.given_back = now;
        }
    }
    s.own = false;
    s.shown.clear();
    s.original.clear();
}

void show(State& s, const SkaterItems& m, std::uint64_t now) {
    const auto component = m.component(s.board);
    auto recipe = m.capture(s.board);
    SkaterItems::check(recipe.key == multiplayer::board_recipe_key, "The board's recipe is not a board's.");
    const auto at = [&](std::uint32_t slot) -> multiplayer::CosmeticSlot* {
        for (auto& item : recipe.items)
            if (item.slot == slot) return &item;
        return nullptr;
    };
    if (const auto fed = skater_items::feed(m, component); fed.follows_outfit()) {
        // The board as the player chose it (a new one, or ours taken back by the game): what we change is
        // remembered, to be put back part by part.
        SkaterItems::check(now - s.given_back > 1000, "Waiting for the game to put the board back.");
        s.feed = fed;
        s.own = false;
        s.shown.clear();
        s.original.clear();
    } else {
        SkaterItems::check(s.own && s.feed && component == s.component, "The board does not follow the player's board: left alone.");
        // The game changed what we put on (it rebuilt the board from the player's): it is the game's again.
        for (const auto& [slot, item] : s.shown)
            if (const auto* now_there = at(slot); !now_there || now_there->asset != item) {
                give_back(s, m, now);
                return;
            }
    }
    bool changed{};
    std::string missing;
    for (const auto& [slot, want] : s.wanted) {
        auto* item = at(slot);
        if (!item) {
            missing += (missing.empty() ? "" : ", ") + want.part;
            continue;
        }
        if (!s.original.contains(slot)) s.original.emplace(slot, *item);
        if (item->asset == want.item) {
            s.shown[slot] = want.item;
            continue;
        }
        item->asset = want.item; // the slot's own parameters stay: they are the right shape for it
        s.shown[slot] = want.item;
        changed = true;
    }
    // Parts no longer wanted get the player's own item back.
    for (auto it = s.shown.begin(); it != s.shown.end();) {
        if (s.wanted.contains(it->first)) {
            ++it;
            continue;
        }
        if (auto* item = at(it->first); item && s.original.contains(it->first)) *item = s.original.at(it->first);
        s.original.erase(it->first);
        it = s.shown.erase(it);
        changed = true;
    }
    if (!missing.empty()) say(s, "this board has no " + missing + " slot");
    if (!changed) return;
    skater_items::apply(m, component, recipe);
    s.own = true;
    s.component = component;
    s.applied = now;
    std::string list;
    for (const auto& [slot, item] : s.shown) {
        const auto part = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.slot == slot; });
        list += std::format("{}{} on the {}", list.empty() ? "" : ", ", item, part == parts.end() ? std::string_view("board") : part->name);
    }
    say(s, list.empty() ? "the player's own board again" : list);
}
} // namespace

std::uint32_t part_slot(std::string_view part) noexcept {
    for (const auto& p : parts)
        if (p.name == part) return p.slot;
    return 0;
}

std::string set(std::string owner, std::string part, std::string item) {
    auto& s = state();
    const auto slot = part_slot(part);
    if (!slot) return "a part is \"deck\", \"grip\", \"grip_cutout\", \"trucks\" or \"wheels\"";
    if (item.empty()) return "an item has no name";
    if (const auto taken = s.wanted.find(slot); taken != s.wanted.end() && taken->second.owner != owner) return taken->second.owner + " has that part";
    if (!content_cache::catalogs().available) return "the paid items are not known yet; try again in a moment";
    if (profile_runtime::reserved_cosmetic(item)) return "that is a paid store item";
    auto& want = s.wanted[slot];
    if (want.owner == owner && want.item == item) return {};
    want = {std::move(owner), std::move(part), std::move(item)};
    s.dirty = true;
    return {};
}

void clear(std::string_view owner, std::string_view part) {
    auto& s = state();
    const auto slot = part.empty() ? 0 : part_slot(part);
    if (std::erase_if(s.wanted, [&](const auto& w) { return w.second.owner == owner && (part.empty() || w.first == slot); })) s.dirty = true;
}

void tick() noexcept {
    auto& s = state();
    if (s.wanted.empty() && !s.own) {
        s.status = "nothing shown";
        return;
    }
    try {
        NoBailSkater skater;
        std::uintptr_t board{};
        if (no_bail_skater(skater)) {
            try {
                board = multiplayer::read_native_board(skater_items::readable, skater.base, skater.entity).entity;
            } catch (const std::exception&) {
                board = 0;
            }
        }
        if (!board) {
            // A respawn or a level change: the next board comes from the player's, with nothing of ours.
            s.board = 0;
            s.own = false;
            s.feed.reset();
            s.shown.clear();
            s.original.clear();
            s.dirty = true;
            s.status = "waiting for a board";
            return;
        }
        const auto now = GetTickCount64();
        const SkaterItems m{skater_items::readable, skater.base};
        if (board != s.board) {
            s.board = board;
            s.own = false;
            s.feed.reset();
            s.shown.clear();
            s.original.clear();
            s.dirty = true;
        }
        const bool menu = multiplayer::sample_game_ui_state(skater.base).in_menu;
        if (menu) s.menu_seen = now;
        if (s.wanted.empty() || menu) {
            if (s.own) give_back(s, m, now);
            s.status = s.wanted.empty() ? "nothing shown" : "a game menu is up: the board is the game's";
            s.dirty = true;
            return;
        }
        // Not before the game has had a second after a menu, nor more than once a second; while shown, a
        // look every 3 s whether the game put the player's own items back.
        if (now - s.given_back < 300 || now - s.menu_seen < 1000 || now - s.applied < 1000 || !(s.dirty || (s.own && now - s.checked > 3000)))
            return;
        s.checked = now;
        show(s, m, now);
        s.dirty = false;
        s.status = std::format("{} part(s) shown", s.shown.size());
    } catch (const std::exception& error) {
        s.status = std::string("waiting: ") + error.what();
        say(s, s.status);
    } catch (...) {
    }
}

std::string status() { return state().status; }
}
