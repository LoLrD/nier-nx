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

## 2026-11-17 — Switch: the menu's EDRAM work (run `ab2`, macOS frame descriptions)

Finer A/B lap in the menu (normal frame 158.7 ms): stencil bit transfer draws 65.3 ms, resolve dumps 51.2 ms, all
transfers 86.8 ms, all resolves 53.5 ms, resolve copies 7.2 ms, host depth store 2.5 ms, resolve clears 0.

The frame, from `gpu_debug_describe_frame` on macOS (same render target cache, `[transfer]` / `[dump]` lines):

1. The menu UI is drawn into a 1280x720 1x colour target at tile 720; the next frame draws a 640x360 4xMSAA scene into
   the same tiles → transfer 1x → 4x colour.
2. Four resolves from the 4x targets, each preceded by a dump of the whole target; the colour target is dumped three
   times with no draw in between.
3. Two stencil-only rectangle draws on the 4x depth target at tile 0. Without a viewport their height was taken from
   the 1280x720 scissor, so the depth target was assumed to reach tile 1440 → transfer colour → depth over
   [720, 1440) (+ 8 stencil bit draws on NVK), then depth → 1x colour before the UI.

Fixes:
- `execute_unclipped_draw_vs_on_cpu = true` and `..._with_scissor = true` (upstream Xenia has the first one on by
  default for exactly this): the rectangles' vertex shader runs on the CPU, the depth target stays within [0, 720), and
  the colour → depth → colour round trip is gone (4x → 1x colour remains). Menu capture identical on macOS (mean
  difference 0.003).
- Redundant dumps skipped (`sdk/UPSTREAM.md`): 3 dumps per menu frame instead of 5. Menu capture against the original
  build: 0.1 % of pixels differ (the animated emblem).
- Console (run `menu1`): menu 6.3 → 15.0 fps (158.7 → 66.7 ms); transfers 2 draws per frame instead of 11, stencil
  bit draws 1.8 ms (was 65.3). Remaining: resolve dumps 31.4 ms (3 dumps), transfers 16.9 ms (2 draws), resolve
  copies 7.2 ms; with all GPU work skipped the frame is 20.7 ms.
- Next suspect: 8-10 ms per dump or transfer pass of 921,600 samples. NAK emits the microcoded legacy `IMUL` on
  SM50 for every 32-bit multiply, including `x / 80` (umul_high) and `x / pitch`; the generated shaders had 8-10 of
  them per sample. Rewritten without integer multiplies (`sdk/UPSTREAM.md`); menu and prologue captures on macOS look
  the same.
- Console (run `menu2`, IMUL-free dump and transfer shaders): menu 15.0 → 41.2 fps (66.7 → 24.3 ms). Resolve dumps
  31.4 → 3.1 ms, transfers 16.9 → 1.3 ms: each pass about 10x faster, so the microcoded `IMUL` was the cost. What is
  left: resolve copies 5.5 ms and texture loads 3.2 ms (both are prebuilt compute shaders with integer addressing),
  draws 2.4 ms. Menu summary: 6.3 → 41.2 fps at the same clocks.

## 2026-11-17 — Switch: first gameplay lap (run `game1`)

Prologue gameplay, same build as `menu2`: 5.1–7.0 fps (~170 ms). Per frame 150–173 draws, 31 resolves (17.4 screens),
150–173 transfer calls with 126 transfer draws (22 render targets; 250 draws before the CPU extent estimation), 28
texture loads. The lap is noisy in gameplay (the scene changes), but: no draws 105 ms (draws ~64 ms), no transfers
~150 ms, nothing 54.6 ms (18.3 fps). In "nothing" the GPU command thread is at 84 % of a core: about 46 ms of CPU per
frame for ~170 draws, so the command processor itself is the second limit. Next run: stack samples and a vertices-only
mode (zero scissor for guest draws) to split draws into vertex and pixel work.

