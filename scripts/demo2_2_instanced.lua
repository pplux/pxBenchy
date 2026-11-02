-- Demo 2.2 instanced: vkCmdDrawIndexed, N instances (demo1's draw). Setup in lib/demo2.lua.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local d = require("lib/demo2")
local m = d.m
d.run(d.make_frame("instanced", function()
  gpu.draw{pipeline = d.pipe, vertex_buffers = {d.vb, d.inst}, index_buffer = d.ib, index_type = m.index_type,
    count = m.index_count, instances = d.N, push = d.push}
end))
