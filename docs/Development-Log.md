Development log (AmbientVoiceSubtitles)
=======================================

**English** · [简体中文](开发日志.md)

This is the human-readable story: it records *why* things are the way they are,
not what the code looks like. For code structure see docs/ARCHITECTURE.md.

v1: why the first attempt was bound to crash
--------------------------------------------
The plan was straightforward: intercept the line, then call the engine's own
gCGUIEntityRenderer::AddScreenMessage to draw the text above the NPC's head.

Result: 0xC0000005 the moment the game loaded. Disassembly showed that the very
first thing AddScreenMessage does is call eCGeometryEntity::GetWorldMatrix(entity)
— I passed NULL, instant access violation.
Lesson: that API wants a *geometry entity* (eCGeometryEntity). The script-layer
Entity* is not one; you have to convert it via eCEntity::GetPropertySetCount /
GetPropertySetAt -> eCEntityPropertySet::GetGeometryEntity().

v2: finding the right entity
----------------------------
Start from the script entity the real bark actually arrives on, walk its property
sets to get the geometry entity, and only then call AddScreenMessage. Every call
carries its own SEH: any failure is logged and the plugin never takes the game
down.

The second problem followed immediately: AddScreenMessage messages are consumed
by the renderer every frame, so posting once flashes for a single frame. The fix
was a queue plus a hook on the vtable slot of gCGUIEntityRenderer::OnBeginFrame
that re-posts the queue every frame. Only after that did subtitles actually stay
on screen.

Text data: from JSON to reading the game directly
-------------------------------------------------
The earliest build shipped its own exported text database (svms_zh.json, exported
from the translation patch's svms table), copied next to bin at install time. The
problem: the text was welded to one patch version, went stale as soon as the game
language or the patch changed, and it meant the plugin was redistributing game
text.

Later this changed to reading the game's own data\compiled\strings.pak:
- parse the G3V0 archive and take svms.tab;
- entries may be zlib-compressed, so the source carries a minimal inflate (no
  dependency on zlib.dll);
- parse the TAB0 multi-language table and pick the column for the current game
  language.

The upside: the plugin contains no game text, follows the game language, and a
patch update no longer requires a plugin update.
The old JSON path is kept as a fallback (used only when the pak cannot be read),
purely for compatibility with old installs.

v17: the language has to follow the game
----------------------------------------
The text in the pak is a set of language columns. Language=auto means asking the
engine for the current voice language, via eCLocAdmin2::GetInstance /
GetVoiceLanguage. Mind the timing: query it after the engine's text subsystem is
up, otherwise you get the default value. When the player changes the language at
runtime, a changed column name triggers a re-read of the pak.

v19: lazy loading (the most expensive lesson)
---------------------------------------------
Early versions did the reading inside DllMain. Result: the plugin grabbed the
strings.pak handle ahead of the engine and touched eCLocAdmin2 too early — the
main menu text fell back to IDs such as HUD2_Label_PressStart. The player only
found out after entering the game, and Risen starts slowly, so every trial cost a
lot.

The fix: DllMain only starts a thread; text is deferred until the first subtitle
is actually about to be drawn (ensure_text). The thread only does module probing
and hooking, and waits for Game/Engine and friends to appear.

v20: the auto-generated default ini has to be the current one
-------------------------------------------------------------
The "if no ini exists, write a default one" text used to be a hand-written copy
inside the .c file. Editing the ini never updated that copy, so it kept claiming
FadeStartM=5 and the French_Text example while the real ini had long been 10.

The fix: that text no longer lives in the source. tools/gen_ini_template.py turns
config/AmbientVoiceSubtitles.ini byte for byte into src/ini_template.h (a C
string array), which is compiled into the asi.
The ini is the single source of truth: edit the ini -> re-run the generator ->
rebuild. The generated header also carries a compile-time check — an array
smaller than 512 bytes fails the build, so a generator that read the wrong file
cannot quietly ship half an ini.

Pitfalls hit along the way
--------------------------
- The embedded template only counts as correct if it is byte-identical to the ini.
  When comparing, search the whole binary to confirm there is exactly one copy and
  that it is byte-equal (and, while you are at it, that the old FadeStartM=5 text
  appears zero times).
- zig 0.17's `zig cc` does not accept --cache-dir; use the ZIG_LOCAL_CACHE_DIR /
  ZIG_GLOBAL_CACHE_DIR environment variables.
- Verify offline before delivering: a code change may not be shipped just because
  it compiles, especially since every trial with this game means restarting it,
  and it loads slowly.
