#include "lua/bind_gpu.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include "lua/gpu_api.h"
#include "lua/vm.h"
#include "timers.h"
#include <tracy/Tracy.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// lua_error longjmps (Lua is built as C): C++ objects live only in *_impl functions, which report errors
// through Err and return before luaL_error is called. lua_CFunction wrappers hold only trivial types.
struct Err {
  char msg[256];
};

static int fail(Err* e, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
  va_end(ap);
  return -1;
}

static const char* const BUF_MT = "pxb.buffer";
static const char* const PIPE_MT = "pxb.pipeline";
static const char* const TIMER_MT = "pxb.gpu_timer";
static const char* const TEX_MT = "pxb.texture";
static const char* const MIP_MT = "pxb.texture_mip";
static char g_key;

// Constructed in place in the Lua userdata; never destructed: __gc empties the optional (moved to the delete queue).
struct BufUD {
  std::optional<GpuBuffer> b; // empty once handed to the delete queue
};
struct PipeUD {
  std::optional<GpuPipeline> p;
};
struct TexUD {
  std::optional<GpuTexture> t; // empty once handed to the delete queue
};
struct MipUD {
  uint32_t mip; // user value 1: the texture userdata (keeps it alive)
};
struct TimerUD {
  uint32_t id; // GpuTimers::user index
};

static GpuLua* ctx(lua_State* L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &g_key);
  GpuLua* g = (GpuLua*)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return g;
}

// ---- deferred destroy ----

static void destroy_dead(GpuLua* g, GpuLua::Dead& d) {
  if (GpuBuffer* b = std::get_if<GpuBuffer>(&d.obj)) {
    g->gpu->buffer_destroy(g->dev, b);
  } else if (GpuPipeline* p = std::get_if<GpuPipeline>(&d.obj)) {
    g->gpu->pipeline_destroy(g->dev, p);
  } else {
    g->gpu->texture_destroy(g->dev, &std::get<GpuTexture>(d.obj));
  }
}

// idle = the device was just waited idle: everything submitted completed. The open frame is not submitted yet, but it
// cannot reference an object enqueued before it opened (that object was unreachable by then).
static void flush_dead(GpuLua* g, bool idle) {
  size_t keep = 0;
  for (size_t i = 0; i < g->dead.size(); i++) {
    GpuLua::Dead& d = g->dead[i];
    bool free_it = idle ? (!g->frame_open || d.tag < g->opens) : g->opens >= d.tag + PXB_FRAMES_IN_FLIGHT;
    if (free_it) {
      destroy_dead(g, d);
    } else {
      g->dead[keep++] = std::move(d);
    }
  }
  g->dead.resize(keep);
}

// The final resume (the run is over, gpu.present returned false) has no frame: calls that record into it raise this.
static void check_frame(lua_State* L, const char* fn) {
  if (!ctx(L)->frame_open) { luaL_error(L, "%s: no frame (the run has ended)", fn); }
}

// ---- argument helpers (no longjmp) ----

static int opt_enum(lua_State* L, int t, const char* key, const char* const* names, int* out, Err* e) {
  int ty = lua_getfield(L, t, key);
  int rc = 0;
  if (ty != LUA_TNIL) {
    if (ty != LUA_TSTRING) {
      rc = fail(e, "%s must be a string", key);
    } else {
      const char* s = lua_tostring(L, -1);
      int i = 0;
      while (names[i] && strcmp(names[i], s)) { i++; }
      if (!names[i]) {
        rc = fail(e, "bad %s '%s'", key, s);
      } else {
        *out = i;
      }
    }
  }
  lua_pop(L, 1);
  return rc;
}

static int opt_bool(lua_State* L, int t, const char* key, bool* out, Err* e) {
  int ty = lua_getfield(L, t, key);
  int rc = 0;
  if (ty == LUA_TBOOLEAN) {
    *out = lua_toboolean(L, -1);
  } else if (ty != LUA_TNIL) {
    rc = fail(e, "%s must be a boolean", key);
  }
  lua_pop(L, 1);
  return rc;
}

// Integer field; required when !has_default.
static int int_field(lua_State* L, int t, const char* key, bool required, lua_Integer* out, Err* e) {
  int ty = lua_getfield(L, t, key);
  int rc = 0;
  if (ty == LUA_TNIL) {
    if (required) { rc = fail(e, "%s is required", key); }
  } else {
    int isnum = 0;
    lua_Integer v = lua_tointegerx(L, -1, &isnum);
    if (ty != LUA_TNUMBER || !isnum) {
      rc = fail(e, "%s must be an integer", key);
    } else {
      *out = v;
    }
  }
  lua_pop(L, 1);
  return rc;
}

static GpuBuffer* test_buf(lua_State* L, int idx) {
  BufUD* u = (BufUD*)luaL_testudata(L, idx, BUF_MT);
  return u && u->b ? &*u->b : nullptr;
}

static GpuPipeline* test_pipe(lua_State* L, int idx) {
  PipeUD* u = (PipeUD*)luaL_testudata(L, idx, PIPE_MT);
  return u && u->p ? &*u->p : nullptr;
}

// Reads a table of numbers at stack index t into out (max n). Returns count or -1.
static int read_floats(lua_State* L, int t, float* out, int max) {
  int n = (int)lua_rawlen(L, t);
  if (n > max) { return -1; }
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, t, i);
    if (lua_type(L, -1) != LUA_TNUMBER) {
      lua_pop(L, 1);
      return -1;
    }
    out[i - 1] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  return n;
}

