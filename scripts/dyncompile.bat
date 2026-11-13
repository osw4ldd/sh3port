@echo off
rem Recompile guest code captured at run time into a DLL (called by the
rem runtime, runtime/src/core/dyn.c).
rem   dyncompile.bat <blob.bin> <base> <out.dll> <exe import .lib> <entry,entry,...>
setlocal
set "ROOT=%~dp0.."
set "BLOB=%~1"
set "BASE=%~2"
set "OUT=%~3"
set "EXELIB=%~4"
set "ENTRIES=%~5"
set "SRC=%~dpn3.c"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem Run from recompiler\ so the ps2recomp package wins over ps2recomp.py in the root.
cd /d "%ROOT%\recompiler" || exit /b 1
python -m ps2recomp.dyn "%BLOB%" --base %BASE% --entries %ENTRIES% -o "%SRC%" || exit /b 1
cl /nologo /O2 /LD /MT /W1 /wd4146 /wd4244 /DPS2_DYN_MODULE /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\runtime\include" "%SRC%" /Fo"%~dpn3.obj" /Fe"%OUT%" /link "%EXELIB%" || exit /b 1
del "%~dpn3.obj" "%~dpn3.exp" "%~dpn3.lib" 2>nul
exit /b 0
