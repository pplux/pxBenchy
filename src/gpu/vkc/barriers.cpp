#include "gpu/vkc/barriers.h"

namespace vkc::part {

bool track(State* tracked, State want, bool image, State* from) {
  if (!((tracked->uses | want.uses) & USE_WRITES) && !(image && layout_of(tracked->uses) != layout_of(want.uses))) {
    tracked->uses |= want.uses;
    return false;
  }
  *from = *tracked;
  *tracked = want;
  return true;
}

uint32_t track_mips(State* states, uint32_t first_mip, uint32_t mip_count, State want, MipTransition* out) {
  uint32_t n = 0;
  bool extend = false; // previous mip got a transition, out[n - 1] may grow
  State prev;          // that mip's tracked state before the transition
  for (uint32_t i = first_mip; i < first_mip + mip_count; i++) {
    State from;
    if (!track(&states[i], want, true, &from)) {
      extend = false;
      continue;
    }
    if (extend && prev.uses == from.uses) {
      out[n - 1].mip_count++;
    } else {
      out[n++] = {i, 1, from};
    }
    extend = true;
    prev = from;
  }
  return n;
}

} // namespace vkc::part
