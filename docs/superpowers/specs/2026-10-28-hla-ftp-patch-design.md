# Half-Life Alyx FTP Patch — design specification

Date: 2026-10-28
Status: design approved in discussion; original specification.

This document preserves the original design proposal. The loader-callback
approach in section 4.2 was superseded during implementation by OpenVR export
wrappers. The implemented hooks, transactional installer and subsequent
verification are documented in [HOW_IT_WORKS](../../HOW_IT_WORKS.md) and
[TESTING](../../TESTING.md).

## 1. Goal

Restore the FTP glitch in Half-Life: Alyx on current SteamVR: repeated fast
teleports through the Jump action. Speedrunners using Quest 2 should no longer
need to install an archived SteamVR 1.21.x build.

Requested behavior:

- Enabled through an ordinary SteamVR binding: **Jump**
  (`/actions/move/in/ShiftDashJump`) on **Grip**, **Use as Button**,
  Click Activation Threshold **50**, Click Deactivation Threshold **100**.
- Works **only in HL:A**, without affecting other games.
- Reapplies the patch on every game launch, avoiding manual reinstallation after
  SteamVR updates while the relevant code remains recognizable.
- Open-source GitHub project with straightforward installation for other
  speedrunners.

Out of scope: other games, other actions, the OpenXR runtime, 32-bit builds,
Linux/Proton, an in-game overlay, and INI-based settings.

## 2. Reverse-engineering findings

Two versions of `vrclient_x64.dll` were examined using REA 6.8.0, Ghidra 12.1.4
and Capstone:

| | Older build: FTP works | Newer build: FTP rejected |
|---|---|---|
| SteamVR | 1.21.12, build 1647034158, March 2022 | build 1792185270 |
| SHA-256 | `ffb250556a9195630ed2d1389e61d6e8365cc6fcfa8fa23f2852ae7785bec0e9` | `f96b015fcb0ba7027d75bd411d14001001d5ab06e039fc95c5c2c7eb37ebfc42` |
| Threshold-filter initialization | RVA `0x289470` | RVA `0x3264e0` |
| Threshold-filter update | RVA `0x28a370` | RVA `0x3276b0` |

### 2.1 How SteamVR treats an analog grip as a button

The `button` mode is compiled into a filter graph:
`/value → threshold → /click → action`.
The `click_activate_threshold` and `click_deactivate_threshold` settings become
the threshold filter's `activate_threshold` and `deactivate_threshold` fields
at offsets `+0x128` and `+0x12c`. Its state is at `+0x130`.

The threshold-filter update function is **the same in both versions**:

```c
float mag2 = x*x + y*y + z*z;          // For grip, this is value squared.
if (pressed) { if (mag2 <  deact*deact) { pressed = 0; emit(false); } }
else         { if (mag2 >= act*act)     { pressed = 1; emit(true);  } }
```

With `act = 0.5`, `deact = 1.0`, and a grip value in `[0.5, 1.0)`, the state
alternates on each update: pressed, released, pressed, and so on. The game
receives repeated Jump presses. This is the input behavior used for FTP.
At a grip value of exactly `1.0`, `mag2 < 1.0` is false and alternation stops.
This follows from the code; controller behavior and cadence require in-headset
verification.

### 2.2 What changed in Valve's fix

The older initialization function reads the thresholds, emits the initial
state, and returns `true`, without validating the relationship between them.

The newer initialization function adds validation after reading the values:

```c
if (act < 1e-5 || act > 1.0) { log("Activation threshold of %f in filter %s is outside the allowable range [ %f , %f ]."); return false; }
if (deact > act)             { log("Activation threshold of %f cannot be less than the deactivation threshold of %f in filter %s."); return false; }
if (deact < 1e-5)            { log("Deactivation threshold of %f will be set to the minimum allowed value %f in filter %s."); deact = 1e-5; }
return true;
```

A 50/100 binding reaches the second branch: filter creation fails, and Jump on
Grip silently stops working. The binding UI, `controllerbindingui.js`, still
provides two independent sliders from 0 to 1, so users can still enter 50/100.

The relevant location in the newer build:

```text
003269c0: f3 0f 10 8b 2c 01 00 00   movss  xmm1, [rbx+0x12c]   ; deact
003269c8: 0f 2f c8                  comiss xmm1, xmm0          ; deact ? act
003269cb: 0f 86 8b 00 00 00         jbe    0x326a5c            ; deact <= act: continue
003269d1: ...                                                  ; otherwise log and return false
003269f0: 48 8d 0d 39 8a 0e 00      lea    rcx, ["Activation threshold of %f cannot be less than…"]
```