// Integer form of read_floats (or a single integer at t): each value must be a Lua integer in [0, 2^32). Returns
// count or -1.
static int read_u32s(lua_State* L, int t, uint32_t* out, int max) {
  bool single = lua_type(L, t) == LUA_TNUMBER;
  int n = single ? 1 : (int)lua_rawlen(L, t);
  if (n > max) { return -1; }
  for (int i = 1; i <= n; i++) {
    if (!single) { lua_rawgeti(L, t, i); }
    int isnum = 0;
    lua_Integer v = lua_type(L, single ? t : -1) == LUA_TNUMBER ? lua_tointegerx(L, single ? t : -1, &isnum) : 0;
    if (!single) { lua_pop(L, 1); }
    if (!isnum || v < 0 || v > (lua_Integer)UINT32_MAX) { return -1; }
    out[i - 1] = (uint32_t)v;
  }
  return n;
}

// ---- gpu.buffer ----

static const char* const MODE_NAMES[] = {"staged", "mapped", nullptr};

static int usage_bit(const char* s, uint32_t* out) {
  static const char* const names[] = {"vertex", "index", "uniform", "storage", "indirect"};
  for (int i = 0; i < 5; i++) {
    if (!strcmp(names[i], s)) {
      *out |= 1u << i; // GPU_BUF_VERTEX..INDIRECT are 1,2,4,8,16
      return 1;
    }
  }
  return 0;
}

static int buffer_impl(lua_State* L, GpuLua* g, Err* e) {
  GpuBufferDesc desc;
  lua_Integer size = 0;
  if (int_field(L, 1, "size", true, &size, e) < 0) { return -1; }
  if (size <= 0) { return fail(e, "size must be > 0"); }
  desc.size = (VkDeviceSize)size;
  int mode = 0;
  if (opt_enum(L, 1, "mode", MODE_NAMES, &mode, e) < 0) { return -1; }
  desc.mode = mode == 0 ? GpuBufferMode::staged : GpuBufferMode::mapped;

  int ty = lua_getfield(L, 1, "usage");
  if (ty == LUA_TSTRING) {
    if (!usage_bit(lua_tostring(L, -1), &desc.usage)) { return fail(e, "bad usage '%s'", lua_tostring(L, -1)); }
  } else if (ty == LUA_TTABLE) {
    int n = (int)lua_rawlen(L, -1);
    for (int i = 1; i <= n; i++) {
      lua_rawgeti(L, -1, i);
      if (lua_type(L, -1) != LUA_TSTRING || !usage_bit(lua_tostring(L, -1), &desc.usage)) {
        return fail(e, "bad usage entry #%d", i);
      }
      lua_pop(L, 1);
    }
  } else {
    return fail(e, "usage must be a string or a list of strings");
  }
  lua_pop(L, 1);
  if (desc.usage == 0) { return fail(e, "usage is empty"); }

  GpuBuffer tmp;
  if (!g->gpu->buffer_create(g->dev, desc, &tmp)) { return fail(e, "buffer creation failed"); }
  new (lua_newuserdatauv(L, sizeof(BufUD), 0)) BufUD{std::move(tmp)};
  luaL_setmetatable(L, BUF_MT);
  return 1;
}

static int l_buffer(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Err e;
  if (buffer_impl(L, ctx(L), &e) < 0) { return luaL_error(L, "gpu.buffer: %s", e.msg); }
  return 1;
}

static GpuBuffer* check_buf(lua_State* L, int idx) {
  BufUD* u = (BufUD*)luaL_checkudata(L, idx, BUF_MT);
  if (!u->b) { luaL_error(L, "buffer destroyed"); }
  return &*u->b;
}

static int buf_open(lua_State* L) {
  GpuBuffer* b = check_buf(L, 1);
  check_frame(L, "buf:open");
  GpuLua* g = ctx(L);
  if (g->in_pass) { return luaL_error(L, "buf:open inside a pass"); }
  // staged: the frame is open (its slot fence waited) before the slot's staging buffer is rewritten
  if (!g->gpu->buffer_open(g->dev, b, g->sc->frame)) { return luaL_error(L, "buf:open failed"); }
  return 0;
}

