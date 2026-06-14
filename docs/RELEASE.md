# Release Process

## Version Source

The app version lives in `VERSION`.

CMake reads `VERSION` and passes it into the app as
`LLM_USAGE_TRAY_VERSION`.

## Pull Requests

`.github/workflows/ci.yml` builds Release artifacts for Windows, macOS, and
Linux for PRs and manual runs.

## Main Branch Pushes

`.github/workflows/release-on-main.yml` does the release build flow:

1. Bumps the patch version in `VERSION`.
2. Commits the version bump with `[skip ci]`.
3. Tags the commit as `vX.Y.Z`.
4. Builds Release artifacts for Windows, macOS, and Linux.
5. Uploads zipped/tarred artifacts.

The workflow needs repository `contents: write` permission, which is declared
in the workflow file.

## Local Release Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target LLMUsageTray
```

The executable is written to:

```text
build\Release\LLMUsageTray.exe
```
