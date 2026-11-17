# Bring-up log

Each entry: symptom → cause → fix, with the evidence. Newest last.

## 2026-11-17 — first code generation (Task 3)

- Edition: `default.xex` 0.0.0.1, title 535107E8, media 507D2720, image base 0x82000000, entry 0x8255CEA0
  (`app/game.lock`). Code generation takes ~3.5 s on the M4.
- First generation failed validation: 5 direct `b` branches to addresses outside any function
  (0x821AE070, 0x82499C50, 0x824EA3D0, 0x8252DF28, 0x8252EA50). `tools/find_gaps.py --check` showed each one starts
  an unclaimed gap (20, 8, 8, 24, 32 bytes) → declared as functions in `app/overrides.toml`.
- Structural gap scan (`tools/gap_fixpoint.py`) to fixpoint: 21 passes in two runs, 370 accepted entries.
- Result: 22,348 functions, 97 generated `.cpp` files (109 MB), 0 warnings. Remaining 11,297 gaps / 74,776 bytes are
  mostly 4-byte padding between functions; the rest were rejected by `gap_classify.py` (stack/LR use or registers
  read before written) and are left for the runtime resolver.

## 2026-11-17 — first macOS runs (Tasks 4–5)

- `--game_data_root` from `nier.toml` is read before the config is loaded → `NierApp::OnConfigurePaths` defaults to
  `<exe_dir>/game_root` (same layout as the Switch build); `tools/build_mac.sh` links `out/mac/game_root` to the disc tree.
- Smoke run `first` (30 s, `gpu_plugin = ""`): no crash. Boot, XAudio, XMA, XAM notifications run; the game probes
  loose files before its CPK archives (`MEDIA\2D\SYSTEM_US.2DP`, `MEDIA\MYSETTING\STAGE_SELECT.INI` — absent on the
  disc, expected). Data lives in `media/layer1.cpk`, `media/layer2.cpk`.
- After 14:28:58.176 only the audio thread logs: the game waits. `VdInitializeRingBuffer` and
  `VdSetGraphicsInterruptCallback` were ignored (no GPU plugin), so no VSync interrupt ever reaches it → Task 6.
- Hang detection does not fire while `[apu]` keeps logging; a game-thread stall looks like `timeout_ok` with no
  progress. Judge progress by the game threads, not by log volume.

## 2026-11-17 — macOS graphics backend (Task 6)

- `gpu_plugin = "xenos"` (Vulkan through the MoltenVK bundled with the SDK, Apple M4): device created, 3 swapchain
  images, no validation errors except "Metal does not support disabling primitive restart" and the missing geometry
  shader (fallback paths used). → **Vulkan branch chosen**; no null driver port needed.
- Smoke run `vulkan2` (50 s): 3045 presents at 1280×720, 92 file requests, 0 unregistered calls, no crash.
  Screenshots: CRIWARE logo (8 s), SQUARE ENIX logo (25 s), title-screen ember animation (40 s).
- Open: at 25 s a dialogue subtitle line is drawn over the SQUARE ENIX logo. Not expected on a logo screen; check
  whether the original shows it (subtitle state/string index or timing).
- `tools/mac_smoke.py --shots 8,15,25` captures the game window (`tools/window_id.swift` finds it by PID).

## 2026-11-17 — boot to the prologue (Task 7)

- Scripted input (`app/input/new_game.tsv`): START on the title, DPAD_LEFT+A pairs through the menus. NieR asks for
  the hero's name (`XamShowKeyboardUI`) → SDK cvar `keyboard_ui_auto_text=Nier`; the "Set your name as Nier?"
  prompt starts on *No*, hence DPAD_LEFT before every A.
- Freeze on the first loading screen: the fork's SPIR-V translator aliased `xe_var_tfetch_lod` to the int
  `xe_var_vfetch_bound` → invalid `OpFAdd(int, float)` in every pixel shader with a LOD bias → MoltenVK rejected the
  pipelines and then faulted in `writeDescriptorSetBinding` on the GPU command thread (found with `--stacks-at` +
  lldb). Fixed in `sdk/src/graphics/pipeline/shader/spirv_translator.cpp` (see `sdk/UPSTREAM.md`). This bug is not
  macOS-specific: the Switch build uses the same translator.
