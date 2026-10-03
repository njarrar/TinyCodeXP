#!/bin/sh
# Cross build from Linux or macOS with an i386-win32 TCC 0.9.27.
# Set TCC to your cross compiler and CC to your host C compiler if they differ.
set -e
cd "$(dirname "$0")"
TCC=${TCC:-i386-win32-tcc}
mkdir -p build
$TCC -mwindows -Wall -o build/xpcode.exe src/editor.c src/explorer.c src/main.c \
    src/palette.c src/syntax.c src/terminal.c -luser32 -lgdi32 -lkernel32
# rsrc.c is plain C with no Win32 calls, so it runs on the build machine
${CC:-cc} -O2 -o build/rsrc tools/rsrc.c
./build/rsrc build/xpcode.exe res/xpcode.ico res/xpcode.manifest
ls -l build/xpcode.exe
