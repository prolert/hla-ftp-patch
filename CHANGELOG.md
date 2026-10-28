# Changelog

## 1.0.0 — 2026-10-28

- Initial Windows x64 release restoring inverted SteamVR button thresholds in
  Half-Life: Alyx through a game-local OpenVR proxy.
- Jump/Grip FTP with activation 50 and deactivation 100; user-confirmed working
  in-game on one setup, with the actual Alyx log confirming the expected patch.
- Conservative pattern scan on each launch, read-only compatibility checker,
  backup-preserving installer and uninstaller, English installation instructions.
- Eight automated test suites including 61 core checks, 17 installer scenarios,
  10 installation-path scenarios, 62 batch-launcher checks,
  forwarding/process-isolation checks and handled memory-fault coverage.
- Robust launcher argument forwarding for Cyrillic/special paths, trailing
  directory separators, and terminals with delayed expansion enabled.
- SteamVR files are never patched on disk. Compatibility with arbitrary future
  SteamVR versions and third-party proxy chains is not guaranteed.
