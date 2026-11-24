#define REX_STORE_CARDS

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wasm-operand-widths"
#include "../../sdk/src/kernel/crt/memory.cpp"
#pragma clang diagnostic pop

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static void Call(void (*hook)(PPCContext&, uint8_t*), uint8_t* base, uint32_t r3, uint32_t r4, uint32_t r5,
                 PPCContext& ctx) {
  ctx = PPCContext{};
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  hook(ctx, base);
}

int main() {
  std::vector<uint8_t> memory(8u << 20, 0xAA);
  uint8_t* base = memory.data();
  PPCContext ctx{};
  {
    const uint32_t src = 0x00300000;
    const uint32_t dst = 0x00500FF0;
    const uint32_t n = 0x30;
    for (uint32_t i = 0; i < n; ++i) {
      base[src + i] = uint8_t(i);
    }
    std::memset(base + (dst >> 12) - 1, 1, 4);
    Call(rexcrt_memcpy, base, dst, src, n, ctx);
    Check(std::memcmp(base + dst, base + src, n) == 0, "memcpy_copies_the_bytes");
    Check(ctx.r3.u32 == dst, "memcpy_returns_the_destination");
    Check(base[(dst >> 12)] == 0 && base[(dst >> 12) + 1] == 0, "memcpy_marks_every_destination_page");
    Check(base[(dst >> 12) - 1] == 1 && base[(dst >> 12) + 2] == 1, "memcpy_leaves_other_pages_unmarked");
  }
  {
    const uint32_t at = 0x00600000;
    for (uint32_t i = 0; i < 64; ++i) {
      base[at + i] = uint8_t(i);
    }
    Call(rexcrt_memcpy, base, at, at + 8, 32, ctx);
    bool moved = true;
    for (uint32_t i = 0; i < 32; ++i) {
      moved &= base[at + i] == uint8_t(i + 8);
    }
    Check(moved, "memcpy_onto_an_earlier_overlap_copies_forward");
  }
  {
    const uint32_t dst = 0x00701000;
    std::memset(base + (dst >> 12) - 1, 1, 3);
    Call(rexcrt_memset, base, dst, 0x15A, 0x1001, ctx);
    bool filled = true;
    for (uint32_t i = 0; i < 0x1001; ++i) {
      filled &= base[dst + i] == 0x5A;
    }
    Check(filled, "memset_fills_with_the_low_byte");
    Check(base[dst + 0x1001] == 0xAA, "memset_stops_at_the_length");
    Check(ctx.r3.u32 == dst, "memset_returns_the_destination");
    Check(base[(dst >> 12)] == 0 && base[(dst >> 12) + 1] == 0, "memset_marks_every_destination_page");
    Check(base[(dst >> 12) - 1] == 1, "memset_leaves_the_previous_page_unmarked");
  }
  {
    const uint32_t dst = 0x00702000;
    base[dst >> 12] = 1;
    Call(rexcrt_memset, base, dst, 0, 0, ctx);
    Check(base[dst >> 12] == 1, "empty_memset_marks_nothing");
  }
  return failures ? 1 : 0;
}
