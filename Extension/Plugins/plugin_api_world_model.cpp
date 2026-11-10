#include "plugin_api.h"
#include "plugin_game.h"
#include "lualib.h"
#include <cctype>
#include <cmath>
#include <filesystem>

// reskate.world_model: a plugin's model drawn into the world (Extension/UI/Overlay/world_model.h): a .rsvm
// (contrib/viewmodel_export.py makes one) at rest, placed in the world each tick, each of its materials a
// colour and an opacity, hidden where the game's own surfaces are in front of it.
namespace dingosdk::plugins {
namespace {
void number_field(lua_State* L, int index, const char* name, float low, float high, float& out) {
    lua_getfield(L, index, name);
    if (!lua_isnil(L, -1)) {
        const double value = luaL_checknumber(L, -1);
        if (!(value >= low && value <= high)) luaL_error(L, "the world model's %s is %g to %g", name, static_cast<double>(low), static_cast<double>(high));
        out = static_cast<float>(value);
    }
    lua_pop(L, 1);
}
void vector_field(lua_State* L, int index, const char* name, float low, float high, Vec3& out) {
    lua_getfield(L, index, name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isvector(L, -1)) luaL_error(L, "the world model's %s is a vector", name);
        const float* v = lua_tovector(L, -1);
        for (int i = 0; i < 3; ++i)
            if (!(v[i] >= low && v[i] <= high)) luaL_error(L, "each part of the world model's %s is %g to %g", name, static_cast<double>(low), static_cast<double>(high));
        out = {v[0], v[1], v[2]};
    }
    lua_pop(L, 1);
}
// {colour = vector, opacity = n} at `index`, over `out`.
void material_at(lua_State* L, int index, Game::WorldModelMaterial& out) {
    vector_field(L, index, "colour", 0, 4, out.colour);
    number_field(L, index, "opacity", 0, 1, out.opacity);
}
} // namespace

struct WorldModelApi {
    static Plugin& caller(lua_State* L, const char* what) { return permitted(L, "world.model", what); }
    // load(path): reads the package's .rsvm file in the background; info().status says when it is ready.
    static int load(lua_State* L) {
        auto& plugin = caller(L, "reskate.world_model.load");
        const auto path = checked_text(L, 1, 200, "a world model's path");
        auto extension = std::filesystem::path(path).extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".rsvm") luaL_error(L, "a world model is a .rsvm file (contrib/viewmodel_export.py makes one)");
        const auto file = package_file(L, plugin, path);
        std::error_code code;
        if (!std::filesystem::is_regular_file(file, code)) luaL_error(L, "\"%s\" is not a file in the plugin's package", path.c_str());
        return result(L, game_of(plugin).world_model_load(plugin.id(), file));
    }
    static int unload(lua_State* L) {
        auto& plugin = caller(L, "reskate.world_model.unload");
        game_of(plugin).world_model_unload(plugin.id());
        return 0;
    }
    // info(): {status = "loading" | "ready" | "failed", why, materials = {names}, near = the game's near
    // distance as measured (nil until it is)}, or nil when this plugin has none loaded.
    static int info(lua_State* L) {
        auto& plugin = caller(L, "reskate.world_model.info");
        const auto found = game_of(plugin).world_model_info(plugin.id());
        if (!found) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 0, 4);
        field_text(L, "status", found->status);
        if (!found->why.empty()) field_text(L, "why", found->why);
        push_texts(L, found->materials);
        lua_setfield(L, -2, "materials");
        if (found->near) {
            lua_pushnumber(L, *found->near);
            lua_setfield(L, -2, "near");
        }
        return 1;
    }
    // show({position = vector, rotation = {x, y, z, w}, scale = 1, colour = vector, opacity = 0.5, glance = 0.5,
    // materials = {[name] = {colour, opacity}}}): drawn for the next 300 ms (so every tick). colour and
    // opacity are for a material not named in materials.
    static int show(lua_State* L) {
        auto& plugin = caller(L, "reskate.world_model.show");
        luaL_checktype(L, 1, LUA_TTABLE);
        Game::WorldModelLook look;
        lua_getfield(L, 1, "position");
        look.position = position_of(L, lua_gettop(L), "the world model's position");
        lua_pop(L, 1);
        lua_getfield(L, 1, "rotation");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            float length{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                const double v = luaL_checknumber(L, -1);
                if (!std::isfinite(v) || std::abs(v) > 1.01) luaL_error(L, "the world model's rotation is a unit quaternion {x, y, z, w}");
                look.rotation[static_cast<std::size_t>(i)] = static_cast<float>(v);
                length += static_cast<float>(v * v);
                lua_pop(L, 1);
            }
            if (!(length > 0.5f)) luaL_error(L, "the world model's rotation is a unit quaternion {x, y, z, w}");
        }
        lua_pop(L, 1);
        number_field(L, 1, "scale", 0.01f, 100, look.scale);
        number_field(L, 1, "glance", 0, 1, look.glance);
        material_at(L, 1, look.standard);
        lua_getfield(L, 1, "materials");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const int materials = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, materials)) {
                if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "the world model's materials are named");
                if (look.materials.size() >= 32) luaL_error(L, "at most 32 materials");
                luaL_checktype(L, -1, LUA_TTABLE);
                Game::WorldModelMaterial material = look.standard;
                material_at(L, lua_gettop(L), material);
                look.materials.emplace_back(lua_tostring(L, -2), material);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        return result(L, game_of(plugin).world_model_show(plugin.id(), std::move(look)));
    }
    static int hide(lua_State* L) {
        auto& plugin = caller(L, "reskate.world_model.hide");
        return result(L, game_of(plugin).world_model_show(plugin.id(), std::nullopt));
    }
};

void install_world_model_api(Plugin&, lua_State* L) {
    add_functions(L, "world_model", {{"load", WorldModelApi::load}, {"unload", WorldModelApi::unload}, {"info", WorldModelApi::info},
                                     {"show", WorldModelApi::show}, {"hide", WorldModelApi::hide}});
}
} // namespace dingosdk::plugins
