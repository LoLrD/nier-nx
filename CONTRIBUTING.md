# Contributing

Bug reports from the console are the most useful thing right now, especially from parts of the game listed as
not tested in the README.

## Reporting a problem

Close the game right after the problem happens and, before starting it again, copy
`sdmc:/switch/nier-nx/nier.log` and `rex_profile.log` (the next start overwrites them). In the issue, say where
in the game it happened, handheld or docked, and whether any overclock was on. Do not attach game files or saves
that contain game data.

## Changing the code

- Never commit game data, generated code (`app/generated`) or anything built from them;
  `python3 tools/check_repo_clean.py` checks this and runs in CI.
- `python3 -m pytest tests -q` should pass. Most tests need the macOS build (`tools/build_mac.sh`).
- A change that claims to make something faster needs numbers from a console run at stock clocks;
  `docs/performance.md` shows the form. Overclocking is not a fix.
- Changes to the files under `sdk/` should stay small and be listed in `sdk/UPSTREAM.md`.

## Translations

Corrections to `lang/ru` are welcome as ordinary pull requests: one line per record, tags and `\n` kept as they
are. `python3 tools/lang/build.py ru` checks them.
