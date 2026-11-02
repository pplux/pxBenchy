// Pointer event queue shared by the platform layers (filled in platform_pump, drained by the UI).
#include "platform/platform.h"

// The UI drains the queue once per ImGui frame: offscreen runs build one every ~1 s, so moves coalesce and the queue
// is bounded (older events dropped past the limit, which only a flood of clicks between two frames can reach).
static constexpr size_t POINTER_QUEUE_MAX = 256;
static std::vector<PlatformPointerEvent> s_queue;

void platform_pointer_push(const PlatformPointerEvent& e) {
  if (e.kind == PlatformPointerEvent::pos && !s_queue.empty() && s_queue.back().kind == PlatformPointerEvent::pos &&
      s_queue.back().touch == e.touch) {
    s_queue.back() = e;
    return;
  }
  if (s_queue.size() >= POINTER_QUEUE_MAX) { s_queue.erase(s_queue.begin()); }
  s_queue.push_back(e);
}

void platform_pointer_events(std::vector<PlatformPointerEvent>* out) {
  out->clear();
  out->swap(s_queue);
}
