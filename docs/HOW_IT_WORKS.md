# How hla-ftp-patch works

This document describes the current implementation and the evidence for it.
On 2026-10-28 the user installed the patch and reported that FTP works in the
game on their setup. The actual Alyx log confirms `Patched` at `0x3269CB`.
Synthetic process fixtures establish proxy behavior. An additional
isolated host loaded the real current SteamVR DLL and verified the in-memory
patch at `0x3269CB`, the `Patched` log, and unchanged on-disk SHA-256, without
invoking actual VR initialization. This verifies the loader/patch boundary in
that host. Gameplay confirmation comes from the user's subsequent test.

## The threshold change

An analog Grip configured as a button is processed through a threshold filter.
The filter has activation and deactivation thresholds and a pressed state.
Inspection of the supplied old SteamVR and installed current build supports the
following behavior: while released, crossing the activation threshold emits a
press; while pressed, falling below the deactivation threshold emits a release.
For the scalar Grip input, squaring the value and thresholds preserves these
comparisons for nonnegative values.

With activation 0.5 and deactivation 1.0, a value from 0.5 inclusive to 1.0
exclusive can therefore alternate pressed/released on successive updates. This
is the behavior used for repeated Jump input. At exactly 1.0 the release condition
is false, so alternation stops. The user has confirmed FTP works in-game;
update-frequency parity with the old runtime and each input boundary have not
been separately measured.

The newer init function rejects deactivation greater than activation. The patch
bypasses that one rejection branch while retaining the activation range check
and the minimum deactivation handling. It affects all threshold-filter instances
using this code inside Alyx, including bindings other than Jump.

## Verified artifacts and bytes

| SteamVR client | SHA-256 |
| --- | --- |
| Installed current DLL | `f96b015fcb0ba7027d75bd411d14001001d5ab06e039fc95c5c2c7eb37ebfc42` |
| Supplied old DLL | `ffb250556a9195630ed2d1389e61d6e8365cc6fcfa8fa23f2852ae7785bec0e9` |

The current image has the diagnostic substring
`cannot be less than the deactivation threshold` in a non-executable section.
The containing C string starts at RVA `0x40F430`. One RIP-relative `lea` at RVA
`0x3269F0` references its start. A nearby register comparison and conditional
branch are:

```text
3269C8: 0F 2F C8             comiss xmm1, xmm0
3269CB: 0F 86 8B 00 00 00    jbe    326A5C
```

The six-byte write at RVA `0x3269CB` is:

```text
before: 0F 86 8B 00 00 00
after:  E9 8C 00 00 00 90
```

The target remains `0x326A5C`; the final byte is a NOP. The three-byte comparison
is unchanged. A supported short-form branch uses `76 rel8` to `EB rel8` instead.

The finder does not assume those RVAs in an arbitrary DLL. It parses a bounded
PE64 image, locates the diagnostic string in non-executable sections, finds its
unique RIP-relative `lea` in executable sections, and checks a nearby
`comiss`/`jbe` pair with a constrained forward target. Ambiguous strings,
references, or candidates are rejected as `PatternChanged`. Recognized patched
bytes produce `AlreadyPatched`. The apply step rechecks the original bytes,
changes page protection, writes the replacement, restores protection, and flushes
the instruction cache.

The exact old artifact lacks this diagnostic and returns `NotNeeded`. Absence of
an anchor in a future build does not prove that an equivalent check has not
moved or been rewritten. Pattern matching supports updates conditionally; it is
not a promise of universal future compatibility.

## Proxy and initialization boundary

The installed game imports `openvr_api.dll` from `game/bin/win64` through
`vr.dll`. The installer preserves that DLL locally as
`openvr_api_ftp_original.dll`. The proxy preserves the original 23 export names
and ordinals. It wraps `VR_InitInternal`, `VR_InitInternal2`,
`VR_GetGenericInterface` and `VR_ShutdownInternal`; other exports forward to the
saved DLL. Wrapper resolution uses the adjacent original's full path.

