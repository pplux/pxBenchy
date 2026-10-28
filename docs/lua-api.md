# Lua API

A benchmark is a Lua 5.5 script. The app runs it once with `lua_pcall` on the render thread; the script's own loop is
the frame loop, and every `gpu.present()` ends a frame. When the script returns (or errors) the app prints the timing
report and tears down.

Sandbox: only `base`, `coroutine`, `math`, `string`, `table`, `utf8`. No `io`, `os`, `package`, `debug`; `dofile` and
`loadfile` are nil. Files go through `pxb.read_file`, `pxb.load_glb`, `pxb.load_image` and `require` (Android: APK assets only).

Errors in API calls raise Lua errors, except where a function is documented to return `nil, err`.

## Minimal script

```lua
local pipe = assert(gpu.pipeline{
  vs = [[
layout(location=0) in vec2 pos;
void main() { gl_Position = view_orientation(vec4(pos, 0.0, 1.0)); }]],
  fs = [[
layout(location=0) out vec4 col;
void main() { col = vec4(1.0, 0.5, 0.2, 1.0); }]],
  vertex_layout = {"float2"},
})
local vb = gpu.buffer{size = 6 * 4, usage = "vertex"}
vb:open(); vb:write(0, {0, -0.5, 0.5, 0.5, -0.5, 0.5}); vb:close()
local ib = gpu.buffer{size = 6, usage = "index"}
ib:open(); ib:write(0, string.pack("<I2I2I2", 0, 1, 2)); ib:close()

local timer = gpu.timer("draw")
for _ = 1, pxb.args.frames_warmup + pxb.args.frames do
  gpu.begin_pass{clear = {0, 0, 0, 1}}
  timer:start()
  gpu.draw{pipeline = pipe, vertex_buffers = {vb}, index_buffer = ib, index_type = "u16", count = 3}
  timer:stop()
  gpu.end_pass()
  gpu.present()
end
```

## Shaders

GLSL 450 for Vulkan, compiled at runtime by glslang. Do not write `#version`.

- Push constants: the pipeline's `push` fields are declared for you as a block named `push` (`push.view_proj`, ...),
  visible in both stages (compute: the compute stage).
- `vec4 view_orientation(vec4 clip)`: every vertex shader must write `gl_Position = view_orientation(...)`, or
  pipeline creation fails. It rotates clip space by the display's transform (Android pre-rotation; identity on desktop).
  Fragment `gl_FragCoord` and derivatives are in the display's native orientation.
- Compute shaders (`cs`) declare their own `layout(local_size_x = ..., ...) in;` and get no `view_orientation`.
- Bindings are declared for you at set 0: `texture` -> `sampler2D name`, `image` -> `rgba8 image2D name`. Scripts never
  write `layout(set = , binding = )`. For graphics pipelines they are visible in both `vs` and `fs`.
- Clip space is Vulkan's: depth 0..1, Y down (`pxb.math.perspective` already flips Y).
- Front face is counter-clockwise.

## gpu

### gpu.buffer{size, usage, mode} -> buffer

- `size`: bytes, > 0.
- `usage`: `"vertex"`, `"index"`, `"uniform"`, `"storage"`, `"indirect"`, or a list of them.
- `mode`: `"staged"` (default: device-local, `close()` records a copy) or `"mapped"` (host-visible, writes are direct).

Writes happen in a session, outside a pass:

- `buf:open()`
- `buf:write(offset, data)`: `data` is a string (raw bytes, e.g. from `string.pack`) or a table of numbers (written as
  float32).
- `buf:close()`: staged buffers record the upload into the current frame; the buffer is ready to draw right after.

Staged buffers can be rewritten every frame: each frame in flight has its own staging memory. Mapped buffers have no
such protection: do not rewrite memory the GPU may still read (call `gpu.wait_idle()` first).

### gpu.texture{width, height, mips} -> texture

