#include "lua/bind_pxb.h"
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "asset/gltf.h"
#include "lua/bind_math.h"
#include "lua/vm.h"
#include "timers.h"
#include "platform/platform.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <stb_image.h>
}

// lua_error longjmps (Lua is built as C): C++ objects live only in *_impl functions, which report errors
// through Err and return before luaL_error is called.
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

static const char* const ATTR_NAMES[] = {"position", "normal", "tangent", "uv0", "uv1", "color0", nullptr};

static void push_vec3(lua_State* L, const float* v) {
  lua_createtable(L, 3, 0);
  for (int i = 0; i < 3; i++) {
    lua_pushnumber(L, v[i]);
    lua_rawseti(L, -2, i + 1);
  }
}

// Returns the number of results (1 table, or 2 for nil, err), or -1 with e filled on bad arguments.
static int load_glb_impl(lua_State* L, const char* path, Err* e) {
  std::vector<AssetAttr> attrs;
  int n = (int)lua_rawlen(L, 2);
  for (int i = 1; i <= n; i++) {
    lua_rawgeti(L, 2, i);
    if (lua_type(L, -1) != LUA_TSTRING) { return fail(e, "attribute list must contain strings"); }
    const char* s = lua_tostring(L, -1);
    int k = 0;
    while (ATTR_NAMES[k] && strcmp(ATTR_NAMES[k], s)) { k++; }
    if (!ATTR_NAMES[k]) { return fail(e, "unknown attribute '%s'", s); }
    attrs.push_back((AssetAttr)k);
    lua_pop(L, 1);
  }

  std::string buf;
  if (!platform_read_file(path, &buf)) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot read file '%s'", path);
    return 2;
  }
  AssetMesh mesh;
  std::string err;
  if (!asset_glb_load(buf.data(), buf.size(), attrs, &mesh, &err)) {
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  lua_createtable(L, 0, 8);
  lua_pushlstring(L, (const char*)mesh.vertices.data(), mesh.vertices.size());
  lua_setfield(L, -2, "vertices");
  lua_pushinteger(L, mesh.vertex_count);
  lua_setfield(L, -2, "vertex_count");
  lua_pushinteger(L, mesh.stride);
  lua_setfield(L, -2, "stride");
  lua_pushlstring(L, (const char*)mesh.indices.data(), mesh.indices.size());
  lua_setfield(L, -2, "indices");
  lua_pushstring(L, mesh.index_type == AssetIndexType::u16 ? "u16" : "u32");
  lua_setfield(L, -2, "index_type");
  lua_pushinteger(L, mesh.index_count);
  lua_setfield(L, -2, "index_count");
  push_vec3(L, mesh.aabb_min);
  lua_setfield(L, -2, "aabb_min");
  push_vec3(L, mesh.aabb_max);
  lua_setfield(L, -2, "aabb_max");
  return 1;
}

static int l_load_glb(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  Err e;
  int n = load_glb_impl(L, path, &e);
  if (n < 0) { return luaL_error(L, "pxb.load_glb: %s", e.msg); }
  return n;
}

// RGBA8, rows top to bottom, tightly packed.
static int l_load_image(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  std::string buf;
  if (!platform_read_file(path, &buf)) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot read file '%s'", path);
    return 2;
  }
  int w, h, n;
  stbi_uc* px = stbi_load_from_memory((const stbi_uc*)buf.data(), (int)buf.size(), &w, &h, &n, 4);
  if (!px) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot decode image '%s': %s", path, stbi_failure_reason());
    return 2;
  }
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "height");
  lua_pushlstring(L, (const char*)px, (size_t)w * h * 4);
  lua_setfield(L, -2, "data");
  stbi_image_free(px);
  return 1;
}

static int read_file_impl(lua_State* L, const char* path) {
  std::string buf;
  if (!platform_read_file(path, &buf)) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot read file '%s'", path);
    return 2;
  }
  lua_pushlstring(L, buf.data(), buf.size());
  return 1;
}

static int l_read_file(lua_State* L) { return read_file_impl(L, luaL_checkstring(L, 1)); }

// CPU timers: objects are full userdata holding an index, data lives in the Timers (outlives the Lua state). A
// sentinel userdata (anchored in the registry) closes Tracy zones of timers left open at lua_close (__gc).
static const char* const CPU_TIMER_MT = "pxb.cpu_timer";

struct CpuTimerUD {
  uint32_t id;
};

static Timers* timer_set(lua_State* L) { return *(Timers**)lua_touserdata(L, lua_upvalueindex(1)); }

static int l_cpu_time(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)timer_now_ns());
  return 1;
}

