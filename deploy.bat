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
bin2fw.exe moduleBox.bin
if errorlevel 1 (
    echo ОШИБКА: bin2fw.exe завершился с ошибкой
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