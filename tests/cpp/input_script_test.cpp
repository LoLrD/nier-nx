#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "nier_input_script.h"

using std::chrono::milliseconds;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static std::filesystem::path Write(const std::string& name, const std::string& text) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path) << text;
  return path;
}

int main() {
  auto good = NierInputScript::Load(Write("nier_good.tsv", "# title\n1000\tSTART\t100\n\n2000\tA+DPAD_DOWN\t50\n"));
  Check(good.has_value(), "valid script loads");
  if (good) {
    Check(good->ButtonsAt(milliseconds(999)) == 0, "nothing before first press");
    Check(good->ButtonsAt(milliseconds(1000)) == kNierButtonStart, "START pressed at 1000 ms");
    Check(good->ButtonsAt(milliseconds(1099)) == kNierButtonStart, "START held until 1099 ms");
    Check(good->ButtonsAt(milliseconds(1100)) == 0, "START released at 1100 ms");
    Check(good->ButtonsAt(milliseconds(2010)) == (kNierButtonA | kNierButtonDpadDown), "A+DPAD_DOWN combined");
  }

  auto stick = NierInputScript::Load(Write("nier_stick.tsv", "1000\tLS_UP+LS_RIGHT\t500\n1200\tA\t10\n"));
  Check(stick.has_value(), "stick directions load");
  if (stick) {
    const uint32_t held = stick->ButtonsAt(milliseconds(1205));
    Check(held == (kNierStickUp | kNierStickRight | kNierButtonA), "stick directions combine with buttons");
    Check(NierStickAxis(held, kNierStickLeft, kNierStickRight) == 32767, "right stick axis full positive");
    Check(NierStickAxis(held, kNierStickDown, kNierStickUp) == 32767, "up stick axis full positive");
    Check(NierStickAxis(kNierStickLeft, kNierStickLeft, kNierStickRight) == -32768, "left stick axis full negative");
    Check(NierStickAxis(0, kNierStickLeft, kNierStickRight) == 0, "released stick centred");
    Check(uint16_t(held) == kNierButtonA, "stick bits stay out of the button word");
  }

  auto bad = NierInputScript::Load(Write("nier_bad.tsv", "1000\tSTART\t100\n2000\tA\t50\n3000\tJUMP\t10\n"));
  Check(!bad.has_value(), "unknown button rejected");
  if (!bad) {
    Check(bad.error() == "bad input script line 3", "error names line 3");
  }

  auto malformed = NierInputScript::Load(Write("nier_malformed.tsv", "1000\tSTART\n"));
  Check(!malformed.has_value() && malformed.error() == "bad input script line 1", "missing duration rejected");

  auto missing = NierInputScript::Load("/nonexistent/route.tsv");
  Check(!missing.has_value() && missing.error() == "cannot open input script /nonexistent/route.tsv",
        "missing file reported");

  return failures == 0 ? 0 : 1;
}
