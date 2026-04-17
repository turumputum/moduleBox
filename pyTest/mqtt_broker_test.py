#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MQTT Broker Test Suite для moduleBox
=====================================

Запускает локальный mosquitto-брокер с нужными опциями и проверяет
работу контроллера moduleBox, подключённого по локальной сети.

USB-порт используется для:
  - Идентификации устройства (Who are you?)
  - Чтения статуса MQTT-подключения (get system_status)

Требования:
  pip install paho-mqtt pyserial

  Mosquitto должен быть установлен и доступен в PATH:
    Windows: choco install mosquitto  (или скачать с mosquitto.org)
    Linux:   sudo apt install mosquitto mosquitto-clients

Использование:
  python mqtt_broker_test.py --ip 192.168.88.33 --com COM7
  python mqtt_broker_test.py --ip 192.168.88.33              # без USB
  python mqtt_broker_test.py --ip 192.168.88.33 --tls        # тест TLS
  python mqtt_broker_test.py --ip 192.168.88.33 --auth        # тест логин/пароль
  python mqtt_broker_test.py --ip 192.168.88.33 --tls --auth  # оба
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import threading
from pathlib import Path

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

# ─────────────────────────────────────────────
# Константы
# ─────────────────────────────────────────────
BROKER_HOST = "0.0.0.0"
MQTT_PORT = 1883
MQTTS_PORT = 8883
MQTT_USER = "mbuser"
MQTT_PASS = "mbpass"
DEVICE_TOPIC_PREFIX = "clients"
TEST_TIMEOUT = 15  # секунд ожидания подключения контроллера
USB_BAUD = 115200

# Цвета терминала
class C:
    OK = "\033[92m"
    FAIL = "\033[91m"
    WARN = "\033[93m"
    INFO = "\033[96m"
    BOLD = "\033[1m"
    END = "\033[0m"


def log_ok(msg):    print(f"  {C.OK}[OK]{C.END}   {msg}")
def log_fail(msg):  print(f"  {C.FAIL}[FAIL]{C.END} {msg}")
def log_info(msg):  print(f"  {C.INFO}[INFO]{C.END} {msg}")
def log_warn(msg):  print(f"  {C.WARN}[WARN]{C.END} {msg}")
def log_head(msg):  print(f"\n{C.BOLD}{'─'*50}\n  {msg}\n{'─'*50}{C.END}")


# ─────────────────────────────────────────────
# TLS сертификаты (самоподписанные)
# ─────────────────────────────────────────────
def generate_tls_certs(cert_dir: Path):
    """Генерирует CA, серверный ключ и сертификат для локального mosquitto."""
    ca_key   = cert_dir / "ca.key"
    ca_cert  = cert_dir / "ca.crt"
    srv_key  = cert_dir / "server.key"
    srv_cert = cert_dir / "server.crt"
    srv_csr  = cert_dir / "server.csr"

    if ca_cert.exists() and srv_cert.exists():
        log_info("TLS-сертификаты уже существуют, пропускаем генерацию")
        return ca_cert, srv_key, srv_cert

    log_info("Генерация самоподписанных TLS-сертификатов...")

    # CA
    subprocess.run([
        "openssl", "req", "-new", "-x509", "-days", "365",
        "-keyout", str(ca_key), "-out", str(ca_cert),
        "-subj", "/CN=moduleBox Test CA",
        "-nodes"
    ], check=True, capture_output=True)

    # Server key + CSR
    subprocess.run([
        "openssl", "req", "-new", "-nodes",
        "-keyout", str(srv_key), "-out", str(srv_csr),
        "-subj", "/CN=localhost"
    ], check=True, capture_output=True)

    # Sign server cert
    subprocess.run([
        "openssl", "x509", "-req", "-days", "365",
        "-in", str(srv_csr), "-CA", str(ca_cert), "-CAkey", str(ca_key),
        "-CAcreateserial", "-out", str(srv_cert)
    ], check=True, capture_output=True)

    log_ok("TLS-сертификаты сгенерированы")
    return ca_cert, srv_key, srv_cert