## 2026-11-17 — Switch: vertices vs pixels, and the CPU floor (run `game2`)

- Lap with the vertices-only mode (zero scissor for guest draws), gameplay noisy (normal 131–185 ms): no draws
  105 ms, no draw pixels 118 ms → guest vertex work ~12 ms, pixel work ~35–45 ms; nothing 54.6 ms.
- Stack samples of the GPU command thread in the "nothing" blocks: 70 % `memcpy` called from
  `IssueCopy_ReadbackResolvePath`, 12 % `nvk_InvalidateMappedMemoryRanges` on the same path. `readback_resolve =
  "fast"` copied every resolve back to guest memory: per gameplay frame 28 readbacks, about 56 MB (13 × 3.77 MB
  full-screen, 4 MB, 2 × 1 MB, then 4–256 KB for the exposure chain; sizes from `[readback]` on macOS).
- Fix: `readback_resolve_max_kb = 256`. macOS prologue captures keep the exposure (mean brightness 75.5 vs 77.0, no
  white pixels).
- Console (run `game3`, `readback_resolve_max_kb = 256`): "nothing" 54.6 → 35.2 ms (the CPU floor dropped), but
  normal gameplay stays at 145–188 ms: GPU-bound. Lap: no draws saves ~68 ms (no draw pixels ~55 ms), no resolves
  ~29 ms, no texture loads ~27 ms, no transfers ~21 ms (noisy).

## 2026-11-17 — internal resolution experiment

