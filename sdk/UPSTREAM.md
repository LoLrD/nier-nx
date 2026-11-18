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
- `src/system/xam/content_manager.cpp` (`ListContent`): the result of `ReadContentHeaderFile` (an `X_RESULT`, Win32
  codes: `X_ERROR_FILE_NOT_FOUND` is 2) was tested with `XFAILED`/`XSUCCEEDED`, which check NTSTATUS severity bits, so a
  missing `.header` counted as success and the game got an uninitialized entry (NieR's 15 Nightmares DLC listed as
  `'@0p'`; save folders without a header the same). Now compared with `X_ERROR_SUCCESS`.
- `include/rex/kernel/xboxkrnl/video.h`, `src/kernel/xboxkrnl/xboxkrnl_video.cpp`: `GuestSwapCount()`, the number of
  `VdSwap` calls (one per guest frame), so a title can feed the F3 debug overlay's FPS line
  (`ReXApp::SetGuestFrameStats`); the SDK had no public frame counter.
- `src/graphics/vulkan/command_processor.cpp`: run-time GPU debugging cvars for bisecting wrong images —
  `gpu_debug_skip_draw_first`/`gpu_debug_skip_draw_count` (skip draws by index within a frame), `gpu_debug_skip_ps`
  (skip draws by pixel shader hash), `gpu_debug_describe_draw`/`gpu_debug_describe_frame` (log shaders, primitive, render
  target, blend and resolve registers of one draw or a whole frame). All off by default.
- `thirdparty/FFmpeg/config.h`: select `config_switch_aarch64.h` for `__SWITCH__`. masseffect-nx ships that header and
  un-ignores a modified `config.h` in its `.gitignore`, but the modified `config.h` is not in its repository, so a Switch
  build of the published tree fails with `#error "no config"`. The WMV3 add-on (`config_masseffect_wmv3.h`) is not
  included: NieR has no WMV movies.
- `src/kernel/xam/xam_ui.cpp` (`XamShowKeyboardUI`, Switch only): show the system software keyboard (libnx `swkbd`)
  with the game's title, description and default text instead of the ImGui keyboard dialog, which has no usable input on
  the console. `keyboard_ui_auto_text` and `headless` still take precedence.
- `src/ui/switch_perf.cpp`, `src/graphics/vulkan/render_target_cache.cpp`: the GPU A/B lap (L+R+ZL) splits transfers
  into stencil bit draws (64) and host depth stores (128), and resolves into render target dumps (256), clears (512)
  and copies (1024); the old "no transfers" (2) and "no resolves" (4) modes still cover all of them. "No draw
  pixels" (2048) gives the guest draws a zero scissor, so the GPU still runs their vertices but shades no pixel.
- `src/graphics/vulkan/render_target_cache.cpp`, `command_processor.cpp`: `gpu_debug_describe_frame` also logs EDRAM
  ownership transfers (`[transfer]`, with the 8 stencil bit draws of a depth destination when the device has no
  `VK_EXT_shader_stencil_export`) and render target dumps for resolves (`[dump]`).
- `include/rex/graphics/vulkan/edram_dump_records.h`, `render_target_cache.cpp`: a resolve does not dump a render target
  again if the same rectangle of it was already dumped to the EDRAM buffer for the same resolve span and nothing has
  changed since (any draw, transfer, resolve clear or cache clear forgets the records; a dump drops the records of the
  EDRAM range it overwrites). NieR resolves the same 640x360 4xMSAA target three times in a row in the menu.
- `src/graphics/vulkan/render_target_cache.cpp`: the generated render target dump and EDRAM transfer shaders compute
  EDRAM addresses without 32-bit integer multiplies and divisions. NAK emits the microcoded legacy `IMUL` on Maxwell
  (SM50, the Switch's GM20B), and a dump or transfer pass of a 640x360 4xMSAA target cost 8-10 ms there. Multiplies by
  constants are sums of shifts, products with the pitch are float products (exact below 2^24), and divisions by a
  constant tile size or the pitch are `uint((float(x) + 0.5) * (1 / d))`, exact for x < 2^16 even with a reciprocal
  off by 2 ulp.
- `readback_resolve_max_kb` (`src/graphics/command_processor.cpp`, Vulkan path): reads back only resolves up to that
  size; `gpu_debug_describe_frame` logs every readback (`[readback]`). With `readback_resolve = "fast"` every resolve
  was copied back to guest memory: in NieR's gameplay 28 per frame, about 56 MB, all `memcpy` on the GPU command thread
  (plus the cache invalidation of the readback buffer and the texture reloads the CPU writes cause). NieR only reads
  its exposure chain (4-256 KB); `readback_resolve_max_kb = 256` keeps the image right.
- `src/graphics/CMakeLists.txt`: on Apple the `rexgpu-xenos` dylib keeps default symbol visibility, so an app can link
  it (NieR's native renderer uses the SDK's shader translator and register file); the plugin loader still opens the
  same image. Other platforms keep hidden visibility; the Switch build is a static archive.
- `include/rex/ui/vulkan/functions/device_1_0.inc`: the device function table also loads `vkCmdBlitImage`,
  `vkCmdClearDepthStencilImage`, `vkFreeDescriptorSets`, `vkCmdWriteTimestamp`, `vkGetQueryPoolResults` and
  `vkCmdCopyImage` (core 1.0), used by NieR's native renderer for resolves, new depth targets, evicted resolve images,
  GPU time per category and EDRAM tile transfers between render targets.
- `src/ui/vulkan/vulkan_presenter.cpp`: guest output images also have `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, so a graphics system
  can blit or clear its frontbuffer into them (NieR's native renderer).
- `include/rex/ui/graphics_upload_buffer_pool.h`, `src/ui/graphics_upload_buffer_pool.cpp`: a pool reports how many pages
  it holds and their size (`page_count`, `page_size`), for NieR's native renderer memory report.
- `include/rex/ui/vulkan/upload_buffer_pool.h`, `src/ui/vulkan/vulkan_upload_buffer_pool.cpp`: an upload buffer pool can
  ask for host-cached memory (flushed explicitly when not coherent). On the Switch, CPU writes to the uncached
  upload memory NVK exposes ran at ~100-180 MB/s in NieR's native renderer.
- `src/ui/switch_perf.cpp`: `RexSwitchPerfUseModes(masks, names, count)` replaces the A/B lap's mode table (up to 16
  modes), so a graphics system with its own skip bits (NieR's native renderer) gets a short lap of its own modes.
