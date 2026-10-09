#!/usr/bin/env python3
"""Keep filesystem elevation pinned to the firmware-13.60-compatible helper."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
title_id = json.loads((root / "app/sce_sys/param.json").read_text())["titleId"]
builder = (root / "tools/build-native-entry.sh").read_text()
helper_builder = (root / "tools/build-lapy-helper.py").read_text()
elevation_header = (root / "native/filesystem/elevation.hpp").read_text()

if title_id != "PPSA98273":
    raise SystemExit(f"Unexpected RBTV+ development title: {title_id}")
if 'LAPY_COMMIT = "153c2362b1bb78475b2fcf46ba71552698ae2f7c"' not in helper_builder:
    raise SystemExit("Lapy helper must remain pinned to the firmware-13.60 startup fix")
if 'SDK = CACHE / "ps5-payload-sdk-v0.43"' not in helper_builder:
    raise SystemExit("Lapy helper must use the separate v0.43 Payload SDK")
if 'SDK_URL = "https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip"' not in helper_builder:
    raise SystemExit("Lapy helper SDK download URL is not the pinned v0.43 release")
if 'SDK_SHA256 = "a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb"' not in helper_builder:
    raise SystemExit("Lapy helper SDK must be checksum-pinned")
if 'PROTOCOL_SHA256 = "bb02c4aa814eaba7a7a423a31b29ff41f786212c2953678cf29434e85fa0f869"' not in helper_builder:
    raise SystemExit("Lapy helper protocol digest must remain pinned")
if '[f"TARGET_TITLE={args.title}"]' not in helper_builder:
    raise SystemExit("Helper build must receive the package's exact title ID")
if 'python3 "$ROOT/tools/build-lapy-helper.py" "$TITLE" "$B/lapy-helper"' not in builder:
    raise SystemExit("Native app builder must generate Lapy helper for the selected title")
if 'cp "$B/lapy-helper/lapy.elf" "$APP/lapy.elf"' not in builder:
    raise SystemExit("Lapy helper must be packaged beside eboot.bin")
if 'cp "$B/lapy-helper/lapy-manifest.json" "$APP/lapy-manifest.json"' not in builder:
    raise SystemExit("Lapy helper manifest must be packaged for traceability")
if 'cp "$B/lapy-helper/Lapy-MIT.txt" "$APP/licenses/Lapy-MIT.txt"' not in builder:
    raise SystemExit("Lapy upstream license must be shipped with the app")
if 'helper_path = "/app0/lapy.elf"' not in elevation_header:
    raise SystemExit("Elevation client default must match the packaged helper path")
elevation_client = (root / "native/filesystem/elevation.cpp").read_text()
for path in ('/mnt/sandbox/', '/data/homebrew/'):
    if path not in elevation_client:
        raise SystemExit(f"Elevation client must try the {path} helper fallback")
if "helper_open_diagnostic()" not in elevation_header or "helper_open_detail" not in (root / "src/main.cpp").read_text():
    raise SystemExit("The recovery screen must show helper-open diagnostics")
if "sandbox-elevator.elf" in builder:
    raise SystemExit("The old firmware-incompatible helper must not be packaged")

print(f"Filesystem elevation helper pinned for {title_id}: Lapy 153c236 / SDK v0.43")
