# Canon IXUS 870 IS -- USB Webcam via CHDK

> USB webcam project for Canon IXUS 870 IS. See [README](README.md) for overview, [Getting Started](docs/getting-started.md) for setup.

## Development Rules (MUST follow)

- **Never add "Co-Authored-By" lines** to commit messages. Do not mention Claude in commits.
- **After every bridge test**: ALWAYS ask the user for the result. Do NOT assume what happened -- the user can see the camera physically. Wait for their response.
- **Document and commit BEFORE any code changes**: After receiving test results from the user, FIRST update docs with the findings, THEN commit. Only AFTER the commit may you proceed with further code changes.
- **Run bridge with `--debug --timeout 10 --no-preview --no-webcam`** during firmware development (per-frame debug logging, graceful shutdown after 10s, no virtual webcam needed).
- **Commit after each bridge test** with a message that describes what was tested and what the result was.
- **Debug frames -- NEVER during streaming**: The debug frame protocol (`spy_debug_reset/add/send`) exists for controlled diagnostic sessions. Each debug frame steals a PTP round-trip, causing one H.264 frame to be missed -> decoder artifacts (proven fact #30). See [Debug Frame Protocol](docs/debug-frame-protocol.md).
- **Check proven-facts.md first**: Always read `docs/proven-facts.md` before making new conclusions or assumptions about camera behavior. This prevents re-investigating settled questions and contradicting verified facts.
- **No double indirection in movie_rec.c**: NEVER dereference a pointer read from another pointer (e.g. `*(*(0xFF93050C) + 0xC4)`). The ARM compiler generates code that crashes the camera. Use hardcoded addresses instead. The ring buffer struct is always at `0x8968`.
- **Long-running camera operations in separate DryOS tasks**: Zoom, focus, or any operation that blocks for >10ms must run via `CreateTask()`, NOT in the PTP handler. Blocking PTP prevents frame retrieval -> missed frames -> artifacts (proven fact #32).
- **USB hangs -- restart bridge first**: If the bridge receives no data, restart it before assuming a camera crash. Some hangs are USB-level only and clearing the USB connection fixes them. Only do a battery pull if restarting the bridge doesn't help.
- **Two documentation files -- keep both updated**:
  - [Development Log](docs/development-log.md) -- chronological history. Append new sections for each test/iteration. Include what was tried, raw test output, failures, and reasoning. This is the full narrative.
  - [Proven Facts](docs/proven-facts.md) -- only verified, 100% confirmed facts. No history, no speculation, no failed approaches. Update this when a test PROVES something new. Remove or correct entries if later tests disprove them. Read this file at the start of each session.

## Key References

- [Proven Facts](docs/proven-facts.md) -- read at the start of each session
- [Getting Started](docs/getting-started.md) -- build, deploy, run, troubleshoot
- [Architecture](docs/architecture.md) -- system design, PTP protocol, data flow
- [Development Log](docs/development-log.md) -- full implementation history

## Build Commands

**CHDK (camera-side) -- Docker:**
```
docker run --rm -v "C:\projects\ixus870IS\chdk:/srv/src" chdkbuild make PLATFORM=ixus870_sd880 PLATFORMSUB=101a fir
```

**PC-side bridge -- VS 2022 Build Tools:**
```
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% -B C:\projects\ixus870IS\bridge\build -S C:\projects\ixus870IS\bridge -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
%CMAKE% --build C:\projects\ixus870IS\bridge\build --config Release
```

## Deploy Commands

```
"C:/projects/ixus870IS/bridge/build/Release/chdk-webcam.exe" --upload "C:/projects/ixus870IS/chdk/bin/DISKBOOT.BIN" "A/DISKBOOT.BIN" --upload "C:/projects/ixus870IS/chdk/CHDK/MODULES/webcam.flt" "A/CHDK/MODULES/webcam.flt" --reboot
```

- `DISKBOOT.BIN` -- upload when `movie_rec.c` or `ptp.c` changes
- `webcam.flt` -- upload when `webcam.c` changes
- **IMPORTANT**: If movie_rec.c or ptp.c changed, you MUST upload DISKBOOT.BIN

## Development Workflow

```
"C:/projects/ixus870IS/bridge/build/Release/chdk-webcam.exe" --debug --timeout 10 --no-preview --no-webcam
```

After each test run, document findings and commit all modified files.
