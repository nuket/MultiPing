# MultiPing

[![Build](https://github.com/nuket/MultiPing/actions/workflows/build.yml/badge.svg)](https://github.com/nuket/MultiPing/actions/workflows/build.yml)

A small Win32 tool that pings a list of hosts once a second and shows, per host, the current
round-trip time, session min / median / avg / max, and a bar graph of the last 60 seconds.

![MultiPing pinging eight LAN hosts and one internet host, one of them timing out](screenshot.png)

## Which executable?

Both are built from the same source and behave identically; every version-dependent feature is resolved
at runtime. The XP build is the more portable of the two.

| | MultiPing.exe | MultiPingXP.exe |
|---|---|---|
| Architecture | x64 | x86 |
| Runs on | 64-bit Windows 7 SP1 and later | 32-bit and 64-bit Windows XP SP3 and later |
| Compiler and CRT | MSVC 14.44, current UCRT | MinGW-w64 GCC (MSYS2 mingw32), msvcrt.dll |
| Size | about 296 KB | about 477 KB |

## Usage

- Type a host name or IPv4/IPv6 address into a row and press Enter (or click elsewhere) to start pinging it.
- The list always ends with an empty row for the next address. Filling it in adds another empty row below.
- Enter moves to the next row; Tab / Shift+Tab move between rows.
- Escape reverts an edited address box. The red circle at the right deletes the row.
- Stats (min / median / avg / max) cover the whole session for that address; the graph shows the last 60 pings,
  one per second, with timeouts drawn in red.
- The window opens sized for 8 rows; a scrollbar appears only when there are more.
- Addresses are saved to `%APPDATA%\MultiPing\addresses.txt` on every change and on exit.
- The window size and position are saved to `window.txt` in the same folder and restored on the next
  start, clamped to the current screen if the display layout has changed.

## Modern Build

`build.bat` produces `MultiPing.exe`, a 64-bit build that requires Windows 7 SP1 or later,
statically linked against the C Runtime; its only imports are `user32`, `gdi32`, `kernel32`, `iphlpapi` and `ws2_32`.

## Windows XP build

`build_mingw.bat` produces `MultiPingXP.exe`, a 32-bit build that runs on Windows XP SP3 and later, using
MinGW-w64 GCC from MSYS2's `mingw32` environment (i686, linked against `msvcrt.dll`, static libgcc and
libstdc++).
