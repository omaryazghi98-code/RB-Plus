#!/usr/bin/env bash
# RBTV+: compile, link and sign the current sources as a native title.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain-env.sh"
ROOT=$STREMIO_ROOT
HERE="$ROOT/native"
TITLE=${1:-PPSA98273}
NAME=${2:-"RBTV+"}
[[ $TITLE =~ ^PPSA[0-9]{5}$ && -n $NAME ]] || { echo "Invalid title identity" >&2; exit 2; }
APP_FILES=${APP_FILES:-"$ROOT/app"}
SDK=$PS5_PAYLOAD_SDK
HB="$SDK/target/user/homebrew/lib"
BP=$BOILERPLATE_DIR
GL=$PS5_OPENGL_PREFIX
TOOL="$BP/build/host/ps5-native-tool"
B="$BUILD_DIR/native"
OUT=$OUT_DIR
[[ -x $TOOL && -f $BP/runtime/libc.prx ]] || { echo "Run ./build.sh deps first" >&2; exit 2; }
mkdir -p "$B/stubs" "$OUT"
python3 "$ROOT/tools/validate-app-metadata.py" "$APP_FILES/sce_sys/param.json"
python3 "$ROOT/tools/build-receipt.py" --fingerprint "$ROOT" > "$B/source-tree.sha256"

"$SDK/bin/prospero-cmake" -S "$ROOT" -B "$B/cmake" -DCMAKE_BUILD_TYPE=Release \
    -DPS5_NATIVE=ON -DPS5_OPENGL_PREFIX="$GL"
# pipefail is deliberate: a compiler error can never package an older archive.
cmake --build "$B/cmake" --parallel "$BUILD_JOBS" 2>&1 | tee "$B/compile.log"
[[ -s $B/cmake/libstremio.a ]] || { echo "The Stremio Plus archive was not built" >&2; exit 1; }

CC="$SDK/bin/prospero-clang"
"$SDK/bin/prospero-clang++" -std=c++20 -O2 -fno-exceptions -fno-rtti \
    -c "$BP/tooling/native/app_crt.cpp" -o "$B/app_crt.o"
for source in shims console_curl ps5_modules heap posix_fixes opengl_shims; do
    "$CC" -O2 -I"$SDK/target/user/homebrew/include" -c "$HERE/$source.c" -o "$B/$source.o"
done
"$CC" -shared -nostdlib -fPIC -Wl,-soname,libSceVideodec2.sprx \
    "$HERE/stubs/videodec2.c" -o "$B/stubs/libSceVideodec2.so"
"$SDK/bin/prospero-pkg-config" --static --libs sdl2 freetype2 libavformat libavcodec \
    libavutil libswresample libswscale libcurl libwebp > "$B/dependencies.txt"
mapfile -d '' -t DEPS < <(python3 - "$B/dependencies.txt" <<'PY'
import pathlib, shlex, sys
for flag in shlex.split(pathlib.Path(sys.argv[1]).read_text()):
    if flag.startswith(('-l', '-L')) and flag not in ('-lm', '-lc'):
        print(flag, end='\0')
PY
)
BUILTINS="$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -s $BUILTINS ]] || { echo "Missing clang builtins; install libclang-rt-18-dev" >&2; exit 2; }
# Payload libc includes inline mmap/mprotect syscalls. Native titles must use
# libkernel's entry points, as in the working ProsperoLight reference.
python3 "$ROOT/tools/native-startup.py" prepare-libc "$SDK/target/lib/libc.a" \
    "$B/libc-native-compat.a" --ar "$SDK/bin/prospero-ar" | tee "$B/native-libc.json"
WRAPS=()
for symbol in fcntl pthread_create pipe malloc free calloc realloc reallocf posix_memalign \
    memalign aligned_alloc valloc malloc_usable_size sceSystemServiceHideSplashScreen; do
    WRAPS+=("--wrap=$symbol")
