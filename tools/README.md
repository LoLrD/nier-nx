# Tools

| Path | What it does |
|---|---|
| `build_host.sh` | Builds the ReXGlue host tool that turns `default.xex` into C++ |
| `codegen.sh` | Checks the game dump and regenerates `app/generated` |
| `build_mac.sh` | Builds the macOS binary (`out/mac/nier`) |
| `build_nro.sh` | Builds the Switch NRO in Docker (`out/nx/nier-nx.nro`); `CMAKE_EXTRA="-DNIER_PGO=use"` applies the profile in `pgo/` |
| `fetch_thirdparty.py` | Downloads the SDK's third-party sources at pinned revisions |
| `check_repo_clean.py` | Fails if game data or generated code is tracked |
| `game/` | Unpacking and verifying your own disc dump, re-encoding the videos |
| `codegen/` | Finding and classifying code the recompiler missed |
| `shaders/` | Translating the game's shaders to SPIR-V and packing the library |
| `lang/` | Building a language pack from `lang/<code>` |
| `switch_prompts/` | Drawing the Switch button icons that replace the Xbox ones |
| `mac/` | Running and smoke-testing the macOS build |
| `console-test/` | Deploying a build to a console over FTP and collecting a run |
| `pgo/` | Collecting a PGO profile from a training run on the console |
| `analysis/` | Reading a collected run: per-period summary, profile symbolization, scripted macOS runs |
| `switch/` | Docker image and CMake toolchain for devkitA64 |
