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

## 2026-11-17 — manual play feedback

- Audio "choppy, like 5 fps": in `audio_dump_output_s` dumps of the menu, exactly every 4th 256-sample frame was all
  zeros (1406 of 5624, always frame index ≡ 0 mod 4), independent of the SDL device buffer size and of
  `audio_xma_in_worker`. The SDL driver released the game's audio semaphore as fast as SDL consumed frames, so NieR's
  audio callback ran in bursts and its XMA voices starved every 4th frame. `audio_sdl_pump = true` (one game frame
  every 5.333 ms, as on the console) → 0 zero frames.
- F3 overlay had no FPS: the title must feed `SetGuestFrameStats`. The SDK now counts `VdSwap` calls
  (`GuestSwapCount()`), `NierFpsMeter` turns them into FPS over 0.5 s windows.
- Logs moved to `out/logs/` (`tools/build_mac.sh` wipes `out/mac`, which deleted the log of the first manual session).
- Menu frame time jumping between 16 and 33 ms (user report; gameplay at 30 FPS was steady): with FIFO, the only
  vsync mode MoltenVK offers here, presenting inside the GPU command thread blocks it until the display refresh, and
  the 60 FPS menu missed every fourth vblank. Menu idle 60–95 s (`app/input/menu_idle.tsv`): p90 34 ms, 26 % of frames
  ≥ 20 ms, max 234 ms → with `present_own_thread = true` p90 18 ms, max 19 ms, still FIFO (no tearing).

## 2026-11-17 — white scenes: CPU reads back resolves

- Tooling: `NIER_CONTROL_FILE` (cvars applied at run time, `capture = <path>` writes the guest frame as PPM via
  `CaptureGuestOutput`, independent of the window) + SDK cvars `gpu_debug_describe_frame`, `gpu_debug_skip_ps`,
  `gpu_debug_skip_draw_first/count`.
- Frame anatomy (prologue gameplay, ~270 draws): scene into EDRAM 0x2D0; 175 `9DCAD296…` packs the scene as RGBM
  (rgb / max, alpha = max · scale + bias); 176–185 `CCB40C96…` downsample to 1×1 (resolves into one mip chain);
  186 `5B08DBAE…` depth of field (depth from a raw D24S8 → RGBA8 resolve); 187 `3A56DB93…` RGBM decode + tone curve
  with an exposure value; 249–270 glow and UI.
