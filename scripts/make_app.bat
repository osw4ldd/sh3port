@echo off
rem Package the ps2native tools as Windows executables in the project root:
rem   ps2native.exe  - graphical front end
rem   ps2recomp.exe  - command-line recompiler
setlocal
set "ROOT=%~dp0.."
set "WORK=%ROOT%\build\pyinstaller"
python -m PyInstaller --noconfirm --onefile --windowed --name ps2native ^
  --paths "%ROOT%\recompiler" --distpath "%ROOT%" --workpath "%WORK%" --specpath "%WORK%" ^
  "%ROOT%\app\ps2native_gui.py" || exit /b 1
python -m PyInstaller --noconfirm --onefile --console --name ps2recomp ^
  --paths "%ROOT%\recompiler" --distpath "%ROOT%" --workpath "%WORK%" --specpath "%WORK%" ^
  "%ROOT%\app\ps2recomp_cli.py" || exit /b 1
