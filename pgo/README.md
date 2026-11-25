# PGO profile

Counters recorded on the console (ten minutes: the village, the prologue with the sealed-verse abilities, a
cutscene) for `CMAKE_EXTRA="-DNIER_PGO=use" tools/build_nro.sh`.

A profile only fits sources that the compiler sees at the path it was recorded at: the path is part of the
profile's file names and of the numbers of functions with internal linkage. `build_root.txt` holds that path and
`tools/build_nro.sh` mounts the repository there inside the Docker container, so the profile fits wherever the
repository is checked out. `trained_at.txt` is the commit it was recorded on.

`tools/pgo/collect.sh` replaces the profile with a new one from a training run; change `build_root.txt` first if
you want another path.
