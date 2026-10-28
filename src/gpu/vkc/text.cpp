#include "gpu/vkc/text.h"
#include "gpu/vkc/pass.h"
#include "gpu/vkc/pipeline.h"
#include "gpu/vkc/resources.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <font8x8_basic.h> // defines font8x8_basic[128][8]: row-major, bit 0 = leftmost pixel

namespace vkc {

static const char* const TEXT_VS = R"(
layout(location=0) in vec2 corner;
layout(location=0) out vec2 uv;
layout(location=1) flat out uint code;
void main() {
  uint i = uint(gl_InstanceIndex);
  uvec4 w4 = i < 16u ? push.c0 : i < 32u ? push.c1 : i < 48u ? push.c2 : i < 64u ? push.c3 : push.c4;
  code = (w4[(i >> 2) & 3u] >> ((i & 3u) * 8u)) & 0x7fu;
  uv = corner * 8.0;
  gl_Position = view_orientation(vec4(push.origin + (vec2(float(i), 0.0) + corner) * push.cell, 0.0, 1.0));
})";

// Font table prepended at init: FONT[2 * code + (row >> 2)] holds rows (row & 3) at bits 8 * (row & 3).
static const char* const TEXT_FS = R"(
layout(location=0) in vec2 uv;
layout(location=1) flat in uint code;
layout(location=0) out vec4 col;
void main() {
  uvec2 p = min(uvec2(uv), uvec2(7u));
  uint row = (FONT[code * 2u + (p.y >> 2)] >> ((p.y & 3u) * 8u)) & 0xffu;
  col = ((row >> p.x) & 1u) != 0u ? push.color : push.bg;
})";

static std::string font_glsl() {
  std::string s = "const uint FONT[256] = uint[256](";
  char buf[16];
  for (int c = 0; c < 128; c++) {
    for (int half = 0; half < 2; half++) {
      uint32_t w = 0;
      for (int r = 0; r < 4; r++) w |= (uint32_t)(uint8_t)font8x8_basic[c][half * 4 + r] << (r * 8);
      snprintf(buf, sizeof(buf), "%s0x%08xu", c || half ? "," : "", w);
      s += buf;
    }
  }
  s += ");\n";
  return s;
}

// Draws one line chunk of n <= GPU_TEXT_CHUNK chars starting at pixel (x, y).
static bool draw_chunk(GpuText* t, GpuPassCtx* ctx, float x, float y, const char* s, uint32_t n, uint32_t scale,
                       const float color[4], const float bg[4]) {
  uint32_t words[GPU_TEXT_CHUNK / 4] = {};
  for (uint32_t i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c >= 128) c = '?';
    words[i / 4] |= (uint32_t)c << ((i % 4) * 8);
  }
  float w = (float)ctx->extent.width, h = (float)ctx->extent.height;
  float origin[2] = {x * 2.0f / w - 1.0f, y * 2.0f / h - 1.0f};
  float cell[2] = {16.0f * scale / w, 16.0f * scale / h};
  uint8_t block[128];
  const GpuLayout& l = t->pipe.push;
  std::string err;
  static const char* const names[] = {"c0", "c1", "c2", "c3", "c4"};
  bool ok = true;
  for (int i = 0; i < 5; i++) ok = ok && layout_pack_u32(l, names[i], &words[i * 4], 4, block, &err);
  ok = ok && layout_pack(l, "color", color, 4, block, &err) && layout_pack(l, "bg", bg, 4, block, &err) &&
       layout_pack(l, "origin", origin, 2, block, &err) && layout_pack(l, "cell", cell, 2, block, &err);
  if (!ok) {
    fprintf(stderr, "error: text: %s\n", err.c_str());
    return false;
  }
  GpuDrawDesc d;
  d.pipeline = &t->pipe;
  d.vertex_buffers[0] = &t->quad;
  d.index_buffer = &t->index;
  d.index_type = GpuIndexType::u16;
  d.index_count = 6;
  d.instance_count = n;
  d.push = block;
  return draw(ctx, d);
}

bool text_draw(GpuText* t, GpuPassCtx* ctx, float x, float y, const char* str, uint32_t scale, const float color[4],
               const float bg[4]) {
  if (!ctx->cmd) {
    fprintf(stderr, "error: text outside a pass\n");
    return false;
  }
  if (!t->pipe.h) return false;
  if (scale == 0) scale = 1;
  float cell = 8.0f * scale;
  float ly = y;
  const char* line = str;
  while (true) {
    const char* end = strchr(line, '\n');
    uint32_t len = end ? (uint32_t)(end - line) : (uint32_t)strlen(line);
    for (uint32_t off = 0; off < len; off += GPU_TEXT_CHUNK) {
      uint32_t n = len - off < GPU_TEXT_CHUNK ? len - off : GPU_TEXT_CHUNK;
      if (!draw_chunk(t, ctx, x + off * cell, ly, line + off, n, scale, color, bg)) return false;
    }
    if (!end) break;
    line = end + 1;
    ly += cell;
  }
  return true;
}

void text_shutdown(GpuDevice* dev, GpuText* t) {
  pipeline_destroy(dev, &t->pipe);
  buffer_destroy(dev, &t->quad);
  buffer_destroy(dev, &t->index);
  t->format = VK_FORMAT_UNDEFINED;
}

} // namespace vkc

namespace vkc::part {

bool text_pipeline_desc(VkFormat color_format, GpuPipelineDesc* desc) {
  static const std::string fs = font_glsl() + TEXT_FS;
  std::string err;
  std::vector<GpuFieldDecl> decls = {{"c0", "uvec4"}, {"c1", "uvec4"}, {"c2", "uvec4"},  {"c3", "uvec4"},
                                     {"c4", "uvec4"}, {"color", "vec4"}, {"bg", "vec4"}, {"origin", "vec2"},
                                     {"cell", "vec2"}};
  if (!layout_build("push", decls, &desc->push, &err)) {
    fprintf(stderr, "error: text layout: %s\n", err.c_str());
    return false;
  }
  desc->vs = TEXT_VS;
  desc->fs = fs.c_str();
  desc->vs_name = "text.vs";
  desc->fs_name = "text.fs";
  desc->vertex_layout = {GpuVertexFormat::float2};
  desc->blend = GpuBlend::alpha;
  desc->color_format = color_format;
  return true;
}

} // namespace vkc::part
