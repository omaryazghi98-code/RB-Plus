#!/usr/bin/env python3
"""Ensure the native filesystem elevation helper targets the packaged title."""
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
metadata = json.loads((root / "app/sce_sys/param.json").read_text())
title_id = metadata["titleId"]

helper = (root / "native/filesystem/helper/main.cpp").read_text()
makefile = (root / "native/filesystem/helper/Makefile").read_text()
builder = (root / "tools/build-native-entry.sh").read_text()

fallback = re.search(r'^#define TARGET_TITLE_ID "([A-Z0-9]+)"$', helper, re.MULTILINE)
if fallback is None:
    raise SystemExit("Elevation helper has no explicit title-ID fallback")
if fallback.group(1) != title_id:
    raise SystemExit(
        f"Elevation helper fallback {fallback.group(1)} does not match packaged title {title_id}"
    )
if "TARGET_TITLE_ID ?= PPSA98273" not in makefile:
    raise SystemExit("Elevation helper Makefile default must match the RBTV+ development title")
if "-DTARGET_TITLE_ID=" not in makefile or '$(TARGET_TITLE_ID)' not in makefile:
    raise SystemExit("Elevation helper build must compile its target ID as a string literal")
if 'TARGET_TITLE_ID="$TITLE" OUTPUT="$B/sandbox-elevator.elf"' not in builder:
    raise SystemExit("Native app builder must pass its selected title ID to the elevation helper")
if "std::memcmp(info.title_id, target_title_id, sizeof(target_title_id)) != 0" not in helper:
    raise SystemExit("Elevation helper must keep its exact caller-title check")

print(f"Filesystem elevation target matches package metadata: {title_id}")