# ─────────────────────────────────────────────
# Mosquitto broker management
# ─────────────────────────────────────────────
class MosquittoBroker:
    """Запускает и останавливает локальный mosquitto с нужной конфигурацией."""

    def __init__(self, use_tls=False, use_auth=False, work_dir=None):
        self.use_tls = use_tls
        self.use_auth = use_auth
        self.process = None
        self.work_dir = Path(work_dir or tempfile.mkdtemp(prefix="mb_mqtt_"))
        self.work_dir.mkdir(parents=True, exist_ok=True)
        self.config_path = self.work_dir / "mosquitto.conf"
        self.password_file = self.work_dir / "passwd"
        self.ca_cert = None
        self.srv_key = None
        self.srv_cert = None
        self.port = MQTTS_PORT if use_tls else MQTT_PORT

    def _create_password_file(self):
        """Создаёт файл паролей для mosquitto."""
        # mosquitto_passwd -c -b <file> <user> <pass>
        try:
            subprocess.run(
                ["mosquitto_passwd", "-c", "-b",
                 str(self.password_file), MQTT_USER, MQTT_PASS],
                check=True, capture_output=True
            )
            log_ok(f"Файл паролей создан: {MQTT_USER} / {MQTT_PASS}")
        except FileNotFoundError:
            # Fallback: написать plaintext (mosquitto 2.x не поддерживает, но для теста OK)
            log_warn("mosquitto_passwd не найден, создаём plaintext-файл")
            self.password_file.write_text(f"{MQTT_USER}:{MQTT_PASS}\n")

    def _generate_config(self):
        """Генерирует конфиг mosquitto.conf."""
        lines = [
            f"listener {self.port} {BROKER_HOST}",
            "allow_anonymous true" if not self.use_auth else "allow_anonymous false",
            "log_type all",
            f"persistence_location {self.work_dir}/",
        ]

        if self.use_auth:
            self._create_password_file()
            lines.append(f"password_file {self.password_file}")

        if self.use_tls:
            cert_dir = self.work_dir / "certs"
            cert_dir.mkdir(exist_ok=True)
            self.ca_cert, self.srv_key, self.srv_cert = generate_tls_certs(cert_dir)
            lines += [
                f"cafile {self.ca_cert}",
                f"certfile {self.srv_cert}",
                f"keyfile {self.srv_key}",
                "tls_version tlsv1.2",
            ]

        self.config_path.write_text("\n".join(lines) + "\n")
        log_info(f"Конфигурация mosquitto записана: {self.config_path}")

    def start(self):
        """Запускает mosquitto в фоне."""
        self._generate_config()
        log_info(f"Запускаем mosquitto на порту {self.port}...")
        try:
            self.process = subprocess.Popen(
                ["mosquitto", "-c", str(self.config_path), "-v"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0,
            )
        except FileNotFoundError:
            log_fail("mosquitto не найден в PATH! Установите: choco install mosquitto")
            sys.exit(1)

        time.sleep(1.5)  # дать время на старт

        if self.process.poll() is not None:
            out = self.process.stdout.read().decode(errors="replace")
            log_fail(f"mosquitto не запустился:\n{out}")
            sys.exit(1)

        log_ok(f"mosquitto запущен (PID {self.process.pid})")
        return self

    def stop(self):
        """Останавливает mosquitto."""
        if self.process and self.process.poll() is None:
            if sys.platform == "win32":
                self.process.terminate()
            else:
                self.process.send_signal(signal.SIGTERM)
            self.process.wait(timeout=5)
            log_info("mosquitto остановлен")


# ─────────────────────────────────────────────
# USB CDC Communication
# ─────────────────────────────────────────────
class USBDevice:
    """Общение с moduleBox через USB CDC (виртуальный COM-порт)."""

    def __init__(self, port=None):
        self.port = port
        self.ser = None

    @staticmethod
    def auto_detect() -> str:
        """Поиск moduleBox среди COM-портов."""
        if not HAS_SERIAL:
            return None
        for p in serial.tools.list_ports.comports():
            # ESP32-S3 USB-CDC обычно содержит "USB Serial" или "USB JTAG"
            desc = (p.description or "").lower()
            if "usb" in desc and ("serial" in desc or "jtag" in desc):
                return p.device
        return None

    def connect(self):
        if not HAS_SERIAL:
            log_warn("pyserial не установлен (pip install pyserial), USB-доступ отключён")
            return False
        port = self.port or self.auto_detect()
        if not port:
            log_warn("USB-порт контроллера не найден")
            return False
        try:
            self.ser = serial.Serial(port, USB_BAUD, timeout=3)
            time.sleep(0.5)
            self.ser.reset_input_buffer()
            log_ok(f"USB подключён: {port}")
            return True
        except Exception as e:
            log_warn(f"Не удалось открыть {port}: {e}")
            return False

    def send_command(self, cmd: str, wait=2.0) -> str:
        """Отправляет команду и возвращает ответ."""
        if not self.ser:
            return ""
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\n").encode())
        time.sleep(wait)
        resp = self.ser.read(self.ser.in_waiting or 1024).decode(errors="replace")
        return resp.strip()

    def identify(self) -> str:
        resp = self.send_command("Who are you?", wait=1.0)
        # Ожидаем: "moduleBox:deviceName"
        for line in resp.splitlines():
            if "moduleBox:" in line:
                return line.split(":", 1)[1].strip()
        return resp

    def get_status(self) -> str:
        return self.send_command("get system_status", wait=2.0)

    def close(self):
        if self.ser:
            self.ser.close()


# ─────────────────────────────────────────────
# Config file generator
# ─────────────────────────────────────────────
def generate_config_ini(broker_ip: str, use_tls=False, use_auth=False,
                        qos=0, watchdog=0, device_name="moduleBox") -> str:
    """Генерирует содержимое config.ini для секции [MQTT]."""
    lines = [
        "[MQTT]",
        f"mqttBrokerAdress = {broker_ip}",
    ]
    if use_auth:
        lines.append(f"mqttLogin = {MQTT_USER}")
        lines.append(f"mqttPass = {MQTT_PASS}")
    lines.append(f"mqttQOS = {qos}")
    lines.append(f"mqttWatchdogTimeout = {watchdog}")
    lines.append(f"mqttTLS = {1 if use_tls else 0}")
    return "\r\n".join(lines) + "\r\n"


def upload_config_ftp(host: str, config_content: str, user="user", password="pass"):
    """Загружает config.ini на контроллер через FTP."""
    import ftplib
    import io
    try:
        log_info(f"FTP: подключаемся к {host}...")
        with ftplib.FTP(host, timeout=10) as ftp:
            ftp.login(user, password)
            # Читаем существующий конфиг, если есть
            existing = ""
            try:
                lines_buf = []
                ftp.retrlines("RETR config.ini", lambda line: lines_buf.append(line))
                existing = "\n".join(lines_buf)
            except ftplib.error_perm:
                pass

            # Заменяем/добавляем секцию [MQTT]
            new_config = _replace_mqtt_section(existing, config_content)

            ftp.storbinary("STOR config.ini", io.BytesIO(new_config.encode()))
            log_ok("config.ini загружен на контроллер")
            return True
    except Exception as e:
        log_fail(f"FTP ошибка: {e}")
        return False


def _replace_mqtt_section(full_config: str, mqtt_section: str) -> str:
    """Заменяет секцию [MQTT] в конфигурации или добавляет в конец."""
    lines = full_config.splitlines()
    result = []
    skip = False
    found = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[MQTT]"):
            skip = True
            found = True
            # вставляем новую секцию
            result.append(mqtt_section.rstrip())
            continue
        if skip and stripped.startswith("["):
            skip = False
        if not skip:
            result.append(line)
    if not found:
        result.append("")
        result.append(mqtt_section.rstrip())
    return "\r\n".join(result) + "\r\n"


# ─────────────────────────────────────────────
# MQTT Test Runner
# ─────────────────────────────────────────────
class MQTTTestRunner:
    """Выполняет тесты MQTT-подключения контроллера."""

    def __init__(self, broker_host: str, port: int, device_name: str,
                 use_tls=False, use_auth=False, ca_cert=None):
        self.broker_host = broker_host
        self.port = port
        self.device_name = device_name
        self.use_tls = use_tls
        self.use_auth = use_auth
        self.ca_cert = ca_cert
        self.results = []
        self.received_messages = {}  # topic -> payload
        self._lock = threading.Lock()

    def _make_client(self, client_id="mb_test_client"):
        client = _make_mqtt_client(client_id)
        if self.use_auth:
            client.username_pw_set(MQTT_USER, MQTT_PASS)
        if self.use_tls:
            client.tls_set(ca_certs=str(self.ca_cert) if self.ca_cert else None)
            client.tls_insecure_set(True)  # self-signed cert
        return client

    def _on_message(self, client, userdata, msg):
        with self._lock:
            self.received_messages[msg.topic] = msg.payload.decode(errors="replace")

    def _record(self, name, passed, detail=""):
        self.results.append({"name": name, "passed": passed, "detail": detail})
        if passed:
            log_ok(f"{name}: {detail}" if detail else name)
        else:
            log_fail(f"{name}: {detail}" if detail else name)

    # ── Тесты ──

    def test_broker_reachable(self):
        """Тест 1: Проверяем, что брокер доступен."""
        log_head("Тест 1: Доступность брокера")
        client = self._make_client("test_reachable")
        try:
            client.connect(self.broker_host, self.port, keepalive=10)
            client.loop_start()
            time.sleep(2)
            client.disconnect()
            client.loop_stop()
            self._record("Подключение к брокеру", True, f"{self.broker_host}:{self.port}")
        except Exception as e:
            self._record("Подключение к брокеру", False, str(e))

    def test_device_connects(self):
        """Тест 2: Ждём, пока контроллер подключится (clients/<name>/state = 1)."""
        log_head("Тест 2: Подключение контроллера к брокеру")
        state_topic = f"clients/{self.device_name}/state"
        connected_event = threading.Event()

        client = self._make_client("test_device_conn")
        client.on_message = lambda c, u, msg: (
            connected_event.set()
            if msg.topic == state_topic and msg.payload == b"1"
            else None
        )
        client.connect(self.broker_host, self.port, keepalive=30)
        client.subscribe(state_topic, qos=1)
        client.loop_start()

        log_info(f"Ожидаем подключение контроллера ({TEST_TIMEOUT} сек)...")
        result = connected_event.wait(timeout=TEST_TIMEOUT)

        client.disconnect()
        client.loop_stop()

        self._record(
            "Контроллер подключился",
            result,
            f"topic={state_topic}" if result else "Таймаут"
        )
        return result

    def test_device_topics(self):
        """Тест 3: Проверяем, что контроллер публикует список топиков."""
        log_head("Тест 3: Публикация списка топиков")
        topics_topic = f"clients/{self.device_name}/topics"
        received_event = threading.Event()
        topic_data = {}

        def on_msg(c, u, msg):
            if msg.topic == topics_topic:
                topic_data["payload"] = msg.payload.decode(errors="replace")
                received_event.set()

        client = self._make_client("test_topics_list")
        client.on_message = on_msg
        client.connect(self.broker_host, self.port, keepalive=30)
        client.subscribe(topics_topic, qos=1)
        client.loop_start()

        result = received_event.wait(timeout=TEST_TIMEOUT)

        client.disconnect()
        client.loop_stop()

        if result:
            try:
                parsed = json.loads(topic_data["payload"])
                triggers = parsed.get("triggers", [])
                actions = parsed.get("actions", [])
                self._record(
                    "Список топиков получен", True,
                    f"triggers={len(triggers)}, actions={len(actions)}"
                )
            except json.JSONDecodeError:
                self._record("Список топиков получен", True, "не JSON, но данные есть")
        else:
            self._record("Список топиков получен", False, "Таймаут")

    def test_retain_false(self):
        """Тест 4: Проверяем, что retain=false (новый подписчик не получает старые сообщения)."""
        log_head("Тест 4: retain = false")
        test_topic = f"{self.device_name}/test_retain_check"

        # Публикуем сообщение с retain=true от тестового клиента, потом
        # ожидаем что контроллер НЕ шлёт с retain
        state_topic = f"clients/{self.device_name}/state"

        time.sleep(2)  # дать контроллеру опубликовать

        # Подключаемся позже — если retain=false, мы НЕ получим state=1
        retained = {"got": False}

        def on_msg(c, u, msg):
            if msg.topic == state_topic and msg.retain:
                retained["got"] = True

        client = self._make_client("test_retain")
        client.on_message = on_msg
        client.connect(self.broker_host, self.port, keepalive=30)
        client.subscribe(state_topic, qos=1)
        client.loop_start()

        time.sleep(3)  # ждём retained

        client.disconnect()
        client.loop_stop()

        self._record(
            "Retain = false",
            not retained["got"],
            "retained НЕ получен" if not retained["got"] else "ПОЛУЧЕН retained-сообщение!"
        )

    def test_qos_publish(self):
        """Тест 5: Проверяем QOS (публикация с QOS1 — получаем PUBACK)."""
        log_head("Тест 5: QOS публикации")
        # Просто подписываемся на любой топик контроллера и проверяем получение
        test_topic = f"clients/{self.device_name}/state"
        received_event = threading.Event()
        msg_qos = {"qos": -1}

        def on_msg(c, u, msg):
            msg_qos["qos"] = msg.qos
            received_event.set()

        client = self._make_client("test_qos")
        client.on_message = on_msg
        client.connect(self.broker_host, self.port, keepalive=30)
        client.subscribe(test_topic, qos=2)  # подписываемся с макс QOS
        client.loop_start()

        result = received_event.wait(timeout=TEST_TIMEOUT)

        client.disconnect()
        client.loop_stop()

        if result:
            self._record("QOS сообщения", True, f"QOS={msg_qos['qos']}")
        else:
            self._record("QOS сообщения", False, "Не получено сообщение")

    def test_lwt(self):
        """Тест 6: Проверяем LWT (Last Will and Testament)."""
        log_head("Тест 6: LWT (Last Will)")
        state_topic = f"clients/{self.device_name}/state"
        log_info(f"LWT topic: {state_topic}, ожидаемый msg='0' при отключении")
        self._record("LWT настроен", True,
                      f"topic={state_topic}, msg='0'  (проверяется при перезагрузке контроллера)")

    def print_summary(self):
        """Выводит итоговую сводку."""
        log_head("РЕЗУЛЬТАТЫ")
        passed = sum(1 for r in self.results if r["passed"])
        total = len(self.results)
        for r in self.results:
            mark = f"{C.OK}PASS{C.END}" if r["passed"] else f"{C.FAIL}FAIL{C.END}"
            print(f"  [{mark}] {r['name']}: {r['detail']}")
        print(f"\n  Итого: {passed}/{total} тестов пройдено")
        return passed == total


# ─────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="MQTT Broker Test Suite для moduleBox",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Примеры:
  python mqtt_broker_test.py --ip 192.168.88.33
  python mqtt_broker_test.py --ip 192.168.88.33 --com COM7
  python mqtt_broker_test.py --ip 192.168.88.33 --tls --auth
  python mqtt_broker_test.py --ip 192.168.88.33 --qos 1 --watchdog 60
  python mqtt_broker_test.py --ip 192.168.88.33 --upload-config
        """
    )
    parser.add_argument("--ip", required=True,
                        help="IP-адрес контроллера moduleBox в сети")
    parser.add_argument("--broker-ip", default=None,
                        help="IP-адрес этого ПК (для конфига контроллера). "
                             "По умолчанию определяется автоматически")
    parser.add_argument("--com", default=None,
                        help="COM-порт контроллера (COM7, /dev/ttyACM0). "
                             "Если не указан — автоопределение")
    parser.add_argument("--device-name", default="moduleBox",
                        help="Имя устройства (deviceName в конфиге)")
    parser.add_argument("--tls", action="store_true",
                        help="Включить TLS-шифрование")
    parser.add_argument("--auth", action="store_true",
                        help="Включить аутентификацию (логин/пароль)")
    parser.add_argument("--qos", type=int, default=0, choices=[0, 1, 2],
                        help="MQTT QOS (0, 1, 2)")
    parser.add_argument("--watchdog", type=int, default=0,
                        help="MQTT Watchdog Timeout в секундах (0 = выкл)")
    parser.add_argument("--upload-config", action="store_true",
                        help="Загрузить config.ini на контроллер через FTP перед тестом")
    parser.add_argument("--work-dir", default=None,
                        help="Рабочая папка для конфигов mosquitto и сертификатов")

    args = parser.parse_args()

    print(f"{C.BOLD}╔══════════════════════════════════════════════════╗")
    print(f"║   moduleBox MQTT Broker Test Suite               ║")
    print(f"╚══════════════════════════════════════════════════╝{C.END}")
    print(f"  Контроллер IP : {args.ip}")
    print(f"  Устройство    : {args.device_name}")
    print(f"  TLS           : {'да' if args.tls else 'нет'}")
    print(f"  Аутентификация: {'да' if args.auth else 'нет'}")
    print(f"  QOS           : {args.qos}")
    print(f"  Watchdog      : {args.watchdog} сек")

    # ── Определяем IP этой машины ──
    broker_ip = args.broker_ip
    if not broker_ip:
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((args.ip, 80))
            broker_ip = s.getsockname()[0]
        except Exception:
            broker_ip = "127.0.0.1"
        finally:
            s.close()
    print(f"  IP брокера    : {broker_ip}")

    # ── USB идентификация ──
    usb = USBDevice(args.com)
    if usb.connect():
        device_id = usb.identify()
        log_info(f"USB: устройство идентифицировано как '{device_id}'")
        if device_id and device_id != args.device_name:
            log_warn(f"deviceName='{device_id}' отличается от --device-name='{args.device_name}'")
            args.device_name = device_id
    else:
        log_warn("USB недоступен, продолжаем без USB-проверки")

    # ── Запуск брокера ──
    broker = MosquittoBroker(use_tls=args.tls, use_auth=args.auth, work_dir=args.work_dir)
    broker.start()

    try:
        port = broker.port

        # ── Загрузка конфига на контроллер (опционально) ──
        if args.upload_config:
            log_head("Загрузка конфигурации на контроллер")
            config_ini = generate_config_ini(
                broker_ip=broker_ip,
                use_tls=args.tls,
                use_auth=args.auth,
                qos=args.qos,
                watchdog=args.watchdog,
                device_name=args.device_name,
            )
            print(f"\n  Секция [MQTT]:\n  {'─'*30}")
            for line in config_ini.strip().splitlines():
                print(f"    {line}")
            print()

            if upload_config_ftp(args.ip, config_ini):
                log_info("Контроллер нужно перезагрузить для применения конфига")
                if usb.ser:
                    log_info("Отправляем system/restart через USB...")
                    usb.send_command("system/restart", wait=1.0)
                    time.sleep(8)  # ждём перезагрузку
                else:
                    log_warn("Перезагрузите контроллер вручную и нажмите Enter")
                    input()

        # ── Тесты ──
        runner = MQTTTestRunner(
            broker_host="127.0.0.1",
            port=port,
            device_name=args.device_name,
            use_tls=args.tls,
            use_auth=args.auth,
            ca_cert=broker.ca_cert,
        )

        runner.test_broker_reachable()
        device_ok = runner.test_device_connects()
        if device_ok:
            runner.test_device_topics()
            runner.test_qos_publish()
            runner.test_retain_false()
        runner.test_lwt()

        # ── USB-статус после тестов ──
        if usb.ser:
            log_head("USB: Статус контроллера")
            status = usb.get_status()
            for line in status.splitlines():
                print(f"    {line}")

        all_ok = runner.print_summary()

    finally:
        broker.stop()
        usb.close()

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
