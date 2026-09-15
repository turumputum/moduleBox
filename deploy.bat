@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo Увеличиваю версию на 0.01 в stateConfig.h...
set "HEADER=components\stateConfig\include\stateConfig.h"
for /f "tokens=3 delims=	 " %%A in ('findstr /C:"#define VERSION" "%HEADER%"') do set "OLD_VER=%%~A"
for /f "tokens=1,2 delims=." %%A in ("%OLD_VER%") do (
    set /a MINOR=%%B+1
    set "MAJOR=%%A"
)
setlocal enabledelayedexpansion
set "NEW_VER=%MAJOR%.!MINOR!"
echo Версия: %OLD_VER% -^> !NEW_VER!
powershell -Command "(Get-Content '%HEADER%') -replace '#define VERSION\s+\"%OLD_VER%\"', '#define VERSION \"!NEW_VER!\"' | Set-Content '%HEADER%'"
endlocal

echo Удаляю старый moduleBox.bin из bootldsd...
del /f "bootldsd\moduleBox.bin" 2>nul

echo Копирую новый moduleBox.bin из build...
copy /y "build\moduleBox.bin" "bootldsd\moduleBox.bin"
if errorlevel 1 (
    echo ОШИБКА: не удалось скопировать moduleBox.bin
    pause
    exit /b 1
)

echo Запускаю bin2fw.exe...
cd bootldsd
"%~dp0bootldsd\bin2fw.exe" moduleBox.bin
if errorlevel 1 (
    echo ОШИБКА: bin2fw.exe завершился с ошибкой
    pause
    exit /b 1
)
cd ..

echo Копирую bootloader и таблицу разделов из build...
copy /y "build\bootloader\bootloader.bin" "bootldsd\bootloader.bin" >nul
copy /y "build\partition_table\partition-table.bin" "bootldsd\partition-table.bin" >nul
if errorlevel 1 (
    echo ОШИБКА: нет build\bootloader\bootloader.bin или build\partition_table\partition-table.bin
    pause
    exit /b 1
)

echo Собираю единый образ для производства moduleBox_full.bin (шьётся в 0x0)...
set "PY=C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"
cd bootldsd
"%PY%" -m esptool --chip esp32s3 merge_bin --flash_mode dio --flash_size 8MB --flash_freq 80m -o moduleBox_full.bin 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 bootldsd.bin 0x50000 moduleBox.bin
if errorlevel 1 (
    echo ОШИБКА: esptool merge_bin завершился с ошибкой
    cd ..
    pause
    exit /b 1
)
cd ..

echo Git add...
git add .

set /p COMMIT_MSG="Введите комментарий коммита: "
if "%COMMIT_MSG%"=="" set COMMIT_MSG=update firmware

echo Git commit...
git commit -m "%COMMIT_MSG%"

echo Git push...
git push

echo Готово!
pause