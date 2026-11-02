-- Demo 2.4 indirect-per-object: N indirect commands of 1 instance each, firstInstance = i (multi-draw indirect).
-- Setup in lib/demo2.lua.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local d = require("lib/demo2")
local m = d.m
local cmds = {}
for i = 0, d.N - 1 do cmds[i + 1] = string.pack(d.CMD, m.index_count, 1, 0, 0, i) end
local ind = gpu.buffer{size = 20 * d.N, usage = "indirect"}
ind:open(); ind:write(0, table.concat(cmds)); ind:close()
d.run(d.make_frame("indirect-per-object", function()
  gpu.draw{pipeline = d.pipe, vertex_buffers = {d.vb, d.inst}, index_buffer = d.ib, index_type = m.index_type,
    indirect = ind, draw_count = d.N, push = d.push}
end))
