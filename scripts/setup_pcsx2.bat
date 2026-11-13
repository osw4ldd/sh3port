@echo off
rem Recreate third_party\pcsx2: clone PCSX2 at the tested commit, apply the
rem ps2native host patch and fetch PCSX2's prebuilt Windows dependencies.
rem Needs: git, curl, 7-Zip.
setlocal
set "ROOT=%~dp0.."
set /p COMMIT=<"%ROOT%\patches\PCSX2_COMMIT"
if not exist "%ROOT%\third_party" mkdir "%ROOT%\third_party"
cd /d "%ROOT%\third_party" || exit /b 1
if not exist pcsx2 git clone https://github.com/PCSX2/pcsx2.git || exit /b 1
cd pcsx2 || exit /b 1
git fetch --depth 1 origin %COMMIT% && git checkout %COMMIT% || exit /b 1
git apply --check "%ROOT%\patches\pcsx2-native-host.patch" && git apply "%ROOT%\patches\pcsx2-native-host.patch"
if not exist deps (
  curl -L -o deps.7z https://github.com/PCSX2/pcsx2-windows-dependencies/releases/download/latest-windows-dependencies/pcsx2-windows-dependencies.7z || exit /b 1
  "C:\Program Files\7-Zip\7z.exe" x -y deps.7z >nul || exit /b 1
  del deps.7z
)
echo PCSX2 ready. Configure with scripts\configure_pcsx2.bat (ps2recomp does this automatically).