static int write_impl(lua_State* L, GpuBuffer* b, lua_Integer off, Err* e) {
  GpuLua* g = ctx(L);
  if (lua_type(L, 3) == LUA_TSTRING) {
    size_t len;
    const char* s = lua_tolstring(L, 3, &len);
    if (!g->gpu->buffer_write(b, (VkDeviceSize)off, s, len)) { return fail(e, "write failed"); }
    return 0;
  }
  if (lua_type(L, 3) != LUA_TTABLE) { return fail(e, "data must be a string or a table of numbers"); }
  size_t n = lua_rawlen(L, 3);
  std::vector<float> v(n);
  for (size_t i = 0; i < n; i++) {
    lua_rawgeti(L, 3, (lua_Integer)i + 1);
    if (lua_type(L, -1) != LUA_TNUMBER) { return fail(e, "data[%zu] is not a number", i + 1); }
    v[i] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  if (!g->gpu->buffer_write(b, (VkDeviceSize)off, v.data(), v.size() * sizeof(float))) {
    return fail(e, "write failed");
  }
  return 0;
}

static int buf_write(lua_State* L) {
  GpuBuffer* b = check_buf(L, 1);
  lua_Integer off = luaL_checkinteger(L, 2);
  if (ctx(L)->in_pass) { return luaL_error(L, "buf:write inside a pass"); }
  if (off < 0) { return luaL_error(L, "buf:write: negative offset"); }
  Err e;
  if (write_impl(L, b, off, &e) < 0) { return luaL_error(L, "buf:write: %s", e.msg); }
  return 0;
}

static int buf_close(lua_State* L) {
  GpuBuffer* b = check_buf(L, 1);
  check_frame(L, "buf:close");
  GpuLua* g = ctx(L);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (g->in_pass) { return luaL_error(L, "buf:close inside a pass"); }
  if (b->mode == GpuBufferMode::staged) { cmd = g->cmd; }
  if (!g->gpu->buffer_close(b, cmd)) { return luaL_error(L, "buf:close failed"); }
  return 0;
}

static int buf_gc(lua_State* L) {
  BufUD* u = (BufUD*)lua_touserdata(L, 1);
  if (u->b) {
    GpuLua* g = ctx(L);
    g->dead.push_back({std::move(*u->b), g->opens});
    u->b.reset();
  }
  return 0;
}

// ---- gpu.texture ----

static void mark_dirty(GpuLua* g, GpuTexture* t) {
  if (std::find(g->dirty.begin(), g->dirty.end(), t) == g->dirty.end()) { g->dirty.push_back(t); }
}

static int texture_impl(lua_State* L, GpuLua* g, Err* e) {
  GpuTextureDesc desc;
  desc.format = VK_FORMAT_R8G8B8A8_UNORM;
  desc.storage = true;
  lua_Integer w = 0, h = 0;
  if (int_field(L, 1, "width", true, &w, e) < 0 || int_field(L, 1, "height", true, &h, e) < 0) { return -1; }
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { return fail(e, "width and height must be in 1..16384"); }
  desc.width = (uint32_t)w;
  desc.height = (uint32_t)h;
  uint32_t full = 1;
  for (uint32_t m = desc.width > desc.height ? desc.width : desc.height; m > 1; m >>= 1) { full++; }
  int ty = lua_getfield(L, 1, "mips");
  if (ty == LUA_TSTRING && !strcmp(lua_tostring(L, -1), "full")) {
    desc.mip_levels = full;
  } else if (ty != LUA_TNIL) {
    int isnum = 0;
    lua_Integer n = lua_tointegerx(L, -1, &isnum);
    if (ty != LUA_TNUMBER || !isnum) { return fail(e, "mips must be an integer or \"full\""); }
    if (n < 1 || n > (lua_Integer)full) { return fail(e, "mips must be in 1..%u", full); }
    desc.mip_levels = (uint32_t)n;
  }
  lua_pop(L, 1);
  if (desc.mip_levels > PXB_MAX_MIPS) { return fail(e, "at most %u mips", PXB_MAX_MIPS); }

  GpuTexture tmp;
  if (!g->gpu->texture_create(g->dev, desc, &tmp)) { return fail(e, "texture creation failed"); }
  new (lua_newuserdatauv(L, sizeof(TexUD), 0)) TexUD{std::move(tmp)};
  luaL_setmetatable(L, TEX_MT);
  return 1;
}

static int l_texture(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Err e;
  if (texture_impl(L, ctx(L), &e) < 0) { return luaL_error(L, "gpu.texture: %s", e.msg); }
  return 1;
}

static GpuTexture* check_tex(lua_State* L, int idx) {
  TexUD* u = (TexUD*)luaL_checkudata(L, idx, TEX_MT);
  if (!u->t) { luaL_error(L, "texture destroyed"); }
  return &*u->t;
}

// Optional 0-based mip argument at idx (default 0), checked against the texture.
static uint32_t check_mip(lua_State* L, GpuTexture* t, int idx) {
  lua_Integer mip = luaL_optinteger(L, idx, 0);
  if (mip < 0 || mip >= (lua_Integer)t->mip_levels) {
    luaL_error(L, "mip %d out of range (%d mips)", (int)mip, (int)t->mip_levels);
  }
  return (uint32_t)mip;
}

static int tex_upload(lua_State* L) {
  GpuTexture* t = check_tex(L, 1);
  size_t len;
  const char* data = luaL_checklstring(L, 2, &len);
  uint32_t mip = check_mip(L, t, 3);
  check_frame(L, "tex:upload");
  GpuLua* g = ctx(L);
  if (g->in_pass) { return luaL_error(L, "tex:upload inside a pass"); }
  GpuBuffer staging;
  if (!g->gpu->texture_upload(g->dev, t, mip, data, len, g->cmd, &staging)) {
    return luaL_error(L, "tex:upload failed");
  }
  mark_dirty(g, t);
  g->dead.push_back({std::move(staging), g->opens});
  return 0;
}

static int tex_size(lua_State* L) {
  GpuTexture* t = check_tex(L, 1);
  uint32_t mip = check_mip(L, t, 2);
  lua_pushinteger(L, t->extent.width >> mip ? t->extent.width >> mip : 1);
  lua_pushinteger(L, t->extent.height >> mip ? t->extent.height >> mip : 1);
  return 2;
}

static int tex_mips(lua_State* L) {
  lua_pushinteger(L, check_tex(L, 1)->mip_levels);
  return 1;
}

static int tex_mip(lua_State* L) {
  GpuTexture* t = check_tex(L, 1);
  lua_Integer mip = luaL_checkinteger(L, 2);
  if (mip < 0 || mip >= (lua_Integer)t->mip_levels) {
    return luaL_error(L, "mip %d out of range (%d mips)", (int)mip, (int)t->mip_levels);
  }
  MipUD* m = (MipUD*)lua_newuserdatauv(L, sizeof(MipUD), 1);
  m->mip = (uint32_t)mip;
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  luaL_setmetatable(L, MIP_MT);
  return 1;
}

static int tex_gc(lua_State* L) {
  TexUD* u = (TexUD*)lua_touserdata(L, 1);
  if (u->t) {
    GpuLua* g = ctx(L);
    g->dirty.erase(std::remove(g->dirty.begin(), g->dirty.end(), &*u->t), g->dirty.end());
    g->dead.push_back({std::move(*u->t), g->opens});
    u->t.reset();
  }
  return 0;
}

// ---- gpu.blit ----

static const char* const FILTER_NAMES[] = {"linear", "nearest", nullptr};

static int blit_impl(lua_State* L, GpuLua* g, Err* e) {
  GpuTexture* tex[2];
  lua_Integer mip[2] = {0, 0};
  const char* keys[2] = {"src", "dst"};
  const char* mip_keys[2] = {"src_mip", "dst_mip"};
  for (int i = 0; i < 2; i++) {
    lua_getfield(L, 1, keys[i]);
    TexUD* u = (TexUD*)luaL_testudata(L, -1, TEX_MT);
    if (!u || !u->t) { return fail(e, "%s must be a texture", keys[i]); }
    tex[i] = &*u->t;
    lua_pop(L, 1);
    if (int_field(L, 1, mip_keys[i], false, &mip[i], e) < 0) { return -1; }
    if (mip[i] < 0 || mip[i] >= (lua_Integer)tex[i]->mip_levels) {
      return fail(e, "%s %d out of range (%d mips)", mip_keys[i], (int)mip[i], (int)tex[i]->mip_levels);
    }
  }
  if (tex[0] == tex[1] && mip[0] == mip[1]) { return fail(e, "src and dst are the same mip"); }
  int filter = 0;
  if (opt_enum(L, 1, "filter", FILTER_NAMES, &filter, e) < 0) { return -1; }
  if (g->in_pass) { return fail(e, "inside a pass"); }
  if (!g->gpu->texture_blit(g->cmd, tex[0], (uint32_t)mip[0], tex[1], (uint32_t)mip[1],
                            filter == 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST)) {
    return fail(e, "blit failed");
  }
  mark_dirty(g, tex[1]);
  return 0;
}

static int l_blit(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  check_frame(L, "gpu.blit");
  Err e;
  if (blit_impl(L, ctx(L), &e) < 0) { return luaL_error(L, "gpu.blit: %s", e.msg); }
  return 0;
}

// ---- gpu.pipeline ----

static const char* const TOPO_NAMES[] = {"triangles", "lines", "points", nullptr};
static const char* const CULL_NAMES[] = {"none", "back", "front", nullptr};
static const char* const BLEND_NAMES[] = {"none", "alpha", nullptr};

static int read_formats(lua_State* L, int t, const char* key, std::vector<GpuVertexFormat>* out, Err* e) {
  int ty = lua_getfield(L, t, key);
  if (ty == LUA_TNIL) {
    lua_pop(L, 1);
    return 0;
  }
  if (ty != LUA_TTABLE) { return fail(e, "%s must be a list of strings", key); }
  int n = (int)lua_rawlen(L, -1);
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, -1, i);
    const char* s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    if (strlen(s) != 6 || strncmp(s, "float", 5) || s[5] < '1' || s[5] > '4') {
      return fail(e, "%s[%d]: expected \"float1\"..\"float4\"", key, i);
    }
    out->push_back((GpuVertexFormat)(s[5] - '1'));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return 0;
}

