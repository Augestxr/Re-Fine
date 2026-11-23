@echo off
rem Lanzador de Re:Fine. Uso: refine.cmd [fps]  (sin fps, manda fps_target de refine.ini).
rem Todo lo que escribe el juego va a %LOCALAPPDATA%\ReFine; junto al exe no se escribe nada.
setlocal
chcp 65001 >nul
set "BASE=%LOCALAPPDATA%\ReFine"
set "VIEJA=%LOCALAPPDATA%\openF"
set "RUTA=%BASE%\ruta_juego.txt"
set "HERE=%~dp0"
if not "%~1"=="" set "REFINE_FPS_TARGET=%~1"

rem La 0.1.0-beta se llamaba openF: partidas, ajustes y cache pasan una vez a la carpeta nueva.
rem Si no se puede mover, se para aqui: seguir dejaria la partida vieja sin usar.
if exist "%BASE%" goto carpeta_lista
if not exist "%VIEJA%" goto carpeta_lista
move "%VIEJA%" "%BASE%" >nul
if errorlevel 1 (
    powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; [void][Windows.Forms.MessageBox]::Show('No se pudo mover %VIEJA% a %BASE%. Cierra lo que use esa carpeta y vuelve a abrir el juego.', 'Re:Fine')"
    exit /b 1
)
if exist "%BASE%\openf.ini" ren "%BASE%\openf.ini" refine.ini
for %%L in (out err anterior.out anterior.err) do if exist "%BASE%\logs\openf.%%L.log" ren "%BASE%\logs\openf.%%L.log" "refine.%%L.log"
:carpeta_lista

for %%D in (hdd0 hdd1 run logs) do if not exist "%BASE%\%%D" mkdir "%BASE%\%%D"
if not exist "%BASE%\refine.ini" copy "%HERE%refine.ini" "%BASE%\refine.ini" >nul

set "GAME="
if exist "%RUTA%" for /f "usebackq delims=" %%L in ("%RUTA%") do set "GAME=%%L"
rem El instalador nuevo deja el EBOOT.elf descifrado junto a los datos (no escribe en la carpeta del juego).
set "ELF=%BASE%\EBOOT.elf"
if not exist "%ELF%" set "ELF=%GAME%\PS3_GAME\USRDIR\EBOOT.elf"
if defined GAME if exist "%ELF%" goto lanzar

powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%elegir_carpeta.ps1" "%RUTA%"
if errorlevel 1 exit /b 1
for /f "usebackq delims=" %%L in ("%RUTA%") do set "GAME=%%L"

:lanzar
if not exist "%BASE%\EBOOT.elf" set "ELF=%GAME%\PS3_GAME\USRDIR\EBOOT.elf"
set "PS3_VFS_ROOT=%GAME%"
set "PS3_HDD0_ROOT=%BASE%\hdd0"
set "PS3_HDD1_ROOT=%BASE%\hdd1"
rem dev_flash (firmware) no se reparte ni se crea: una corrida con la ruta inexistente (titulo + cancion) no la pidio (docs/instalador.md 8).
set "PS3_DEV_FLASH=%BASE%\dev_flash"
set "PS3_HLE_UNRESOLVED=fail"
set "PS3_TITLE=Re:Fine"

cd /d "%BASE%\run"
if exist "%BASE%\logs\refine.out.log" move /y "%BASE%\logs\refine.out.log" "%BASE%\logs\refine.anterior.out.log" >nul
if exist "%BASE%\logs\refine.err.log" move /y "%BASE%\logs\refine.err.log" "%BASE%\logs\refine.anterior.err.log" >nul
"%HERE%ReFine.exe" "%ELF%" >"%BASE%\logs\refine.out.log" 2>"%BASE%\logs\refine.err.log"
