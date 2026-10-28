# Binary verification, 2026-10-28

This is static inspection of the locally installed Windows x64 binaries. No game,
SteamVR process, installed file, registry setting, or binding was changed. The
game was not launched during this static investigation. Subsequent user-confirmed
in-game testing is recorded in [TESTING.md](TESTING.md).

## Artifact identities

| Artifact | SHA-256 | File bytes |
| --- | --- | ---: |
| Installed SteamVR `bin/vrclient_x64.dll` | `f96b015fcb0ba7027d75bd411d14001001d5ab06e039fc95c5c2c7eb37ebfc42` | 5,098,648 |
| Previously supplied old `vrclient_x64.dll` | `ffb250556a9195630ed2d1389e61d6e8365cc6fcfa8fa23f2852ae7785bec0e9` | 4,546,432 |
| Installed HL:A `game/bin/win64/openvr_api.dll` | `263ef76f3b00687e041fc0f995128b62cc9f21ba7a93906f5cadac1c07ef76d8` | 819,664 |
| Installed HL:A `game/bin/win64/vr.dll` | `a8456328f0816414f4d1bd23f2b83fe62123100d913fe021f0d4899fd38f30c5` | 872,600 |

The current and old SteamVR hashes match the design specification. Their PE
TimeDateStamp values are respectively 1792184440 and 1647032450; these are not
Steam depot build identifiers. Current SizeOfImage is `0x4F5000`.

The exact old artifact was retained from the earlier investigation outside this
repository. Supply a local copy through `HLAFTP_OLD_VRCLIENT` to repeat the test;
the SHA-256 above identifies the expected file.

## Patch identity

An independent PE mapping and executable-section RIP-relative `lea` scan found
one occurrence of the validation message, beginning at RVA `0x40F430`, and exactly
one matching code reference, at RVA `0x3269F0`.

The installed current DLL contains the specified comparison and branch:

```text
RVA 3269C8: 0F 2F C8             comiss xmm1, xmm0
RVA 3269CB: 0F 86 8B 00 00 00    jbe    326A5C
RVA 3269F0: 48 8D 0D 39 8A 0E 00 lea rcx, [rip+E8A39]
```

The preceding load reads the deactivation threshold at object offset `0x12C`.
The six-byte replacement `E9 8C 00 00 00 90` preserves the target `0x326A5C`.
Only six bytes are written; the three-byte comparison remains unchanged.

The old artifact has no occurrence of the anchor substring. Its retained prior
REA/Ghidra init analysis also lacks the added validation block. Absence of the
substring is verified for this specific artifact; by itself, string absence in
an arbitrary future version cannot prove that equivalent validation is absent.

## Original OpenVR exports

REA 6.8.0 `inspect-native-exports` reports 23 named exports, ordinal base 1,
AMD64, complete coverage, no forwarders, and no ordinal-only exports:

| Ordinal | Name |
| ---: | --- |
| 1 | `LiquidVR` |
| 2 | `VRCompositorSystemInternal` |
| 3 | `VRControlPanel` |
| 4 | `VRHeadsetView` |
| 5 | `VROculusDirect` |
| 6 | `VRPaths` |
| 7 | `VRRenderModelsInternal` |
| 8 | `VRSceneGraph` |
| 9 | `VRTrackedCameraInternal` |
| 10 | `VRVirtualDisplay` |
| 11 | `VR_GetGenericInterface` |
| 12 | `VR_GetInitToken` |
| 13 | `VR_GetRuntimePath` |
| 14 | `VR_GetStringForHmdError` |
| 15 | `VR_GetVRInitErrorAsEnglishDescription` |
| 16 | `VR_GetVRInitErrorAsSymbol` |
| 17 | `VR_InitInternal` |
| 18 | `VR_InitInternal2` |
| 19 | `VR_IsHmdPresent` |
| 20 | `VR_IsInterfaceVersionValid` |
| 21 | `VR_IsRuntimeInstalled` |
| 22 | `VR_RuntimePath` |
| 23 | `VR_ShutdownInternal` |

