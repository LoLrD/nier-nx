# Language packs

`lang/<code>` holds a translation of the game's text as tables: one file per game text file, one line per record,
`record:column<TAB>text`. Only the translated text is stored; the original text stays in your own copy of the game.

## Russian (`lang/ru`)

An unofficial fan translation of the 2010 Xbox 360 version, in which the hero is the father. It covers the
interface, dialogue, subtitles of the cutscenes and the title menu; `glossary.md` is the list of names and terms
it keeps to. The first draft is a machine translation checked against the Japanese text, with the key scenes
proofread by hand, so expect rough lines here and there. Corrections are welcome.

## Building and using a pack

```bash
python3 tools/lang/build.py ru                        # writes out/lang/ru from your unpacked game
tools/console-test/deploy.sh --language ru            # copies it to the console and turns it on
```

The Cyrillic glyphs, the title menu and a few other pictures with text are drawn with fonts that ship with
macOS. Their drawings are stored in `lang/ru/art/typeface.json`, so the pack builds the same on any system. If
you change a text that is drawn (`titles.tsv`, the four reports in the prologue), rebuild on macOS to redraw it;
elsewhere the build stops and says which text has no drawing.

By hand: copy `out/lang/ru` to `sdmc:/switch/nier-nx/lang/ru` and put
`nier_language_dir = "sdmc:/switch/nier-nx/lang/ru"` at the top of `nier.toml`. The pack takes the place of the
English text; leave the setting empty to get the original back.

How the replacement works is described in [docs/design/russian-translation.md](../docs/design/russian-translation.md).
