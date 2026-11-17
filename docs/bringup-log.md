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
