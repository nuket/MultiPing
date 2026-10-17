@echo off
setlocal
rem Builds MultiPing64.exe with MSVC, statically linked CRT (/MT), no external DLLs beyond Windows system ones.
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
rem Git short revision shown under the copyright; "unknown" when not in a repo or git is missing.
set GIT_REV=unknown
for /f "usebackq delims=" %%r in (`git rev-parse --short HEAD 2^>nul`) do set GIT_REV=%%r
rc /nologo MultiPing.rc
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /GL /EHsc /DUNICODE /D_UNICODE /DGIT_REV=\"%GIT_REV%\" /Fe:MultiPing64.exe MultiPing.cpp MultiPing.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib iphlpapi.lib ws2_32.lib
if errorlevel 1 exit /b 1
del /q MultiPing.obj MultiPing.res 2>nul
echo Built MultiPing64.exe
