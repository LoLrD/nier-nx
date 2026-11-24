# Third-party notices

| Project | Used for | License |
|---|---|---|
| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty, built on [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) | `tools/extract_iso.py`, `tools/build_host.sh`, `tools/fetch_thirdparty.py`, and the structure of the build and analysis scripts | GPL-3.0 |
| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) native renderer | design of `app/src/native/` (ring and vblank threads, MMIO handling, the replacement of the SDK's Xenos plugin through `config.graphics`); PM4 and register semantics follow the ReXGlue SDK's `CommandProcessor` | GPL-3.0 |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev, with the masseffect-nx changes | `tools/shaders/XenosRecomp/` | MIT (`tools/shaders/XenosRecomp/LICENSE.md`); masseffect-nx changes GPL-3.0 |
| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) shader tools | `tools/shaders/translate/` (HLSL front end, container check), `tools/shaders/pack/` (packer), `tools/shaders/compile_spirv_one.sh`, `app/src/native/shader_library.{h,cpp}` (library format) | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 by Tom Clay, with portions from [Xenia](https://xenia.jp) | `sdk/` | BSD-3-Clause (`sdk/LICENSE`) |
| Horizon (Switch) layer of the SDK by [StevensND](https://github.com/StevensND) | `sdk/src/**/*_switch.*`, `sdk/src/ui/switch_*.cpp`, `sdk/src/input/switch/`, `sdk/src/audio/switch/`, `sdk/cmake/rexglue_switch.cmake`, `sdk/switch_compat/` | BSD-3-Clause |
| Scripts by [StevensND](https://github.com/StevensND) | `tools/fetch_thirdparty.py` | GPL-3.0 |
| [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) by AMD, through the SDK's precompiled output shaders | `app/src/native/shaders/fsr_easu_alpha.frag`, `fsr_rcas_alpha.frag` (decompiled from `sdk/src/ui/shaders/vulkan_spirv/guest_output_ffx_fsr_*`, alpha kept) | MIT |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) (Mesa NVK/NAK for Horizon) with the masseffect-nx patch | `mesa/` build scripts and `mesa/mesa-switch-masseffect.patch`; the driver is built outside the repository and linked into the NRO | MIT (Mesa licenses per file) |

Everything outside `sdk/` is GPL-3.0 (`LICENSE`).
