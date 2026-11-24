#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "nier_control_file.h"

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  const auto parsed = ParseNierControl("# comment\ngpu_debug_skip_draw_first = 120\n\n gpu_debug_skip_draw_count=8 \nbad line\n");
  Check(parsed.size() == 2, "two settings parsed, comment, blank and bad lines ignored");
  Check(parsed.size() == 2 && parsed[0] == std::pair<std::string, std::string>{"gpu_debug_skip_draw_first", "120"},
        "first setting with spaces around '='");
  Check(parsed.size() == 2 && parsed[1] == std::pair<std::string, std::string>{"gpu_debug_skip_draw_count", "8"},
        "second setting trimmed");
  Check(ParseNierControl("").empty(), "empty file");
  Check(SplitNierValues(" true, false ,,0.6") == std::vector<std::string>{"true", "false", "0.6"},
        "values split on commas, trimmed, empty ones dropped");
  Check(SplitNierValues("").empty(), "no values");
  Check(NierAbIndex(0, 20000, 2) == 0 && NierAbIndex(19999, 20000, 2) == 0, "first period takes the first value");
  Check(NierAbIndex(20000, 20000, 2) == 1 && NierAbIndex(40000, 20000, 2) == 0, "periods cycle through the values");
  Check(NierAbIndex(65000, 20000, 3) == 0, "three values cycle too");
  Check(NierAbIndex(5000, 0, 2) == 0, "zero period keeps the first value");
  return failures == 0 ? 0 : 1;
}
