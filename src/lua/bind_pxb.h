#pragma once

struct lua_State;
struct TimerSet;

// Registers the global `pxb` table (pxb.load_glb, cpu_time, cpu_timer, read_file, math (bind_math.h), args). CPU timer
// data goes to `ts`. pxb.args.frames = `frames` (--frames, 0 = run indefinitely), pxb.args.frames_warmup =
// `frames_warmup`.
void bind_pxb_register(lua_State* L, TimerSet* ts, long long frames, long long frames_warmup);
