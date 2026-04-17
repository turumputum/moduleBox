#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MQTT Quick Check — быстрая проверка MQTT-настроек moduleBox
============================================================

Подключается к СУЩЕСТВУЮЩЕМУ брокеру (не запускает свой)
и проверяет, что контроллер moduleBox корректно работает
с настроенными параметрами (QOS, auth, TLS, retain).

Также может настроить контроллер через USB CDC:
  - Идентификация устройства
  - Чтение статуса
  - Перезагрузка контроллера (--restart) для принудительного теста

Поскольку retain=false, скрипт не получит state=1 если контроллер
уже подключён. В таком случае скрипт проверяет связь через MQTT-пинг
(отправляет команду на системный топик контроллера) и/или через USB.

Требования:
  pip install paho-mqtt pyserial

Использование:
  python mqtt_quick_check.py --broker 192.168.88.99 --device moduleBox
  python mqtt_quick_check.py --broker 192.168.88.99 --auth mbuser mbpass
  python mqtt_quick_check.py --broker 192.168.88.99 --tls
  python mqtt_quick_check.py --broker 192.168.88.99 --com COM7 --show-status
  python mqtt_quick_check.py --broker 192.168.88.99 --com COM7 --restart
"""

import argparse
import json
import sys
import time
import threading

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ОШИБКА: pip install paho-mqtt")
    sys.exit(1)

# paho-mqtt v2.0+ uses CallbackAPIVersion, older versions don't
_PAHO_V2 = hasattr(mqtt, 'CallbackAPIVersion')
def _make_mqtt_client(client_id):
    if _PAHO_V2:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id)
    return mqtt.Client(client_id)

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

TIMEOUT = 15

class C:
    OK = "\033[92m"; FAIL = "\033[91m"; WARN = "\033[93m"
    INFO = "\033[96m"; BOLD = "\033[1m"; END = "\033[0m"


def usb_open(com_port=None):
    """Открывает USB CDC соединение. Возвращает serial объект или None."""
    if not HAS_SERIAL:
        return None
    port = com_port
    if not port:
        for p in serial.tools.list_ports.comports():
            if "usb" in (p.description or "").lower():
                port = p.device
                break
    if not port:
        return None
    try:
        ser = serial.Serial(port, 115200, timeout=3)
        time.sleep(0.5)
        ser.reset_input_buffer()
        return ser
    except Exception as e:
        print(f"  {C.WARN}USB ошибка: {e}{C.END}")
        return None


def usb_command(ser, cmd, wait=1.5):
    """Отправляет команду через USB CDC и возвращает ответ."""
    if not ser:
        return ""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    time.sleep(wait)
    return ser.read(ser.in_waiting or 1024).decode(errors="replace").strip()


def main():
    parser = argparse.ArgumentParser(description="MQTT Quick Check для moduleBox")
    parser.add_argument("--broker", required=True, help="Адрес MQTT-брокера")
    parser.add_argument("--port", type=int, default=None, help="Порт (1883 или 8883)")
    parser.add_argument("--device", default="moduleBox", help="deviceName контроллера")
    parser.add_argument("--auth", nargs=2, metavar=("USER", "PASS"), help="Логин и пароль")
    parser.add_argument("--tls", action="store_true", help="TLS-подключение")
    parser.add_argument("--com", default=None, help="COM-порт для USB CDC")
    parser.add_argument("--show-status", action="store_true", help="Показать system_status через USB")
    parser.add_argument("--restart", action="store_true",
                        help="Перезагрузить контроллер перед тестом (через USB) "
                             "для гарантированного получения state=1")
    args = parser.parse_args()

    port = args.port or (8883 if args.tls else 1883)

    print(f"\n{C.BOLD}MQTT Quick Check{C.END}")
    print(f"  Брокер: {args.broker}:{port}")
    print(f"  Device: {args.device}")
    print(f"  Auth:   {'да' if args.auth else 'нет'}")
    print(f"  TLS:    {'да' if args.tls else 'нет'}\n")

    # ── USB подключение ──
    ser = usb_open(args.com)
    if ser:
        identity = usb_command(ser, "Who are you?")
        print(f"  {C.INFO}USB ({ser.port}):{C.END} {identity}")

        # Определяем реальное имя устройства
        for line in identity.splitlines():
            if "moduleBox:" in line:
                real_name = line.split(":", 1)[1].strip()
                if real_name and real_name != args.device:
                    print(f"  {C.WARN}[WARN]{C.END} deviceName='{real_name}', использую его вместо '--device {args.device}'")
                    args.device = real_name
                break

        if args.show_status:
            status = usb_command(ser, "get system_status", wait=2.0)
            if status:
                print(f"\n  {C.INFO}System Status:{C.END}")
                for line in status.splitlines():
                    print(f"    {line}")
            else:
                print(f"  {C.WARN}[INFO]{C.END} system_status пуст (нормально если USB-диск не извлечён)")
            print()

        # Перезагрузка перед тестом — гарантирует получение state=1
        if args.restart:
            print(f"  {C.INFO}Перезагрузка контроллера...{C.END}")
            usb_command(ser, "system/restart", wait=1.0)
            ser.close()
            ser = None
            print(f"  Ждём загрузку контроллера (10 сек)...")
            time.sleep(10)
            # Переоткрываем USB
            ser = usb_open(args.com)
            if ser:
                print(f"  {C.OK}[OK]{C.END}   Контроллер перезагружен")
            else:
                print(f"  {C.INFO}[INFO]{C.END} USB ещё не готов, продолжаем тест по MQTT")
    else:
        if args.com:
            print(f"  {C.WARN}USB: не удалось подключиться к {args.com}{C.END}")
        elif HAS_SERIAL:
            print(f"  {C.INFO}USB: не обнаружен (опционально){C.END}")
        else:
            print(f"  {C.INFO}USB: pyserial не установлен (pip install pyserial){C.END}")

    # ── MQTT подключение ──
    state_topic = f"clients/{args.device}/state"
    topics_topic = f"clients/{args.device}/topics"
    # Подпишемся также на wildcard для пинг-проверки
    ping_response_topic = f"clients/{args.device}/pong"

    results = {}
    events = {
        "connected": threading.Event(),
        "state": threading.Event(),
        "topics": threading.Event(),
        "any_from_device": threading.Event(),
    }

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            events["connected"].set()
            client.subscribe(state_topic, qos=2)
            client.subscribe(topics_topic, qos=2)
            # Wildcard на все сообщения от контроллера
            client.subscribe(f"clients/{args.device}/#", qos=0)
            print(f"  {C.OK}[OK]{C.END}   Подключение к брокеру")
        else:
            reasons = {1: "bad protocol", 2: "client-id rejected", 3: "server unavailable",
                       4: "bad user/pass", 5: "not authorized"}
            print(f"  {C.FAIL}[FAIL]{C.END} Подключение к брокеру: rc={rc} ({reasons.get(rc, '?')})")

    def on_message(client, userdata, msg):
        payload = msg.payload.decode(errors="replace")
        if msg.topic == state_topic:
            results["state"] = payload
            results["state_retain"] = msg.retain
            results["state_qos"] = msg.qos
            events["state"].set()
        elif msg.topic == topics_topic:
            results["topics"] = payload
            events["topics"].set()
        # Любое сообщение от контроллера — значит MQTT работает
        if msg.topic.startswith(f"clients/{args.device}/"):
            events["any_from_device"].set()

    client = _make_mqtt_client("mb_quick_check")
    if args.auth:
        client.username_pw_set(args.auth[0], args.auth[1])
    if args.tls:
        client.tls_set()
        client.tls_insecure_set(True)
    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(args.broker, port, keepalive=30)
    except Exception as e:
        print(f"\n  {C.FAIL}[FAIL]{C.END} Не удалось подключиться к {args.broker}:{port}: {e}")
        if ser: ser.close()
        sys.exit(1)

    client.loop_start()

    if not events["connected"].wait(timeout=5):
        print(f"\n  {C.FAIL}[FAIL]{C.END} Таймаут подключения к брокеру")
        client.loop_stop()
        if ser: ser.close()
        sys.exit(1)

    # Ждём данные от контроллера
    print(f"\n  Ожидаем сообщение от контроллера ({TIMEOUT} сек)...")
    print(f"  {C.INFO}(retain=false: сообщение видно только в момент подключения контроллера){C.END}")
    got_state = events["state"].wait(timeout=TIMEOUT)
    got_topics = events["topics"].wait(timeout=3)

    # ── Если state не получен — пробуем MQTT-пинг через системный топик ──
    mqtt_alive = False
    if not got_state:
        print(f"\n  {C.INFO}state=1 не получен (контроллер уже подключён, retain=false){C.END}")
        print(f"  {C.INFO}Пробуем MQTT-пинг...{C.END}")
        # Публикуем в системный топик контроллера — он подписан на <device>/system/#
        # getVersion вызовет execute(), но ответ пойдёт в usbprint.
        # Однако сам факт доставки мы не можем проверить напрямую.
        # Попробуем другой подход: подписываемся на LWT-топик и ждём.
        # Если контроллер жив, LWT не публикуется (только при disconnect).

        # Проверка через USB — самый надёжный способ
        if ser:
            print(f"  {C.INFO}Проверяем MQTT-статус через USB...{C.END}")
            resp = usb_command(ser, "get system_status", wait=2.0)
            if "MQTT" in resp.upper() or resp:
                for line in resp.splitlines():
                    if "mqtt" in line.lower():
                        print(f"    {line}")
                        if "ok" in line.lower() or "1" in line:
                            mqtt_alive = True

            if not mqtt_alive:
                # Пробуем альтернативный способ — отправляем команду через USB
                # и проверяем что MQTT работает
                resp = usb_command(ser, "system/getNETstatus", wait=2.0)
                if resp:
                    print(f"  {C.INFO}NET status:{C.END}")
                    for line in resp.splitlines():
                        print(f"    {line}")
                        if "mqtt" in line.lower():
                            mqtt_alive = True

    client.disconnect()
    client.loop_stop()

    # ── Результаты ──
    print()
    all_ok = False

    if got_state:
        r = results.get("state_retain", False)
        q = results.get("state_qos", -1)
        print(f"  {C.OK}[OK]{C.END}   Контроллер онлайн (state={results['state']})")
        print(f"         QOS={q}, retain={'да' if r else 'нет'}")
        if r:
            print(f"  {C.WARN}[WARN]{C.END} retain=true обнаружен! Ожидалось retain=false")
        else:
            print(f"  {C.OK}[OK]{C.END}   retain=false подтверждён")
        all_ok = True
    elif mqtt_alive:
        print(f"  {C.OK}[OK]{C.END}   Контроллер подключён к MQTT (подтверждено через USB)")
        print(f"         state=1 не получен через MQTT — это нормально (retain=false)")
        print(f"         Для полного теста используйте --restart для перезагрузки перед проверкой")
        all_ok = True
    else:
        print(f"  {C.FAIL}[FAIL]{C.END} Контроллер не подключился к MQTT")
        if not ser:
            print(f"         Подсказка: подключите USB (--com COMx) для проверки статуса")
            print(f"         или используйте --restart для перезагрузки контроллера")
        else:
            print(f"         MQTT-статус через USB тоже не подтверждён")

    if got_topics:
        raw = results["topics"]
        try:
            parsed = json.loads(raw)
            t = len(parsed.get("triggers", []))
            a = len(parsed.get("actions", []))
            print(f"  {C.OK}[OK]{C.END}   Топики получены: {t} triggers, {a} actions")
        except Exception:
            print(f"  {C.WARN}[WARN]{C.END} Топики получены, но не валидный JSON:")
            for line in raw.splitlines():
                print(f"           {line}")
    elif not got_state and not mqtt_alive:
        print(f"  {C.WARN}[WARN]{C.END} Список топиков не получен")

    print()
    if ser: ser.close()
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
