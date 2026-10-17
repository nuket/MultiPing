@echo off
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
