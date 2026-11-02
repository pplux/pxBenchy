-- Basic smoke test: one multicolored triangle, fixed frame count (the app prints the timer report).

-- x, y, r, g, b per vertex (Vulkan clip space, Y down)
local verts = {
   0.0, -0.6,  1, 0, 0,
   0.6,  0.6,  0, 1, 0,
  -0.6,  0.6,  0, 0, 1,
}
local vb = gpu.buffer{size = #verts * 4, usage = "vertex"}
vb:open(); vb:write(0, verts); vb:close()
local idx = string.pack("<I2I2I2", 0, 1, 2)
local ib = gpu.buffer{size = #idx, usage = "index"}
ib:open(); ib:write(0, idx); ib:close()

local pipe = assert(gpu.pipeline{
  vs = [[
layout(location=0) in vec2 pos;
layout(location=1) in vec3 color;
layout(location=0) out vec3 vcolor;
void main() {
  vcolor = color;
  gl_Position = view_orientation(vec4(pos, 0.0, 1.0));
}]],
  fs = [[
layout(location=0) in vec3 vcolor;
layout(location=0) out vec4 col;
void main() {
  col = vec4(vcolor, 1.0);
}]],
  vertex_layout = {"float2", "float3"},
  cull = "none",
})

repeat
  gpu.begin_pass{clear = {0.05, 0.05, 0.1, 1}}
  gpu.draw{pipeline = pipe, vertex_buffers = {vb}, index_buffer = ib, index_type = "u16",
    count = 3, instances = 1}
  gpu.end_pass()
until not gpu.present()