static int cpu_timer_impl(lua_State* L, Timers* ts, const char* name, Err* e) {
  for (const CpuTimer& c : ts->cpu) {
    if (c.name == name) { return fail(e, "timer '%s' already exists", name); }
  }
  CpuTimer c;
  c.name = name;
#ifdef TRACY_ENABLE
  // Tracy may read source locations until process exit: stable storage, never freed
  struct Loc {
    std::string name, file;
    ___tracy_source_location_data data;
  };
  static std::deque<Loc>* locs = new std::deque<Loc>();
  Loc& l = locs->emplace_back();
  int line = 0;
  l.name = name;
  l.file = vm_caller(L, &line); // the script line that created the timer
  l.data = {l.name.c_str(), "pxb.cpu_timer", l.file.c_str(), (uint32_t)line, 0};
  c.loc = &l.data;
#endif
  ts->cpu.push_back(std::move(c));
  uint32_t id = (uint32_t)ts->cpu.size() - 1;
  ts->order.push_back({false, id});
  CpuTimerUD* u = (CpuTimerUD*)lua_newuserdatauv(L, sizeof(CpuTimerUD), 0);
  u->id = id;
  luaL_setmetatable(L, CPU_TIMER_MT);
  return 1;
}

static int l_cpu_timer(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  Err e;
  if (cpu_timer_impl(L, timer_set(L), name, &e) < 0) { return luaL_error(L, "pxb.cpu_timer: %s", e.msg); }
  return 1;
}

static int cpu_timer_start(lua_State* L) {
  uint32_t id = ((CpuTimerUD*)luaL_checkudata(L, 1, CPU_TIMER_MT))->id;
  Timers* ts = timer_set(L);
  CpuTimerOpen o = {id, 0, false, {}};
#ifdef TRACY_ENABLE
  o.zone = ___tracy_emit_zone_begin((const ___tracy_source_location_data*)ts->cpu[id].loc, 1);
#endif
  o.t0 = timer_now_ns();
  ts->open.push_back(o);
  return 0;
}

static int cpu_timer_stop_impl(Timers* ts, uint32_t id, Err* e) {
  int64_t t1 = timer_now_ns();
  const char* name = ts->cpu[id].name.c_str();
  bool is_open = false;
  for (const CpuTimerOpen& o : ts->open) { is_open |= o.id == id; }
  if (!is_open) { return fail(e, "timer '%s' is not open", name); }
  CpuTimerOpen& top = ts->open.back();
  if (top.id != id) {
    return fail(e, "timer '%s' stopped out of order (innermost open timer is '%s')", name,
                ts->cpu[top.id].name.c_str());
  }
#ifdef TRACY_ENABLE
  ___tracy_emit_zone_end(top.zone);
#endif
  if (!top.discard) { ts->pending.push_back({id, t1 - top.t0}); } // committed or dropped at its frame end
  ts->open.pop_back();
  return 0;
}

static int cpu_timer_stop(lua_State* L) {
  uint32_t id = ((CpuTimerUD*)luaL_checkudata(L, 1, CPU_TIMER_MT))->id;
  Err e;
  if (cpu_timer_stop_impl(timer_set(L), id, &e) < 0) { return luaL_error(L, "cpu_timer:stop: %s", e.msg); }
  return 0;
}

static int cpu_timer_results(lua_State* L) {
  uint32_t id = ((CpuTimerUD*)luaL_checkudata(L, 1, CPU_TIMER_MT))->id;
  const std::vector<int64_t>& r = timer_set(L)->cpu[id].ns;
  lua_createtable(L, (int)r.size(), 0);
  for (size_t i = 0; i < r.size(); i++) {
    lua_pushinteger(L, (lua_Integer)r[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int sentinel_gc(lua_State* L) {
  Timers* ts = *(Timers**)lua_touserdata(L, 1);
  std::string names = timer_set_close_open(ts);
  if (!names.empty()) {
    printf("warning: pxb timers still open: %s\n", names.c_str());
    fflush(stdout);
  }
  return 0;
}

void bind_pxb_register(lua_State* L, Timers* ts) {
  lua_createtable(L, 0, 4);
  lua_pushcfunction(L, l_load_glb);
  lua_setfield(L, -2, "load_glb");
  lua_pushcfunction(L, l_load_image);
  lua_setfield(L, -2, "load_image");

  Timers** sentinel = (Timers**)lua_newuserdatauv(L, sizeof(Timers*), 0);
  *sentinel = ts;
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, sentinel_gc);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, "pxb.cpu_timers");

  // timer methods and pxb.cpu_timer share the sentinel as upvalue
  luaL_newmetatable(L, CPU_TIMER_MT);
  lua_createtable(L, 0, 3);
  static const luaL_Reg methods[] = {
      {"start", cpu_timer_start}, {"stop", cpu_timer_stop}, {"results", cpu_timer_results}, {nullptr, nullptr}};
  lua_pushvalue(L, -3);
  luaL_setfuncs(L, methods, 1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_pushcclosure(L, l_cpu_timer, 1);
  lua_setfield(L, -2, "cpu_timer");
  lua_pushcclosure(L, l_cpu_time, 0);
  lua_setfield(L, -2, "cpu_time");
  lua_pushcfunction(L, l_read_file);
  lua_setfield(L, -2, "read_file");
  bind_math_push(L);
  lua_setfield(L, -2, "math");
  lua_setglobal(L, "pxb");
}
