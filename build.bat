@echo off
rem Builds XP Code with Tiny C Compiler (0.9.27).
rem Put tcc.exe on PATH, or unpack TCC into a "tcc" folder next to this file.
setlocal
cd /d "%~dp0"
set TCC=tcc
if exist "%~dp0tcc\tcc.exe" set TCC="%~dp0tcc\tcc.exe"
if not exist build mkdir build
echo Compiling xpcode.exe...
%TCC% -mwindows -Wall -o build\xpcode.exe src\editor.c src\explorer.c src\main.c src\palette.c src\syntax.c src\terminal.c -luser32 -lgdi32 -lkernel32
if errorlevel 1 goto fail
echo Compiling rsrc.exe...
%TCC% -o build\rsrc.exe tools\rsrc.c
if errorlevel 1 goto fail
build\rsrc.exe build\xpcode.exe res\xpcode.ico res\xpcode.manifest
if errorlevel 1 goto fail
for %%F in (build\xpcode.exe) do echo Done: build\xpcode.exe, %%~zF bytes
exit /b 0
:fail
echo Build failed.
exit /b 1
