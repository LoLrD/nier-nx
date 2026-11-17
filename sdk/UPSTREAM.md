# Origin of this SDK

Copied from the `sdk/` folder of [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) at commit
`650e14ef109fb0d39bd68ba8d7af61fe459265b8` (2026-10-09): a fork of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
v0.10.0 (commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`) with the Horizon (Nintendo Switch) layer by StevensND and the
code generator and runtime changes of masseffect-nx. The third-party sources are fetched with `tools/fetch_thirdparty.py`.

## Local changes

- `cmake/rexglue_helpers.cmake` (`rexglue_configure_target`, desktop branch): add imgui's include path to the title
  target. The fork links imgui privately into rexruntime and fixed the include path only in
  `cmake/rexglue_switch.cmake`; `rex_app.cpp` and `rex/ui/style.h` include `imgui.h`, so desktop builds failed.
- `cmake/rexglue_helpers.cmake` (`_rexglue_stage_macos_vulkan_runtime`): locate `MoltenVK_icd.json` next to the
  helper file instead of `${REXGLUE_ROOT}/cmake`. `REXGLUE_ROOT` is a directory-scoped variable of the SDK, empty in
  the title's directory where the post-build copy is configured, so macOS links failed copying `/cmake/MoltenVK_icd.json`.
- `src/kernel/xam/xam_ui.cpp` (`XamShowKeyboardUI`): cvar `keyboard_ui_auto_text`. When set, keyboard prompts are
  answered with that text at once, without the ImGui dialog, so automated runs can pass name entry (NieR asks for the
  hero's name after the difficulty menu; `headless` answers with the empty default text).
- `src/graphics/pipeline/shader/spirv_translator.cpp`: `var_main_tfetch_lod_` was assigned the int variable
  `xe_var_vfetch_bound` (a broken merge of the fork's vfetch bound tracking: `var_main_tfetch_lod_ = var_main_vfetch_bound_ =
  createVariable(int ...)`, with the float `xe_var_tfetch_lod` created and dropped). Every pixel shader with a texture
  LOD bias then did `OpFAdd(int, float)`: invalid SPIR-V that MoltenVK rejects ("no matching function for call to
  spvFAdd"), and the failed pipelines led to a crash in MoltenVK's descriptor update on the GPU command thread (NieR
  froze on the first loading screen). Each variable now has its own declaration, as in upstream ReXGlue.
