@echo off
setlocal
rem Builds MultiPing.exe with MSVC, statically linked CRT (/MT), no external DLLs beyond Windows system ones.
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
cl /nologo /W4 /O2 /MT /GL /EHsc /DUNICODE /D_UNICODE /Fe:MultiPing.exe MultiPing.cpp ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib iphlpapi.lib ws2_32.lib
if errorlevel 1 exit /b 1
del /q MultiPing.obj 2>nul
echo Built MultiPing.exe
