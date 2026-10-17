@echo off
rem MIT License
rem
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
rem Builds MultiPingXP.exe: 32-bit, Windows XP compatible.
rem Uses the v141_xp toolset (MSVC 14.16) with the Windows 7.1A SDK headers/libs and a static CRT.
set VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community
set SDK71=C:\Program Files (x86)\Microsoft SDKs\Windows\v7.1A

rem The UCRT from SDK 10.0.10240.0 is the last one that runs on XP; newer ones import
rem InitializeCriticalSectionEx / FlsAlloc, which XP's kernel32 lacks. The XP support component
rem installs only that SDK's UCRT part, so the paths are set by hand rather than via vcvarsall.
set UCRT=C:\Program Files (x86)\Windows Kits\10
set UCRTVER=10.0.10240.0

call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 -vcvars_ver=14.16 >nul
if errorlevel 1 ( echo vcvarsall failed - is the v141 toolset installed? & exit /b 1 )
if not exist "%SDK71%\Include\windows.h" ( echo Windows 7.1A SDK not found at "%SDK71%" & exit /b 1 )
if not exist "%UCRT%\Lib\%UCRTVER%\ucrt\x86\libucrt.lib" ( echo UCRT %UCRTVER% not found & exit /b 1 )

set INCLUDE=%VCToolsInstallDir%include;%SDK71%\Include;%UCRT%\Include\%UCRTVER%\ucrt
set LIB=%VCToolsInstallDir%lib\x86;%SDK71%\Lib;%UCRT%\Lib\%UCRTVER%\ucrt\x86
set PATH=%SDK71%\Bin;%PATH%

set GIT_REV=unknown
for /f "usebackq delims=" %%r in (`git rev-parse --short HEAD 2^>nul`) do set GIT_REV=%%r

rc /nologo /fo MultiPingXP.res MultiPing.rc
if errorlevel 1 exit /b 1
cl /nologo /W4 /wd4458 /O2 /MT /EHsc /Zc:threadSafeInit- /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0502 /D_USING_V110_SDK71_ ^
   /DGIT_REV=\"%GIT_REV%\" /Fo:MultiPingXP.obj /Fe:MultiPingXP.exe MultiPing.cpp MultiPingXP.res ^
   /link /SUBSYSTEM:WINDOWS,5.01 /MANIFEST:EMBED /OPT:REF /OPT:ICF user32.lib gdi32.lib iphlpapi.lib ws2_32.lib
if errorlevel 1 exit /b 1
del /q MultiPingXP.obj MultiPingXP.res 2>nul
echo Built MultiPingXP.exe
