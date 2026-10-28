#pragma once
#include "gpu/vk.h"

// Barrier decisions shared by the modes: which transitions are needed and the tracked state after them. Records
// nothing; each mode's ensure_state translates the transitions to its own barriers.
namespace vkc::part {

// Barrier when tracked or wanted uses have a write (USE_WRITES), or (image) the layout differs: returns true, sets
// *from = tracked and tracked = want. Read->read with the same layout returns false and ORs want's uses into tracked,
// so a later write waits on all prior readers.
bool track(State* tracked, State want, bool image, State* from);

// Mips [first_mip, first_mip + mip_count) going from `from` to the wanted state.
struct MipTransition {
  uint32_t first_mip;
  uint32_t mip_count;
  State from;
};

// Per-mip track over states[first_mip, first_mip + mip_count) (image rule); consecutive mips with the same tracked
// state share one transition. Writes at most mip_count entries to out, returns how many.
uint32_t track_mips(State* states, uint32_t first_mip, uint32_t mip_count, State want, MipTransition* out);

} // namespace vkc::part
