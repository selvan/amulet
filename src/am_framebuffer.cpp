#include "amulet.h"

static int create_framebuffer(lua_State *L) {
    int nargs = am_check_nargs(L, 1);
    am_framebuffer *fb = am_new_userdata(L, am_framebuffer);
    fb->width = -1;
    fb->height = -1;
    fb->framebuffer_id = am_create_framebuffer();
    am_texture2d *texture = am_get_userdata(L, am_texture2d, 1);
    if (texture->buffer != NULL) {
        texture->buffer->update_if_dirty();
    }
    fb->color_attachment0 = texture;
    fb->color_attachment0_ref = fb->ref(L, 1);
    am_bind_framebuffer(fb->framebuffer_id);
    am_set_framebuffer_texture2d(AM_FRAMEBUFFER_COLOR_ATTACHMENT0, AM_TEXTURE_COPY_TARGET_2D, texture->texture_id);
    fb->width = texture->width;
    fb->height = texture->height;
    fb->depth_renderbuffer_id = 0;
    if (nargs > 1 && lua_toboolean(L, 2)) {
        fb->depth_renderbuffer_id = am_create_renderbuffer();
        am_bind_renderbuffer(fb->depth_renderbuffer_id);
        am_set_renderbuffer_storage(AM_RENDERBUFFER_FORMAT_DEPTH_COMPONENT16,
            fb->width, fb->height);
        am_set_framebuffer_renderbuffer(AM_FRAMEBUFFER_DEPTH_ATTACHMENT, fb->depth_renderbuffer_id);
        am_clear_framebuffer(false, true, false); // clear depth buffer
    }
    am_framebuffer_status status = am_check_framebuffer_status();
    if (status != AM_FRAMEBUFFER_STATUS_COMPLETE) {
        return luaL_error(L, "framebuffer incomplete");
    }
    return 1;
}

static int render_node_to_framebuffer(lua_State *L) {
    am_framebuffer *fb = am_get_userdata(L, am_framebuffer, 1);
    am_scene_node *node = am_get_userdata(L, am_scene_node, 2);
    am_render_state *rstate = &am_global_render_state;
    if (fb->color_attachment0->buffer != NULL) {
        fb->color_attachment0->buffer->update_if_dirty();
    }
    rstate->setup(fb->framebuffer_id, false, fb->width, fb->height, fb->depth_renderbuffer_id != 0);
    node->render(rstate);
    return 0;
}

static int framebuffer_gc(lua_State *L) {
    am_framebuffer *fb = am_get_userdata(L, am_framebuffer, 1);
    am_delete_framebuffer(fb->framebuffer_id);
    if (fb->depth_renderbuffer_id != 0) {
        am_delete_renderbuffer(fb->depth_renderbuffer_id);
    }
    return 0;
}

static int clear_framebuffer(lua_State *L) {
    int nargs = am_check_nargs(L, 1);
    am_framebuffer *fb = am_get_userdata(L, am_framebuffer, 1);
    bool clear_color = true;
    bool clear_depth = true;
    bool clear_stencil = true;
    if (nargs > 1) {
        clear_color = lua_toboolean(L, 2);
    }
    if (nargs > 2) {
        clear_depth = lua_toboolean(L, 3);
    }
    if (nargs > 3) {
        clear_stencil = lua_toboolean(L, 4);
    }
    am_bind_framebuffer(fb->framebuffer_id);
    am_clear_framebuffer(clear_color, clear_depth, clear_stencil);
    return 0;
}

static void register_framebuffer_mt(lua_State *L) {
    lua_newtable(L);

    am_set_default_index_func(L);
    am_set_default_newindex_func(L);

    lua_pushcclosure(L, framebuffer_gc, 0);
    lua_setfield(L, -2, "__gc");

    lua_pushcclosure(L, render_node_to_framebuffer, 0);
    lua_setfield(L, -2, "render");

    lua_pushcclosure(L, clear_framebuffer, 0);
    lua_setfield(L, -2, "clear");

    lua_pushstring(L, "framebuffer");
    lua_setfield(L, -2, "tname");

    am_register_metatable(L, MT_am_framebuffer, 0);
}

void am_open_framebuffer_module(lua_State *L) {
    luaL_Reg funcs[] = {
        {"framebuffer",    create_framebuffer},
        {NULL, NULL}
    };
    am_open_module(L, AMULET_LUA_MODULE_NAME, funcs);
    register_framebuffer_mt(L);
}