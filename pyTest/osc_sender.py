import time
from pythonosc.udp_client import SimpleUDPClient

# Настройки OSC
OSC_IP = "192.168.88.134"
OSC_PORT = 9000
OSC_ADDRESS = "/mbGlassPanTouch/smartLed_5"

# Создаём клиент
client = SimpleUDPClient(OSC_IP, OSC_PORT)

print(f"Отправка OSC-сообщений на {OSC_IP}:{OSC_PORT}{OSC_ADDRESS}")

try:
    while True:
        # Отправляем 1
        client.send_message(OSC_ADDRESS, 1)
        print("Отправлено: 1")
        time.sleep(1)

        # Отправляем 0
        client.send_message(OSC_ADDRESS, 0)
        print("Отправлено: 0")
        time.sleep(1)

except KeyboardInterrupt:
    print("\nОстановка скрипта...")