static bool field_present(lua_State* L, int t, const char* key) {
  bool present = lua_getfield(L, t, key) != LUA_TNIL;
  lua_pop(L, 1);
  return present;
}

static int str_field(lua_State* L, int t, const char* key, std::string* out, Err* e) {
  if (lua_getfield(L, t, key) != LUA_TSTRING) { return fail(e, "%s must be a string (GLSL source)", key); }
  *out = lua_tostring(L, -1);
  lua_pop(L, 1);
  return 0;
}

// Returns 1 (userdata pushed), 2 (nil, err pushed), or -1 (bad arguments, e filled).
static int pipeline_impl(lua_State* L, GpuLua* g, Err* e) {
  GpuPipelineDesc desc;
  std::string vs, fs, cs;
  bool compute = field_present(L, 1, "cs");
  if (compute) {
    static const char* const graphics_only[] = {"vs",       "fs",   "vertex_layout", "instance_layout",
                                                "topology", "cull", "blend",         "depth"};
    for (const char* k : graphics_only) {
      if (field_present(L, 1, k)) { return fail(e, "%s cannot be used with cs", k); }
    }
    if (str_field(L, 1, "cs", &cs, e) < 0) { return -1; }
    desc.cs = cs.c_str();
  } else {
    if (str_field(L, 1, "vs", &vs, e) < 0 || str_field(L, 1, "fs", &fs, e) < 0) { return -1; }
    desc.vs = vs.c_str();
    desc.fs = fs.c_str();
  }

  std::vector<GpuFieldDecl> decls;
  int ty = lua_getfield(L, 1, "push");
  if (ty == LUA_TTABLE) {
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
        return fail(e, "push must map names to type strings");
      }
      decls.push_back({lua_tostring(L, -2), lua_tostring(L, -1)});
      lua_pop(L, 1);
    }
  } else if (ty != LUA_TNIL) {
    return fail(e, "push must be a table");
  }
  lua_pop(L, 1);

  {
    ty = lua_getfield(L, 1, "bindings");
    if (ty == LUA_TTABLE) {
      lua_pushnil(L);
      while (lua_next(L, -2)) {
        const char* kind = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        if (lua_type(L, -2) != LUA_TSTRING) { return fail(e, "bindings must map names to \"texture\" or \"image\""); }
        if (!strcmp(kind, "texture")) {
          desc.bindings.push_back({lua_tostring(L, -2), GpuBindingKind::texture});
        } else if (!strcmp(kind, "image") && compute) {
          desc.bindings.push_back({lua_tostring(L, -2), GpuBindingKind::image});
        } else if (!strcmp(kind, "image")) {
          return fail(e, "bindings.%s: \"image\" is only allowed on compute pipelines", lua_tostring(L, -2));
        } else {
          return fail(e, "bindings.%s: expected \"texture\" or \"image\"", lua_tostring(L, -2));
        }
        lua_pop(L, 1);
      }
      // binding index = position in ascending name order
      std::sort(desc.bindings.begin(), desc.bindings.end(),
                [](const GpuBindingDecl& a, const GpuBindingDecl& b) { return a.name < b.name; });
    } else if (ty != LUA_TNIL) {
      return fail(e, "bindings must be a table");
    }
    lua_pop(L, 1);
  }
  if (!compute) {
    if (read_formats(L, 1, "vertex_layout", &desc.vertex_layout, e) < 0) { return -1; }
    if (read_formats(L, 1, "instance_layout", &desc.instance_layout, e) < 0) { return -1; }
    int topo = 0, cull = 0, blend = 0;
    if (opt_enum(L, 1, "topology", TOPO_NAMES, &topo, e) < 0 || opt_enum(L, 1, "cull", CULL_NAMES, &cull, e) < 0 ||
        opt_enum(L, 1, "blend", BLEND_NAMES, &blend, e) < 0) {
      return -1;
    }
    desc.topology = (GpuTopology)topo;
    desc.cull = (GpuCull)cull;
    desc.blend = (GpuBlend)blend;
    bool depth = false;
    if (opt_bool(L, 1, "depth", &depth, e) < 0) { return -1; }
    desc.depth_test = desc.depth_write = depth;
    desc.depth_format = depth ? GPU_PASS_DEPTH_FORMAT : VK_FORMAT_UNDEFINED;
    desc.color_format = g->sc->format.format;
  }

  std::string err;
  if (!g->gpu->layout_build("push", decls, &desc.push, &err)) {
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  GpuPipeline pipe;
  if (!g->gpu->pipeline_create(g->dev, desc, &pipe, &err)) {
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  new (lua_newuserdatauv(L, sizeof(PipeUD), 0)) PipeUD{std::move(pipe)};
  luaL_setmetatable(L, PIPE_MT);
  return 1;
}

static int l_pipeline(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  Err e;
  int n = pipeline_impl(L, ctx(L), &e);
  if (n < 0) { return luaL_error(L, "gpu.pipeline: %s", e.msg); }
  return n;
}

static int pipe_gc(lua_State* L) {
  PipeUD* u = (PipeUD*)lua_touserdata(L, 1);
  if (u->p) {
    GpuLua* g = ctx(L);
    g->dead.push_back({std::move(*u->p), g->opens});
    u->p.reset();
  }
  return 0;
}

// ---- passes and draws ----

static int begin_pass_impl(lua_State* L, GpuLua* g, Err* e) {
  GpuPassDesc d;
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    if (lua_type(L, 1) != LUA_TTABLE) { return fail(e, "argument must be a table"); }
    int ty = lua_getfield(L, 1, "clear");
    if (ty == LUA_TTABLE) {
      if (read_floats(L, lua_gettop(L), d.clear_color, 4) != 4) { return fail(e, "clear must be {r, g, b, a}"); }
      d.clear = true;
    } else if (ty != LUA_TNIL) {
      return fail(e, "clear must be {r, g, b, a}");
    }
    lua_pop(L, 1);
    if (opt_bool(L, 1, "depth", &d.depth, e) < 0) { return -1; }
  }
  if (g->in_pass) { return fail(e, "pass already open"); }
  for (GpuTexture* t : g->dirty) { g->gpu->ensure_state_tex(g->cmd, t, {vkc::SAMPLED_FRAGMENT}); }
  g->dirty.clear();
  if (!g->gpu->pass_begin(g->dev, g->sc, &g->pass, g->cmd, d)) { return fail(e, "begin_pass failed"); }
  g->in_pass = true;
  return 0;
}