The string containing `cannot be less than the deactivation threshold` occurs
only in the newer inspected build. It has exactly one code reference: the `lea`
at RVA `0x3269f0`.

### 2.3 Where the code runs

Filter messages such as
`Unable to find input 'z' in filter click_button_actions_move_…` appear in
`Steam\logs\vrclient_hlvr.txt`. This indicates that HL:A's filter graph is built
and evaluated by `vrclient_x64.dll` **inside the `hlvr.exe` process**. Patching
that process's memory does not modify `vrserver` or other applications.

HL:A loads `openvr_api.dll` from `game\bin\win64\`, through a static import in
`vr.dll`. That DLL has 23 exports, including `VR_InitInternal2`,
`VR_GetGenericInterface` and `LiquidVR`.

## 3. Solution

Apply an in-memory patch to `vrclient_x64.dll` inside `hlvr.exe`: replace the
conditional `jbe` from section 2.2 with an unconditional `jmp` to the same target.
The `deact > act` branch will no longer reject the filter.

The activation-range check and minimum deactivation value remain intact. For a
50/100 binding, the resulting threshold behavior matches the older SteamVR.

Deliver the code as an `openvr_api.dll` proxy in the HL:A directory. This was
the approved distribution approach, originally called option A.

## 4. Components

### 4.1 `ftp_core`: static library

The image-search logic has no WinAPI dependency.

**`pe_image`** parses a PE64 image in mapped, RVA-addressed form, as the loader
lays it out in memory: DOS/NT headers and the section list, including section
RVA, size and executable flags. A `MapFileImage(bytes) -> vector<uint8_t>` helper
maps a disk file into RVA layout for `ftp_check` and tests.

**`patcher`**:

```cpp
// Found: a site was located, but no bytes were written (FindPatchSite).
// Patched: replacement bytes were written (ApplyPatch).
enum class PatchStatus { Found, Patched, AlreadyPatched, NotNeeded, PatternChanged, Error };
struct PatchSite { uint32_t rva; std::vector<uint8_t> before, after; };
struct FindResult { PatchStatus status; std::optional<PatchSite> site; std::string detail; };

FindResult FindPatchSite(const uint8_t* image, size_t imageSize);  // No side effects.
```

`FindPatchSite` algorithm:

1. Search non-executable sections for the substring
   `cannot be less than the deactivation threshold` and locate the start of its
   containing C string, after the preceding `\0`. If absent, return `NotNeeded`.
   The known older SteamVR build has no such check and needs no patch.
2. In executable sections, find all `lea r64, [rip+disp32]` instructions whose
   target is the string start. Supported encoding: REX `0x48`/`0x4C`, opcode
   `8D`, and `modrm & 0xC7 == 0x05`. If there is not exactly one reference,
   return `PatternChanged`.
3. Within `[lea − 0x80, lea)`, find candidates satisfying all of these conditions:
   - `jbe rel32` (`0F 86`) or `jbe rel8` (`76`);
   - immediately preceded by register-to-register `comiss xmm, xmm`:
     `[REX 0x40–0x4F] 0F 2F modrm`, with `mod == 11b`;
   - branch target after the `lea`, no farther than `lea + 0x200`.

   Exactly one candidate produces `Found`. If the equivalent unconditional
   `jmp` is present instead, encoded as `E9 rel32 90` or `EB rel8`, return
   `AlreadyPatched`. Otherwise return `PatternChanged`.
4. Replacement: `0F 86 rel32` → `E9 rel32' 90`, with
   `rel32' = target − (site + 5)`; or `76 rel8` → `EB rel8`.

```cpp
FindResult ApplyPatch(uint8_t* image, size_t imageSize);  // Find, protect, write, flush.
```

`ApplyPatch` calls `FindPatchSite`. For `Found`, it verifies that the current
bytes still match `before`, writes `after`, and returns `Patched`. Other
statuses are returned without writing. Writing uses `VirtualProtect` and
`FlushInstructionCache`, with original page protection restored afterward.

### 4.2 `openvr_api.dll`: original proxy-loading proposal, x64

This subsection records the original loader-notification design. It is
**superseded** by the implemented export wrappers described in
[HOW_IT_WORKS](../../HOW_IT_WORKS.md).

- `exports.def`: forward the 23 exports using
  `Name=openvr_api_ftp_original.Name`.
  Keep the original alongside the proxy as `openvr_api_ftp_original.dll`,
  without additional dots in its base name so forwarders resolve correctly.