- Runtime-resolved functions (indirect calls into unclaimed gaps): 0x82154BA8, 0x82155DD0, 0x82153A08, 0x82152740,
  0x82269DA8 — one `runtime_gap_resolver.py --apply` each.
- Crash in the prologue: "Unresolved branch from 0x8226045C to 0x82260468". The structural gap scan had declared
  0x8226044C (a lone `blr` inside the jump-table switch sub_82260360) as a function, cutting the switch short. Same
  pattern at 0x823FE8F8 (loop tail of sub_823FE7C0). Both removed and the owners given explicit `end`s; the generated
  code now has 0 `Unresolved branch` fatals. `tools/gap_fixpoint.py` now rejects candidates whose scratch listing has
  an unresolved branch.
- Reached: opening credits, prologue cutscene ("Go on... Take my power..."), first tutorial prompt
  ("Press X to attack and LT to defend"). The scene behind the prompt is white — snowstorm or a render bug, to check.
- Pointer scan (`NIER_DUMP_IMAGE` + `tools/pointer_scan.py`): 15 data pointers start unclaimed gaps; 14 (>= 8 bytes)
  declared at once instead of one crash per 15-minute run. One more runtime-resolved function: 0x8214DA18.
- **Acceptance (Task 7):** generated tree deleted and regenerated with `tools/codegen.sh` (0 unresolved branches),
  `tools/build_mac.sh`, then `mac_smoke.py --seconds 600 --name accept` with `app/input/prologue.tsv`: `timeout_ok`,
  20,131 presents (last one 3 s before the end), 194 file requests, 0 unregistered calls, 0 critical log lines. The
  first playable area is the 2053 prologue; the scripted route plays its fights (level up, combos, the Grimoire's
  magic bar) for 10 minutes.

### Open issues found during bring-up (not blocking phase 1)

- **White gameplay on macOS.** Cutscenes, menus, HUD and minimap render correctly; the 3D scene during gameplay is
  white. Ruled out: depth format conversion (`depth_float24_convert_in_pixel_shader=true`), MSAA
  (`native_2x_msaa=false`), HDR render-target formats (gameplay renders to k_8_8_8_8 only). Next: capture one frame
  and find the full-screen pass that writes white (likely a resolve or a texture fetch in post-processing). Unknown
  whether it reproduces on the Switch (NVK) — check first thing in phase 2.
- **Audio crackle from the main menu on** (user report; the title screen is clean). The signal the game hands to SDL
  is clean in `audio_dump_output_s` dumps of the title (30–50 s) and the menu (55–80 s): no clipping, no gaps, no
  kinks at 256-sample frame boundaries, and the SDL callback never ran out of frames. So the crackle is between SDL
  and CoreAudio (device buffer). Trying `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2048`; awaiting the user's listening test.
- **Tearing** (user report): the SDK allows `IMMEDIATE` present mode on desktop → `vulkan_allow_present_mode_immediate
  = false` in `nier.toml`.

## 2026-11-17 — 15 Nightmares DLC (Task 8)

- Layout the SDK expects: `<content_marketplace_root>/0000000000000000/535107E8/00000002/<package>` (raw STFS is
  mounted directly). `tools/unpack_dumps.sh` links `marketplace/0000000000000000` to `content/`; `nier.toml` sets
  `dlc_enable = true`, `content_marketplace_root = "assets/marketplace"`.
- NieR enumerates Marketplace content right after boot. The entry first came out as `'@0p'`: `ListContent` tested the
  `X_RESULT` of `ReadContentHeaderFile` with `XFAILED`, so a missing `.header` (`X_ERROR_FILE_NOT_FOUND` = 2) counted
  as success and the game got an uninitialized entry. Fixed (see `sdk/UPSTREAM.md`); now
  `'B5849AE068830A162D28A72C2E07667A19BCD61853' (displayed as '15 Nightmares')`.
- Not verified yet: mounting (`[dlc] mount ...`). The game did not open the package in the first 2 minutes; it
  likely mounts it when the player reaches the DLC's entry point. Check during phase 3 playthrough.
