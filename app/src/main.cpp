#include "generated/default/nier_init.h"

#include "nier_app.h"

#ifdef __SWITCH__
#include <sys/iosupport.h>
extern "C" int __system_argc;
extern "C" char** __system_argv;
extern "C" const devoptab_t dotab_stdnull;
namespace {
const bool kArgvByDefault = [] {
  static char path[] = "sdmc:/switch/nier-nx/nier-nx.nro";
  static char* argv[] = {path, nullptr};
  if (__system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0]) {
    __system_argc = 1;
    __system_argv = argv;
  }
  if (!devoptab_list[STD_OUT]) devoptab_list[STD_OUT] = &dotab_stdnull;
  if (!devoptab_list[STD_ERR]) devoptab_list[STD_ERR] = &dotab_stdnull;
  return true;
}();
}  // namespace
#endif

REXCVAR_DEFINE_STRING(nier_input_script, "", "NieR", "TSV of scripted pad presses: <ms>\\t<BUTTON+BUTTON>\\t<ms held>");
REXCVAR_DEFINE_STRING(nier_control_file, "", "NieR",
                      "File read every 0.3 s for 'cvar = value' lines (NIER_CONTROL_FILE wins); on the Switch for "
                      "example sdmc:/switch/nier/control.txt");
REXCVAR_DEFINE_STRING(nier_ab_flag, "", "NieR",
                      "A/B inside one run: this cvar cycles through nier_ab_values, nier_ab_period_s seconds each; "
                      "every switch is logged as '[ab] <cvar> = <value>'");
REXCVAR_DEFINE_STRING(nier_ab_values, "true,false", "NieR", "Comma-separated values nier_ab_flag cycles through");
REXCVAR_DEFINE_INT32(nier_ab_period_s, 20, "NieR", "Seconds each value of nier_ab_values stays set");
REXCVAR_DEFINE_INT32(nier_pgo_dump_s, 600, "NieR",
                     "PGO training builds (NIER_PGO=generate) only: seconds after start when the profile is written "
                     "to sdmc:/switch/nier/pgo");

REX_DEFINE_APP(nier, NierApp::Create)
