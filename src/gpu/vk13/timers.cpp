#include "gpu/vk13.h"
#include <cstdio>

namespace vk13 {

static void write_ts(VkCommandBuffer cmd, VkPipelineStageFlagBits2 stage, VkQueryPool pool, uint32_t q) {
  vkCmdWriteTimestamp2(cmd, stage, pool, q);
}

bool timers_frame_begin(GpuDevice* dev, GpuSwapchain* sc, GpuTimers* t, VkCommandBuffer cmd) {
  if (!t->enabled) { return true; }
  if (t->cmd) {
    fprintf(stderr, "error: timers frame_begin inside a frame\n");
    return false;
  }
  GpuTimerSlot& s = t->slots[sc->frame];
  vkc::part::timers_resolve_slot(dev, t, &s);
  vkCmdResetQueryPool(cmd, s.pool, 0, PXB_TIMER_QUERIES);
  write_ts(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s.pool, 0);
#ifdef TRACY_ENABLE
  static constexpr tracy::SourceLocationData frame_loc{"gpu_frame", __func__, __FILE__, __LINE__, 0};
  t->frame_zone = std::make_unique<tracy::VkCtxScope>(t->tracy, &frame_loc, cmd, true);
#endif
  s.used = 2;
  s.seq = t->next_seq++;
  s.pending = true;
  s.ended = false;
  s.break_interval = false;
  s.overflow_printed = false;
  s.records.clear();
  t->cur = sc->frame;
  t->cmd = cmd;
  return true;
}

static void close_zone(GpuTimerOpen* o) {
  o->zone.reset(); // writes the zone end timestamp into the cmd it was created with
}

bool timers_frame_end(GpuTimers* t, VkCommandBuffer cmd) {
  if (!t->enabled) { return true; }
  if (!t->cmd || cmd != t->cmd) {
    fprintf(stderr, "error: timers frame_end outside a frame\n");
    return false;
  }
  for (size_t i = t->open.size(); i-- > 0;) {
    fprintf(stderr, "error: GPU timer '%s' still open at frame end, discarded\n", t->user[t->open[i].id].name.c_str());
    close_zone(&t->open[i]);
  }
  t->open.clear();
  t->frame_zone.reset();
  GpuTimerSlot& s = t->slots[t->cur];
  write_ts(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, s.pool, 1);
  s.ended = true;
  t->cmd = VK_NULL_HANDLE;
  return true;
}

bool timer_begin(GpuTimers* t, VkCommandBuffer cmd, uint32_t id) {
  if (!t->enabled) { return true; }
  const char* name = t->user[id].name.c_str();
  if (!t->cmd || cmd != t->cmd) {
    fprintf(stderr, "error: GPU timer '%s' start outside the frame command buffer\n", name);
    return false;
  }
  for (const GpuTimerOpen& o : t->open) {
    if (o.id == id) {
      fprintf(stderr, "error: GPU timer '%s' already open\n", name);
      return false;
    }
  }
  GpuTimerSlot& s = t->slots[t->cur];
  GpuTimerOpen o;
  o.id = id;
  o.cmd = cmd;
  if (s.used + 2 > PXB_TIMER_QUERIES) {
    if (!s.overflow_printed) {
      fprintf(stderr, "error: GPU timer queries exhausted (%u per frame), timers dropped\n", PXB_TIMER_QUERIES);
    }
    s.overflow_printed = true;
  } else {
    o.query = s.used;
    s.used += 2;
    write_ts(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s.pool, o.query);
#ifdef TRACY_ENABLE
    o.zone =
        std::make_unique<tracy::VkCtxScope>(t->tracy, (const tracy::SourceLocationData*)t->user[id].loc, cmd, true);
#endif
  }
  t->open.push_back(std::move(o));
  return true;
}

bool timer_end(GpuTimers* t, VkCommandBuffer cmd, uint32_t id) {
  if (!t->enabled) { return true; }
  const char* name = t->user[id].name.c_str();
  if (t->open.empty() || t->open.back().id != id) {
    bool is_open = false;
    for (const GpuTimerOpen& o : t->open) { is_open |= o.id == id; }
    if (is_open) {
      fprintf(stderr, "error: GPU timer '%s' stop: not the innermost open timer\n", name);
    } else {
      fprintf(stderr, "error: GPU timer '%s' stop: not open\n", name);
    }
    return false;
  }
  GpuTimerOpen& o = t->open.back();
  if (cmd != o.cmd) {
    fprintf(stderr, "error: GPU timer '%s' stop in a different command buffer than its start\n", name);
    return false;
  }
  if (o.query != UINT32_MAX) {
    GpuTimerSlot& s = t->slots[t->cur];
    write_ts(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, s.pool, o.query + 1);
    s.records.push_back({o.id, o.query});
    close_zone(&o);
  }
  t->open.pop_back();
  return true;
}

} // namespace vk13
