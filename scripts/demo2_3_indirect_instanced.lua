-- Demo 2.3 indirect-instanced: one indirect command, N instances (demo1's draw, arguments from a buffer). Setup in
-- lib/demo2.lua.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local d = require("lib/demo2")
local m = d.m
local ind = gpu.buffer{size = 20, usage = "indirect"}
ind:open(); ind:write(0, string.pack(d.CMD, m.index_count, d.N, 0, 0, 0)); ind:close()
d.run(d.make_frame("indirect-instanced", function()
  gpu.draw{pipeline = d.pipe, vertex_buffers = {d.vb, d.inst}, index_buffer = d.ib, index_type = m.index_type,
    indirect = ind, draw_count = 1, push = d.push}
end))