static int l_begin_pass(lua_State* L) {
  check_frame(L, "gpu.begin_pass");
  Err e;
  if (begin_pass_impl(L, ctx(L), &e) < 0) { return luaL_error(L, "gpu.begin_pass: %s", e.msg); }
  return 0;
}

static int l_end_pass(lua_State* L) {
  GpuLua* g = ctx(L);
  if (!g->in_pass) { return luaL_error(L, "gpu.end_pass: no pass open"); }
  if (g->pass.cmd && !g->gpu->pass_end(&g->pass)) { return luaL_error(L, "gpu.end_pass failed"); }
  g->in_pass = false;
  return 0;
}

// Packs the push table on top of the stack into block per pipe->push. Returns 0 or -1 (via e).
static int pack_push(lua_State* L, GpuPipeline* pipe, uint8_t* block, Err* e) {
  GpuLua* g = ctx(L);
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    if (lua_type(L, -2) != LUA_TSTRING) { return fail(e, "push keys must be strings"); }
    const char* name = lua_tostring(L, -2);
    std::string err;
    if (g->gpu->layout_field_is_int(pipe->push, name)) {
      uint32_t ivals[4];
      int n = -1;
      if (lua_type(L, -1) == LUA_TNUMBER || lua_type(L, -1) == LUA_TTABLE) {
        n = read_u32s(L, lua_gettop(L), ivals, 4);
      }
      if (n < 0) { return fail(e, "push.%s must be an integer or a list of up to 4 integers in [0, 2^32)", name); }
      if (!g->gpu->layout_pack_u32(pipe->push, name, ivals, (uint32_t)n, block, &err)) {
        return fail(e, "push: %s", err.c_str());
      }
      lua_pop(L, 1);
      continue;
    }
    float vals[16];
    int n;
    if (lua_type(L, -1) == LUA_TNUMBER) {
      vals[0] = (float)lua_tonumber(L, -1);
      n = 1;
    } else if (lua_type(L, -1) == LUA_TTABLE) {
      n = read_floats(L, lua_gettop(L), vals, 16);
      if (n < 0) { return fail(e, "push.%s must be a number or a list of up to 16 numbers", name); }
    } else {
      return fail(e, "push.%s must be a number or a list of numbers", name);
    }
    if (!g->gpu->layout_pack(pipe->push, name, vals, (uint32_t)n, block, &err)) {
      return fail(e, "push: %s", err.c_str());
    }
    lua_pop(L, 1);
  }
  return 0;
}

struct DispatchBind {
  GpuTexture* tex;
  uint32_t mip;
  bool image;
};