- Skipping 187 left a pale, overexposed scene; skipping the 1×1 chain changed nothing. Ruled out: exp bias in
  resolves (0), texture `exp_adjust` (the fork's word-3 reading matches current upstream), fork changes to the render
  target cache.
- Cause (hint from the user: Xenia/RPCS3 had the same white screen on NieR; RPCS3 fixes such games with "Write Color
  Buffers"): the game computes exposure on the CPU from the downsampled frame, and the SDK default
  `readback_resolve = "none"` never copies resolve results back to guest memory, so the CPU read zeros and maxed out
  the exposure. `readback_resolve = "fast"` (previous frame, copied every frame) → normal image (mean brightness
  218 → 94, white pixels 80 % → 0 %). Cost on the Switch to be measured in phase 2 (`some` = copy on cache miss).

## 2026-11-17 — Switch: first runs (phase 2)

- Build: devkitA64 in Docker (`tools/switch/docker.sh`), NVK from mesa-switch with the masseffect-nx patch (`mesa/`),
  `tools/build_nro.sh` → `out/nx/nier-nx.nro`. FFmpeg's `config.h` had no Horizon branch (see `sdk/UPSTREAM.md`).
- Launch: hbmenu in title-takeover mode (hold R while starting a game). sys-botbase 2.5 crashed the `hid` service
  (fatal 2001-0132, three identical reports, one of them on the user's own launch) whenever a game started with R
  held → disabled. The netloader never accepted nxlink, so the NRO goes to `sdmc:/switch/nier-nx/` over FTP
  (`tools/console-test/deploy.sh`), the user starts and closes the game, and `tools/console-test/collect.sh` fetches
  the log, the profile, the matching ELF and new Atmosphère reports. A file the game is writing cannot be read over
  FTP until it exits.
- First run: logos, title, menus and the prologue on the console, no crash.
- Name entry: the SDK's ImGui keyboard dialog cannot be used on the console → `XamShowKeyboardUI` opens the Horizon
  software keyboard (`sdk/UPSTREAM.md`). Confirmed on the console.

### Switch baseline

Handheld, `readback_resolve = "fast"`, manual play, 10 s blocks of `logs/rex/rex_profile.log`. The profiler read CPU
1020 MHz, GPU 460.8 MHz, memory 1600 MHz: the official configuration 0x92220007, +50 % GPU over the 307.2 MHz handheld
default. The NRO does not request it (`RexSwitchApmApply` is never called), so it was set outside the game; check the
console's clock tools before the next baseline. `collect.sh` now records the clocks in `summary.json`.

| Where | fps | frame | per frame |
|---|---|---|---|
| Title and menus (11 blocks) | 6.0–6.3 | ~160 ms | 5–24 draws, 5 resolves, 11 transfer draws |
| Prologue gameplay (3 blocks) | 1.4–1.5 | ~680 ms | 130–170 draws, 28 resolves (14–15 screens), 130–170 EDRAM transfers (250 transfer draws, 45 render targets) |

Every frame is over 40 ms. `readback_resolve = "none"` gives the same 151–166 ms in the menu.

### Where the time goes (profile with stack sampling)

- The game's render thread (guest entry 0x8258D948) uses 100 % of a core, but it only spins: `sub_82573758` waits
  until the GPU read pointer (its writeback address is at `[device+10896]`) passes a ring position and polls
  `sub_82579D50` (`db16cyc` spin hints, 5000 ms watchdog through `sub_8257D028`). This is Direct3D waiting for ring
  space, not game work.
- The GPU command thread (priority 0x2C) is blocked in the kernel 87–95 % of the time. Menu: 78 % in
  `VulkanCommandProcessor::BeginSubmission` → `vkWaitForFences` (at most 3 frames in flight). Gameplay: 69 % in
  `nvk_queue_submit` (the Horizon channel throttles in-flight submissions), 20 % in the readback fence. libnx counts
  850–930 ms of fence waits per second.
- So the frame is bound by the console GPU. In the menu ~160 ms of GPU per frame for a handful of draws is far more
  than that work should take, so most of the GPU time goes somewhere the profile does not show yet. Next: GPU
  timestamps per category (draws, EDRAM transfers, resolves, readback, present).

## 2026-11-17 — Switch: GPU A/B lap in the menu (run `ab1`)

The SDK's A/B lap (L+R+ZL, `sdk/src/ui/switch_perf.cpp`) drops one part of the GPU work per 10 s block;
`tools/console-test/ab_table.py` turns the profile into a table. Main menu, GPU-bound, normal frame 158.7 ms:

| Mode | fps | frame | saves |
|---|---|---|---|
| no draws | 6.5 | 153.8 ms | 4.9 ms |
| no transfers | 14.0 | 71.4 ms | 87.3 ms |
| no resolves | 9.9 | 101.0 ms | 57.7 ms |
| no texture loads | 6.6 | 151.5 ms | 7.2 ms |
| no presentation effect | 6.3 | 158.7 ms | 0 |
| no tile dispatch | 6.4 | 156.2 ms | 2.5 ms |
| nothing | 56.0 | 17.9 ms | 140.9 ms |

- The menu's own draws cost ~5 ms. EDRAM transfers (3 render targets, 11 transfer draws per frame) cost ~87 ms and
  5 resolves ~58 ms: about 8 ms per transfer draw and 11 ms per resolve, far above a full-screen pass on this GPU.
- Suspect for the transfers: NVK has no `VK_EXT_shader_stencil_export`, so every transfer into a depth target adds 8
  stencil bit draws (`need_stencil_bit_draws` in `render_target_cache.cpp`); masseffect-nx measured the same pattern
  at 133 ms per frame. 1 depth + 8 stencil bits + 2 colour = 11 draws would match.
- The gameplay lap did not start: at 1.4 fps the combo has to be held until the game polls input.
- Next: the lap now also splits transfers (stencil bits, host depth store) and resolves (dumps, clears, copies).
