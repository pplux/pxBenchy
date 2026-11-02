-- Demo 2: demo1_instancing (same data, camera, shaders, floor), grid drawn direct vs vkCmdDrawIndexedIndirect.
-- Six modes alternate every frame, each with its own CPU + GPU timers (report + Tracy zones). The GPU timer "pass"
-- spans the whole pass: inside a pass, timestamps are not meaningful on tiled GPUs (Apple, Mali: they read 0).
--   none: no grid draw (clear + floor only), baseline for the other modes
--   instanced: vkCmdDrawIndexed, N instances (demo1's draw)
--   indirect-instanced: one indirect command, N instances (demo1's draw, arguments from a buffer)
--   indirect-per-object: N indirect commands of 1 instance each, firstInstance = i (multi-draw indirect)
--   indirect-per-object-empty: indirect-per-object with indexCount = 0 in every command (all objects culled, as a
--     compute culling pass would leave them): cost of the commands alone, nothing rendered
--   indirect-instanced-upload: indirect-instanced with the command rewritten every frame (staged buffer: CPU write,
--     copy + barriers inside the "pass" timer)
-- Runs --frames-warmup + --frames frames (the app resets timers after warm-up and prints the report).
local MODEL = "assets/duck.glb"
local FLOOR_MODEL = "assets/placeholder_cube.glb"
local GRID = 100
local N = GRID * GRID

local m = assert(pxb.load_glb(MODEL, {"position", "normal"}))
local vb = gpu.buffer{size = #m.vertices, usage = "vertex"}
vb:open(); vb:write(0, m.vertices); vb:close()
local ib = gpu.buffer{size = #m.indices, usage = "index"}
ib:open(); ib:write(0, m.indices); ib:close()

-- one float3 offset per instance, grid in XZ centered at the origin, spacing from the mesh AABB
local ext = 0
for i = 1, 3 do ext = math.max(ext, m.aabb_max[i] - m.aabb_min[i]) end
local spacing = 1.5 * ext
local half = (GRID - 1) / 2 * spacing
local offsets, k = {}, 0
for z = 0, GRID - 1 do
  for x = 0, GRID - 1 do
    offsets[k + 1] = x * spacing - half
    offsets[k + 2] = 0
    offsets[k + 3] = z * spacing - half
    k = k + 3
  end
end
local inst = gpu.buffer{size = #offsets * 4, usage = "vertex"}
inst:open(); inst:write(0, offsets); inst:close()

-- indirect arguments, VkDrawIndexedIndirectCommand {indexCount, instanceCount, firstIndex, vertexOffset, firstInstance}
local CMD = "<I4I4I4i4I4"
local ind_instanced = gpu.buffer{size = 20, usage = "indirect"}
ind_instanced:open(); ind_instanced:write(0, string.pack(CMD, m.index_count, N, 0, 0, 0)); ind_instanced:close()
local cmds = {}
for i = 0, N - 1 do cmds[i + 1] = string.pack(CMD, m.index_count, 1, 0, 0, i) end
local ind_upload = gpu.buffer{size = 20, usage = "indirect"} -- rewritten every frame
local ind_per_object = gpu.buffer{size = 20 * N, usage = "indirect"}
ind_per_object:open(); ind_per_object:write(0, table.concat(cmds)); ind_per_object:close()
for i = 0, N - 1 do cmds[i + 1] = string.pack(CMD, 0, 1, 0, 0, i) end
local ind_per_object_empty = gpu.buffer{size = 20 * N, usage = "indirect"}
ind_per_object_empty:open(); ind_per_object_empty:write(0, table.concat(cmds)); ind_per_object_empty:close()

-- one big red cube underneath the grid (uniform scale), top face just below the instances
local c = assert(pxb.load_glb(FLOOR_MODEL, {"position", "normal"}))
local cvb = gpu.buffer{size = #c.vertices, usage = "vertex"}
cvb:open(); cvb:write(0, c.vertices); cvb:close()
local cib = gpu.buffer{size = #c.indices, usage = "index"}
cib:open(); cib:write(0, c.indices); cib:close()
local cext = 0
for i = 1, 3 do cext = math.max(cext, c.aabb_max[i] - c.aabb_min[i]) end
local big_scale = (2 * half + spacing) / cext
local big_offset = {}
for i = 1, 3 do big_offset[i] = -big_scale * (c.aabb_min[i] + c.aabb_max[i]) / 2 end
big_offset[2] = m.aabb_min[2] - big_scale * c.aabb_max[2]
local big_inst = gpu.buffer{size = 3 * 4, usage = "vertex"}
big_inst:open(); big_inst:write(0, big_offset); big_inst:close()

-- camera: look at the origin from above at an angle, fitting the grid's bounding sphere
local w, h = gpu.swapchain_size()
local fovy = math.rad(45)
local radius = math.sqrt(2) * half + ext
local dist = radius / math.sin(fovy / 2)
local elev = math.rad(50)
-- depth range scaled to the scene (model units vary), far covers the floor cube
local near, far = 0.1 * dist, dist + 4 * radius
local eye = {0, dist * math.sin(elev), dist * math.cos(elev)}

local M = pxb.math
-- right-handed look-at at the origin, Y up; perspective depth 0..1, Y flipped for the native Vulkan viewport
local view_proj = M.mul(M.perspective(fovy, w / h, near, far), M.look_at(eye, {0, 0, 0}, {0, 1, 0}))
local light_dir = M.normalize({0.4, 1.0, 0.6}) -- direction towards the light

local pipe = assert(gpu.pipeline{
  vs = [[
layout(location=0) in vec3 pos;
layout(location=1) in vec3 nrm;
layout(location=2) in vec3 offset;
layout(location=0) out vec3 wpos;
layout(location=1) out vec3 wnrm;
void main() {
  wpos = pos * push.scale + offset;
  wnrm = nrm;
  gl_Position = view_orientation(push.view_proj * vec4(wpos, 1.0));
}]],
  fs = [[
layout(location=0) in vec3 wpos;
layout(location=1) in vec3 wnrm;
layout(location=0) out vec4 col;
void main() {
  vec3 n = normalize(wnrm);
  vec3 l = normalize(push.light_dir);
  vec3 v = normalize(push.eye - wpos);
  vec3 base = push.color;
  float diff = max(dot(n, l), 0.0);
  float spec = diff > 0.0 ? pow(max(dot(n, normalize(l + v)), 0.0), 32.0) : 0.0;
  col = vec4(base * (0.15 + 0.85 * diff) + vec3(0.3) * spec, 1.0);
}]],
  push = {view_proj = "mat4", light_dir = "vec3", eye = "vec3", scale = "vec3", color = "vec3"},
  vertex_layout = {"float3", "float3"},
  instance_layout = {"float3"},
  -- the Y flip in proj inverts the apparent winding (glTF CCW reads as CW in framebuffer space), pipeline front face is fixed CCW
  cull = "back",
  depth = true,
})

local push = {view_proj = view_proj, light_dir = light_dir, eye = eye, scale = {1, 1, 1}, color = {0.8, 0.55, 0.3}}
local big_push = {view_proj = view_proj, light_dir = light_dir, eye = eye,
  scale = {big_scale, big_scale, big_scale}, color = {0.8, 0.05, 0.05}}
local CLEAR = {0.05, 0.05, 0.1, 1}
local function draw_floor()
  gpu.draw{pipeline = pipe, vertex_buffers = {cvb, big_inst}, index_buffer = cib, index_type = c.index_type,
    count = c.index_count, instances = 1, push = big_push}
end

local none_cpu, none_pass = pxb.cpu_timer("frame none"), gpu.timer("pass none")
local function frame_none()
  none_cpu:start()
  none_pass:start()
  gpu.begin_pass{clear = CLEAR, depth = true}
  draw_floor()
  gpu.end_pass()
  none_pass:stop()
  gpu.present()
  none_cpu:stop()
end

local instanced_cpu, instanced_pass = pxb.cpu_timer("frame instanced"), gpu.timer("pass instanced")
local function frame_instanced()
  instanced_cpu:start()
  instanced_pass:start()
  gpu.begin_pass{clear = CLEAR, depth = true}
  gpu.draw{pipeline = pipe, vertex_buffers = {vb, inst}, index_buffer = ib, index_type = m.index_type,
    count = m.index_count, instances = N, push = push}
  draw_floor()
  gpu.end_pass()
  instanced_pass:stop()
  gpu.present()
  instanced_cpu:stop()
end

local ind_inst_cpu, ind_inst_pass = pxb.cpu_timer("frame indirect-instanced"), gpu.timer("pass indirect-instanced")
local function frame_indirect_instanced()
  ind_inst_cpu:start()
  ind_inst_pass:start()
  gpu.begin_pass{clear = CLEAR, depth = true}
  gpu.draw{pipeline = pipe, vertex_buffers = {vb, inst}, index_buffer = ib, index_type = m.index_type,
    indirect = ind_instanced, draw_count = 1, push = push}
  draw_floor()
  gpu.end_pass()
  ind_inst_pass:stop()
  gpu.present()
  ind_inst_cpu:stop()
end

local per_obj_cpu, per_obj_pass = pxb.cpu_timer("frame indirect-per-object"), gpu.timer("pass indirect-per-object")
local function frame_indirect_per_object()
  per_obj_cpu:start()
  per_obj_pass:start()
  gpu.begin_pass{clear = CLEAR, depth = true}
  gpu.draw{pipeline = pipe, vertex_buffers = {vb, inst}, index_buffer = ib, index_type = m.index_type,
    indirect = ind_per_object, draw_count = N, push = push}
  draw_floor()
  gpu.end_pass()
  per_obj_pass:stop()
  gpu.present()
  per_obj_cpu:stop()
end

local empty_cpu, empty_pass =
  pxb.cpu_timer("frame indirect-per-object-empty"), gpu.timer("pass indirect-per-object-empty")
local function frame_indirect_per_object_empty()
  empty_cpu:start()
  empty_pass:start()
  gpu.begin_pass{clear = CLEAR, depth = true}
  gpu.draw{pipeline = pipe, vertex_buffers = {vb, inst}, index_buffer = ib, index_type = m.index_type,
    indirect = ind_per_object_empty, draw_count = N, push = push}
  draw_floor()
  gpu.end_pass()
  empty_pass:stop()
  gpu.present()
  empty_cpu:stop()
end

local upload_cpu, upload_pass =
  pxb.cpu_timer("frame indirect-instanced-upload"), gpu.timer("pass indirect-instanced-upload")
local function frame_indirect_instanced_upload()
  upload_cpu:start()
  upload_pass:start()
  ind_upload:open(); ind_upload:write(0, string.pack(CMD, m.index_count, N, 0, 0, 0)); ind_upload:close()
  gpu.begin_pass{clear = CLEAR, depth = true}
  gpu.draw{pipeline = pipe, vertex_buffers = {vb, inst}, index_buffer = ib, index_type = m.index_type,
    indirect = ind_upload, draw_count = 1, push = push}
  draw_floor()
  gpu.end_pass()
  upload_pass:stop()
  gpu.present()
  upload_cpu:stop()
end

-- one frame of each mode per iteration, (--frames-warmup + --frames) / 6 iterations; --frames 0 = until the window closes
local total = pxb.args.frames_warmup + pxb.args.frames
for _ = 1, pxb.args.frames == 0 and math.maxinteger or total // 6 do
  frame_none()
  frame_instanced()
  frame_indirect_instanced()
  frame_indirect_per_object()
  frame_indirect_per_object_empty()
  frame_indirect_instanced_upload()
end
