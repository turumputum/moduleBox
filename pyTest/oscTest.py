from pythonosc import udp_client
import paho.mqtt.client as mqtt
import time

# Создание OSC клиента
osc_client = udp_client.SimpleUDPClient("10.0.10.29", 8023)  # Замените IP и порт на конфигурацию вашего устройства


# Список OSC адресов для каждой группы светодиодов
osc_addresses = [
    "/ledCube/smartLed_0",
    "/ledCube/smartLed_1",
    "/ledCube/smartLed_2",
    "/ledCube/smartLed_3",
    "/ledCube/smartLed_4",
    "/ledCube/smartLed_5",
]

# Бесконечный цикл для поочередного включения групп
while True:
    for i, address in enumerate(osc_addresses):
        # Включаем текущую группу (1) через OSC
        osc_client.send_message(address, 1)
        # Включаем текущую группу (1) через MQTT

        # Выключаем все остальные группы (0) через OSC
        for j, other_address in enumerate(osc_addresses):
            if j != i:
                osc_client.send_message(other_address, 0)
                # Выключаем все остальные группы (0) через MQTT

        # Ждем одну секунду перед переходом к следующей группе
        time.sleep(1)
