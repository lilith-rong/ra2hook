# Runtime INI hot reload

This document records the implementation and test boundary of ra2hook's
single-player runtime INI system. Startup injection remains documented in
`INJECT_HOOK_ANALYSIS.md`; runtime reload is a separate pipeline.

## 1. Current status

Implemented:

- external WPF control panel under `ui/`;
- `ReadDirectoryChangesW` monitoring with debounce and stable-write checks;
- named-pipe IPC at `\\.\pipe\ra2hook-runtime-v1`;
- game-thread command queue and `RuntimeTick`;
- campaign/skirmish-only hard gate;
- full desired-state rebuild, validation, rollback, and last-valid-state restore;
- native `RulesClass::Read_*` and `AbstractTypeClass::LoadFromINI` routing;
- live type baselines captured with `SaveToINI` before the first write in a match;
- per-key `Immediate`, `FutureObjects`, `ControlledReload`, and
  `RestartRequired` results in the UI.

Not yet verified:

- the new C++ files have not been compiled on this host because MSVC/Visual
  Studio Build Tools are absent;
- no game run has tested the tick hook, engine reload methods, Ares, or Phobos;
- extension-only Ares/Phobos fields remain best-effort and have no generic
  rollback contract.

The WPF project has been built locally with .NET SDK 8.0.423 with zero warnings,
and a self-contained `win-x64` single-file publish completed successfully.

## 2. Runtime pipeline

```text
ra2hook/runtime/*.ini or UI command
  -> watcher / named pipe worker thread
  -> bounded command queue
  -> game-thread RuntimeTick
  -> parse private include tree into overlay CCINIClass
  -> classify and reject unsafe keys
  -> build staging INI from rules + live type baseline + overlay
  -> RulesClass::Read_* / AbstractTypeClass::LoadFromINI
```

Worker threads never parse engine objects and never write game memory. All
`CCINIClass`, `RulesClass`, and TypeClass operations run from `RuntimeTick`.

Every reload rebuilds the complete desired state from the directory. Removing a
key or file therefore rolls that value back instead of layering patches forever.
A syntax/read/include failure leaves the last valid applied state untouched.

## 3. Tick hook

Target executable:

```text
gamemd.exe
MD5    56d582a1d6f3c144d3adc867d7a4d91b
SHA256 7cd005d263fde203d9c84548200a057a8df61d724da3c6bd1e521eeb61cd0747
```

IDA shows `MainLoop` at `0x55D360` returns once per outer game-loop iteration;
the actual outer back edge is in caller `0x48CCC0` around `0x48CE8A`.

The selected normal-frame point is:

```text
0x55DE3A  89 0D 64 B5 A8 00  mov [0xA8B564], ecx
```

It is a complete six-byte instruction on the normal return path. Current public
Phobos source uses `0x55D360`, `0x55D871`, `0x55DEC1`, and `0x55DED5` for its
frame-step hooks, but not `0x55DE3A`. Ares occupancy still requires real-machine
verification.

## 4. Configuration

Runtime writes are disabled by default:

```ini
[Runtime]
Enabled=yes
AutoApply=yes
Directory=ra2hook\runtime
DebounceMs=500
```

`Enabled` is loaded once. Changing it while the game is running requires a game
restart. The UI can pause/resume `AutoApply` for the current process.

Files in `Directory` are merged by case-insensitive filename order. The shared
`IniOverlay` parser supports the same private `[#include]` behavior as startup
inject: current body first, then include entries in source order (including
repeated `+=file.ini` keys), later values
winning. Include cycles, missing files, malformed lines, and depth over 32 reject
the reload transaction. Startup inject also stages each source file before copying
it to an engine INI, so a failed include tree cannot leave a half-merged file.

## 5. Safety classes

| Class | Behavior |
|---|---|
| `Immediate` | Native live-read type data such as weapon/warhead/projectile scalars. |
| `FutureObjects` | Type values that existing objects may have copied already, such as unit `Strength`. New objects receive the new type state. |
| `ControlledReload` | Known `RulesClass::Read_*` section or native virtual `LoadFromINI` path. |
| `RestartRequired` | Recognized but not written at runtime. |

The current restart-required guard covers:

- type/list registration sections (`VehicleTypes`, `BuildingTypes`, `Warheads`,
  `WeaponTypes`, `Projectiles`, and the community compatibility alias `Projectile`,
  `Countries`, `Sides`, trigger/team/script/task-force registries, and similar);
- resource/layout keys such as `Image`, `Voxel`, `SHP`, `VXL`, `HVA`, `Palette`,
  `Foundation`, and `Locomotor`;
- resource-heavy or structural classes such as animation, overlay, smudge,
  terrain, voxel animation, trigger, team, script, task force, and AI trigger
  types;
- unknown or extension-only sections that have no native routing target.

Known native type sections are loaded through their virtual `LoadFromINI`.
Ares/Phobos keys inside those sections may be observed by extension hooks, but
that behavior is experimental; the UI labels this path as best-effort.

`[Easy]`, `[Normal]`, and `[Difficult]` share the native
`RulesClass::Read_Difficulties` route. A section literally named `[Difficulty]`
is not routed: IDA shows `0x66D270` expects the INI object in `ECX`, a destination
`DifficultyStruct*` in `EDX`, and the section name on the stack, so the YRpp
`Read_Difficulty(CCINIClass*)` declaration is not a safe callable wrapper here.
Such entries are rejected as unsupported instead of risking memory corruption.

## 6. Baseline and rollback

Before the first write to each type in a match, ra2hook calls its virtual
`SaveToINI` into a match-local baseline. This preserves map/game-mode changes
that may not exist in global `INI_Rules`.

Reload order:

1. parse and validate the new complete overlay;
2. roll back the previous runtime state;
3. capture baselines for newly touched types;
4. apply the new staging state;
5. on failure, roll back the candidate and reapply the previous valid state.

If either recovery step fails, ra2hook keeps the affected state tracked instead
of reporting a successful restore. A later reload or the automatic leave-match
rollback retries the baseline write.

Leaving the single-player match automatically rolls back and clears captured
type baselines. Global `RulesClass` sections currently roll back through
`INI_Rules`; map-specific changes to those global sections need explicit game
testing because `RulesClass` has no generic `SaveToINI` counterpart.

## 7. Single-player gate

Writes are accepted only when all conditions are true:

- `SessionClass::Instance.CurrentlyInGame`;
- game mode is Campaign or Skirmish;
- game mode is not LAN or Internet;
- session is not recording, replay playback, or attract mode.

Manual reload and file-change reload are blocked otherwise. Runtime state is
rolled back when leaving an allowed match so it cannot carry into a later
network/replay session.

## 8. Control panel

The external UI is `ui/RA2Hook.RuntimeUI.csproj` (`net8.0-windows`). It provides:

- game-root and runtime-directory selection;
- patch file create/edit/delete and atomic save;
- unsaved-edit protection during refresh, file switches, root switches, and exit;
- directory refresh and local key preview;
- connection, mode, single-player, generation, and queue status;
- auto-apply pause/resume, manual apply, and rollback;
- old/new values, safety class, result, and result filtering.

The Action publishes a self-contained `Release/ra2hook/ra2hook-ui.exe`. Keep it
under `<game>/ra2hook/`, alongside the `dump`, `inject`, and `runtime`
directories. The UI automatically uses its executable directory's parent as
the game root, and the target machine does not need a separately installed
.NET runtime.

## 9. Real-machine test order

1. Build the Action artifact and extract it into the game directory. Keep
   `ra2hook.dll` beside `gamemd.exe` and `ra2hook-ui.exe` under `ra2hook/`.
2. Set `[Runtime] Enabled=yes`; create `ra2hook/runtime/10-test.ini`.
3. Start Campaign or Skirmish and open `ra2hook-ui.exe`.
4. Test a weapon scalar, for example an existing weapon's `Damage` or `ROF`.
5. Test a `Strength` change and compare an existing unit with a newly created one.
6. Put `Image`, `Foundation`, or `Locomotor` in the patch and confirm it is listed
   as `RestartRequired` without being applied.
7. Introduce malformed syntax and confirm the previous valid generation remains.
8. Delete a previously applied key/file and confirm rollback to the baseline.
9. Exit the match and confirm status reports rollback before entering LAN/Internet.
10. Repeat with only ra2hook, Ares + ra2hook, Phobos + ra2hook, and both extensions.

Keep `ra2hook.log`, DLL versions, executable hashes, the patch files, and the UI
result table for each test run.