// Parses the `bind` field of the table at index 1 against pipe->bindings (shared by gpu.draw and gpu.dispatch).
static int parse_bind(lua_State* L, GpuPipeline* pipe, std::vector<DispatchBind>* out, Err* e) {
  std::vector<DispatchBind>& binds = *out;
  binds.assign(pipe->bindings.size(), DispatchBind{nullptr, 0, false});
  int bty = lua_getfield(L, 1, "bind");
  if (bty != LUA_TNIL && bty != LUA_TTABLE) { return fail(e, "bind must be a table"); }
  if (bty == LUA_TNIL && !pipe->bindings.empty()) { return fail(e, "bind is required (pipeline has bindings)"); }
  if (bty == LUA_TTABLE) {
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      if (lua_type(L, -2) != LUA_TSTRING) { return fail(e, "bind keys must be strings"); }
      const char* name = lua_tostring(L, -2);
      size_t k = 0;
      while (k < pipe->bindings.size() && pipe->bindings[k].name != name) { k++; }
      if (k == pipe->bindings.size()) { return fail(e, "bind.%s: no such binding in the pipeline", name); }
      DispatchBind& b = binds[k];
      if (pipe->bindings[k].kind == GpuBindingKind::texture) {
        TexUD* u = (TexUD*)luaL_testudata(L, -1, TEX_MT);
        if (!u || !u->t) { return fail(e, "bind.%s must be a (live) texture", name); }
        b = {&*u->t, 0, false};
      } else {
        MipUD* m = (MipUD*)luaL_testudata(L, -1, MIP_MT);
        if (!m) { return fail(e, "bind.%s must be a texture mip (tex:mip(i))", name); }
        lua_getiuservalue(L, -1, 1);
        TexUD* u = (TexUD*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        if (!u || !u->t) { return fail(e, "bind.%s: texture destroyed", name); }
        b = {&*u->t, m->mip, true};
      }
      lua_pop(L, 1);
    }
    for (size_t k = 0; k < binds.size(); k++) {
      if (!binds[k].tex) { return fail(e, "bind.%s is missing", pipe->bindings[k].name.c_str()); }
    }
    for (size_t i = 0; i < binds.size(); i++) {
      for (size_t j = 0; j < binds.size(); j++) {
        if (binds[i].tex == binds[j].tex && !binds[i].image && binds[j].image) {
          return fail(e, "bind.%s and bind.%s use the same texture as texture and image",
                      pipe->bindings[i].name.c_str(), pipe->bindings[j].name.c_str());
        }
      }
    }
  }
  lua_pop(L, 1);
  return 0;
}

static int draw_impl(lua_State* L, GpuLua* g, Err* e) {
  if (!g->in_pass) { return fail(e, "outside a pass"); }
  GpuDrawDesc d;
  lua_getfield(L, 1, "pipeline");
  GpuPipeline* pipe = test_pipe(L, -1);
  lua_pop(L, 1);
  if (!pipe) { return fail(e, "pipeline must be a (live) pipeline"); }
  if (pipe->compute) { return fail(e, "compute pipeline in gpu.draw"); }
  d.pipeline = pipe;

  if (lua_getfield(L, 1, "vertex_buffers") != LUA_TTABLE) { return fail(e, "vertex_buffers must be {vb[, inst]}"); }
  lua_rawgeti(L, -1, 1);
  d.vertex_buffers[0] = test_buf(L, -1);
  lua_pop(L, 1);
  if (!d.vertex_buffers[0]) { return fail(e, "vertex_buffers[1] must be a (live) buffer"); }
  if (lua_rawgeti(L, -1, 2) != LUA_TNIL) {
    d.vertex_buffers[1] = test_buf(L, -1);
    if (!d.vertex_buffers[1]) { return fail(e, "vertex_buffers[2] must be a (live) buffer"); }
  }
  lua_pop(L, 2);

  lua_getfield(L, 1, "index_buffer");
  d.index_buffer = test_buf(L, -1);
  lua_pop(L, 1);
  if (!d.index_buffer) { return fail(e, "index_buffer must be a (live) buffer"); }

  static const char* const idx_names[] = {"u16", "u32", nullptr};
  int idx = -1;
  if (lua_getfield(L, 1, "index_type") == LUA_TNIL) { return fail(e, "index_type is required (\"u16\"|\"u32\")"); }
  lua_pop(L, 1);
  if (opt_enum(L, 1, "index_type", idx_names, &idx, e) < 0) { return -1; }
  d.index_type = idx == 0 ? GpuIndexType::u16 : GpuIndexType::u32;

  // indirect = buffer of draw_count VkDrawIndexedIndirectCommand (20 B each); replaces count / instances
  lua_getfield(L, 1, "indirect");
  bool indirect = !lua_isnil(L, -1);
  d.indirect = indirect ? test_buf(L, -1) : nullptr;
  lua_pop(L, 1);
  if (indirect) {
    if (!d.indirect) { return fail(e, "indirect must be a (live) buffer"); }
    if (!(d.indirect->usage & GPU_BUF_INDIRECT)) { return fail(e, "indirect buffer needs usage \"indirect\""); }
    lua_Integer n = 0;
    if (int_field(L, 1, "draw_count", true, &n, e) < 0) { return -1; }
    if (n < 0) { return fail(e, "draw_count must be >= 0"); }
    if (n > 1 && !g->dev->multi_draw_indirect) {
      return fail(e, "draw_count > 1 needs multiDrawIndirect + drawIndirectFirstInstance (not supported)");
    }
    if (n > (lua_Integer)g->dev->props.limits.maxDrawIndirectCount) {
      return fail(e, "draw_count %lld > maxDrawIndirectCount %u", (long long)n,
                  g->dev->props.limits.maxDrawIndirectCount);
    }
    if ((VkDeviceSize)n * sizeof(VkDrawIndexedIndirectCommand) > d.indirect->size) {
      return fail(e, "indirect buffer too small for draw_count %lld", (long long)n);
    }
    d.draw_count = (uint32_t)n;
  } else {
    lua_Integer count = 0, inst = 1;
    if (int_field(L, 1, "count", true, &count, e) < 0 || int_field(L, 1, "instances", false, &inst, e) < 0) {
      return -1;
    }
    if (count < 0 || inst < 0) { return fail(e, "count and instances must be >= 0"); }
    d.index_count = (uint32_t)count;
    d.instance_count = (uint32_t)inst;
  }

  std::vector<DispatchBind> binds;
  if (parse_bind(L, pipe, &binds, e) < 0) { return -1; }

  uint8_t block[128] = {};
  int ty = lua_getfield(L, 1, "push");
  bool have_push = ty == LUA_TTABLE;
  if (ty != LUA_TNIL && !have_push) { return fail(e, "push must be a table"); }
  if (have_push && pack_push(L, pipe, block, e) < 0) { return -1; }
  lua_pop(L, 1);
  d.push = pipe->push.size > 0 ? block : nullptr;
  if (!binds.empty()) {
    std::vector<VkImageView> views(binds.size());
    for (size_t k = 0; k < binds.size(); k++) {
      const GpuTexture* t = binds[k].tex;
      bool ready = true;
      for (uint32_t m = 0; m < t->mip_levels; m++) {
        if (vkc::layout_of(t->state[m].uses) != vkc::Layout::SHADER_READ_ONLY) { ready = false; }
      }
      if (!ready) {
        return fail(e, "bind.%s: texture not readable (never written, or written inside the pass)",
                    pipe->bindings[k].name.c_str());
      }
      views[k] = t->view;
    }
    VkDescriptorSet set = g->gpu->descriptors_alloc(g->dev, &g->descriptors, g->sc->frame, pipe->set_layout);
    if (!set) { return fail(e, "descriptor set allocation failed"); }
    g->gpu->descriptors_write(g->dev, set, pipe->bindings, views.data());
    vkCmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->layout, 0, 1, &set, 0, nullptr);
  }
  if (!g->gpu->draw(&g->pass, d)) { return fail(e, "draw failed"); }
  return 0;
}

