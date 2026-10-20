@echo off
rem MIT License
rem
rem MultiPing
rem Copyright (c) 2026 Max Vilimpoc
rem
rem Permission is hereby granted, free of charge, to any person obtaining a copy
rem of this software and associated documentation files (the "Software"), to deal
rem in the Software without restriction, including without limitation the rights
rem to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
rem copies of the Software, and to permit persons to whom the Software is
rem furnished to do so, subject to the following conditions:
rem
rem The above copyright notice and this permission notice shall be included in all
rem copies or substantial portions of the Software.
rem
rem THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
rem IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
rem FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
rem AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
rem LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
rem OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
rem SOFTWARE.

setlocal
rem Builds MultiPingXP.exe with MinGW-w64 GCC from MSYS2's mingw32 environment: 32-bit, linked
rem against msvcrt.dll, so it runs on Windows XP SP3 and later, 32- and 64-bit. Static libgcc and
rem libstdc++, so no MinGW runtime DLLs are needed beside it.
rem
rem MSYS2 is looked for in this order:
rem   %MSYS2_ROOT%            set it to override
rem   %LOCALAPPDATA%\msys64   the per-user install made by dotfiles\setup-windows.bat (Msys2 step)
rem   C:\msys64               the machine-wide install, e.g. on GitHub's windows-latest runner
rem The mingw32 GCC package is mingw-w64-i686-gcc (pacman -S mingw-w64-i686-gcc).
if not defined MSYS2_ROOT if exist "%LOCALAPPDATA%\msys64\mingw32\bin\gcc.exe" set "MSYS2_ROOT=%LOCALAPPDATA%\msys64"
if not defined MSYS2_ROOT if exist "C:\msys64\mingw32\bin\gcc.exe" set "MSYS2_ROOT=C:\msys64"
if not defined MSYS2_ROOT (
    echo MSYS2 with mingw-w64-i686-gcc not found: set MSYS2_ROOT, or install it per-user with
    echo dotfiles\setup-windows.bat -Only Msys2, or machine-wide from https://www.msys2.org/
    exit /b 1
)
set "BIN=%MSYS2_ROOT%\mingw32\bin"
if not exist "%BIN%\gcc.exe" ( echo mingw32 GCC not found in "%BIN%" ^(pacman -S mingw-w64-i686-gcc^) & exit /b 1 )

rem windres finds its preprocessor (gcc) through the PATH.
set "PATH=%BIN%;%PATH%"

set GIT_REV=unknown
for /f "usebackq delims=" %%r in (`git rev-parse --short HEAD 2^>nul`) do set GIT_REV=%%r
rem MULTIPING_GIT_REVISION when the caller knows it: systems-build.py, building a tree synced
rem without its .git.
if defined MULTIPING_GIT_REVISION set "GIT_REV=%MULTIPING_GIT_REVISION%"

"%BIN%\windres.exe" -O coff -o MultiPingXP.res MultiPing.rc
if errorlevel 1 exit /b 1
rem -Wno-missing-field-initializers: `{ sizeof x }` is the idiomatic Win32 zero-init of a sized struct.
rem -Wno-cast-function-type: GetProcAddress results are cast to the real signatures on purpose.
"%BIN%\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type -municode -mwindows ^
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0502 -DWINVER=0x0502 -DGIT_REV=\"%GIT_REV%\" ^
    -o MultiPingXP.exe MultiPing.cpp MultiPingXP.res ^
    -static -static-libgcc -static-libstdc++ -s ^
    -Wl,--subsystem,windows:5.01 -Wl,--major-os-version,5 -Wl,--minor-os-version,1 ^
    -luser32 -lgdi32 -liphlpapi -lws2_32
if errorlevel 1 exit /b 1
del /q MultiPingXP.res 2>nul
echo Built MultiPingXP.exe with %BIN%\g++.exe
