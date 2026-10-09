# Filesystem elevation

RBTV+ checks the filesystem operations needed by appdata before starting
application workers. The app uses a bundled, exact-title Lapy helper from
[PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)
when the sandbox cannot list or inspect the console's real `/data` mount.

## Startup flow

1. The native client probes `/data` and the RBTV+ appdata directory.
2. If restricted, it tries `/app0/lapy.elf`, then the title's
   `/mnt/sandbox/PPSA98273_000/app0/lapy.elf` mount, then the extracted
   `/data/homebrew/PPSA98273/lapy.elf` path. Each location is attempted with
   both the kernel open API and POSIX `open()`. It then connects to the local
   ELF loader at `127.0.0.1:9021` and streams the helper over that connection.
   If every path fails, the recovery screen reports the individual open
   results instead of just saying that the helper is unavailable.
3. The client and helper exchange the versioned 24-byte `ELV1` request /
   prepare / prepared / response protocol. The client acknowledges the prepare
   step with `seteuid(geteuid())`, allowing the helper to verify the expected
   credential clone before applying its filesystem grant.
4. The app reruns the storage probe. It only starts account and cache workers
   after the file and directory operations it needs actually pass.

The helper is pinned to source commit
`153c2362b1bb78475b2fcf46ba71552698ae2f7c` and built separately with PS5
Payload SDK v0.43. That revision includes the upstream credential-layout fix
for firmware 13.60. The main application and download-writer can continue to
use the project's normal SDK. The generated helper manifest is shipped beside
`lapy.elf` for traceability; the build does not claim console validation of
the generated RBTV+ package.

## Storage and diagnostics

Persistent app data is stored under `/data/RBTVPlus/appdata` and startup
receipts under `/data/RBTVPlus`. Existing data is not deleted or chmod'ed by
the elevation path. A missing or inaccessible directory is not treated as a
new, signed-out account. If elevation or the post-grant probe fails, the app
shows a recovery screen before account/cache workers start.

The app does not reserve a `/download0` volume in package metadata. The
Lapy helper's one-shot ELF path used here does not install a persistent
service. The independent `download-writer.elf` remains packaged for
download operations.

## Rebuilding and checks

The native build script fetches a pinned source commit, validates its source
and protocol digests, verifies the v0.43 SDK archive checksum, and packages
`lapy.elf`, its manifest, and the MIT license.

Run the static elevation and storage checks with:

```sh
python3 tests/check_elevation_title.py
python3 tests/run_ps5_storage_tests.py
```