- `sub_82496830` (graphics init) fills a config block before calling `sub_82597120`: +80/+84 = 1280/720, +128/+132 =
  640/360, +136/+140 = 1280/720 (+120 = 720), plus pool sizes. The 3D scene itself is drawn at 640x360 with 4xMSAA and
  read back as 1280x720 1x (the game's own trick), the post chain runs at 1280x720.
- Experiment (generated code edited by hand, not committed): 960/540 and 480/270 in that block. On macOS the whole
  frame (scene, post, HUD) comes out at 960x540 and the presenter scales it; only the dialogue text is still laid out
  for 1280x720 (shifted right and down; its icons are placed right). Being measured on the console.
- Console (run `game4`, 960x540 config block): gameplay still ~6 fps. The smaller targets moved the EDRAM layout, and
  transfers went from 126 to ~209 draws per frame (57 render targets instead of 22); the image was broken too (dialogue
  text, camera framing). Reverted (the generated code is regenerated, nothing was committed).
- Guest video mode 960x544 (`video_mode_width/height`, the lever that gave Mass Effect +35 %): NieR calls
  `VdQueryVideoMode` but its render targets stay 1280x720, so it does not size them from the video mode.

## 2026-11-18 — native renderer N1: skeleton on macOS

- `nier_renderer = "native"` replaces the SDK's Xenos plugin with `app/src/native/` (`NativeGraphicsSystem`): PM4 ring
  thread, register mirror, vblank thread, interrupts, read-pointer writeback; draws, shader loads and resolves are only
  counted, the screen stays black. PM4 semantics are ported from the SDK's `CommandProcessor` and covered by
  `tests/test_native_skeleton.py`; the EVENT_WRITE_SHD counter counts vblanks as well as swaps, as in the SDK.
- 90 s `menu_idle` runs on the M4: Xenos 5074 frames, native 5382 (`[native] present`), both `timeout_ok`, the same
  8 files opened and 92 probes failed, no unregistered calls, exit code 0.
- Native reports per 10 s: 600 swaps (60 fps), 1800 interrupts, ~13,200 WAIT_REG_MEM; in the menu 14,400 draws and 3000
  resolves = 24 draws and 5 resolves per frame, the same as the Xenos path's frame description.

## 2026-11-18 — native renderer N2: where NieR's shaders come from

- Containers (0x102A11xx) found by `tools/shaders/extract_containers.py`: 5095 unique (default image + both CPKs,
  1175 CRILAYLA blocks), 912 in a guest memory dump taken in the prologue.
- Shaders the game loads (`nier_shader_dump_dir`, native renderer, menu + prologue routes): 98 (54 PS, 44 VS). Only 27
  match a container (PS byte-exact, VS without the fetch fields D3D patches); every one of those 26 containers
  translates to valid SPIR-V. The other 71 are loaded by IM_LOAD from physical memory in the regions of loaded game
  resources (0x12C…, 0x16B…, 0x195…–0x1D5…); their microcode is found raw in uncompressed CPK files, preceded by float
  literals, with no container header near it: NieR keeps them in its own resource format.
- XenosRecomp (masseffect-nx fork) on the 5095 disc containers: 2221 HLSL, 1862 valid SPIR-V; rejections: vertex fetch
  without a declared element (2306), conditional EXEC on an undeclared boolean constant (568), DXC errors (359).
- Raw microcode wrapped as a register-linked container (masseffect-nx `wrap_raw_shader.py`): 1 of 6 samples reaches
  valid SPIR-V (undeclared vertex fetches rejected, depth export not declared).
- Decision (user, 2026-11-18): the native renderer translates shaders at run time with the SDK's `SpirvShaderTranslator`
  (design decision 3 revised). `NativeShaders` keeps `VulkanShader`s by XXH3 of the loaded microcode and translates
  each new (shader, modification) pair at the draw; modifications are ported from `VulkanPipelineCache` (host render
  targets, no float24 in the pixel shader). The app links `rexgpu-xenos` directly (exported on macOS).
- Acceptance on the M4: menu route 10 translations, prologue route 98 unique shaders loaded and 100 translations, 0
  failures, 34 ms in total, at most 1 ms for one shader (NAK on the console will be slower: measure in N2.5/N4).

## 2026-11-18 — native renderer: what NieR's frames use (input for N3a)

Temporary counters in the native path (reverted), route title → menu → prologue on the M4, cumulative over 300 s:
- Prologue frame: ~80–110 draws and ~14 resolves. Draws: indexed triangle strips with 16-bit indices (752k), auto-indexed
  quad lists (402k: sprites, UI, particles), auto-indexed rectangle lists (74k: full-screen passes), auto-indexed
  triangle lists and strips (20k), point lists (24). No 32-bit indices, triangle fans, lines or memexport.
- Render targets: colour only `k_8_8_8_8`, depth only `kD24S8`. MSAA 1x or 4x; the 4x surfaces (640 pitch at tile 720,
  320 at 184, 520 at 0) are read back as 1x surfaces of twice the size at the same base. Depth-only passes (mode 5)
  render the 1024×1024 shadow map (pitch 520 4x).
- Resolves: colour to `k_8_8_8_8` (1280×720 ×~10 per frame, then 512, 256, 128, 64, 32×32 … 32×2: the exposure chain),
  one `k_8` 640×360, depth (raw copy) 1280×720 and 1024×1024. No resolve has MSAA at copy time.
- Textures: `k_24_8` 1024×1024 and 1280×720 (sampled depth from the depth resolves, the most used), DXT1/DXT2_3/DXT4_5
  up to 2048×2048 (some with gamma signs), `k_8_8_8_8`, `k_CTX1`, a signed `k_16_16_16_16_EXPAND` 64×64 cube map, and a
  linear `k_32_32_32_32_FLOAT` 1024×1 vertex texture in most vertex shaders (bones); `k_8` linear 1280×720 / 640×360 on
  the title.

## 2026-11-18 — native renderer N3a: a rough picture on macOS

- `NativeRenderer` (`app/src/native/`): a 512 MB host-visible mirror of guest physical memory (vertex and index ranges
  copied at record time, changes found by XXH3, waits only when a changed range is still read by a pending submission),
  render targets keyed by EDRAM base/pitch/format with MSAA samples stored as pixels, runtime-translated shaders with
  the SDK's binding layout, CPU-untiled textures with versions, resolves into images (colour by blit, depth by a compute
  shader into `R32_SFLOAT`, readbacks of resolves up to 256 KB one submission late), dynamic rendering, up to 3
  submissions in flight; VdSwap blits the frontbuffer's resolve image into the presenter's guest output.
