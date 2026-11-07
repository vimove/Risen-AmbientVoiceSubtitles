# AmbientVoiceSubtitles

**English** · [简体中文](须知.md)

Floating subtitles above NPCs for their ambient voice lines ("barks") in
*Risen* (2009).

Risen plays a short voice line whenever an NPC spots you, tells you to put your
weapon away, complains about the weather, and so on. There is audio but no text.
This ASI plugin draws that text above the speaker's head for a few seconds.

The plugin ships **no text of its own**. It reads the subtitle strings straight
out of the game's own `data\compiled\strings.pak` (the `svms` table) and follows
the language selected in the game options. Players who run a translation patch
see the translated text; players on English see English.

## Documentation

This README is available in two languages:

- **English** — this page
- **简体中文** — [须知.md](须知.md)

Other documents — each one exists in both languages, the link goes to the
version in this README's language:

- Maintainer notes (hooks, data flow, gotchas):
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — English
  · [中文版](docs/技术笔记.md)
- End-user install guide: [Installation.txt](Installation.txt) — English
  · [中文版](安装说明.txt)
- Development log: [docs/Development-Log.md](docs/Development-Log.md) — English
  · [中文版](docs/开发日志.md)

## Features

- Subtitles above the NPC who is talking, not a fixed corner HUD element.
- Works with English, German, French, Italian, Spanish, Polish and Russian game
  text; `Language=auto` follows whatever the game is set to.
- Works with **translation patches** that replace one of those columns (most
  Risen translation patches replace the English column — set the game to English).
- Every option is live: edit the `.ini`, save, and it applies within ~2 seconds.
  No restart, no reloading a save.
- Optional distance fading, configurable height, duration, fade-out time,
  colours and how many messages may be on screen at once.

## Requirements

- *Risen* (2009), Steam version. Only tested against the current default Steam
  build; the 2009 original and the public beta branch (both selectable under
  *Steam → Betas*) were not tested.
- An ASI loader in the game's `bin` folder (a `d3d9.dll` based on
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)).
  If you already run other ASI mods, you have it.

## Installation

1. Copy the `bin` folder from the release zip into the game's `bin` folder,
   e.g. `...\Steam\steamapps\common\Risen\bin`.
2. Edit `AmbientVoiceSubtitles.ini` if you want to change anything. Changes
   apply within about 2 seconds **after you save the file** — no restart, no
   reloading a save.

Result:

```
bin\d3d9.dll                      (ASI loader, yours)
bin\AmbientVoiceSubtitles.asi     (this plugin)
bin\AmbientVoiceSubtitles.ini     (config)
```

If the `.ini` is missing, the plugin writes a default one next to the `.asi`.
The default text is generated from [`config/AmbientVoiceSubtitles.ini`](config/AmbientVoiceSubtitles.ini)
at build time, so it always matches the documented options.

## Configuration

`AmbientVoiceSubtitles.ini` (every key is documented inline, in both English
and Chinese):

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | master switch |
| `Language` | `auto` | `auto` follows the game's voice language; or force `English` / `German` / `French` / `Italian` / `Spanish` / `Polish` / `Russian`; or a raw column name such as `French_Text` |
| `HeightCm` | `230` | height above the NPC's head, in centimetres |
| `DurationMs` | `3000` | how long one line stays on screen, in milliseconds (3000 = 3 s) |
| `FadeMs` | `800` | fade-out time: how long the text takes to turn transparent before it disappears, in milliseconds (`0` = no fade, it just vanishes) |
| `DistanceFade` | `1` | fade with distance |
| `FadeStartM` | `10` | distance where fading starts (metres) |
| `FadeEndM` | `15` | distance where the text is fully gone |
| `MaxMessages` | `8` | how many lines may be on screen at once |
| `Color` | `FFFFFFFF` | `AARRGGBB`; 6-digit `RRGGBB` also accepted (`FF` alpha is added) |

## How it works (short version)

- Hooks `Entity::StartSaySVM` (via the `Script_Game.dll` import table) to learn
  which entity just started talking.
- Hooks `gCSvmAdmin::FormatSampleTitle` (via the `Script.dll` import table) to
  get the `SVM_<voice>_<svm>` key of the line that is about to play.
- Looks that key up in `strings.pak` → `svms` table → the column matching the
  game language.
- Positions the text with `gCGUIEntityRenderer::AddScreenMessage` using the
  speaker's geometry entity, and redraws queued messages from a hook in the
  renderer's `OnBeginFrame` vtable slot.

Details, including the resolved mangled names and the gotchas that cost the most
time, are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Building

Requirements: Windows, and either [zig](https://ziglang.org/download/) (no
Visual Studio needed) or Visual Studio's C++ toolchain.

```bat
python tools\gen_ini_template.py     :: only needed after editing config\*.ini
tools\build.bat                   :: uses zig if available, else MSVC cl
```

Output: `build\AmbientVoiceSubtitles.asi`. Copy it to the game's `bin` folder.

`src\ini_template.h` is generated — do not edit it by hand. It embeds
`config\AmbientVoiceSubtitles.ini` byte for byte; the plugin writes it out when
the user has no `.ini` yet.

## Project layout

```
src/AmbientVoiceSubtitles.c       the plugin (single translation unit, plain C)
src/ini_template.h                generated: default .ini as a C string
config/AmbientVoiceSubtitles.ini  the shipped default config
tools/gen_ini_template.py         .ini -> src/ini_template.h
tools/build.bat                   build helper (zig or MSVC)
docs/ARCHITECTURE.md              hooks, data flow, gotchas (maintainers, English)
docs/技术笔记.md                  the same document in Chinese
docs/Development-Log.md           development log (English)
docs/开发日志.md                  development log (Chinese)
Installation.txt                  end-user install guide (English)
安装说明.txt                      end-user install guide (Chinese)
```

## Troubleshooting

The plugin writes `bin\AmbientVoiceSubtitles.log`. Useful lines:

- `=== AmbientVoiceSubtitles v20 loaded pid=... ===` — the plugin loaded. `v20`
  is the development iteration number, not the mod's version (the release is
  version 1.0); a log that says `v20` is current.
- `PAK loaded: N keys, column=English_Text` — text was read; check the column
  matches the language you expect.
- `PAK not found` / very small `N` — `strings.pak` could not be read.
- no log file at all — the plugin was never loaded; check the ASI loader.

When reporting a bug, please attach the log (or the relevant lines), the game
version, and whether any translation patch is installed.

## Known limitations

- At most 8 lines on screen at once.
- The speaker is matched by "started talking within the last 1.5 s and is the
  nearest such entity". Two NPCs in the same spot saying the same line can make
  the subtitle appear over the other one.
- A few lines have no matching row in the game data and are skipped silently.

## Contributing

Bug reports and pull requests are welcome. The project is deliberately kept
small and dependency-free: one C file, no external libraries (the zlib inflate
needed for `strings.pak` is implemented in the source). If you want to take the
project over, everything needed is in this repository — build instructions
above, hook inventory in `docs/ARCHITECTURE.md`.

## License

MIT — see [LICENSE](LICENSE).

Not affiliated with Piranha Bytes or THQ Nordic. *Risen* and its assets belong
to their respective owners; this repository contains no game data.
