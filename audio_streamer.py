#!/usr/bin/env python3
"""
Кроссплатформенное приложение для вещания звукового потока в сеть
Поддерживает Windows, Linux и macOS
"""

import sys
import threading
import subprocess
import platform
import tkinter as tk
from tkinter import ttk, messagebox
import json
import os

try:
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gst, GLib
    HAS_GSTREAMER = True
except ImportError:
    HAS_GSTREAMER = False

class AudioStreamer:
    def __init__(self):
        self.pipeline = None
        self.main_loop = None
        self.is_streaming = False
        self.current_process = None
        
        if HAS_GSTREAMER:
            Gst.init(None)
    
    def get_audio_devices(self):
        """Получить список доступных аудио устройств"""
        devices = []
        system = platform.system()
        
        if system == "Windows":
            devices = self._get_windows_devices()
        elif system == "Linux":
            devices = self._get_linux_devices()
        elif system == "Darwin":  # macOS
            devices = self._get_macos_devices()
        
        return devices
    
    def _get_windows_devices(self):
        """Получить Windows WASAPI устройства"""
        devices = []
        try:
            # Используем PowerShell для получения списка устройств
            cmd = [
                'powershell', '-Command',
                'Get-AudioDevice | Format-Table -Property Name, ID -AutoSize'
            ]
            result = subprocess.run(cmd, capture_output=True, text=True)
            
            # Парсим вывод и добавляем тестовые устройства
            devices.append({
                'name': 'Default Input Device',
                'id': 'default',
                'type': 'wasapi2src'
            })
            devices.append({
                'name': 'System Audio (Loopback)',
                'id': 'loopback',
                'type': 'wasapi2src'
            })
            
        except Exception as e:
            print(f"Ошибка получения Windows устройств: {e}")
            devices.append({
                'name': 'Default Input Device',
                'id': 'default',
                'type': 'wasapi2src'
            })
        
        return devices
    
    def _get_linux_devices(self):
        """Получить Linux ALSA/PulseAudio устройства"""
        devices = []
        try:
            # Проверяем PulseAudio
            result = subprocess.run(['pactl', 'list', 'sources'], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                devices.append({
                    'name': 'Default PulseAudio Input',
                    'id': 'default',
                    'type': 'pulsesrc'
                })
            
            # Проверяем ALSA
            result = subprocess.run(['arecord', '-l'], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                devices.append({
                    'name': 'Default ALSA Input',
                    'id': 'default',
                    'type': 'alsasrc'
                })
                
        except Exception as e:
            print(f"Ошибка получения Linux устройств: {e}")
            devices.append({
                'name': 'Default Input Device',
                'id': 'default',
                'type': 'alsasrc'
            })
        
        return devices
    
    def _get_macos_devices(self):
        """Получить macOS Core Audio устройства"""
        devices = []
        try:
            devices.append({
                'name': 'Default macOS Input',
                'id': 'default',
                'type': 'osxaudiosrc'
            })
        except Exception as e:
            print(f"Ошибка получения macOS устройств: {e}")
        
        return devices
    
    def start_stream(self, device_info, channels, host, port):
        """Запустить стрим"""
        if self.is_streaming:
            return False
        
        system = platform.system()
        
        if HAS_GSTREAMER:
            return self._start_gstreamer_stream(device_info, channels, host, port)
        else:
            return self._start_command_stream(device_info, channels, host, port)
    
    def _start_gstreamer_stream(self, device_info, channels, host, port):
        """Запустить стрим через GStreamer API"""
        try:
            pipeline_str = self._build_pipeline_string(device_info, channels, host, port)
            self.pipeline = Gst.parse_launch(pipeline_str)
            
            self.pipeline.set_state(Gst.State.PLAYING)
            self.is_streaming = True
            
            def run_loop():
                self.main_loop = GLib.MainLoop()
                self.main_loop.run()
            
            self.loop_thread = threading.Thread(target=run_loop)
            self.loop_thread.daemon = True
            self.loop_thread.start()
            
            return True
            
        except Exception as e:
            print(f"Ошибка запуска GStreamer: {e}")
            return False
    
    def _start_command_stream(self, device_info, channels, host, port):
        """Запустить стрим через gst-launch-1.0"""
        try:
            pipeline_str = self._build_pipeline_string(device_info, channels, host, port)
            cmd = ['gst-launch-1.0'] + pipeline_str.split()
            
            self.current_process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            
            self.is_streaming = True
            return True
            
        except Exception as e:
            print(f"Ошибка запуска gst-launch-1.0: {e}")
            return False
    
    def _build_pipeline_string(self, device_info, channels, host, port):
        """Построить строку pipeline для GStreamer"""
        system = platform.system()
        source_type = device_info['type']
        
        if system == "Windows":
            if device_info['id'] == 'loopback':
                source = f"{source_type} loopback=true"
            else:
                source = f"{source_type}"
        else:
            source = f"{source_type}"
        
        pipeline = (
            f"{source} ! "
            f"audio/x-raw,channels={channels},rate=48000 ! "
            f"audioconvert ! audioresample ! "
            f"rtpL16pay ! "
            f"udpsink host={host} port={port} auto-multicast=true"
        )
        
        return pipeline
    
    def stop_stream(self):
        """Остановить стрим"""
        if not self.is_streaming:
            return
        
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
            self.pipeline = None
            
        if self.main_loop:
            self.main_loop.quit()
            self.main_loop = None
            
        if self.current_process:
            self.current_process.terminate()
            self.current_process = None
        
        self.is_streaming = False


class AudioStreamerGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Audio Network Streamer")
        self.root.geometry("500x400")
        
        self.streamer = AudioStreamer()
        self.setup_gui()
        self.load_settings()
        
        # Обновляем список устройств
        self.refresh_devices()
    
    def setup_gui(self):
        """Настройка интерфейса"""
        # Основной фрейм
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Выбор устройства
        ttk.Label(main_frame, text="Аудио устройство:").grid(row=0, column=0, sticky=tk.W, pady=5)
        self.device_var = tk.StringVar()
        self.device_combo = ttk.Combobox(main_frame, textvariable=self.device_var, width=50)
        self.device_combo.grid(row=0, column=1, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        
        # Кнопка обновления устройств
        ttk.Button(main_frame, text="Обновить", command=self.refresh_devices).grid(row=0, column=3, pady=5)
        
        # Настройки каналов
        ttk.Label(main_frame, text="Каналы:").grid(row=1, column=0, sticky=tk.W, pady=5)
        self.channels_var = tk.StringVar(value="2")
        channels_combo = ttk.Combobox(main_frame, textvariable=self.channels_var, values=["1", "2"], width=10)
        channels_combo.grid(row=1, column=1, sticky=tk.W, pady=5)
        
        # Настройки сети
        ttk.Label(main_frame, text="Host:").grid(row=2, column=0, sticky=tk.W, pady=5)
        self.host_var = tk.StringVar(value="239.0.7.1")
        host_entry = ttk.Entry(main_frame, textvariable=self.host_var, width=20)
        host_entry.grid(row=2, column=1, sticky=tk.W, pady=5)
        
        ttk.Label(main_frame, text="Port:").grid(row=2, column=2, sticky=tk.W, pady=5, padx=(10, 0))
        self.port_var = tk.StringVar(value="7777")
        port_entry = ttk.Entry(main_frame, textvariable=self.port_var, width=10)
        port_entry.grid(row=2, column=3, sticky=tk.W, pady=5)
        
        # Кнопки управления
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=3, column=0, columnspan=4, pady=20)
        
        self.start_button = ttk.Button(button_frame, text="Запустить стрим", command=self.start_stream)
        self.start_button.pack(side=tk.LEFT, padx=5)
        
        self.stop_button = ttk.Button(button_frame, text="Остановить стрим", command=self.stop_stream)
        self.stop_button.pack(side=tk.LEFT, padx=5)
        
        # Статус
        self.status_var = tk.StringVar(value="Готов к работе")
        status_label = ttk.Label(main_frame, textvariable=self.status_var)
        status_label.grid(row=4, column=0, columnspan=4, pady=10)
        
        # Лог
        log_frame = ttk.LabelFrame(main_frame, text="Лог", padding="5")
        log_frame.grid(row=5, column=0, columnspan=4, sticky=(tk.W, tk.E, tk.N, tk.S), pady=10)
        
        self.log_text = tk.Text(log_frame, height=10, width=60)
        scrollbar = ttk.Scrollbar(log_frame, orient=tk.VERTICAL, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scrollbar.set)
        
        self.log_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        scrollbar.grid(row=0, column=1, sticky=(tk.N, tk.S))
        
        # Настройка растяжения
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(5, weight=1)
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
    
    def refresh_devices(self):
        """Обновить список устройств"""
        self.log("Обновление списка устройств...")
        devices = self.streamer.get_audio_devices()
        
        device_names = [f"{device['name']} ({device['type']})" for device in devices]
        self.device_combo['values'] = device_names
        
        if device_names:
            self.device_combo.set(device_names[0])
        
        self.devices = devices
        self.log(f"Найдено устройств: {len(devices)}")
    
    def start_stream(self):
        """Запустить стрим"""
        if not self.devices:
            messagebox.showerror("Ошибка", "Нет доступных аудио устройств")
            return
        
        device_index = self.device_combo.current()
        if device_index == -1:
            messagebox.showerror("Ошибка", "Выберите устройство")
            return
        
        device_info = self.devices[device_index]
        channels = int(self.channels_var.get())
        host = self.host_var.get()
        port = int(self.port_var.get())
        
        self.log(f"Запуск стрима: {device_info['name']} -> {host}:{port}")
        
        if self.streamer.start_stream(device_info, channels, host, port):
            self.status_var.set("Стрим запущен")
            self.start_button.config(state='disabled')
            self.stop_button.config(state='normal')
            self.log("Стрим успешно запущен")
            self.save_settings()
        else:
            self.log("Ошибка запуска стрима")
            messagebox.showerror("Ошибка", "Не удалось запустить стрим")
    
    def stop_stream(self):
        """Остановить стрим"""
        self.log("Остановка стрима...")
        self.streamer.stop_stream()
        
        self.status_var.set("Стрим остановлен")
        self.start_button.config(state='normal')
        self.stop_button.config(state='disabled')
        self.log("Стрим остановлен")
    
    def log(self, message):
        """Добавить сообщение в лог"""
        self.log_text.insert(tk.END, f"{message}\n")
        self.log_text.see(tk.END)
    
    def save_settings(self):
        """Сохранить настройки"""
        settings = {
            'device': self.device_var.get(),
            'channels': self.channels_var.get(),
            'host': self.host_var.get(),
            'port': self.port_var.get()
        }
        
        try:
            with open('streamer_settings.json', 'w') as f:
                json.dump(settings, f, indent=2)
        except Exception as e:
            print(f"Ошибка сохранения настроек: {e}")
    
    def load_settings(self):
        """Загрузить настройки"""
        try:
            if os.path.exists('streamer_settings.json'):
                with open('streamer_settings.json', 'r') as f:
                    settings = json.load(f)
                
                self.device_var.set(settings.get('device', ''))
                self.channels_var.set(settings.get('channels', '2'))
                self.host_var.set(settings.get('host', '239.0.7.1'))
                self.port_var.set(settings.get('port', '7777'))
        except Exception as e:
            print(f"Ошибка загрузки настроек: {e}")


def main():
    # Проверка зависимостей
    if not HAS_GSTREAMER:
        print("Предупреждение: GStreamer Python bindings не найдены.")
        print("Приложение будет использовать gst-launch-1.0 command line tool.")
        print("Убедитесь, что GStreamer установлен в системе.")
    
    root = tk.Tk()
    app = AudioStreamerGUI(root)
    
    def on_closing():
        if app.streamer.is_streaming:
            app.stop_stream()
        root.destroy()
    
    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.mainloop()


if __name__ == "__main__":
    main()
