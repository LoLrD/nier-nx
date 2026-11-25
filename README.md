# nier-nx

An unofficial native port of **NieR** (Xbox 360, 2010) to the Nintendo Switch, made by static recompilation: the
game's PowerPC code is translated to C++ ahead of time and built into a homebrew application, and the game's
graphics commands are drawn by a renderer written for the Switch GPU. A macOS build exists as the development
target.

No game data and no game code are in this repository. You need your own disc dump, and you build the application
from it yourself.

## Status

- The whole game starts, plays, saves and loads; the "15 Nightmares" DLC is supported.
- **30 frames per second at stock clocks** in handheld mode (CPU 1020 MHz, GPU 460.8 MHz, memory 1600 MHz, three
  cores): villages, fights and the sealed-verse abilities with over a thousand draws per frame. No overclock is
  needed and none is assumed anywhere in the code.
- The scene is rendered at a reduced resolution and upscaled to 720p with FSR 1.
- Button icons are redrawn for the Switch layout.
- A Russian language pack is included (see [lang/README.md](lang/README.md)).
- Tested on a Switch OLED only.

### What has been tested

Measured on the console, handheld mode, stock clocks, with the logs kept in `docs/performance.md`:

| Tested | Result |
|---|---|
| The story from the prologue to the Aerie: the village, the Northern Plains, the Lost Shrine with its boss fight, the Aerie and the fight against Hook alongside Kainé | Played through with no softlock and no crash, clean audio in cutscenes. Measured over one two-hour session without a restart: 30 fps in 739 of 740 ten-second periods, 100 slow frames out of 232,641, memory use levels off after about 20 minutes |
| Menus, saving and loading, the Russian language pack | Work |

That is roughly the first fifth of one playthrough (about 1 h 45 min of a 9.5-hour run of the first route).

Not tested yet: everything after the Aerie, starting with Seafront, the Junk Heap and the Forest of Myth (later
areas and bosses may be heavier than anything above), the second half of the game, the later routes and endings,
docked mode, and the DLC in play. Reports are welcome.

Known issues: a short stutter the first time new shaders or pipelines are met (they are cached on the SD card
after that) and short hitches while textures stream in. See [docs/performance.md](docs/performance.md) for the
measurements behind all of this.

## How it works

| Part | Where | What it does |
|---|---|---|
| Recompiled game | `app/generated` (not in git) | C++ made from `default.xex` by the ReXGlue SDK |
| Application | `app/src` | Start-up, settings, hooks into the game, language and icon replacement |
| Native renderer | `app/src/native` | Reads the game's Xbox 360 GPU command stream and draws it with Vulkan |
| SDK | `sdk` | ReXGlue SDK with its Horizon layer: kernel, threads, audio, input, file systems |
| Graphics driver | `mesa` | Scripts and a patch to build Mesa NVK for the Switch, linked into the application |

The design documents are in [docs/](docs/README.md).

## What you need

- The disc dump `Nier (USA, Europe) (En,Ja,Fr,De)` and, optionally, the DLC package. `app/game.lock` lists the
  hashes the port is built against; other editions are refused.
- A Switch that can run homebrew.
- A build machine with Docker, CMake, Ninja, clang, Python 3 and 7-Zip. The scripts have been used on macOS on
  Apple silicon only.

## Build

```bash
tools/game/unpack_dumps.sh <game.7z> <dlc.zip>   # verifies and unpacks your dump into ~/work/nier-nx-data (NIER_DATA)
tools/build_host.sh                              # builds the recompiler
tools/codegen.sh                                 # translates default.xex into app/generated
tools/build_mac.sh                               # optional: macOS build, run it with tools/mac/play_mac.sh
mesa/build_mesa_docker.sh                        # builds the graphics driver, once
tools/build_nro.sh                               # builds out/nx/nier-nx.nro
```

Add `CMAKE_EXTRA="-DNIER_PGO=use"` in front of the last command to build with the profile in [pgo/](pgo/README.md),
recorded on the console; it takes roughly 15 to 20 % off the render thread. The Docker image is pinned to the
devkitA64 release the port was measured with.

## Install

Copy to the SD card:

```
sdmc:/switch/nier-nx/
  nier-nx.nro      out/nx/nier-nx.nro
  nier.toml        out/nx/nier.toml
  game_root/       the unpacked disc (default.xex, media/, ...)
  marketplace/     optional DLC
```

Start it from the homebrew menu in application mode (hold R while starting any game), not from the album.
`tools/console-test/deploy.sh` does the copying over FTP; see [docs/switch-setup.md](docs/switch-setup.md).

### One installable NSP

The same build can be installed as an application, with the game data inside it, so it starts from the HOME menu:

```bash
python3 -m pip install cryptography pillow
python3 tools/build_nsp.py --keys /path/to/prod.keys --language out/lang/ru
```

The result is `out/nsp/nier-nx.nsp`, about 6.5 GB: the program, `game_root`, the DLC from `assets/marketplace` and,
with `--language`, a built language pack. The icon is cut from the dashboard art on your own disc; `--icon` takes
another picture. `prod.keys` is the key dump of your own console (Lockpick_RCM); only `header_key` and
`key_area_key_application_00` are read from it, and nothing is stored. Install the NSP with DBI or another installer
on a console with signature patches.

Saves, caches and logs stay in `sdmc:/switch/nier-nx/`, the same folder the NRO uses, so both share one save. A
`nier.toml` in that folder replaces the packaged settings. Neither the NSP nor the keys belong in the repository.

## Settings

`nier.toml` next to the NRO. The ones worth knowing:

| Setting | Meaning |
|---|---|
| `nier_resolution_level` | Scene resolution: 0 full, 1 and 2 reduced (`nier_level1_scale`, `nier_level2_scale`) |
| `nier_dynamic_resolution` | Lower the resolution level by itself when the GPU falls behind |
| `nier_language_dir` | Folder of a built language pack; empty keeps the original text |
| `nier_npc_update_scale` | Distance scale for updating far-away characters; below 1.0 saves CPU time in villages |

## Tests

```bash
python3 -m pytest tests -q
```

## Credits

This port stands on other people's work; the full list with licenses is in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from Xenia
- The Horizon layer of the SDK by [StevensND](https://github.com/StevensND)
- [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty: the starting point for the
  build scripts and the design of the native renderer
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev
- [mesa-switch](https://github.com/danfromtico/mesa-switch): Mesa NVK for Horizon
- FidelityFX Super Resolution 1 by AMD

## How it was made

Most of the code, the tooling and the documentation were written with an AI coding assistant (Claude) under the
author's direction: the author set the goals and constraints, ran every build on a real console and decided from
the measurements what to keep. The Russian language pack is a machine translation checked against the Japanese
text, with the key scenes proofread by hand. The performance numbers come from logs of real console runs.

## Legal

No affiliation with Square Enix, Cavia, Microsoft or Nintendo. "NieR" is a trademark of Square Enix. This
repository contains no assets or program code from the original game; you must provide your own legally obtained
copy. Running homebrew requires custom firmware, which is against Nintendo's terms of service and can get a
console banned.

Source code outside `sdk/` is under GPL-3.0 ([LICENSE](LICENSE)); the SDK is under BSD-3-Clause (`sdk/LICENSE`).
