#include "amulet.h"

static void init_reserved_refs(lua_State *L);
static void init_metatable_registry(lua_State *L);
static void open_stdlualibs(lua_State *L);
static void init_require_func(lua_State *L);
static bool run_embedded_scripts(lua_State *L, bool worker);
static void set_arg_global(lua_State *L, int argc, char** argv);
static void am_set_version(lua_State *L);

am_engine *am_init_engine(bool worker, int argc, char** argv) {
#if defined(AM_LUAJIT) && defined(AM_64BIT)
    // luajit requires using luaL_newstate on 64 bit
    lua_State *L = luaL_newstate();
    if (L == NULL) return NULL;
    am_allocator *allocator = NULL;
#else
    am_allocator *allocator = am_new_allocator();
    lua_State *L = lua_newstate(&am_alloc, allocator);
    if (L == NULL) {
        am_destroy_allocator(allocator);
        return NULL;
    }
#endif
    am_engine *eng = new am_engine();
    eng->allocator = allocator;
    eng->L = L;
    eng->worker = worker;
    init_reserved_refs(L);
    init_metatable_registry(L);
    open_stdlualibs(L);
    set_arg_global(L, argc, argv);
    init_require_func(L);
    am_init_traceback_func(L);
    am_open_userdata_module(L);
    am_open_logging_module(L);
    am_open_math_module(L);
    am_open_time_module(L);
    am_open_buffer_module(L);
    am_open_json_module(L);
    am_open_utf8_module(L);
    if (!worker) {
        am_open_actions_module(L);
        am_open_window_module(L);
        am_open_scene_module(L);
        am_open_program_module(L);
        am_open_texture2d_module(L);
        am_open_framebuffer_module(L);
        am_open_image_module(L);
        am_open_model_module(L);
        am_open_depthbuffer_module(L);
        am_open_culling_module(L);
        am_open_blending_module(L);
        am_open_transforms_module(L);
        am_open_renderer_module(L);
        am_open_audio_module(L);
    }
    am_set_globals_metatable(L);
    am_set_version(L);
    if (!run_embedded_scripts(L, worker)) {
        lua_close(L);
        return NULL;
    }
    return eng;
}

void am_destroy_engine(am_engine *eng) {
    if (!eng->worker) {
        // Audio must be destroyed before closing the lua state, because
        // closing the lua state will destroy the root audio node.
        am_destroy_audio();
        am_destroy_windows(eng->L);
    }
    lua_close(eng->L);
    if (eng->allocator != NULL) {
        am_destroy_allocator(eng->allocator);
    }
    delete eng;
    am_reset_log_cache(); // XXX what about other running engines?
}

static void init_reserved_refs(lua_State *L) {
    int i, j;
    do {
        lua_pushboolean(L, 1);
        i = luaL_ref(L, LUA_REGISTRYINDEX);
    } while (i < AM_RESERVED_REFS_START - 1);
    if (i != AM_RESERVED_REFS_START - 1) {
        am_abort("Internal Error: AM_RESERVED_REFS_START too low\n");
    }
    for (i = AM_RESERVED_REFS_START; i < AM_RESERVED_REFS_END; i++) {
        lua_pushboolean(L, 1);
        j = luaL_ref(L, LUA_REGISTRYINDEX);
        if (i != j) {
            am_abort("Internal Error: non-contiguous refs\n");
        }
        j++;
    }
}

static void init_metatable_registry(lua_State *L) {
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawseti(L, LUA_REGISTRYINDEX, AM_METATABLE_REGISTRY);
    lua_setglobal(L, "_metatable_registry");
}

static void set_arg_global(lua_State *L, int argc, char** argv) {
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; i++) {
        char *arg = argv[i];
        lua_pushstring(L, arg);
        lua_rawseti(L, -2, i+1);
    }
    lua_setglobal(L, "arg");
}

static void open_stdlualibs(lua_State *L) {
    am_requiref(L, "base",      luaopen_base);
    am_requiref(L, "package",   luaopen_package);
    am_requiref(L, "math",      luaopen_math);
#if defined(AM_LUA52) || defined(AM_LUA53)
    // luajit and lua51 open coroutine in luaopen_base
    am_requiref(L, "coroutine", luaopen_coroutine);
#endif
    am_requiref(L, "string",    luaopen_string);
    am_requiref(L, "table",     luaopen_table);
    am_requiref(L, "os",        luaopen_os);
    am_requiref(L, "io",        luaopen_io);
    am_requiref(L, "debug",     luaopen_debug);

#ifdef AM_LUAJIT
    am_requiref(L, "ffi",       luaopen_ffi);
    am_requiref(L, "jit",       luaopen_jit);
#endif
}

static void init_require_func(lua_State *L) {
    lua_newtable(L);
    lua_rawseti(L, LUA_REGISTRYINDEX, AM_MODULE_TABLE);
    lua_pushcclosure(L, am_load_module, 0);
    lua_setglobal(L, "require");
}

static void am_set_version(lua_State *L) {
    lua_getglobal(L, AMULET_LUA_MODULE_NAME);
    lua_pushstring(L, am_version);
    lua_setfield(L, -2, "version");
    lua_pop(L, 1); // am table
}

#define MAX_CHUNKNAME_SIZE 100

static bool run_embedded_script(lua_State *L, const char *filename) {
    char chunkname[MAX_CHUNKNAME_SIZE];
    am_embedded_file_record *rec = am_get_embedded_file(filename);
    if (rec == NULL) {
        am_log0("embedded file '%s' missing", filename);
        return false;
    }
    snprintf(chunkname, MAX_CHUNKNAME_SIZE, "@embedded-%s", filename);
    int r = luaL_loadbuffer(L, (char*)rec->data, rec->len, chunkname);
    if (r == 0) {
        if (!am_call(L, 0, 0)) {
            return false;
        }
    } else {
        const char *msg = lua_tostring(L, -1);
        am_log0("%s", msg);
        return false;
    }
    return true;
}

static bool run_embedded_scripts(lua_State *L, bool worker) {
    return
        run_embedded_script(L, "lua/compat.lua") &&
        run_embedded_script(L, "lua/traceback.lua") &&
        run_embedded_script(L, "lua/setup.lua") &&
        run_embedded_script(L, "lua/type.lua") &&
        run_embedded_script(L, "lua/extra.lua") &&
        run_embedded_script(L, "lua/config.lua") &&
        run_embedded_script(L, "lua/time.lua") &&
        run_embedded_script(L, "lua/buffer.lua") &&
        run_embedded_script(L, "lua/shaders.lua") &&
        run_embedded_script(L, "lua/shapes.lua") &&
        run_embedded_script(L, "lua/text.lua") &&
        run_embedded_script(L, "lua/events.lua") &&
        run_embedded_script(L, "lua/actions.lua") &&
        run_embedded_script(L, "lua/audio.lua") &&
        run_embedded_script(L, "lua/tweens.lua") &&
        run_embedded_script(L, "lua/cameras.lua") &&
        run_embedded_script(L, "lua/default_font.lua");
}

