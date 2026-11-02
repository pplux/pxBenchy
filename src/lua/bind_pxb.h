#pragma once

struct lua_State;
struct Timers;

// Registers the global `pxb` table (pxb.load_glb, cpu_time, cpu_timer, read_file, math (bind_math.h)). CPU timer data
// goes to `ts`.
void bind_pxb_register(lua_State* L, Timers* ts);
