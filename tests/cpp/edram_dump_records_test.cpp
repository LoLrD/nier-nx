#include <cstdio>
#include <cstdlib>

#include "edram_dump_records.h"

using rex::graphics::vulkan::EdramDumpRecords;
using rex::graphics::vulkan::EdramDumpSpan;

namespace {

int rt_a, rt_b;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

EdramDumpSpan Span(const void* rt, uint32_t start, uint32_t end) {
  return EdramDumpSpan{rt, 16, 16, 0, 45, 0, 16, start, end};
}

}  // namespace

int main() {
  EdramDumpRecords records;
  Expect(!records.Contains(Span(&rt_a, 720, 1440)), "empty set holds nothing");

  records.Add(Span(&rt_a, 720, 1440));
  Expect(records.Contains(Span(&rt_a, 720, 1440)), "added span is found");
  Expect(!records.Contains(EdramDumpSpan{&rt_a, 16, 16, 0, 44, 0, 16, 720, 1424}), "other rectangle of the same target");

  Expect(!records.Contains(EdramDumpSpan{&rt_a, 32, 16, 0, 45, 0, 16, 720, 1440}), "other resolve pitch");

  records.Add(Span(&rt_b, 0, 720));
  Expect(records.Contains(Span(&rt_a, 720, 1440)), "dump of a disjoint range keeps the record");
  Expect(records.Contains(Span(&rt_b, 0, 720)), "second record kept");

  records.Add(Span(&rt_b, 1000, 1100));
  Expect(!records.Contains(Span(&rt_a, 720, 1440)), "overlapping dump drops the record");

  records.Clear();
  records.Add(Span(&rt_a, 100, 200));
  records.Add(Span(&rt_b, 1900, 2300));
  Expect(!records.Contains(Span(&rt_a, 100, 200)), "dump wrapping past the EDRAM end drops the record at its start");

  records.Clear();
  Expect(!records.Contains(Span(&rt_b, 1900, 2300)), "clear forgets everything");
  return 0;
}