## Initialization timing

REA `inspect-pe-imports` establishes that `vr.dll` statically imports these eight
names from `openvr_api.dll`: `VR_GetVRInitErrorAsEnglishDescription`,
`VR_ShutdownInternal`, `VRControlPanel`, `VR_InitInternal2`,
`VR_GetVRInitErrorAsSymbol`, `VR_IsInterfaceVersionValid`, `VR_GetInitToken`, and
`VR_GetGenericInterface`. No delay-import directory is present.

Independent Capstone decoding establishes the following local call sequence:

1. `vr.dll` calls `VR_InitInternal2` at RVA `0x4F13`, inside the initialization
   helper at `0x4EF0`. After successful initialization this helper acquires an
   interface through `VR_GetGenericInterface` at `0x5071`.
2. The larger initialization routine calls that helper at `0x9D56`, then later
   requests `IVRInput_011` through `VR_GetGenericInterface` at `0xA0D6` and stores
   the returned pointer at object offset `0x7E0`.
3. It constructs the `cfg/actions.json` path at `0xA157` and invokes the first
   virtual method on that same input object at `0xA1E2`, passing that path.
   Valve's [public OpenVR header](https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr.h)
   identifies the first `IVRInput` virtual method as `SetActionManifestPath`.

Consequently a patch applied after the original `VR_InitInternal2` returns, before
returning control to HL:A, precedes this explicit manifest submission. An
additional `VR_GetGenericInterface` wrapper can patch an already loaded runtime
before returning an input interface. This supports replacing the loader callback
with normal exported-function wrappers for this installed HL:A build.

There is also a generic-interface request at `0x9D42` before the initialization
helper call. A wrapper must tolerate an absent runtime and retry after init;
failure to find `vrclient_x64.dll` must not permanently consume its attempt.

The original OpenVR legacy `VR_InitInternal` at `0x1BE0` clears the startup-info
argument and jumps directly to its own `VR_InitInternal2` at `0x1BF0`. The original
generic-interface export calls the client-core virtual method and has no direct
self-recursion. Ten special accessor exports call the original generic-interface
entry directly. If forwarded, those internal calls bypass the proxy's generic
wrapper; HL:A's directly imported initialization entry remains the principal
patch point. Wrapper calls should resolve through the explicitly loaded original
module, preserving arguments, return values, and error outputs.

These observations do not establish that runtime initialization cannot start
asynchronous binding work, or cannot use a Steam partner manifest before HL:A's
explicit call. They establish the observed application call order. Runtime
initialization timing beyond that static order is not proved. Subsequent
successful user testing is documented separately in [TESTING.md](TESTING.md).

## Retained evidence

Complete new REA outputs and independent analysis scripts/results were retained
outside the repository in the local investigation's `hla-ftp-evidence` directory:

- `openvr-exports/`: retained REA export evidence,
  `ev_73fcb8c34c630e5c099d55dde56776657809a0a458ee9c13cdb61aedec7acefc`.
- `vr-imports.json`: complete REA import evidence,
  `ev_03026ae234def2b62691662ab11593c772fda74435c5b77109ba88e565bb4ca2`.
- `static-verification.json`, `call-order.json`: independently mapped bytes,
  hashes, instruction decoding, string references, and application call sites.
- `verify.py`, `calls.py`: scripts used for the independent PE/Capstone checks.

The previous Ghidra 12.1.4 analyses remain in the earlier scratchpad's `rea/`
directory: current init evidence
`ev_de62c1b4233bb6933bdc79957d5f06b87bb2eb0b527ecbd94b7e796501fe71a3`
and old init evidence
`ev_c1144d7a276e630c95f6ee43e132af804d7a9910cf047e41fc2fa0915b62a48e`.
Their subject hashes match the independently rehashed artifacts above. No Valve
binary, dump, or decompiled function body is included in this repository.
