# Verification — 2026-10-28

Local Windows x64 Release build, MSVC 19.44 / Windows SDK 10.0.26100.

## Automated checks

- Core: 61 synthetic cases cover PE bounds, ambiguous patterns, short/long branch targets, idempotence, protected memory writes, page protection restoration, and handled read/write faults.
- Loader fixtures: `hlvr.exe` patches before returning initialization, forwards all 23 named exports with original ordinals, preserves initialization failures, and patches again after shutdown/reload. `other_game.exe` changes no bytes and writes no patch log.
- Fault fixture: an unreadable section raises an access violation during scanning. Forwarding continues and shutdown unloads the module; the retained handle is released after SEH handling.
- Checker: synthetic statuses, malformed JSON/PE, bounded input, Unicode/escaped paths, explicit path/default discovery, exit codes and unchanged file hashes.
- Installer: 17 temporary-directory scenarios, including discovery, repeat install/uninstall, game update, foreign proxy, missing/corrupt backup, failed compatibility check, changed package, running game, and file-lock rollback. Installed game files were not changed by these tests.
- Installer paths: 10 end-to-end scenarios on writable C: and D: drives cover Cyrillic, spaces, brackets, ampersands, percent/exclamation signs, trailing separators, actual batch install/uninstall from another working directory, modern/legacy Steam libraries and a custom installation directory. Registry reads use a test fixture; no registry values or real games are changed. D: cases are skipped when the drive is absent or its root is not writable.
- Batch launchers: 62 argument/exit-code checks cover named and positional paths, Unicode and special characters, trailing separators and drive roots, both cmd delayed-expansion modes, and invalid/missing/duplicate arguments. Paths cross the PowerShell boundary as environment data, never interpolated code.
- Eight CTest suites cover these checks. Optional real-binary tests use `HLAFTP_CURRENT_VRCLIENT` and `HLAFTP_OLD_VRCLIENT`; no Valve file is part of the repository or package.

## Actual binary observations

Current SteamVR SHA-256 `f96b015fcb0ba7027d75bd411d14001001d5ab06e039fc95c5c2c7eb37ebfc42`: finder returns `Found`, RVA `0x3269cb`, bytes `0F 86 8B 00 00 00` → `E9 8C 00 00 00 90`.

Old SHA-256 `ffb250556a9195630ed2d1389e61d6e8365cc6fcfa8fa23f2852ae7785bec0e9`: finder returns `NotNeeded`.

The actual current SteamVR DLL was also loaded into an isolated fixture process named `hlvr.exe`. The proxy changed the expected branch in that loaded image and logged `Patched`. The file hash on disk remained unchanged. The fixture used a fake OpenVR initializer; it did **not** initialize SteamVR, run Alyx, or submit real bindings.

REA export/import and call-order evidence is summarized in [RESEARCH_VERIFICATION.md](RESEARCH_VERIFICATION.md).

## Real in-game confirmation

FTP was reported working after installation and an in-game test on one setup.
This is user-observed gameplay evidence, separate from automated fixture tests.

The real game's `game/bin/win64/hla_ftp.log` was also read after that report.
It identifies the installed Alyx `hlvr.exe` and SteamVR `vrclient_x64.dll`, with
`Patched`, `TimeDateStamp=0x6ad29078`, `SizeOfImage=0x4f5000`, RVA `0x3269cb`,
and the expected six-byte replacement. Personal paths and the raw log are not
included in this repository.

This confirms successful FTP on the user's setup. Comparative cadence,
other-game behavior, every pressure boundary, and restart/uninstall checks
were not individually reported.

## Acceptance checklist for further testing

1. Close Alyx, extract the release ZIP and run `install.bat`. Keep the created original DLL next to the proxy.
2. Start SteamVR and Alyx with Quest 2. Confirm `Patched` in `game/bin/win64/hla_ftp.log` and no inverted-threshold rejection in Steam's `logs/vrclient_hlvr.txt`.
3. Set Jump on Grip, Use as Button, activation 50/deactivation 100. Save. Check repeated teleports while holding `0.5 <= grip < 1.0`, release to stop, and compare cadence against the old runtime.
4. Check menu/restart/save-load behavior and a normal binding. Test another game separately; its behavior should remain unchanged.
5. Close Alyx, run `uninstall.bat`, then confirm normal startup with the restored original.

Static call-order evidence shows the patch runs before Alyx explicitly submits
its action manifest. Successful gameplay on one setup does not prove that no
runtime worker could execute that function earlier in other configurations.
The build remains experimental pending broader compatibility testing.
