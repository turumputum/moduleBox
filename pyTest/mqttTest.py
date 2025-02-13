from pythonosc import udp_client
import paho.mqtt.client as mqtt
import time

# Создание MQTT клиента с указанием callback_api_version
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)  # Используем версию API 1
mqtt_client.connect("192.168.88.99", 1883, 60)  # Замените IP и порт на конфигурацию вашего MQTT брокера

# Список OSC адресов для каждой группы светодиодов
mqtt_addresses = [
    "ledCube/smartLed_0",
    "ledCube/smartLed_1",
    "ledCube/smartLed_2",
    "ledCube/smartLed_3",
    "ledCube/smartLed_4",
    "ledCube/smartLed_5",
]

# Бесконечный цикл для поочередного включения групп
while True:
    for i, address in enumerate(mqtt_addresses):
        # Включаем текущую группу (1) через OSC
        # Включаем текущую группу (1) через MQTT
        mqtt_client.publish(address, "1")

        # Выключаем все остальные группы (0) через OSC
        for j, other_address in enumerate(mqtt_addresses):
            if j != i:
                # Выключаем все остальные группы (0) через MQTT
                mqtt_client.publish(other_address, "0")

        # Ждем одну секунду перед переходом к следующей группе
        time.sleep(1)
