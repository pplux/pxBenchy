#pragma once
#include <string>

struct lua_State;

lua_State* vm_create();
bool vm_run(lua_State* L, const std::string& path);
// From a C function: raises an error that ends vm_run quietly, returning true.
int vm_stop(lua_State* L);
void vm_destroy(lua_State* L);
