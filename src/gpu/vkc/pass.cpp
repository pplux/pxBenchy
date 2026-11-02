#include "gpu/vkc/pass.h"
#include <cstdio>
#include "gpu/vkc/resources.h"

namespace vkc {

static bool check_buffer(const GpuBuffer* buf, const char* what) {
  if (buf->state.uses & USE_WRITES) {
    fprintf(stderr, "error: draw: %s buffer has a pending write (close it before the pass)\n", what);
    return false;
  }
  return true;
}

bool draw(GpuPassCtx* ctx, const GpuDrawDesc& d) {
  if (!ctx->cmd) {
    fprintf(stderr, "error: draw outside a pass\n");
    return false;
  }
  if (!d.pipeline || !d.vertex_buffers[0] || !d.index_buffer) {
    fprintf(stderr, "error: draw needs pipeline, vertex_buffers[0] and index_buffer\n");
    return false;
  }
  if (d.pipeline->push.size > 0 && !d.push) {
    fprintf(stderr, "error: draw: pipeline has %u bytes of push constants but push is null\n",
            (unsigned)d.pipeline->push.size);
    return false;
  }
  if (!check_buffer(d.vertex_buffers[0], "vertex") || !check_buffer(d.index_buffer, "index")) { return false; }
  if (d.vertex_buffers[1] && !check_buffer(d.vertex_buffers[1], "instance")) { return false; }
  if (d.indirect && !check_buffer(d.indirect, "indirect")) { return false; }

  VkCommandBuffer cmd = ctx->cmd;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline->h);
  VkBuffer vbs[2] = {d.vertex_buffers[0]->h, VK_NULL_HANDLE};
  VkDeviceSize offsets[2] = {0, 0};
  uint32_t vb_count = 1;
  if (d.vertex_buffers[1]) {
    vbs[1] = d.vertex_buffers[1]->h;
    vb_count = 2;
  }
  vkCmdBindVertexBuffers(cmd, 0, vb_count, vbs, offsets);
  vkCmdBindIndexBuffer(cmd, d.index_buffer->h, 0,
                       d.index_type == GpuIndexType::u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
  if (d.pipeline->push.size > 0) {
    vkCmdPushConstants(cmd, d.pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       d.pipeline->push.size, d.push);
  }
  if (d.indirect) {
    vkCmdDrawIndexedIndirect(cmd, d.indirect->h, 0, d.draw_count, sizeof(VkDrawIndexedIndirectCommand));
  } else {
    vkCmdDrawIndexed(cmd, d.index_count, d.instance_count, 0, 0, 0);
  }
  return true;
}

void pass_shutdown(GpuDevice* dev, GpuPassCtx* ctx) {
  texture_destroy(dev, &ctx->depth);
  ctx->cmd = VK_NULL_HANDLE;
}

} // namespace vkc
