-- Demo 2.5 indirect-per-object-empty: demo2_4 with indexCount = 0 in every command (all objects culled, as a compute
-- culling pass would leave them): cost of the commands alone, nothing rendered. Setup in lib/demo2.lua.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local d = require("lib/demo2")
local m = d.m
local cmds = {}
for i = 0, d.N - 1 do cmds[i + 1] = string.pack(d.CMD, 0, 1, 0, 0, i) end
local ind = gpu.buffer{size = 20 * d.N, usage = "indirect"}
ind:open(); ind:write(0, table.concat(cmds)); ind:close()
d.run(d.make_frame("indirect-per-object-empty", function()
  gpu.draw{pipeline = d.pipe, vertex_buffers = {d.vb, d.inst}, index_buffer = d.ib, index_type = m.index_type,
    indirect = ind, draw_count = d.N, push = d.push}
end))
