@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Users\Tristan\Documents\GitHub\pcsx2
msbuild PCSX2_qt.sln /m /v:m /p:Configuration=Release /p:Platform=x64