static int l_draw(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  check_frame(L, "gpu.draw");
  Err e;
  if (draw_impl(L, ctx(L), &e) < 0) { return luaL_error(L, "gpu.draw: %s", e.msg); }
  return 0;
}

// ---- gpu.dispatch ----

static int dispatch_impl(lua_State* L, GpuLua* g, Err* e) {
  lua_getfield(L, 1, "pipeline");
  GpuPipeline* pipe = test_pipe(L, -1);
  lua_pop(L, 1);
  if (!pipe) { return fail(e, "pipeline must be a (live) pipeline"); }
  if (!pipe->compute) { return fail(e, "graphics pipeline in gpu.dispatch"); }

  uint32_t groups[3] = {1, 1, 1};
  if (lua_getfield(L, 1, "groups") == LUA_TNIL) { return fail(e, "groups is required"); }
  int n = lua_type(L, -1) == LUA_TTABLE ? read_u32s(L, lua_gettop(L), groups, 3) : -1;
  lua_pop(L, 1);
  if (n < 1) { return fail(e, "groups must be a list of 1 to 3 integers"); }
  for (int i = n; i < 3; i++) { groups[i] = 1; }
  for (int i = 0; i < 3; i++) {
    if (groups[i] < 1) { return fail(e, "groups must be >= 1"); }
  }

  std::vector<DispatchBind> binds;
  if (parse_bind(L, pipe, &binds, e) < 0) { return -1; }

  uint8_t block[128] = {};
  int ty = lua_getfield(L, 1, "push");
  if (ty != LUA_TNIL && ty != LUA_TTABLE) { return fail(e, "push must be a table"); }
  if (ty == LUA_TTABLE && pack_push(L, pipe, block, e) < 0) { return -1; }
  lua_pop(L, 1);

  if (g->in_pass) { return fail(e, "inside a pass"); }

  std::vector<VkImageView> views(binds.size());
  for (size_t k = 0; k < binds.size(); k++) {
    const DispatchBind& b = binds[k];
    if (b.image) {
      g->gpu->ensure_state_mips(g->cmd, b.tex, b.mip, 1, {vkc::STORAGE_RW_COMPUTE});
      views[k] = b.tex->mip_views[b.mip];
      mark_dirty(g, b.tex);
    } else {
      g->gpu->ensure_state_mips(g->cmd, b.tex, 0, b.tex->mip_levels, {vkc::SAMPLED_COMPUTE});
      views[k] = b.tex->view;
    }
  }
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!binds.empty()) {
    set = g->gpu->descriptors_alloc(g->dev, &g->descriptors, g->sc->frame, pipe->set_layout);
    if (!set) { return fail(e, "descriptor set allocation failed"); }
    g->gpu->descriptors_write(g->dev, set, pipe->bindings, views.data());
  }
  vkCmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->h);
  if (set) { vkCmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0, 1, &set, 0, nullptr); }
  if (pipe->push.size > 0) {
    vkCmdPushConstants(g->cmd, pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pipe->push.size, block);
  }
  vkCmdDispatch(g->cmd, groups[0], groups[1], groups[2]);
  return 0;
}

static int l_dispatch(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  check_frame(L, "gpu.dispatch");
  Err e;
  if (dispatch_impl(L, ctx(L), &e) < 0) { return luaL_error(L, "gpu.dispatch: %s", e.msg); }
  return 0;
}

// ---- present / idle ----

static void wait_idle(GpuLua* g) {
  {
    ZoneScopedN("wait_idle"); // CPU blocked on the GPU
    vkDeviceWaitIdle(g->dev->device);
  }
  flush_dead(g, true);
}

// gpu.present continues here when the run resumes the script: true with the next frame open, false at the final
// resume (the run is over, no frame).
static int present_k(lua_State* L, int, lua_KContext) {
  lua_pushboolean(L, ctx(L)->frame_open);
  return 1;
}

