#include <cstdint>
#include <cstdio>

#include "ring_state.h"

using nier::native::RingState;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  {
    RingState ring;
    ring.Initialize(0x1000, 11);
    Check(ring.base() == 0x1000 && ring.size_words() == 4096, "size_from_log2");
  }
  {
    RingState ring;
    Check(!ring.HasWork(0), "has_work_without_ring");
    ring.Initialize(0x1000, 2);
    ring.SetWritePointer(8 + 3);
    Check(ring.write_pointer() == 11 && ring.HasWork(0) && !ring.HasWork(3), "has_work_masks_pointer");
  }
  {
    RingState ring;
    ring.Initialize(0x1000, 2);
    ring.Initialize(0x2000, 2);
    Check(ring.generation() == 2 && ring.base() == 0x2000, "ring_generation_resets_read");
  }
  return failures ? 1 : 0;
}
