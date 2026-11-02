-- Demo 2.6 indirect-instanced-upload: demo2_3 with the command rewritten every frame (staged buffer: CPU write, copy +
-- barriers inside the "pass" timer). Setup in lib/demo2.lua.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local d = require("lib/demo2")
local m = d.m
local ind = gpu.buffer{size = 20, usage = "indirect"}
d.run(d.make_frame("indirect-instanced-upload", function()
  gpu.draw{pipeline = d.pipe, vertex_buffers = {d.vb, d.inst}, index_buffer = d.ib, index_type = m.index_type,
    indirect = ind, draw_count = 1, push = d.push}
end, function()
  ind:open(); ind:write(0, string.pack(d.CMD, m.index_count, d.N, 0, 0, 0)); ind:close()
end))
