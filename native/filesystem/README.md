# Filesystem integration

Stremio Plus uses the filesystem capability helper from
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
adapted through ProsperoLight. The implementation retains BlackBearReloaded's
copyright and GPL-3.0-or-later license.

## Startup

`native/ps5_storage.cpp` checks the filesystem operations needed by downloads
before starting application workers. If access is unavailable, `elevation.cpp`
submits the bundled `sandbox-elevator.elf` to the console's local ELF loader on
port 9021. The helper validates the target process and Title ID `PPSA74126`.
The versioned request and response messages are defined in `protocol.hpp`.

The application checks access again after a successful grant and resolves its
existing app and data mounts. Account settings and artwork caches keep their
existing storage location. The helper is requested during startup and does not
install a persistent service.

## Storage and diagnostics

Downloads and diagnostics use `/data/Stremio`. Startup checks cover directory
access, file creation, reading, writing, synchronization, renaming and removal.
File permissions alone do not replace the filesystem capability required by
the console's loader.

When the log directory is accessible, startup stages are recorded in
`boot-current.txt`, with the preceding launch retained as `boot-last.txt`.
If filesystem access fails, the application continues using its sandbox
mounts and reports the unavailable storage. It does not create an alternative
log directory.

## Building

The native build compiles `helper/main.cpp` with the PS5 payload SDK, validates
the ELF with `validate-helper.py`, and places `sandbox-elevator.elf` beside
`eboot.bin`. The client and helper share the same protocol header and timeout.

Run the host storage checks with:

```sh
python3 tests/run_ps5_storage_tests.py
```