Patching is gated on the process basename `hlvr.exe`. Initialization wrappers
call the original function, then inspect the loaded `vrclient_x64.dll` before
returning to the caller. The generic-interface wrapper checks around the original
call, covering an already loaded runtime and interfaces obtained later. Shutdown
invalidates cached inspection state so initialization can inspect again. An
early call with no runtime loaded does not consume a later patch attempt.

The original proposal used `LdrRegisterDllNotification` to patch immediately on
load. It was replaced: Microsoft states that calls into other modules from
`LdrDllNotification` are unsafe, which rules out that callback performing the
patching/logging sequence. The implemented `DllMain` does no patching or file I/O;
normal exported function calls perform the work. See Microsoft's
[callback documentation](https://learn.microsoft.com/en-us/windows/win32/devnotes/ldrdllnotification).

The inspected HL:A `vr.dll` provides these static application call-order facts:

| RVA in `vr.dll` | Operation |
| --- | --- |
| `0x4F13` | Calls imported `VR_InitInternal2` inside helper `0x4EF0`. |
| `0x9D56` | Calls that initialization helper. |
| `0xA0D6` | Obtains `IVRInput_011` through `VR_GetGenericInterface`. |
| `0xA157` | Constructs `cfg/actions.json`. |
| `0xA1E2` | Invokes the first virtual method on that input interface with the path. |

Valve's [public OpenVR header](https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr.h)
declares `SetActionManifestPath` as the first `IVRInput` virtual method. This
supports patching after initialization and before HL:A's explicit manifest call.
There is also a generic-interface request before initialization; it must be
allowed to proceed when no client is loaded.

These facts do not prove that runtime init cannot start asynchronous binding work
or consult a Steam partner manifest before that explicit call. They establish
the application's visible order. The user's successful in-game test confirms
the desired behavior on one setup, but does not prove absence of a timing race
across other runtime versions or configurations.

## Scope, diagnostics and installation

The modified image belongs to the current Alyx process. `vrserver`, other games,
and the SteamVR DLL on disk are unchanged. Exiting Alyx discards the memory patch.
No original Valve binary or decompiled function body is distributed with this
project. A pre-existing third-party proxy can be preserved as the local original,
but compatibility with that chain requires separate testing.

`ftp_check.exe` maps file bytes into an RVA-addressed buffer and calls the same
finder without applying a patch. It reports `Found`, `AlreadyPatched`,
`NotNeeded`, `PatternChanged`, or `Error`. The installer runs this preflight
before mutating the game directory and stops on unsupported/error results.
The runtime proxy writes `hla_ftp.log` next to itself, including module identity,
result, patch RVA, and bytes. Normal operation requires permission to write that
directory; installation ordinarily does not need elevation.

In the source checkout, the original proposal is
[the design specification](superpowers/specs/2026-10-28-hla-ftp-patch-design.md).
Its loader-callback section is superseded by the implementation described here.
[Research verification](RESEARCH_VERIFICATION.md) records exact artifact
identities, all export ordinals, methods, and retained evidence references.

## In-game confirmation and remaining acceptance checks

The user reported successful in-game FTP on 2026-10-28. A read of the real
`game/bin/win64/hla_ftp.log` independently confirmed the correct executable,
SteamVR module, patch RVA and before/after bytes. The following checklist is
retained for other installations and the checks not individually reported:

1. Install with Alyx closed, launch it, and confirm a fresh log reports `Patched`
   for the expected SteamVR client module.
2. Bind Jump to Grip as a button at activation 50/deactivation 100 and save.
3. Check repeated jumps while holding Grip within `[0.5, 1.0)` and verify that
   full squeeze stops the alternation.
4. Confirm the corresponding invalid-threshold message is absent from
   `Steam/logs/vrclient_hlvr.txt` for that binding.
5. Verify another game is unaffected and receives no patch log from this proxy.

The user report establishes working FTP on that setup; it does not individually
confirm every checklist item or compatibility with every future SteamVR build.
