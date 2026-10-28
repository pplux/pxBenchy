#include "gpu/vkc/timers.h"
#include <algorithm>
#include <cstdio>
#include <deque>

namespace vkc {

bool timers_init(GpuDevice* dev, GpuTimers* t) {
  *t = {};
  uint32_t n = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(dev->physical, &n, nullptr);
  std::vector<VkQueueFamilyProperties> fams(n);
  vkGetPhysicalDeviceQueueFamilyProperties(dev->physical, &n, fams.data());
  uint32_t bits = dev->queue_family < n ? fams[dev->queue_family].timestampValidBits : 0;
  float period = dev->props.limits.timestampPeriod;
  if (bits == 0 || (!dev->props.limits.timestampComputeAndGraphics && period == 0.0f)) {
    fprintf(stderr, "warning: GPU timestamps unsupported, GPU timers disabled\n");
    return true;
  }
  t->tracy = dev->tracy;
  t->period_ns = period;
  t->mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
  VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  ci.queryCount = PXB_TIMER_QUERIES;
  for (GpuTimerSlot& s : t->slots) {
    VkResult r = vkCreateQueryPool(dev->device, &ci, nullptr, &s.pool);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateQueryPool failed (%d)\n", (int)r);
      timers_shutdown(dev, t);
      return false;
    }
  }
  t->enabled = true;
  return true;
}

void timers_shutdown(GpuDevice* dev, GpuTimers* t) {
  for (GpuTimerSlot& s : t->slots)
    if (s.pool) vkDestroyQueryPool(dev->device, s.pool, nullptr);
  *t = {};
}

static double to_ns(const GpuTimers* t, uint64_t a, uint64_t b) { return double((b - a) & t->mask) * t->period_ns; }

void timers_break_interval(GpuTimers* t) {
  if (t->enabled && !t->cmd && t->slots[t->cur].ended) t->slots[t->cur].break_interval = true;
}

void timers_resolve_all(GpuDevice* dev, GpuTimers* t) {
  if (!t->enabled) return;
  GpuTimerSlot* order[PXB_FRAMES_IN_FLIGHT];
  for (uint32_t i = 0; i < PXB_FRAMES_IN_FLIGHT; i++) order[i] = &t->slots[i];
  std::sort(order, order + PXB_FRAMES_IN_FLIGHT,
            [](const GpuTimerSlot* a, const GpuTimerSlot* b) { return a->seq < b->seq; });
  for (GpuTimerSlot* s : order) part::timers_resolve_slot(dev, t, s);
}

void timers_clear(GpuTimers* t) {
  t->busy_ns.clear();
  t->interval_ns.clear();
  t->has_prev = false;
  for (GpuUserTimer& u : t->user) u.ns.clear();
}

uint32_t timer_create(GpuTimers* t, const char* name) {
  for (const GpuUserTimer& u : t->user)
    if (u.name == name) return UINT32_MAX;
  GpuUserTimer u;
  u.name = name;
#ifdef TRACY_ENABLE
  // Tracy may read source locations until process exit: stable storage, never freed
  struct Loc {
    std::string name;
    tracy::SourceLocationData data;
  };
  static std::deque<Loc>* locs = new std::deque<Loc>();
  Loc& l = locs->emplace_back();
  l.name = name;
  l.data = {l.name.c_str(), "gpu.timer", __FILE__, __LINE__, 0};
  u.loc = &l.data;
#endif
  t->user.push_back(std::move(u));
  return (uint32_t)t->user.size() - 1;
}

const std::vector<double>& timers_busy(const GpuTimers* t) { return t->busy_ns; }
const std::vector<double>& timers_interval(const GpuTimers* t) { return t->interval_ns; }
const std::vector<double>& timer_results(const GpuTimers* t, uint32_t id) { return t->user[id].ns; }

} // namespace vkc

namespace vkc::part {

void timers_resolve_slot(GpuDevice* dev, GpuTimers* t, GpuTimerSlot* s) {
  if (!s->pending) return;
  s->pending = false;
  if (!s->ended) return;
  uint64_t d[2 * PXB_TIMER_QUERIES]; // value, availability
  VkResult r = vkGetQueryPoolResults(dev->device, s->pool, 0, s->used, sizeof(uint64_t) * 2 * s->used, d,
                                     sizeof(uint64_t) * 2,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (r != VK_SUCCESS && r != VK_NOT_READY) {
    fprintf(stderr, "error: vkGetQueryPoolResults failed (%d)\n", (int)r);
    return;
  }
  if (d[1]) { // frame start available
    if (d[3]) t->busy_ns.push_back(to_ns(t, d[0], d[2]));
    if (t->has_prev) t->interval_ns.push_back(to_ns(t, t->prev_start, d[0]));
    t->has_prev = !s->break_interval;
    t->prev_start = d[0];
  }
  for (const GpuTimerRecord& rec : s->records) {
    const uint64_t* b = &d[2 * rec.query];
    const uint64_t* e = &d[2 * (rec.query + 1)];
    if (b[1] && e[1]) t->user[rec.id].ns.push_back(to_ns(t, b[0], e[0]));
  }
}

} // namespace vkc::part
