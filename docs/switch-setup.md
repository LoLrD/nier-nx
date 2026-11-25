# Preparing the console for test runs

The test scripts in `tools/console-test` copy builds to the console and bring logs back over FTP, so the game
only has to be started and closed by hand.

## Once

1. Connect the console to the same network as the build machine and note its IP address
   (Settings → Internet → Connection Status). A fixed address in the router's DHCP settings saves trouble.
2. Install an FTP sysmodule such as [sys-ftpd](https://github.com/cathery/sys-ftpd) (port 5000) and reboot.
3. Copy `tools/console-test/credentials.env.example` to `credentials.env` next to it and put the address in.
   That file is ignored by git.
4. Put the game on the SD card, about 7 GB:

```
sdmc:/switch/nier-nx/
  nier-nx.nro
  nier.toml
  game_root/                  the unpacked disc (default.xex, media/, ...)
  marketplace/0000000000000000/535107E8/00000002/...   DLC, optional
  nier.log                    log of the last start
```

## A test run

1. `tools/console-test/deploy.sh` copies the NRO and its settings. `--route app/input/<file>.tsv` adds scripted
   input, `--stacks` turns stack sampling on in the profile, `--language ru` adds a language pack.
2. Start the game from the homebrew menu in application mode (hold R while starting any game).
3. Close it. The log cannot be read over FTP while the game is open.
4. `tools/console-test/collect.sh NAME` brings the log, the profile and any new crash reports to
   `out/console-test/NAME`; `tools/analysis/periods.py out/console-test/NAME` prints one line per ten seconds.

Keep the console on the charger with auto-sleep off, and overclocking off: the numbers in
`docs/performance.md` are for stock clocks.

sys-botbase is not used: on some custom firmware builds it crashes the `hid` service when a game is started in
application mode.