// Yields to the run, which ends the frame (HUD, screenshot, offscreen, windows) and opens the next one before resuming,
// or resumes once more without a frame when the run is over.
static int l_present(lua_State* L) {
  GpuLua* g = ctx(L);
  if (!g->frame_open) { return luaL_error(L, "gpu.present: the run has ended"); }
  if (g->in_pass) { return luaL_error(L, "gpu.present: a pass is still open"); }
  return lua_yieldk(L, 0, 0, present_k);
}

static int l_wait_idle(lua_State* L) {
  wait_idle(ctx(L));
  return 0;
}

// ---- timers ----

static int timer_impl(lua_State* L, GpuLua* g, const char* name, Err* e) {
  int line = 0;
  std::string file = vm_caller(L, &line);
  uint32_t id = g->gpu->timer_create(&g->timers->gpu, name, file.c_str(), (uint32_t)line);
  if (id == UINT32_MAX) { return fail(e, "timer '%s' already exists", name); }
  g->timers->order.push_back({true, id});
  TimerUD* u = (TimerUD*)lua_newuserdatauv(L, sizeof(TimerUD), 0);
  u->id = id;
  luaL_setmetatable(L, TIMER_MT);
  return 1;
}

static int l_timer(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  Err e;
  if (timer_impl(L, ctx(L), name, &e) < 0) { return luaL_error(L, "gpu.timer: %s", e.msg); }
  return 1;
}

static int timer_start(lua_State* L) {
  uint32_t id = ((TimerUD*)luaL_checkudata(L, 1, TIMER_MT))->id;
  check_frame(L, "gpu_timer:start");
  GpuLua* g = ctx(L);
  if (!g->gpu->timer_begin(&g->timers->gpu, g->cmd, id)) { return luaL_error(L, "gpu_timer:start failed"); }
  return 0;
}

static int timer_stop(lua_State* L) {
  uint32_t id = ((TimerUD*)luaL_checkudata(L, 1, TIMER_MT))->id;
  check_frame(L, "gpu_timer:stop");
  GpuLua* g = ctx(L);
  if (!g->gpu->timer_end(&g->timers->gpu, g->cmd, id)) { return luaL_error(L, "gpu_timer:stop failed"); }
  return 0;
}

static int timer_results(lua_State* L) {
  uint32_t id = ((TimerUD*)luaL_checkudata(L, 1, TIMER_MT))->id;
  GpuLua* g = ctx(L);
  const std::vector<double>& v = g->gpu->timer_results(&g->timers->gpu, id);
  lua_createtable(L, (int)v.size(), 0);
  for (size_t i = 0; i < v.size(); i++) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int l_swapchain_size(lua_State* L) {
  GpuLua* g = ctx(L);
  lua_pushinteger(L, g->sc->view_extent.width);
  lua_pushinteger(L, g->sc->view_extent.height);
  return 2;
}

// ---- setup / teardown ----

bool bind_gpu_init(GpuLua* g, GpuApi api, GpuDevice* dev, GpuSwapchain* sc, Timers* timers) {
  g->api = api;
  g->gpu = gpu_api_table(api);
  g->dev = dev;
  g->sc = sc;
  g->timers = timers;
  if (!g->gpu->timers_init(dev, &g->timers->gpu)) {
    fprintf(stderr, "error: gpu_timers_init failed\n");
    return false;
  }
  return true;
}

void bind_gpu_register(lua_State* L, GpuLua* g) {
  lua_pushlightuserdata(L, g);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &g_key);

  luaL_newmetatable(L, BUF_MT);
  lua_createtable(L, 0, 3);
  lua_pushcfunction(L, buf_open);
  lua_setfield(L, -2, "open");
  lua_pushcfunction(L, buf_write);
  lua_setfield(L, -2, "write");
  lua_pushcfunction(L, buf_close);
  lua_setfield(L, -2, "close");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, buf_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, TEX_MT);
  lua_createtable(L, 0, 4);
  lua_pushcfunction(L, tex_upload);
  lua_setfield(L, -2, "upload");
  lua_pushcfunction(L, tex_size);
  lua_setfield(L, -2, "size");
  lua_pushcfunction(L, tex_mips);
  lua_setfield(L, -2, "mips");
  lua_pushcfunction(L, tex_mip);
  lua_setfield(L, -2, "mip");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, tex_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, MIP_MT);
  lua_pop(L, 1);

  luaL_newmetatable(L, PIPE_MT);
  lua_pushcfunction(L, pipe_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  luaL_newmetatable(L, TIMER_MT);
  lua_createtable(L, 0, 3);
  lua_pushcfunction(L, timer_start);
  lua_setfield(L, -2, "start");
  lua_pushcfunction(L, timer_stop);
  lua_setfield(L, -2, "stop");
  lua_pushcfunction(L, timer_results);
  lua_setfield(L, -2, "results");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  static const luaL_Reg funcs[] = {
      {"buffer", l_buffer},       {"texture", l_texture},   {"blit", l_blit},
      {"dispatch", l_dispatch},   {"pipeline", l_pipeline}, {"begin_pass", l_begin_pass},
      {"end_pass", l_end_pass},   {"draw", l_draw},         {"present", l_present},
      {"wait_idle", l_wait_idle}, {"timer", l_timer},       {"swapchain_size", l_swapchain_size},
      {nullptr, nullptr},
  };
  luaL_newlib(L, funcs);
  lua_setglobal(L, "gpu");
}

void bind_gpu_frame_opened(GpuLua* g) {
  g->opens++;
  flush_dead(g, false);
}

void bind_gpu_wait_idle(GpuLua* g) { wait_idle(g); }

void bind_gpu_shutdown(GpuLua* g) {
  wait_idle(g);
  g->gpu->pass_shutdown(g->dev, &g->pass);
  g->gpu->timers_shutdown(g->dev, &g->timers->gpu);
  g->gpu->descriptors_destroy(g->dev, &g->descriptors);
}
