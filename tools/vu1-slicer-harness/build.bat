@echo off
rem Builds the REAL pcsx2\GS\Remix\RemixVU1Slice.cpp into slice.exe. See harness.cpp.
setlocal
set ROOT=%~dp0..\..
if not defined VCINSTALLDIR call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /DFMT_HEADER_ONLY /I shim /I "%ROOT%\pcsx2" /I "%ROOT%\3rdparty\fmt\include" harness.cpp "%ROOT%\pcsx2\GS\Remix\RemixVU1Slice.cpp" /Fe:slice.exe /Fo:obj\