- VERSIONINFO resource: `ProductName = "HLA FTP Patch"`, version `1.0.0`.
  The installer uses this marker to recognize its own DLL.
- Proposed `DllMain(DLL_PROCESS_ATTACH)` sequence:
  1. If the executable name is not `hlvr.exe`, compared case-insensitively,
     perform no patching.
  2. Resolve `LdrRegisterDllNotification` through
     `GetProcAddress(ntdll, "LdrRegisterDllNotification")` and register a callback.
  3. If `vrclient_x64.dll` is already loaded, locate it with `GetModuleHandleW`
     and patch immediately.
- Proposed `LDR_DLL_NOTIFICATION_REASON_LOADED` callback: if `BaseDllName` is
  `vrclient_x64.dll`, call `ApplyPatch(DllBase, SizeOfImage)` once, guarded
  against repeated application. The intended timing was after mapping and
  before runtime initialization and binding construction.
- Proposed `DLL_PROCESS_DETACH`: unregister through
  `LdrUnregisterDllNotification`.
- Wrap patch access in SEH, `__try/__except`, so unexpected memory faults
  produce `Error` and allow the game to continue without FTP.

The final implementation does not scan, patch or log inside loader callbacks.

### 4.3 `hla_ftp.log`

Store the log beside the proxy DLL and begin a new log for each process launch.
Include the time, patch version, executable path, runtime DLL path,
`TimeDateStamp`, `SizeOfImage`, status, patch RVA, before/after bytes and detail.
Mirror messages to `OutputDebugStringA`.

### 4.4 `ftp_check.exe`: console utility

- Accept a `vrclient_x64.dll` path as an argument. Otherwise read
  `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` and use
  `runtime[0] + "\bin\vrclient_x64.dll"`.
- Read the file, map it by RVA through `MapFileImage`, and call `FindPatchSite`.
  Do not modify the file.
- Print a clear verdict: the patch can be applied at a given RVA; the known
  older runtime does not need the patch; or Valve changed the code and a patch
  update may be required.
- Return `0` for `Found`, `NotNeeded` and `AlreadyPatched`, and `1` for
  `PatternChanged` or `Error`.

### 4.5 Installer: original proposal

`install.bat` launches `install.ps1` with `-ExecutionPolicy Bypass`;
`uninstall.bat` launches `uninstall.ps1`.

Original installation sequence:

1. If `hlvr.exe` is running, stop and ask the user to close the game.
2. Discover HL:A using Steam's path in
   `HKCU\Software\Valve\Steam\SteamPath`, libraries in
   `steamapps\libraryfolders.vdf`, and `appmanifest_546560.acf`.
   Resolve the game directory ending in
   `steamapps\common\Half-Life Alyx\game\bin\win64`.
   Allow `-GamePath` to override discovery.
3. If the current `openvr_api.dll` has a ProductName other than
   `HLA FTP Patch`, preserve it as `openvr_api_ftp_original.dll`. It may be a
   fresh original after a game update or file verification, or another proxy
   such as openvr_fsr or vrperfkit, creating a forwarding chain.
4. If the current DLL is already this proxy, update it. If its original backup
   is missing, stop with an error and recommend Steam's
   **Verify integrity of game files**.
5. Copy in the proxy and run `ftp_check.exe` to show compatibility status.

The initial proposal replaced an existing old backup. The final installer
instead archives stale backups, checks compatibility before changing the game,
and uses transactional replacement and rollback.

Original uninstallation proposal: if the current DLL is this proxy and the
backup exists, remove the proxy and restore the backup as `openvr_api.dll`.

## 5. Data flow

Original proposed flow, with the superseded loader callback:

```text
hlvr.exe → vr.dll → openvr_api.dll (proxy) → openvr_api_ftp_original.dll
                         │ DllMain: hlvr.exe? → LdrRegisterDllNotification
VR_InitInternal2 → LoadLibrary(SteamVR\bin\vrclient_x64.dll)
                         └→ callback → FindPatchSite → jbe→jmp (6 bytes) → hla_ftp.log
SetActionManifestPath → construct bindings → threshold(0.50/1.00) accepted
each input update → pressed toggles → ShiftDashJump on/off → FTP
```

The implemented flow applies the patch through OpenVR initialization/interface
wrappers instead. The short-branch encoding uses 2 bytes; the long-branch
replacement shown above uses 6 bytes.

## 6. Error handling

