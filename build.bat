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
rem Builds MultiPing.exe with MSVC, statically linked CRT (/MT), no external DLLs beyond Windows system ones.
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
rem Git short revision shown under the copyright; "unknown" when not in a repo or git is missing.
set GIT_REV=unknown
for /f "usebackq delims=" %%r in (`git rev-parse --short HEAD 2^>nul`) do set GIT_REV=%%r
rc /nologo MultiPing.rc
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /GL /EHsc /DUNICODE /D_UNICODE /DGIT_REV=\"%GIT_REV%\" /Fe:MultiPing.exe MultiPing.cpp MultiPing.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib iphlpapi.lib ws2_32.lib
if errorlevel 1 exit /b 1
del /q MultiPing.obj MultiPing.res 2>nul
echo Built MultiPing.exe
