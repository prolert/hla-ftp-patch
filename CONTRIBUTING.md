# Contributing

Bug reports, compatibility results and focused pull requests are welcome.

## Build and test

Use Windows x64 with CMake 3.20+, Visual Studio Build Tools with the C++ workload,
and a Windows SDK. In Developer PowerShell:

```powershell
git clone https://github.com/prolert/hla-ftp-patch.git
cd hla-ftp-patch
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Close Alyx before running the complete test suite: installer tests check that
`hlvr.exe` is not running. Tests use temporary fixtures and do not install the
patch into your game. A headset is not required for automated tests.
The path suite also uses an isolated temporary folder on writable D: drives,
when present, to verify installation across drive letters.

Optional real-binary finder tests accept `HLAFTP_CURRENT_VRCLIENT` and
`HLAFTP_OLD_VRCLIENT` environment variables pointing to your local DLLs. These
tests read files without changing them. Keep all Valve binaries outside Git.

To assemble a local installation package:

```powershell
cmake --install build --config Release --prefix dist/package
```

## Pull requests

- Keep changes focused and describe the problem and resulting behavior.
- Include the relevant test results. Add regression coverage for behavior fixes.
- Preserve the `hlvr.exe` process restriction and conservative pattern matching.
  Ambiguous or unsupported code must remain unmodified.
- Preserve the original DLL backup and installer rollback behavior.
- Do not commit game files, Valve DLLs, decompiled source, memory dumps, build
  output, credentials, or personal machine paths. `.gitignore` covers common
  cases, but check `git diff --cached` before committing.

## Bug reports and compatibility results

Include the patch version, SteamVR version, Windows version, headset/controller,
steps to reproduce, and the observed result. If useful, include the relevant
`hla_ftp.log` status or `ftp_check.exe` output.

Before posting logs, redact your Windows username, personal paths, and any
unrelated information. Report what you tested directly; successful pattern
matching alone does not establish working FTP in-game.

Contributions are licensed under the project's [MIT License](LICENSE).
