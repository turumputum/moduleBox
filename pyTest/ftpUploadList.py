#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import ftplib
import sys
import os
from typing import List

def upload_file_to_ftp(host: str, username: str, password: str, local_file_path: str, remote_filename: str = None) -> bool:
    """
    Загружает файл на FTP-сервер
    
    Args:
        host: адрес FTP-сервера
        username: имя пользователя
        password: пароль
        local_file_path: путь к локальному файлу
        remote_filename: имя файла на сервере (если None, используется имя локального файла)
    
    Returns:
        bool: True если успешно, False если ошибка
    """
    try:
        # Подключаемся к FTP-серверу
        print(f"Подключаемся к {host}...")
        with ftplib.FTP(host) as ftp:
            ftp.login(username, password)
            print(f"Успешно подключились к {host}")
            
            # Определяем имя файла на сервере
            if remote_filename is None:
                remote_filename = os.path.basename(local_file_path)
            
            # Переходим в корневой каталог (на всякий случай)
            ftp.cwd("/")
            
            # Загружаем файл
            print(f"Загружаем файл {local_file_path} как {remote_filename}...")
            with open(local_file_path, 'rb') as file:
                ftp.storbinary(f"STOR {remote_filename}", file)
            
            print(f"Файл успешно загружен на {host}")
            return True
            
    except ftplib.error_perm as e:
        print(f"Ошибка доступа на {host}: {e}")
        return False
    except ftplib.error_temp as e:
        print(f"Временная ошибка на {host}: {e}")
        return False
    except Exception as e:
        print(f"Ошибка при работе с {host}: {e}")
        return False

def main():
    # Проверяем аргументы командной строки
    if len(sys.argv) != 2:
        print("Использование: python ftp_upload.py <путь_к_файлу>")
        print("Пример: python ftp_upload.py myfile.txt")
        sys.exit(1)
    
    local_file_path = sys.argv[1]
    
    # Проверяем существование файла
    if not os.path.isfile(local_file_path):
        print(f"Ошибка: файл '{local_file_path}' не существует")
        sys.exit(1)
    
    # Список контроллеров (можно вынести в отдельный файл или сделать конфигурируемым)
    controllers = [
        {"host": "mbGlassPanBt.local", "username": "user", "password": "pass"},
        {"host": "mbGlassPanTouch.local", "username": "user", "password": "pass"},
        {"host": "mbRetractor.local", "username": "user", "password": "pass"},
        {"host": "mbRetrophone.local", "username": "user", "password": "pass"},
        # Добавьте другие контроллеры здесь
    ]
    
    print(f"Начинаем загрузку файла '{local_file_path}' на {len(controllers)} контроллеров")
    print("-" * 50)
    
    success_count = 0
    failed_count = 0
    
    # Проходим по всем контроллерам
    for i, controller in enumerate(controllers, 1):
        print(f"[{i}/{len(controllers)}] Обрабатываем {controller['host']}")
        
        if upload_file_to_ftp(
            controller["host"],
            controller["username"],
            controller["password"],
            local_file_path
        ):
            success_count += 1
        else:
            failed_count += 1
        
        print()  # Пустая строка для разделения
    
    # Выводим итоги
    print("=" * 50)
    print("Загрузка завершена!")
    print(f"Успешно: {success_count}")
    print(f"Неудачно: {failed_count}")

if __name__ == "__main__":
    main()