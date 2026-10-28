#pragma once

struct lua_State;

// Pushes the `pxb.math` table: functions on plain Lua number arrays (no objects). Vectors have any length (cross: 3);
// matrices are 16 numbers, column-major (element row r, col c at index c * 4 + r + 1), right-handed, Vulkan clip space
// (depth 0..1, Y down). Every function returns new arrays; arguments are never modified.
void bind_math_push(lua_State* L);