- `width`, `height`: 1..16384. Format is RGBA8 UNORM (data treated as linear), usable for sampling and storage.
- `mips`: level count (default 1) or `"full"` (down to 1x1). At most 16.

Mips are 0-based. Outside a pass:

- `tex:upload(data, mip)`: `data` is a string of RGBA8 texels, rows top to bottom, tightly packed, exactly
  `4 * w * h` bytes for that mip (`mip` defaults to 0). Recorded into the current frame.
- `tex:size(mip) -> w, h` (`mip` defaults to 0), `tex:mips() -> n`.
- `tex:mip(i)`: a handle to the single level `i`, for `image` bindings.

### gpu.blit{src, src_mip, dst, dst_mip, filter}

Outside a pass: scales the whole `src_mip` level of texture `src` into the whole `dst_mip` level of `dst`
(`vkCmdBlitImage`). Mips default to 0; `src` and `dst` can be the same texture with different mips. `filter`:
`"linear"` (default) or `"nearest"`. Barriers are inserted by the framework.

### gpu.dispatch{pipeline, groups, bind, push}

Outside a pass: runs a compute `pipeline` over `groups = {x[, y[, z]]}` workgroups (missing = 1, each >= 1). `bind` maps
every binding name of the pipeline to a value: a `texture` binding takes a texture (all mips, sampled), an `image`
binding takes `tex:mip(i)` (read/write). Missing or unknown names, a wrong value type, or one texture bound both as
`texture` and `image` raise an error. `push` works like in `gpu.draw`. Barriers are inserted by the framework.

### gpu.pipeline{...} -> pipeline | nil, err

| field | meaning |
|---|---|
| `vs`, `fs` | GLSL source strings (required for a graphics pipeline) |
| `cs` | GLSL compute source: makes a compute pipeline. Exclusive with `vs`/`fs` and with every graphics-only field below (`vertex_layout`, `instance_layout`, `topology`, `cull`, `blend`, `depth`) |
| `bindings` | `{name = "texture" \| "image"}`; graphics pipelines accept `"texture"` only. Binding index = position in ascending name order |
| `push` | `{name = type}`, types `"float"`, `"vec2"`, `"vec3"`, `"vec4"`, `"mat4"`, `"uint"`, `"uvec4"`; at most 128 bytes |
| `vertex_layout` | per-vertex attributes, list of `"float1"`..`"float4"`, buffer slot 1 |
| `instance_layout` | per-instance attributes, same formats, buffer slot 2; locations continue after `vertex_layout` |
| `topology` | `"triangles"` (default), `"lines"`, `"points"` |
| `cull` | `"none"` (default), `"back"`, `"front"` |
| `blend` | `"none"` (default), `"alpha"` |
| `depth` | `true`: depth test + write (needs a pass with `depth = true`) |

Returns `nil, err` on shader compile/link errors (`err` names the stage and line).

### gpu.begin_pass{clear, depth} / gpu.end_pass()

- `clear = {r, g, b, a}`: clear the color target; omitted: keep (load) its contents.
- `depth = true`: attach the shared depth buffer, cleared to 1.0.

Textures written since the last pass (upload, blit, dispatch `image`) are made readable by shaders automatically
before the pass starts.

One pass at a time. The target is the screen, or the offscreen image with `--offscreen` (scripts do not change).

### gpu.draw{...}

| field | meaning |
|---|---|
| `pipeline` | from `gpu.pipeline` |
| `vertex_buffers` | `{vertex_buffer, instance_buffer}` (instance buffer optional) |
| `index_buffer`, `index_type` | index buffer and `"u16"` / `"u32"` (both required) |
| `count`, `instances` | index count and instance count (default 1) |
| `indirect`, `draw_count` | instead of `count`/`instances`: a buffer with usage `"indirect"` holding `draw_count` `VkDrawIndexedIndirectCommand` (20 bytes each: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance) |
| `bind` | `{name = texture}`: every binding of the pipeline (required iff it has bindings); missing or unknown names raise an error. Textures must have been written before the pass |
| `push` | `{name = value}`: numbers or lists of numbers for float fields, integers for `uint`/`uvec4` |

