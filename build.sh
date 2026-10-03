#!/bin/sh
# Cross build from Linux with an i386-win32 TCC 0.9.27 and Wine (for the
# resource step). Set TCC to your cross compiler command if it differs.
set -e
cd "$(dirname "$0")"
TCC=${TCC:-i386-win32-tcc}
mkdir -p build
$TCC -mwindows -Wall -o build/xpcode.exe src/editor.c src/explorer.c src/main.c \
    src/palette.c src/syntax.c src/terminal.c -luser32 -lgdi32 -lkernel32
$TCC -o build/rsrc.exe tools/rsrc.c
wine build/rsrc.exe build/xpcode.exe res/xpcode.ico res/xpcode.manifest
ls -l build/xpcode.exe
