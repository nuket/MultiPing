# MultiPing

A small Win32 tool that pings a list of hosts once a second and shows, per host, the current
round-trip time, session min / median / avg / max, and a bar graph of the last 60 seconds.

![MultiPing pinging nine hosts, two of them timing out](screenshot.png)

## Which executable?

Both are built from the same source and behave identically; every version-dependent feature is resolved
at runtime. The XP build is the more portable of the two.

| | MultiPing.exe | MultiPingXP.exe |
|---|---|---|
| Architecture | x64 | x86 |
| Runs on | 64-bit Windows 7 SP1 and later | 32-bit and 64-bit Windows XP SP3 and later |
| Compiler and CRT | MSVC 14.44, current UCRT | MSVC 14.16, UCRT from 2015 |
| Size | about 296 KB | about 225 KB |

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

Run `build.bat` from a normal command prompt (it calls `vcvars64.bat` from VS 2022 Community itself).

Output is `MultiPing.exe` (64-bit), statically linked against the CRT (`/MT`); its only imports are `user32`, 
`gdi32`, `kernel32`, `iphlpapi` and `ws2_32`. It requires Windows 7 SP1 or later.

The build script embeds the current git short revision, shown under the copyright at the top right.

The application icon (`MultiPing.ico`) is generated from the Segoe UI Emoji waving hand by
`tools/make_icon.py` (needs Python with Pillow) and embedded via `MultiPing.rc`.

## Windows XP build

`build_xp.bat` produces `MultiPingXP.exe`, a 32-bit build that runs on Windows XP SP3 and later. It needs
the "C++ Windows XP Support for VS 2017 (v141) tools" component of Visual Studio 2022, which brings the
MSVC 14.16 toolset, the Windows 7.1A SDK and the UCRT from SDK 10.0.10240.0 (the last one that runs on XP).

On XP, the app uses Tahoma instead of Segoe UI, shows the app icon beside the title instead of the emoji (no emoji
font), and draws the delete button without antialiasing (no Direct2D). Everything else works the same. 
