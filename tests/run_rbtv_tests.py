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
    common = [
        os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-Wall", "-Wextra",
        "-Wpedantic", "-pthread",
        "-I" + str(project / "src"),
        "-I" + str(project / "third_party"),
    ]
    with tempfile.TemporaryDirectory(prefix="rbtv-native-tests-") as temp:
        temp = Path(temp)
        proto_binary = temp / "test-rbtv-proto"
        proto_command = common + [
            str(project / "tests/test_rbtv_proto.cpp"),
            str(project / "src/rbtv_proto.cpp"),
            "-o", str(proto_binary),
        ]
        subprocess.run(proto_command, check=True)
        subprocess.run([str(proto_binary)], check=True)

        api_binary = temp / "test-rbtv-api"
        api_command = common + [
            str(project / "tests/test_rbtv_api.cpp"),
            str(project / "src/rbtv_api.cpp"),
            str(project / "src/rbtv_proto.cpp"),
            str(project / "src/http.cpp"),
            str(project / "src/util.cpp"),
            "-o", str(api_binary), *curl_flags,
        ]
        subprocess.run(api_command, check=True)
        subprocess.run([str(api_binary)], check=True)

if __name__ == "__main__":
    main()
