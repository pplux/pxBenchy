#pragma once
#include <string>

struct lua_State;

lua_State* vm_create();
// The script runs as a coroutine: gpu.present yields to the caller's frame loop, which resumes it for the next frame
// or stops it by not resuming (vm_destroy). Errors are reported (with a traceback) before returning error.
enum class VmState { yield, done, error };
VmState vm_start(lua_State* L, const std::string& path); // loads the script and runs it to its first yield
VmState vm_resume(lua_State* L);                         // after a yield: runs it to the next one
void vm_destroy(lua_State* L);
