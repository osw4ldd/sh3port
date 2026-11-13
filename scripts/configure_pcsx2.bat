@echo off
rem Configure the PCSX2-based host build (MSVC + Ninja, no Qt UI).
rem Usage: scripts\configure_pcsx2.bat [extra cmake args]
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0.."
set "SRC=%ROOT%\third_party\pcsx2"
set "BUILD=%ROOT%\build\pcsx2"
cmake -S "%SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_PREFIX_PATH="%SRC%\deps" ^
  -DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=ON -DENABLE_TESTS=OFF %*