- `nier_frame_dump_dir` / `nier_frame_dump_interval` write presented frames as PPM from inside the renderer: window
  captures are black whenever the Mac's screen is locked or asleep (`screencapture` of the whole screen gives zeros), and
  a sleeping display blocks presentation (FIFO) for minutes — runs need `caffeinate -d`.
- Bug found on the way: the PM4 executor (N1) did not read VGT_DMA_BASE / VGT_DMA_SIZE from DRAW_INDX packets, so every
  indexed draw (all scene geometry) had zero indices.
- Route title → prologue on the M4 (MoltenVK, D32S8 depth because the device has no D24S8 attachments): every draw is
  drawn (0 skipped by primitive, target format, texture format or shader), 30 fps in the prologue cinematic, ~8400
  resolves and ~3300 readbacks per 10 s, mirror ~52 MB/s copied, 0 mirror waits, 0 black frames, live textures stable
  (~150). Logos, title, menus, dialogs, credits and the 3D scenes of the opening are recognizable.
- Not right yet (input for N3b): many frames of the prologue cinematic are washed out white or show blocky 8×8
  patterns, and large flat triangles flash over the scene; no EDRAM ownership transfers between aliased targets, no
  gamma ramp, depth resolves take sample 0.
- `tools/compare_paths.py` runs the Xenos and native paths at once on the same input route and captures frames through
  the control file's `capture` command at the same seconds (sheet `compare.png`). The prologue cinematic on the native
  path flashed white with large flat triangles and 8×8 garbage while Xenos was right; serializing every draw with the
  GPU did not help, copying every vertex range on every draw did. Cause: when a mirror range was copied, other keys
  overlapping it kept their hashes, so a range whose guest bytes went back to an earlier content was never copied
  again while the mirror held the other key's bytes. Overlapping keys are now marked stale on every copy; the cinematic
  matches Xenos frame for frame (16 captures between 102 and 147 s).

## 2026-11-18 — native renderer N3b: the prologue matches Xenos on macOS

- Review fixes of N3a (opus reviewer): the presenter's guest output images get TRANSFER_DST (the validation layer
  reported `VUID-vkCmdBlitImage-dstImage-00224` every frame), a draw re-syncs its ranges after a mid-draw flush,
  resolve images go stale when guest memory at their address changes, unused textures / resolve images / mirror keys
  are evicted after 600 frames, pixel shaders are translated per sample scale (VPOS on 4x targets), readbacks are
  limited to rectangles that fit the 256 KB buffer.
- Depth of field read the depth resolve as `k_8_8_8_8` (raw D24S8 bytes); depth resolves now publish an `R32_SFLOAT`
  image for `k_24_8` fetches and an RGBA8 image with the packed bits (R = stencil, G..A = depth low..high).
- Shadows of the native frame were brighter than Xenos (Xenos ≈ native^1.2): NieR writes a non-linear gamma ramp
  through `DC_LUT_30_COLOR` (one entry per `DC_LUT_RW_INDEX`), which the register mirror now keeps (and
  `DC_LUT_SEQ_COLOR`); VdSwap presents through a compute pass that applies the 256-entry ramp. Mean colours of the
  captures at 160–220 s now match Xenos within ±2.
- `tools/compare_paths.py` over the whole prologue route (300–600 s): the native path plays the whole prologue (snow
  fight, Grimoire Noir, the crowd of Shades, the red Shade) without crashes, hangs or visible artefacts; it runs
  ~80 s ahead of Xenos on the same script because loading is faster. Validation layer: no errors from the renderer.
- Not done: EDRAM ownership transfers between aliased targets, depth resolves take sample 0, no FXAA; areas after the
  prologue are not checked yet.
