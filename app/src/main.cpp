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

REX_DEFINE_APP(nier, NierApp::Create)
