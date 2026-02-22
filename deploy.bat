@echo off
chcp 65001 >nul
cd /d "%~dp0"

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