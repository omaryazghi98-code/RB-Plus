#!/usr/bin/env python3
"""Offline tests for the native RBTV protobuf decoder and signature builder."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

def main():
    project = Path(__file__).resolve().parents[1]
    curl_flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libcurl"], text=True
    ))
    with tempfile.TemporaryDirectory(prefix="rbtv-native-tests-") as temp:
        binary = Path(temp) / "test-rbtv"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-Wall", "-Wextra",
            "-Wpedantic", "-pthread",
            "-I" + str(project / "src"),
            "-I" + str(project / "third_party"),
            str(project / "tests/test_rbtv_proto.cpp"),
            str(project / "tests/test_rbtv_api.cpp"),
            str(project / "src/rbtv_proto.cpp"),
            str(project / "src/rbtv_api.cpp"),
            str(project / "src/http.cpp"),
            str(project / "src/util.cpp"),
            "-o", str(binary), *curl_flags,
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
