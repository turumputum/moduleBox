# Audio Network Streamer

Кроссплатформенное приложение для вещания звукового потока в сеть с графическим интерфейсом.

## Особенности

- **Кроссплатформенность**: Работает на Windows, Linux и macOS
- **Выбор устройств**: Автоматическое обнаружение аудио устройств
- **Настройка параметров**: Выбор каналов, адреса и порта
- **Простой интерфейс**: Интуитивно понятный GUI на tkinter
- **Сохранение настроек**: Автоматическое сохранение конфигурации

## Требования

### Системные требования
- Python 3.6 или выше
- GStreamer 1.0 или выше

### Установка GStreamer

#### Windows
1. Скачайте и установите GStreamer с официального сайта: https://gstreamer.freedesktop.org/download/
2. Убедитесь, что GStreamer добавлен в PATH

#### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
sudo apt-get install python3-gi python3-gi-cairo gir1.2-gstreamer-1.0
```

#### macOS
```bash
brew install gstreamer
brew install pygobject3 gtk+3
```

## Установка

1. Клонируйте репозиторий или скачайте файлы
2. Установите зависимости (если доступны):
```bash
pip install PyGObject  # Опционально для лучшей производительности
```

## Запуск

```bash
python audio_streamer.py
```

## Использование

1. **Выбор устройства**: Выберите аудио устройство из выпадающего списка
   - На Windows: WASAPI устройства (включая loopback для записи системного звука)
   - На Linux: PulseAudio или ALSA устройства
   - На macOS: Core Audio устройства

2. **Настройка параметров**:
   - **Каналы**: Выберите 1 (моно) или 2 (стерео)
   - **Host**: IP-адрес для multicast (по умолчанию 239.0.7.1)
   - **Port**: Порт для передачи (по умолчанию 7777)

3. **Управление стримом**:
   - Нажмите "Запустить стрим" для начала вещания
   - Нажмите "Остановить стрим" для остановки

## Параметры стрима

- **Формат**: PCM 16-bit
- **Частота дискретизации**: 48 kHz
- **Протокол**: RTP over UDP
- **Кодировка**: L16 (несжатый PCM)

## Приём стрима

Для приёма стрима используйте:

```bash
# GStreamer
gst-launch-1.0 udpsrc port=7777 ! application/x-rtp,encoding-name=L16,channels=2,rate=48000 ! rtpL16depay ! audioconvert ! autoaudiosink

# VLC
vlc rtp://239.0.7.1:7777

# FFmpeg
ffplay -f rtp rtp://239.0.7.1:7777
```

## Файлы конфигурации

Настройки сохраняются в файле `streamer_settings.json` в том же каталоге, что и приложение.

## Устранение неполадок

### Ошибка "GStreamer не найден"
- Убедитесь, что GStreamer установлен и доступен в PATH
- Проверьте установку командой: `gst-launch-1.0 --version`

### Нет доступных устройств
- Проверьте, что микрофон или аудио устройство подключено
- На Linux убедитесь, что PulseAudio или ALSA работают корректно
- Попробуйте обновить список устройств кнопкой "Обновить"

### Проблемы с сетью
- Убедитесь, что multicast трафик разрешён в сети
- Проверьте настройки брандмауэра
- Попробуйте использовать unicast адрес вместо multicast

## Расширенные возможности

### Командная строка GStreamer
Приложение генерирует pipeline для GStreamer. Для расширенной настройки можно использовать прямые команды:

```bash
# Windows (системный звук)
gst-launch-1.0 wasapi2src loopback=true ! audio/x-raw,channels=2,rate=48000 ! audioconvert ! audioresample ! rtpL16pay ! udpsink host=239.0.7.1 port=7777 auto-multicast=true

# Linux (PulseAudio)
gst-launch-1.0 pulsesrc ! audio/x-raw,channels=2,rate=48000 ! audioconvert ! audioresample ! rtpL16pay ! udpsink host=239.0.7.1 port=7777 auto-multicast=true

# macOS
gst-launch-1.0 osxaudiosrc ! audio/x-raw,channels=2,rate=48000 ! audioconvert ! audioresample ! rtpL16pay ! udpsink host=239.0.7.1 port=7777 auto-multicast=true
```

## Лицензия

Это приложение распространяется свободно для использования в личных и коммерческих целях.