done
"$SDK/bin/prospero-lld" -T "$HERE/ps5-app.ld" --eh-frame-hdr \
    --version-script "$BP/tooling/native/app-symbols.map" -e _start -o "$B/pie.elf" \
    "${WRAPS[@]}" -L "$SDK/target/lib" -L "$HB" -L "$GL/lib" \
    --undefined=ps5_agc_gate2_run \
    --allow-multiple-definition --defsym=__cxa_thread_atexit_impl=0 --defsym=__syscall=0 \
    "$B/app_crt.o" "$B/shims.o" "$B/console_curl.o" "$B/ps5_modules.o" \
    "$B/heap.o" "$B/posix_fixes.o" "$B/opengl_shims.o" \
    --whole-archive "$B/cmake/libstremio.a" --no-whole-archive \
    --start-group "$GL/lib/libPS5OpenGL.a" "$HB/libdht.a" "$HB/libminiupnpc.a" \
    "$HB/libfribidi.a" "${DEPS[@]}" "$SDK/target/lib/libc++.a" \
    "$SDK/target/lib/libc++abi.a" "$SDK/target/lib/libunwind.a" "$B/libc-native-compat.a" \
    "$BUILTINS" --end-group \
    --as-needed "$GL/lib/libSceAgc.so" "$GL/lib/libSceAgcDriver.so" "$SDK"/target/lib/*.so "$B"/stubs/*.so
python3 "$ROOT/tools/native-startup.py" audit "$B/pie.elf" | tee "$B/native-startup-audit.json"
python3 "$HERE/check_imports.py" "$B/pie.elf" "$GL/lib" "$SDK/target/lib" "$B/stubs" \
    | tee "$B/imports.log"
"$TOOL" link --in "$B/pie.elf" --out "$B/eboot.elf" --stub-dir "$SDK/target/lib" \
    --stub "$GL/lib/libSceAgc.so" --stub "$GL/lib/libSceAgcDriver.so" \
    --stub "$B/stubs/libSceVideodec2.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
python3 "$ROOT/tools/native-startup.py" configure-process "$B/eboot.elf" \
    | tee "$B/native-process.json"

# Build the exact-title Lapy helper with SDK v0.43. This pinned revision
# includes the upstream credential-layout fix for firmware 13.60; its ELF
# protocol remains compatible with native/filesystem/elevation.cpp.
python3 "$ROOT/tools/build-lapy-helper.py" "$TITLE" "$B/lapy-helper"
make -C "$HERE/download_writer/helper" PS5_PAYLOAD_SDK="$SDK" OUTPUT="$B/download-writer.elf"
python3 "$HERE/filesystem/validate-helper.py" "$B/download-writer.elf"

# Assemble separately; the previous output survives any compile/sign/asset error.
APP=$(mktemp -d "$OUT/.$TITLE.assemble.XXXXXX")
trap 'rm -rf -- "$APP"' EXIT
mkdir -p "$APP/sce_sys" "$APP/sce_module"
"$TOOL" self --sign --in "$B/eboot.elf" --out "$APP/eboot.bin" --magic 0x1D3D154F
(cd "$BP/runtime" && sha256sum --check --strict libc.prx.sha256)
cp "$BP/runtime/libc.prx" "$APP/sce_module/libc.prx"
cp "$B/lapy-helper/lapy.elf" "$APP/lapy.elf"
cp "$B/lapy-helper/lapy-manifest.json" "$APP/lapy-manifest.json"
cp "$B/download-writer.elf" "$APP/download-writer.elf"
python3 - "$APP_FILES/sce_sys/param.json" "$APP/sce_sys/param.json" "$TITLE" "$NAME" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
title, name = sys.argv[3:]
if data['titleId'] != title:
    data['titleId'] = title
    data['conceptId'] = title[4:]
    data['contentId'] = f'UP9000-{title}_00-RBTVPLUS00000001'
localized = data['localizedParameters']
localized.setdefault(localized.get('defaultLanguage', 'en-US'), {})['titleName'] = name
localized.setdefault('en-US', {})['titleName'] = name
pathlib.Path(sys.argv[2]).write_text(json.dumps(data, indent=2) + '\n')
PY
python3 "$ROOT/tools/validate-app-metadata.py" "$APP/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ ! -f $APP_FILES/sce_sys/$asset ]] || cp "$APP_FILES/sce_sys/$asset" "$APP/sce_sys/"
done
# Replace inherited Stremio launcher artwork with an RBTV+ wordmark icon.
python3 "$ROOT/tools/make-rbtv-icon.py" "$APP/sce_sys/icon0.png"
bash "$BP/tools/validate-assets.sh" "$APP/sce_sys"
for directory in assets fonts hui licenses; do
    [[ ! -d $APP_FILES/$directory ]] || cp -a "$APP_FILES/$directory" "$APP/"
done
# Preserve the helper's upstream MIT license in the installed app.
mkdir -p "$APP/licenses"
cp "$B/lapy-helper/Lapy-MIT.txt" "$APP/licenses/Lapy-MIT.txt"
cp "$APP_FILES/ca-bundle.crt" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" "$APP/"
# The PS5 cannot enumerate app0 sound folders; preserve an explicit index.
python3 - "$APP/hui/audio" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
if root.is_dir():
    for folder in [root, *root.rglob('*')]:
        if folder.is_dir():
            names = sorted(p.name for p in folder.iterdir() if p.is_file() and p.name != 'index.txt')
            if names:
                (folder / 'index.txt').write_text('\n'.join(names) + '\n')
PY
chmod 0755 "$APP/eboot.bin" "$APP/sce_module/libc.prx"
python3 "$ROOT/tools/build-receipt.py" "$ROOT" "$APP" "$B/pie.elf" "$B/source-tree.sha256"
rm -rf -- "$OUT/$TITLE"
mv -- "$APP" "$OUT/$TITLE"
trap - EXIT
rm -f -- "$OUT/$TITLE.zip"
(cd "$OUT" && zip -qr "$TITLE.zip" "$TITLE")
echo "Built $OUT/$TITLE from the current sources."
