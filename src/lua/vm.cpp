#include "lua/vm.h"
#include "platform/platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tracy/Tracy.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#if defined(__ANDROID__)
#include <android/log.h>
#endif

static void report_error(const std::string& msg) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_ERROR, "pxbenchy", "error: %s", msg.c_str());
#else
  fprintf(stderr, "error: %s\n", msg.c_str());
#endif
}

static int vm_print(lua_State* L) {
  int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; i++) {
    size_t len;
    const char* s = luaL_tolstring(L, i, &len);
    if (i > 1) { line += '\t'; }
    line.append(s, len);
    lua_pop(L, 1);
  }
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "pxbenchy", "%s", line.c_str());
#else
  line += '\n';
  fwrite(line.data(), 1, line.size(), stdout);
  fflush(stdout);
#endif
  return 0;
}

static char g_co; // registry key of the script coroutine (vm_start)

// Mirrors lauxlib's luaL_newstate (alloc, panic, warnings) with the allocator reporting to Tracy pool "lua".
static void* lua_alloc(void*, void* ptr, size_t, size_t nsize) {
  if (nsize == 0) {
    if (ptr) { TracyFreeN(ptr, "lua"); }
    free(ptr);
    return nullptr;
  }
  void* p = realloc(ptr, nsize);
  if (p) {
    if (ptr) { TracyFreeN(ptr, "lua"); }
    TracyAllocN(p, nsize, "lua");
  }
  return p;
}

static int lua_panic(lua_State* L) {
  const char* msg = (lua_type(L, -1) == LUA_TSTRING) ? lua_tostring(L, -1) : "error object is not a string";
  fprintf(stderr, "PANIC: unprotected error in call to Lua API (%s)\n", msg);
  fflush(stderr);
  return 0;
}

static void warn_off(void* ud, const char* message, int tocont);
static void warn_on(void* ud, const char* message, int tocont);
static void warn_cont(void* ud, const char* message, int tocont);

static int warn_control(lua_State* L, const char* message, int tocont) {
  if (tocont || *(message++) != '@') { return 0; }
  if (strcmp(message, "off") == 0) {
    lua_setwarnf(L, warn_off, L);
  } else if (strcmp(message, "on") == 0) {
    lua_setwarnf(L, warn_on, L);
  }
  return 1;
}

static void warn_off(void* ud, const char* message, int tocont) { warn_control((lua_State*)ud, message, tocont); }

static void warn_cont(void* ud, const char* message, int tocont) {
  lua_State* L = (lua_State*)ud;
  fprintf(stderr, "%s", message);
  if (tocont) {
    lua_setwarnf(L, warn_cont, L);
  } else {
    fprintf(stderr, "\n");
    lua_setwarnf(L, warn_on, L);
  }
  fflush(stderr);
}

static void warn_on(void* ud, const char* message, int tocont) {
  if (warn_control((lua_State*)ud, message, tocont)) { return; }
  fprintf(stderr, "Lua warning: ");
  warn_cont(ud, message, tocont);
}

// Reads + compiles path through the platform layer; pushes the chunk, or an error message and returns false.
static bool load_module(lua_State* L, const char* path) {
  std::string buf;
  if (!platform_read_file(path, &buf)) {
    lua_pushfstring(L, "module not found: '%s'", path);
    return false;
  }
  std::string chunk = std::string("@") + path;
  return luaL_loadbufferx(L, buf.data(), buf.size(), chunk.c_str(), "t") == LUA_OK;
}

// require(name): runs scripts/<name>.lua once (text only), caches its result (true if it returns nil).
static int vm_require(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_settop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "pxb.loaded");
  if (lua_getfield(L, 2, name) != LUA_TNIL) { return 1; }
  lua_pop(L, 1);
  const char* path = lua_pushfstring(L, "scripts/%s.lua", name);
  if (!load_module(L, path)) { return lua_error(L); }
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, 2, name);
  return 1;
}

lua_State* vm_create() {
  lua_State* L = lua_newstate(lua_alloc, nullptr, luaL_makeseed(nullptr));
  if (L) {
    lua_atpanic(L, lua_panic);
    lua_setwarnf(L, warn_on, L);
  }
  if (!L) {
    report_error("cannot create Lua state");
    return nullptr;
  }
  // No io/os/package/debug: file access goes through the platform layer (pxb.read_file, require). linit.c (which
  // references every library) is not compiled: open each one as luaL_openselectedlibs does.
  static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine},
                                  {LUA_MATHLIBNAME, luaopen_math}, {LUA_STRLIBNAME, luaopen_string},
                                  {LUA_TABLIBNAME, luaopen_table}, {LUA_UTF8LIBNAME, luaopen_utf8}};
  for (const luaL_Reg& lib : libs) {
    luaL_requiref(L, lib.name, lib.func, 1);
    lua_pop(L, 1);
  }
  lua_pushnil(L);
  lua_setglobal(L, "dofile");
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "pxb.loaded");
  lua_pushcfunction(L, vm_require);
  lua_setglobal(L, "require");
  lua_pushcfunction(L, vm_print);
  lua_setglobal(L, "print");
  return L;
}

// Resumes co; on an error reports its message and co's traceback (co keeps its stack after an error).
static VmState resume(lua_State* L, lua_State* co) {
  int nres = 0;
  int rc = lua_resume(co, L, 0, &nres);
  if (rc == LUA_YIELD || rc == LUA_OK) {
    lua_pop(co, nres);
    return rc == LUA_YIELD ? VmState::yield : VmState::done;
  }
  lua_xmove(co, L, 1); // error object
  const char* msg = lua_tostring(L, -1);
  if (msg == nullptr) {
    if (!luaL_callmeta(L, -1, "__tostring") || lua_type(L, -1) != LUA_TSTRING) {
      lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, -1));
    }
    msg = lua_tostring(L, -1);
  }
  luaL_traceback(L, co, msg, 0);
  report_error(lua_tostring(L, -1));
  lua_settop(L, 0);
  return VmState::error;
}

VmState vm_start(lua_State* L, const std::string& path) {
  std::string buf;
  if (!platform_read_file(path.c_str(), &buf)) {
    report_error("cannot read script '" + path + "'");
    return VmState::error;
  }
  lua_State* co = lua_newthread(L);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &g_co); // anchored until vm_destroy
  std::string chunk = "@" + path;
  if (luaL_loadbufferx(co, buf.data(), buf.size(), chunk.c_str(), "t") != LUA_OK) {
    const char* m = lua_tostring(co, -1);
    report_error(m ? m : "(unknown error)");
    return VmState::error;
  }
  return resume(L, co);
}

VmState vm_resume(lua_State* L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &g_co);
  lua_State* co = lua_tothread(L, -1);
  lua_pop(L, 1);
  return resume(L, co);
}

void vm_destroy(lua_State* L) {
  if (L) { lua_close(L); }
}
