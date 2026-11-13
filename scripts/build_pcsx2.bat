@echo off
rem Build a target of the PCSX2-based host build.  Usage: scripts\build_pcsx2.bat <target>
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0.."
cmake --build "%ROOT%\build\pcsx2" --target %1