| Condition | Behavior |
|---|---|
| Process is not `hlvr.exe` | Forward exports only; no patching. |
| Validation string absent, as in the known older runtime | Return `NotNeeded`, log the result, leave memory unchanged. |
| String references or candidates are not unique | Return `PatternChanged`, log the result, leave memory unchanged; continue without FTP. |
| Already patched | Return `AlreadyPatched`; do not write again. |
| `VirtualProtect` failure, changed bytes, or a handled memory fault | Return `Error` and log diagnostics; allow the game to continue. |
| `openvr_api_ftp_original.dll` missing | HL:A cannot start because the loader cannot resolve the original; the installer must prevent this state. |

## 7. Build

- CMake 3.20 or newer, MSVC with Visual Studio 2022 or a compatible newer
  toolchain, x64, C++20, static CRT through `/MT`. No separate VC++ redistributable
  should be needed.
- Targets: `ftp_core` static library, `openvr_api` shared proxy, `ftp_check`
  executable, and `ftp_tests` registered with CTest.
- GitHub Actions on `windows-latest`: Release build and `ctest`. A `v*` tag
  publishes a ZIP containing the proxy, checker, installer/uninstaller scripts,
  shared script helpers, README and other distribution documentation.

## 8. Testing

**Synthetic tests, including CI:** construct a minimal mapped PE64 image with
the anchor string in `.rdata` and `comiss`, a branch and a `lea` in `.text`.
Required cases:

- `jbe rel32` becomes `Patched`, with `E9 rel32' 90` and the same target;
- `jbe rel8` becomes `Patched`, with `EB rel8`;
- no anchor string produces `NotNeeded`;
- an anchor without a `lea` produces `PatternChanged`;
- two candidates produce `PatternChanged`;
- a second call after successful patching produces `AlreadyPatched`;
- invalid PE headers produce `Error`.

**Real binaries, when provided locally; otherwise skip:**

- Current `SteamVR\bin\vrclient_x64.dll` should produce `Found`. For the known
  newer build identified by the SHA-256 in section 2, expect RVA `0x3269cb`,
  with `0F 86 8B 00 00 00` → `E9 8C 00 00 00 90`. Other builds require their
  own compatibility checks.
- The older file supplied through `HLAFTP_OLD_VRCLIENT` should produce
  `NotNeeded`.

**Headset acceptance:**

1. Install and launch HL:A. Confirm `Patched` in `hla_ftp.log`.
2. Bind Jump to Grip with thresholds 50/100 and save.
3. Confirm the binding does not produce the
   `cannot be less than the deactivation threshold` message in
   `vrclient_hlvr.txt`.
4. Confirm repeated jumps while Grip is held between 50% and less than 100%.
5. Check that another game, such as Boneworks, behaves normally and does not
   write this proxy's `hla_ftp.log`.

Subsequent automated and in-game results are recorded in
[TESTING](../../TESTING.md).

## 9. Repository layout

```text
CMakeLists.txt
src/        dllmain.cpp, patcher.{h,cpp}, pe_image.{h,cpp}, log.{h,cpp}, exports.def, version.rc
tools/      ftp_check.cpp
tests/      ftp_tests.cpp and additional integration tests
scripts/    install.bat, install.ps1, uninstall.bat, uninstall.ps1, common.ps1
docs/       HOW_IT_WORKS.md, TESTING.md, media/, superpowers/specs/
README.md   English overview, installation, bindings, updates, removal and troubleshooting
CONTRIBUTING.md
LICENSE     MIT
.github/workflows/build.yml
```

Do not include SteamVR binaries, Valve's decompiled source or memory dumps in
the repository. Technical documentation should contain addresses, short
disassembly excerpts and original explanations of the observed logic.

## 10. Risks

- **Filter update frequency.** The update function matches the 2022 build, but
  its call frequency in a newer runtime requires headset testing. A cadence
  difference is a separate investigation.
- **Alyx updates or file verification.** Steam may restore the original
  `openvr_api.dll`; run `install.bat` again afterward.
- **Valve rewrites the function.** An unrecognized pattern produces
  `PatternChanged`; the game continues without FTP. The checker and log help
  identify the need for a patch update. An absent string alone does not prove
  compatibility with an unknown future runtime.
- **Antivirus false positives.** Proxy DLLs may be flagged. Open source and
  reproducible CI builds help users inspect what is shipped.
- **Leaderboard rules.** Category rules and moderators determine whether the
  modification is accepted. Documentation should clearly explain what changes.
