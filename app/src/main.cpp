#include "generated/default/nier_init.h"

#include "nier_app.h"

REXCVAR_DEFINE_STRING(nier_input_script, "", "NieR", "TSV of scripted pad presses: <ms>\\t<BUTTON+BUTTON>\\t<ms held>");

REX_DEFINE_APP(nier, NierApp::Create)
