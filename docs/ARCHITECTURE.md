# Architecture and maintainer notes

**English** · [简体中文](技术笔记.md)

This is the "how and why" document for anyone who wants to fix or extend the
plugin. The whole plugin is one C file (`src/AmbientVoiceSubtitles.c`); there
are no external dependencies and no C++ runtime.

Everything the plugin needs from the game is resolved **by decorated name** at
runtime (`GetProcAddress` / import-table patching), so it does not depend on
fixed addresses or offsets and survives a recompiled game build as long as the
exported symbols stay the same. There is **no** signature scanning and no
hard-coded RVA.

## Load model

1. Ultimate ASI Loader loads `AmbientVoiceSubtitles.asi` as a normal DLL.
2. `DllMain` only spawns `init_thread` — it does no engine work at all
   (see [Gotchas](#gotchas)).
3. `init_thread` reads the config, then polls for `Game.dll`, `SharedBase.dll`,
   `Script.dll`, `Script_Game.dll` and `Engine.dll` (300 ms steps, up to
   ~36 minutes).
4. As each module appears, it resolves the symbols below and installs the two
   import-table hooks. When the essential set is resolved it snapshots the
   module list (to tell engine memory from mod memory in the log) and hooks the
   renderer vtable, then the thread exits.

## Hooks

### 1. `Entity::StartSaySVM` — "someone started talking"

```
?StartSaySVM@Entity@@QEAAXAEAV1@VbCString@@_N2@Z      (Script.dll, imported by Script_Game.dll)
```

Patched in `Script_Game.dll`'s import table. `this` is the speaking entity, the
`bCString` is the svm name. The hook copies both into a 16-slot ring buffer
(`push_recent`, `GetTickCount` timestamped) and calls the original.

### 2. `gCSvmAdmin::FormatSampleTitle` — "which subtitle key is about to play"

```
?FormatSampleTitle@gCSvmAdmin@@QEBA_NAEBVbCString@@0AEAV2@@Z   (Game.dll, imported by Script.dll)
```

Patched in `Script.dll`'s import table. The hook calls the original first, then

- reads the `voice` and `svm` `bCString` arguments (via `ReadProcessMemory`,
  which is simply a safe read that never faults on a bad pointer),
- builds the lookup key `SVM_<voice>_<svm>`,
- looks it up in the loaded `svms` table,
- finds the speaker registered in the last 1500 ms with the same svm
  (`find_recent`),
- queues the text for drawing (`show_text`).

If either the key or the speaker is missing, it logs `NOKEY` / `NOSPEAKER` and
does nothing — a missing subtitle is never fatal.

### 3. `gCGUIEntityRenderer::OnBeginFrame` — the draw tick

```
?OnBeginFrame@gCGUIEntityRenderer@@UEAAXPEAVeCGuiDevice2@@PEAVeCSprite@@@Z
```

`AddScreenMessage` messages are consumed by the renderer every frame, so the
plugin cannot just post one and forget it. Instead it hooks the renderer
instance's `OnBeginFrame` **vtable slot** (the instance comes from
`gCGUIEntityRenderer::GetInstance()`): the hook draws everything currently in the
queue through `gCGUIEntityRenderer::DrawScreenMessage`, then calls the original
`OnBeginFrame`.

The slot is found by scanning the first 128 entries of the vtable for the
`OnBeginFrame` address, then `VirtualProtect` + swap. The hook is installed once.

### Engine APIs used

| Symbol | Used for |
| --- | --- |
| `gCGUIEntityRenderer::AddScreenMessage` | queue a world-anchored message |
| `gCGUIEntityRenderer::DrawScreenMessage` | draw one queued message (called from the frame hook) |
| `gCGUIEntityRenderer::GetInstance` | renderer instance / vtable |
| `bCUnicodeString::bCUnicodeString(wchar_t*)` | build the string argument |
| `eCEntity::GetPropertySetCount` / `GetPropertySetAt` | walk property sets |
| `eCEntityPropertySet::GetGeometryEntity` | Script entity → geometry entity (required by `AddScreenMessage`) |
| `eCGeometryEntity::GetWorldMatrix` | head position |
| `Entity::GetPosition` | distance for fading |
| `eCApplication::GetInstance` / `GetCurrentCamera` / `GetMainCamera` / `AccessPosition` | camera position for fading |
| `eCLocAdmin2::GetInstance` / `GetVoiceLanguage` | `Language=auto` |
| `gCHudModule2::GetSystem` / `gCHudSystem2::ShowSubtitle` | fallback when no geometry entity exists |

Every engine call is wrapped in SEH (`__try` / `__except`). A failure logs
`... EXCEPTION code=<n>` and skips that subtitle; the game keeps running. This is
deliberate: the plugin must never be able to crash the game because a symbol
resolved to something unexpected.

## Text loading (`strings.pak`)

`ensure_text()` is **lazy**: the first time a subtitle is actually requested, it
loads:

1. `pak_path()` — walks up from the `bin` folder (up to 4 levels) looking for
   `data\compiled\strings.pak`. That covers both `bin\` and a nested layout.
2. G3V0 archive: magic `G3V0` at offset +4, directory of `(name, offset, size,
   compressed)` entries. The plugin only needs `svms.tab`.
3. The `svms.tab` entry may be zlib-compressed; the plugin carries its own
   minimal inflate (`inf_zlib`, RFC 1950 wrapper + RFC 1951 deflate, fixed and
   dynamic Huffman). No zlib DLL is required.
4. TAB0 table: a small multi-language table format. Blocks are UTF-16LE strings;
   the block named `svms` holds keys, the language blocks hold one value per
   key, in order. The plugin stores `key -> value` for the selected column.

Column selection (`lang_to_col`):

- explicit `Language=<name>` wins (`English` → `English_Text`, …);
- a value that already looks like a column name (`*_Text`) is used verbatim —
  this is the escape hatch for translation patches that add their own column;
- `auto` asks `eCLocAdmin2::GetVoiceLanguage()` and maps it.

If the language changes while the game is running (e.g. the player switches
language in the options), `init_thread` notices the column differs from the one
that was loaded and reloads the pak.

If the requested column does not exist in the pak, the plugin falls back to
`English_Text` and logs `PAK column <x> missing, fallback English_Text` — a typo
in `Language=` therefore shows English text rather than nothing.

If `strings.pak` cannot be read at all, the plugin falls back to the legacy
JSON text dump (`AmbientVoiceSubtitles.zh.json`, or `svms_zh.json`) if one is
present, and logs that it did so. That path exists only for old installs; the pak
is authoritative.

## Configuration

`AmbientVoiceSubtitles.ini` is read with `GetPrivateProfileIntA` /
`GetPrivateProfileStringA`, so it must stay plain ASCII/ANSI-compatible in the
key/value area (the comments are UTF-8 and are ignored by the parser). If the
file is missing, `write_default_ini` writes `AVS_INI_TEMPLATE` next to the
`.asi`; that array is generated from `config/AmbientVoiceSubtitles.ini` by
`tools/gen_ini_template.py`, which is the only way to change the default text.

`maybe_reload_config` checks the file's write time while running, so edits apply
within a couple of seconds without a restart.

## Fallbacks

- No geometry entity for the speaker → `hud_fallback()` uses
  `gCHudSystem2::ShowSubtitle` (a normal HUD subtitle at the bottom). Better than
  nothing, and it keeps the feature usable if `AddScreenMessage` ever changes.
- No pak → JSON text dump (see above).
- No renderer instance yet → the message is skipped and retried on the next bark.

## Gotchas

These cost real debugging time; please keep them in mind when changing the code.

1. **Do not read `strings.pak` (and do not touch `eCLocAdmin2`) from
   `DllMain`.** An earlier build did that: it grabbed the pak handle before the
   engine's text subsystem was ready, and the main menu fell back to raw IDs
   (`HUD2_Label_PressStart` style). Text loading is lazy for exactly this reason.
2. **`AddScreenMessage` needs a real geometry entity.** Passing `NULL` crashes
   immediately: the first thing it does is call
   `eCGeometryEntity::GetWorldMatrix`. Script entities must be converted via the
   property sets (`GetPropertySetAt(i)->GetGeometryEntity()`).
3. **One `AddScreenMessage` is not enough.** The renderer consumes the queue
   every frame, so the plugin re-posts from the `OnBeginFrame` hook.
4. **`bCString` arguments are not NUL-terminated C strings you can trust** —
   always read them through `read_bcstr` (`ReadProcessMemory`), which cannot
   fault.
5. **Everything is resolved by decorated name**, so keep the mangled names in
   sync with the game build; if a symbol is missing the log says so
   (`api ... = 0000000000000000`).
6. The plugin is 64-bit only (PE32+); the game is 64-bit.

## Debugging

`bin\AmbientVoiceSubtitles.log` is appended to on every run (previous runs stay
at the top), and a single run contains, in order:

- `=== AmbientVoiceSubtitles v20 loaded pid=... ===`
- resolved API addresses (`api addsmsg=... getinst=...`)
- `SAYSVM speaker=... svm="..."`
- `BARK key=SVM_..._... speaker=... text="..."`
- `PAK loaded: N keys, column=...`
- `VT hooked OnBeginFrame slot=N` once the vtable hook is in place

The `v20` in the first line is the development iteration number, not the mod's
version (the release is version 1.0), so a log that says `v20` is up to date.

`NOKEY`, `NOSPEAKER`, `PAK not found`, `VT NOT hooked` and `... EXCEPTION
code=` are the interesting failure lines. See the end-user guide
(`安装说明.txt`) for what to ask players for in a bug report.
