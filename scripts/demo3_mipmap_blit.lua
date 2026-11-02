-- Demo 3 (blit): full mip-chain generation of a 4096x4096 texture with gpu.blit (vkCmdBlitImage, linear), one blit per
-- level. Compare with demo3_mipmap_compute.lua (same frames, same draw) on overall time.
-- Level 0 is uploaded once; levels 1..N-1 are regenerated every frame. Timers: "blit" (whole chain) and
-- "blit/L1".."blit/L12" (per level), each as GPU row and CPU row (CPU = recording time only). The mip chain is then drawn
-- as a strip, one quad per level with textureLod. Barriers between levels are inserted by the framework.
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local img = assert(pxb.load_image("assets/aerial_rocks_02_4k.jpg"))
local tex = gpu.texture{width = img.width, height = img.height, mips = "full"}
tex:upload(img.data, 0)
local n = tex:mips()

-- unit quad (position, uv)
local vb = gpu.buffer{size = 16 * 4, usage = "vertex"}
vb:open(); vb:write(0, {0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1}); vb:close()
local ib = gpu.buffer{size = 12, usage = "index"}
ib:open(); ib:write(0, string.pack("<I2I2I2I2I2I2", 0, 1, 2, 0, 2, 3)); ib:close()
local show = assert(gpu.pipeline{
  vs = [[
layout(location=0) in vec4 a;
layout(location=0) out vec2 uv;
void main() {
  uv = a.zw;
  gl_Position = view_orientation(vec4(push.rect.xy + a.xy * push.rect.zw, 0.0, 1.0));
}]],
  fs = [[
layout(location=0) in vec2 uv;
layout(location=0) out vec4 col;
void main() { col = textureLod(tex, uv, push.lod); }]],
  vertex_layout = {"float4"}, push = {rect = "vec4", lod = "float"}, bindings = {tex = "texture"},
})

-- mip strip in pixels: level 0 on the left (square, full height), levels 1.. shelf-packed to its right, sizes halving
-- with a 24 px minimum so the small levels stay visible
local w, h = gpu.swapchain_size()
local PAD, MIN = 4, 24
local S = math.min(h - 2 * PAD, w * 0.6)
local rects = {{PAD, PAD, S}}
local x, y, row_h = S + 2 * PAD, PAD, 0
for i = 1, n - 1 do
  local s = math.max(S / 2 ^ i, MIN)
  if x + s > w - PAD then x, y, row_h = S + 2 * PAD, y + row_h + PAD, 0 end
  rects[i + 1] = {x, y, s}
  x = x + s + PAD
  row_h = math.max(row_h, s)
end
for i, r in ipairs(rects) do -- pixels to clip space (Y down)
  r.rect = {r[1] / w * 2 - 1, r[2] / h * 2 - 1, r[3] / w * 2, r[3] / h * 2}
  r.lod = i - 1
end

local function make(name)
  local m = {cpu = pxb.cpu_timer(name), gpu = gpu.timer(name), cpu_l = {}, gpu_l = {}}
  for i = 1, n - 1 do
    m.cpu_l[i] = pxb.cpu_timer(name .. "/L" .. i)
    m.gpu_l[i] = gpu.timer(name .. "/L" .. i)
  end
  return m
end
local modes = {blit = make("blit")}

local function level(m, i, f)
  m.cpu_l[i]:start(); m.gpu_l[i]:start()
  f()
  m.gpu_l[i]:stop(); m.cpu_l[i]:stop()
end

local function gen_blit()
  local m = modes.blit
  m.cpu:start(); m.gpu:start()
  for i = 1, n - 1 do
    level(m, i, function() gpu.blit{src = tex, src_mip = i - 1, dst = tex, dst_mip = i} end)
  end
  m.gpu:stop(); m.cpu:stop()
end

for frame = 1, pxb.args.frames_warmup + pxb.args.frames do
  gen_blit()
  gpu.begin_pass{clear = {0.05, 0.05, 0.1, 1}}
  for _, r in ipairs(rects) do
    gpu.draw{pipeline = show, vertex_buffers = {vb}, index_buffer = ib, index_type = "u16", count = 6,
      bind = {tex = tex}, push = {rect = r.rect, lod = r.lod}}
  end
  gpu.end_pass()
  gpu.present()
end