Inside a pass only.

### gpu.text(x, y, str, {scale, color, bg})

Bitmap text (8x8 font, ASCII) inside a pass. `x`, `y` in pixels from the top-left; cells are `8 * scale` pixels
(`scale` integer >= 1, default 1); `\n` starts a new line. `color` and `bg` are `{r, g, b, a}` (default white on
transparent).

### gpu.present() -> presented

Ends the frame: submits, presents (not in `--offscreen`), handles window events, the warm-up reset and swapchain
recreation. Returns whether an image reached the screen. Raises `quit requested` once the window is closed.

### gpu.timer(name) -> timer

GPU timestamp timer. `timer:start()` / `timer:stop()` in the same frame (nested timers stop in reverse order);
`timer:results()` returns the durations in nanoseconds. Each timer is a report row and a Tracy GPU zone. Timestamps
inside a pass are not meaningful on tiled GPUs (Apple): time the whole pass there.

### gpu.swapchain_size() -> width, height

Size of the render target as the script sees it (landscape on a landscape app, whatever the display rotation).

### gpu.wait_idle()

Blocks until the GPU is idle.

## pxb

### pxb.args

`pxb.args.frames` (`--frames`, 0 = run until the window closes) and `pxb.args.frames_warmup` (`--frames-warmup`).
Loop `frames_warmup + frames` times; timers are cleared at the warm-up's last present.

### pxb.cpu_timer(name) -> timer

CPU wall-clock timer: `timer:start()`, `timer:stop()` (strictly nested), `timer:results()` (nanoseconds). Each timer is
a report row and a Tracy CPU zone.

### pxb.cpu_time() -> ns

Monotonic clock in nanoseconds (integer).

### pxb.load_glb(path, attributes) -> mesh | nil, err

Loads the first mesh primitive of a `.glb`. `attributes` lists, in order, any of `"position"`, `"normal"`,
`"tangent"`, `"uv0"`, `"uv1"`, `"color0"`. Returns:

- `vertices` (string, interleaved float32 in the requested order), `vertex_count`, `stride` (bytes)
- `indices` (string), `index_type` (`"u16"` / `"u32"`), `index_count`
- `aabb_min`, `aabb_max` (`{x, y, z}`)

### pxb.load_image(path) -> image | nil, err

Decodes a PNG or JPEG (stb_image), always to 4 channels. Returns `width`, `height` and `data` (string, RGBA8, rows top
to bottom, tightly packed).

### pxb.read_file(path) -> string | nil, err

Whole file as a string.

### pxb.math

Plain functions on number arrays; they return new arrays and never modify arguments. Matrices are 16 numbers,
column-major (row `r`, column `c` at index `c * 4 + r + 1`).

| function | result |
|---|---|
| `add(a, b)`, `sub(a, b)` | element-wise, same length (up to 16) |
| `scale(v, s)` | `v * s` |
| `dot(a, b)`, `length(v)` | number |
| `normalize(v)` | unit vector (error on zero length) |
| `cross(a, b)` | 3-component cross product |
| `identity()` | identity matrix |
| `mul(a, b)` | matrix product `a * b` |
| `transform(m, v)` | `m * v`, `v` with 4 components |
| `transpose(m)` | transposed matrix |
| `inverse(m)` | inverse, or `nil, "singular matrix"` |
| `perspective(fovy, aspect, near, far)` | projection, `fovy` in radians, depth 0..1, Y flipped |
| `look_at(eye, target, up)` | right-handed view matrix |
| `translation(v)`, `scaling(v)` | from a 3-component vector |
| `rotation(axis, angle)` | counter-clockwise around `axis`, radians |

## require(name)

Runs `scripts/<name>.lua` once (text only, `name` passed as its argument) and caches its result (`true` when it
returns nil).
