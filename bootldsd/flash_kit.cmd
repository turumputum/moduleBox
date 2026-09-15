@echo off
setlocal
cd /d "%~dp0"

rem Full device flash with the loader kit (ASCII only: cmd.exe breaks on UTF-8 text in .cmd):
rem   0x00000  bootloader.bin       custom bootloader (main project, bootloader_components/main)
rem   0x08000  partition-table.bin  partition table (partitions.csv in repo root)
rem   0x10000  bootldsd.bin         update loader (bootldsd/loader)
rem   0x50000  moduleBox.bin        firmware
rem Usage: flash_kit.cmd COM9 [baud]
rem Same result as "idf.py flash" from the repo root.

if "%~1"=="" (
    echo Usage: %~nx0 COMx [baud]
    exit /b 1
)
set "PORT=%~1"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=921600"

set "PY=C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"

for %%F in (bootloader.bin partition-table.bin bootldsd.bin moduleBox.bin) do (
    if not exist "%%F" (
        echo Missing file: %%F
        exit /b 1
    )
)

"%PY%" -m esptool --chip esp32s3 -p %PORT% -b %BAUD% --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 bootldsd.bin 0x50000 moduleBox.bin
exit /b %ERRORLEVEL%
