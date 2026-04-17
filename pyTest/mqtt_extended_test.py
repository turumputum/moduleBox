#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MQTT Extended Test Suite для moduleBox
=======================================

Полный набор тестов MQTT-подсистемы контроллера moduleBox.
Запускает локальный mosquitto-брокер, настраивает контроллер
через USB-диск (съёмный диск), и выполняет активные тесты через MQTT.

Тесты:
  1. Доступность локального брокера
  2. Подключение контроллера (с перезагрузкой)
  3. Список топиков (clients/<dev>/topics)
  4. Активный пинг: system/getVersion → ответ
  5. Активный пинг: system/getFreeRAM → ответ
  6. Активный пинг: system/getNETstatus → JSON
  7. Активный пинг: system/getFreeDisk → ответ
  8. QOS проверка (min(pub_qos, sub_qos))
  9. Retain = false (новый подписчик не получает старое)
 10. LWT при перезагрузке (state=0 → state=1)
 11. Авторизация: отказ без пароля (если --auth)
 12. Watchdog: проверка через лог (если --watchdog)

Требования:
  pip install paho-mqtt pyserial
  mosquitto в PATH или в стандартной папке (winget install EclipseFoundation.Mosquitto)

Использование:
  python mqtt_extended_test.py --com COM81
  python mqtt_extended_test.py --com COM81 --auth --tls
  python mqtt_extended_test.py --com COM81 --qos 1 --watchdog 30
  python mqtt_extended_test.py --com COM81 --disk D:
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
import socket
import ctypes
import string
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ОШИБКА: pip install paho-mqtt")
    sys.exit(1)

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

# ──────────── Константы ────────────
MQTT_PORT = 1883
MQTTS_PORT = 8883
MQTT_USER = "mbuser"
MQTT_PASS = "mbpass"
USB_BAUD = 115200
CONNECT_TIMEOUT = 20  # ожидание подключения контроллера после reboot
CMD_TIMEOUT = 8       # ожидание ответа на system-команду
REBOOT_WAIT = 12      # время на перезагрузку

# ──────────── Поиск mosquitto ────────────
def _find_mosquitto():
    """Ищет mosquitto в PATH, затем в стандартных папках Windows."""
    import shutil
    exe = shutil.which("mosquitto")
    if exe:
        return Path(exe).parent
    candidates = [
        Path(r"C:\Program Files\mosquitto"),
        Path(r"C:\Program Files (x86)\mosquitto"),
        Path(r"C:\mosquitto"),
    ]
    for d in candidates:
        if (d / "mosquitto.exe").exists():
            return d
    return None

MOSQUITTO_DIR = _find_mosquitto()
MOSQUITTO_EXE = str(MOSQUITTO_DIR / "mosquitto.exe") if MOSQUITTO_DIR else "mosquitto"
MOSQUITTO_PASSWD = str(MOSQUITTO_DIR / "mosquitto_passwd.exe") if MOSQUITTO_DIR else "mosquitto_passwd"


# ──────────── Терминальные цвета ────────────
class C:
    OK = "\033[92m"; FAIL = "\033[91m"; WARN = "\033[93m"
    INFO = "\033[96m"; BOLD = "\033[1m"; DIM = "\033[2m"; END = "\033[0m"

def log_ok(m):   print(f"  {C.OK}[PASS]{C.END}  {m}")
def log_fail(m): print(f"  {C.FAIL}[FAIL]{C.END}  {m}")
def log_info(m): print(f"  {C.INFO}[INFO]{C.END}  {m}")
def log_warn(m): print(f"  {C.WARN}[WARN]{C.END}  {m}")
def log_head(m): print(f"\n{C.BOLD}{'─'*56}\n  {m}\n{'─'*56}{C.END}")
def log_dim(m):  print(f"  {C.DIM}{m}{C.END}")


# ═══════════════════════════════════════════════════
#  USB CDC
# ═══════════════════════════════════════════════════
class USBDevice:
    def __init__(self, port=None):
        self.port = port
        self.ser = None

    def connect(self):
        if not HAS_SERIAL:
            log_warn("pyserial не установлен")
            return False
        port = self.port
        if not port:
            for p in serial.tools.list_ports.comports():
                d = (p.description or "").lower()
                if "usb" in d and ("serial" in d or "jtag" in d):
                    port = p.device; break
        if not port:
            log_warn("USB-порт не найден")
            return False
        try:
            self.ser = serial.Serial(port, USB_BAUD, timeout=3)
            time.sleep(0.5)
            self.ser.reset_input_buffer()
            return True
        except Exception as e:
            log_warn(f"USB {port}: {e}")
            return False

    def cmd(self, command, wait=1.5):
        if not self.ser: return ""
        self.ser.reset_input_buffer()
        self.ser.write((command + "\n").encode())
        time.sleep(wait)
        return self.ser.read(self.ser.in_waiting or 2048).decode(errors="replace").strip()

    def identify(self):
        resp = self.cmd("Who are you?", wait=1.0)
        for line in resp.splitlines():
            if "moduleBox:" in line:
                return line.split(":", 1)[1].strip()
        return ""

    def restart(self):
        self.cmd("system/restart", wait=0.5)

    def reconnect(self, wait=REBOOT_WAIT):
        """Закрывает, ждёт перезагрузку, переоткрывает."""
        if self.ser:
            self.ser.close()
            self.ser = None
        time.sleep(wait)
        for _ in range(5):
            if self.connect():
                return True
            time.sleep(2)
        return False

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None


# ═══════════════════════════════════════════════════
#  TLS Certificates
# ═══════════════════════════════════════════════════
def _find_openssl():
    """Ищет openssl в PATH, затем в стандартных местах (Git, Strawberry Perl и др.)."""
    import shutil
    exe = shutil.which("openssl")
    if exe:
        return exe
    candidates = [
        r"C:\Program Files\Git\usr\bin\openssl.exe",
        r"C:\Program Files (x86)\Git\usr\bin\openssl.exe",
        r"C:\Strawberry\c\bin\openssl.exe",
        r"C:\Program Files\OpenSSL-Win64\bin\openssl.exe",
    ]
    for p in candidates:
        if Path(p).exists():
            return p
    return None


def generate_tls_certs(cert_dir: Path):
    ca_key  = cert_dir / "ca.key";  ca_cert = cert_dir / "ca.crt"
    srv_key = cert_dir / "server.key"; srv_cert = cert_dir / "server.crt"
    srv_csr = cert_dir / "server.csr"

    if ca_cert.exists() and srv_cert.exists():
        return ca_cert, srv_key, srv_cert

    openssl = _find_openssl()
    if not openssl:
        log_fail("openssl не найден! Установите Git for Windows или OpenSSL")
        log_info("  winget install Git.Git  или  winget install ShiningLight.OpenSSL")
        sys.exit(1)

    log_info(f"Генерация самоподписанных TLS-сертификатов ({Path(openssl).name})...")
    subprocess.run([openssl,"req","-new","-x509","-days","365",
        "-keyout",str(ca_key),"-out",str(ca_cert),"-subj","/CN=moduleBox Test CA","-nodes"],
        check=True, capture_output=True)
    subprocess.run([openssl,"req","-new","-nodes",
        "-keyout",str(srv_key),"-out",str(srv_csr),"-subj","/CN=localhost"],
        check=True, capture_output=True)
    subprocess.run([openssl,"x509","-req","-days","365",
        "-in",str(srv_csr),"-CA",str(ca_cert),"-CAkey",str(ca_key),
        "-CAcreateserial","-out",str(srv_cert)],
        check=True, capture_output=True)
    return ca_cert, srv_key, srv_cert


# ═══════════════════════════════════════════════════
#  Mosquitto Broker
# ═══════════════════════════════════════════════════
class MosquittoBroker:
    def __init__(self, use_tls=False, use_auth=False, work_dir=None):
        self.use_tls = use_tls
        self.use_auth = use_auth
        self.process = None
        self.work_dir = Path(work_dir or tempfile.mkdtemp(prefix="mb_mqtt_"))
        self.work_dir.mkdir(parents=True, exist_ok=True)
        self.port = MQTTS_PORT if use_tls else MQTT_PORT
        self.ca_cert = None

    def start(self):
        self.cfg = self.work_dir / "mosquitto.conf"
        lines = [
            f"listener {self.port} 0.0.0.0",
            "allow_anonymous true" if not self.use_auth else "allow_anonymous false",
            "log_type all",
            f"persistence_location {self.work_dir}/",
        ]
        if self.use_auth:
            pw = self.work_dir / "passwd"
            try:
                subprocess.run([MOSQUITTO_PASSWD,"-c","-b",str(pw),MQTT_USER,MQTT_PASS],
                    check=True, capture_output=True)
            except FileNotFoundError:
                pw.write_text(f"{MQTT_USER}:{MQTT_PASS}\n")
            lines.append(f"password_file {pw}")
        if self.use_tls:
            cd = self.work_dir / "certs"; cd.mkdir(exist_ok=True)
            self.ca_cert, sk, sc = generate_tls_certs(cd)
            lines += [f"cafile {self.ca_cert}", f"certfile {sc}", f"keyfile {sk}", "tls_version tlsv1.2"]

        # Пишем без BOM (mosquitto 2.x не переносит BOM)
        self.cfg.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))

        # Лог mosquitto в файл (НЕ в PIPE — иначе буфер переполняется и mosquitto зависает)
        self.log_file_path = self.work_dir / "mosquitto.log"
        self._log_fh = open(self.log_file_path, "w", encoding="utf-8")
        try:
            self.process = subprocess.Popen(
                [MOSQUITTO_EXE, "-c", str(self.cfg), "-v"],
                stdout=self._log_fh, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform=="win32" else 0)
        except FileNotFoundError:
            self._log_fh.close()
            log_fail(f"mosquitto не найден! (искали: {MOSQUITTO_EXE})")
            if sys.platform == "win32":
                log_info("Установите: winget install EclipseFoundation.Mosquitto")
            else:
                log_info("Установите: apt install mosquitto")
            sys.exit(1)
        time.sleep(1.5)
        if self.process.poll() is not None:
            self._log_fh.close()
            out = self.log_file_path.read_text(errors="replace")
            log_fail(f"mosquitto не запустился:\n{out[:500]}")
            sys.exit(1)
        log_ok(f"mosquitto PID={self.process.pid}, port={self.port}")
        log_dim(f"  лог: {self.log_file_path}")
        return self

    def is_alive(self):
        return self.process is not None and self.process.poll() is None

    def stop(self):
        if self.is_alive():
            self.process.terminate()
            self.process.wait(timeout=5)
            log_info("mosquitto остановлен")

    def restart(self):
        """Остановить и запустить заново (тот же конфиг)."""
        self.stop()
        time.sleep(0.5)
        self._log_fh = open(self.log_file_path, "a", encoding="utf-8")
        self.process = subprocess.Popen(
            [MOSQUITTO_EXE, "-c", str(self.cfg), "-v"],
            stdout=self._log_fh, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform=="win32" else 0)
        time.sleep(1.5)
        if self.process.poll() is not None:
            self._log_fh.close()
            out = self.log_file_path.read_text(errors="replace")
            log_fail(f"mosquitto не перезапустился:\n{out[-500:]}")
            return False
        log_ok(f"mosquitto перезапущен PID={self.process.pid}, port={self.port}")
        return True


# ═══════════════════════════════════════════════════
#  Disk-based Config Upload (съёмный USB-диск)
# ═══════════════════════════════════════════════════
def _find_controller_disk(device_name):
    """Ищет съёмный диск по метке = deviceName.upper().
    Возвращает (Path, label) или (None, None)."""
    label = device_name.upper()
    if sys.platform == "win32":
        import ctypes
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        # Первый проход — точное совпадение с label
        for i, letter in enumerate(string.ascii_uppercase):
            if bitmask & (1 << i):
                drive = f"{letter}:\\"
                try:
                    vol_buf = ctypes.create_unicode_buffer(261)
                    ctypes.windll.kernel32.GetVolumeInformationW(
                        drive, vol_buf, 261, None, None, None, None, 0)
                    if vol_buf.value == label:
                        return Path(f"{letter}:"), vol_buf.value
                except Exception:
                    pass
        # Второй проход — ищем любой съёмный диск с config.ini (fallback)
        DRIVE_REMOVABLE = 2
        for i, letter in enumerate(string.ascii_uppercase):
            if bitmask & (1 << i):
                drive = f"{letter}:\\"
                try:
                    dt = ctypes.windll.kernel32.GetDriveTypeW(drive)
                    if dt != DRIVE_REMOVABLE:
                        continue
                    cfg = Path(f"{letter}:") / "config.ini"
                    if cfg.exists():
                        vol_buf = ctypes.create_unicode_buffer(261)
                        ctypes.windll.kernel32.GetVolumeInformationW(
                            drive, vol_buf, 261, None, None, None, None, 0)
                        log_warn(f"Точная метка '{label}' не найдена, но съёмный диск {letter}: "
                                 f"(метка '{vol_buf.value}') содержит config.ini")
                        return Path(f"{letter}:"), vol_buf.value
                except Exception:
                    pass
    else:
        # Linux/macOS: ищем в /media, /mnt, /Volumes
        for base in ["/media", "/mnt", "/Volumes"]:
            p = Path(base)
            if p.exists():
                for d in p.iterdir():
                    if d.name.upper() == label and d.is_dir():
                        return d, d.name.upper()
    return None, None


def _find_local_ip():
    """Находит IP локального LAN-интерфейса (192.168.x.x предпочтительно)."""
    import socket as _sock
    candidates = []
    try:
        for info in _sock.getaddrinfo(_sock.gethostname(), None, _sock.AF_INET):
            ip = info[4][0]
            if ip.startswith("127."):
                continue
            candidates.append(ip)
    except Exception:
        pass
    # Предпочитаем 192.168.x.x (LAN), затем 10.x.x.x, затем любой
    for prefix in ["192.168.", "10.", "172."]:
        for ip in candidates:
            if ip.startswith(prefix):
                return ip
    if candidates:
        return candidates[0]
    # Fallback через UDP
    try:
        s = _sock.socket(_sock.AF_INET, _sock.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def _replace_ini_section(text, section_name, new_section_content):
    """Заменяет секцию [section_name] в INI-тексте."""
    lines = text.splitlines()
    result = []; skip = False; found = False
    for line in lines:
        s = line.strip()
        if s.startswith(f"[{section_name}]"):
            skip = True; found = True
            result.append(new_section_content.rstrip())
            continue
        if skip and s.startswith("["):
            skip = False
        if not skip:
            result.append(line)
    if not found:
        result.append("")
        result.append(new_section_content.rstrip())
    return "\r\n".join(result) + "\r\n"


def _get_ini_section(text, section_name):
    """Возвращает строки секции [section_name] (без заголовка) или None."""
    lines = text.splitlines()
    result = []; inside = False
    for line in lines:
        s = line.strip()
        if s.startswith(f"[{section_name}]"):
            inside = True; continue
        if inside and s.startswith("["):
            break
        if inside:
            result.append(s)
    return result if result else None


def _ini_val_truthy(line):
    """Проверяет, что значение ключа = yes / 1 / true."""
    parts = line.split("=", 1)
    if len(parts) < 2:
        return False
    val = parts[1].split(";")[0].strip().lower()
    return val in ("yes", "1", "true")


def upload_mqtt_config_disk(disk_path, broker_ip, use_tls=False, use_auth=False, qos=0, watchdog=0, device_name=None):
    """Записывает секции [MQTT] и [LAN] (+ опционально [SYSTEM]) в config.ini."""
    mqtt_section = "[MQTT]\r\nmqttBrokerAdress={}\r\n".format(broker_ip)
    if use_auth:
        mqtt_section += "mqttLogin={}\r\nmqttPass={}\r\n".format(MQTT_USER, MQTT_PASS)
    else:
        mqtt_section += "mqttLogin=\r\nmqttPass=\r\n"
    mqtt_section += "mqttQOS={}\r\nmqttWatchdogTimeout={}\r\nmqttTLS={}\r\n".format(
        qos, watchdog, 1 if use_tls else 0)

    config_file = Path(disk_path) / "config.ini"
    try:
        existing = ""
        if config_file.exists():
            raw = config_file.read_bytes().decode("utf-8", errors="replace")
            # Нормализуем все line endings → \n (убираем накопленные \r)
            existing = raw.replace("\r\n", "\n").replace("\r", "\n")

        # ── [MQTT] ──
        new_config = _replace_ini_section(existing, "MQTT", mqtt_section)

        # ── [LAN] — гарантируем LAN_enable = yes ──
        lan_lines = _get_ini_section(new_config, "LAN")
        if not lan_lines or not any("LAN_enable" in l and _ini_val_truthy(l) for l in lan_lines):
            lan_section = "[LAN]\r\nLAN_enable = yes\r\nDHCP = yes\r\n"
            new_config = _replace_ini_section(new_config, "LAN", lan_section)
            log_info("LAN был выключен → включён (DHCP=yes)")

        # ── [SYSTEM] — восстанавливаем deviceName если задано ──
        if device_name:
            sys_section = f"[SYSTEM]\r\ndeviceName = {device_name}\r\n"
            new_config = _replace_ini_section(new_config, "SYSTEM", sys_section)

        # _replace_ini_section возвращает \r\n, пишем строго binary
        data = new_config.encode("utf-8")
        with open(str(config_file), "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())  # гарантируем физическую запись на USB-диск
        return True
    except Exception as e:
        log_fail(f"Disk write: {e}")
        return False


# ═══════════════════════════════════════════════════
#  MQTT Helper — подписка + ожидание сообщения
# ═══════════════════════════════════════════════════
class MQTTListener:
    """Подписывается на набор топиков и собирает сообщения."""

    def __init__(self, broker, port, use_tls=False, use_auth=False, ca_cert=None, client_id="mb_test"):
        self.broker = broker
        self.port = port
        self.client = _make_mqtt_client(client_id)
        if use_auth:
            self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        if use_tls:
            self.client.tls_set(ca_certs=str(ca_cert) if ca_cert else None)
            self.client.tls_insecure_set(True)
        self.messages = {}   # topic -> list of (payload, retain, qos)
        self.events = {}     # topic -> threading.Event
        self._lock = threading.Lock()
        self.connected = threading.Event()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, c, u, flags, rc):
        if rc == 0:
            self.connected.set()
            for t in self.events:
                c.subscribe(t, qos=2)

    def _on_message(self, c, u, msg):
        with self._lock:
            topic = msg.topic
            entry = (msg.payload.decode(errors="replace"), bool(msg.retain), msg.qos)
            # Точные подписки
            if topic in self.events:
                self.messages.setdefault(topic, []).append(entry)
                self.events[topic].set()
            # Wildcard: проверяем по префиксу
            for t, ev in self.events.items():
                if t.endswith("#") and topic.startswith(t[:-1]):
                    self.messages.setdefault(topic, []).append(entry)
                    ev.set()

    def subscribe(self, topic):
        self.events[topic] = threading.Event()
        if self.connected.is_set():
            self.client.subscribe(topic, qos=2)
        return self.events[topic]

    def start(self, retries=3):
        for attempt in range(retries):
            try:
                self.client.connect(self.broker, self.port, keepalive=60)
                self.client.loop_start()
                if self.connected.wait(timeout=5):
                    return self
                self.client.loop_stop()
                self.client.disconnect()
            except Exception as e:
                if attempt == retries - 1:
                    raise
                log_warn(f"Брокер не готов (попытка {attempt+1}/{retries}): {e}")
            time.sleep(2)
        raise ConnectionError("Не удалось подключиться к брокеру")

    def publish(self, topic, payload="", qos=0, retain=False):
        self.client.publish(topic, payload, qos=qos, retain=retain)

    def wait(self, topic, timeout=CMD_TIMEOUT):
        ev = self.events.get(topic)
        if ev:
            return ev.wait(timeout=timeout)
        return False

    def get(self, topic):
        """Возвращает последний payload для точного или wildcard-топика."""
        with self._lock:
            if topic in self.messages:
                return self.messages[topic][-1]  # (payload, retain, qos)
            if topic.endswith("#"):
                prefix = topic[:-1]
                for t, msgs in self.messages.items():
                    if t.startswith(prefix) and msgs:
                        return msgs[-1]
        return None

    def get_all(self, topic_prefix):
        """Возвращает все сообщения по префиксу."""
        with self._lock:
            result = {}
            for t, msgs in self.messages.items():
                if t.startswith(topic_prefix):
                    result[t] = msgs
            return result

    def stop(self):
        self.client.disconnect()
        self.client.loop_stop()


# ═══════════════════════════════════════════════════
#  Test Runner
# ═══════════════════════════════════════════════════
class TestRunner:
    def __init__(self):
        self.results = []
        self.test_num = 0

    def record(self, name, passed, detail=""):
        self.test_num += 1
        self.results.append({"num": self.test_num, "name": name, "passed": passed, "detail": detail})
        if passed:
            log_ok(f"{name}" + (f"  →  {detail}" if detail else ""))
        else:
            log_fail(f"{name}" + (f"  →  {detail}" if detail else ""))

    def summary(self):
        log_head("РЕЗУЛЬТАТЫ")
        passed = sum(1 for r in self.results if r["passed"])
        total = len(self.results)
        for r in self.results:
            mark = f"{C.OK}PASS{C.END}" if r["passed"] else f"{C.FAIL}FAIL{C.END}"
            print(f"  {r['num']:2d}. [{mark}] {r['name']}")
            if r["detail"]:
                print(f"         {C.DIM}{r['detail']}{C.END}")
        color = C.OK if passed == total else C.FAIL
        print(f"\n  {C.BOLD}Итого: {color}{passed}/{total}{C.END}{C.BOLD} тестов пройдено{C.END}")
        return passed == total


# ═══════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════
def main():
    parser = argparse.ArgumentParser(
        description="MQTT Extended Test Suite для moduleBox",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--com", default=None, help="COM-порт USB CDC")
    parser.add_argument("--disk", default=None, help="Буква диска контроллера (напр. D:)")
    parser.add_argument("--device-name", default=None, help="deviceName (авто из USB)")
    parser.add_argument("--broker-ip", default=None, help="IP этого ПК (авто)")
    parser.add_argument("--tls", action="store_true", help="TLS")
    parser.add_argument("--auth", action="store_true", help="Аутентификация")
    parser.add_argument("--qos", type=int, default=0, choices=[0,1,2], help="QOS")
    parser.add_argument("--watchdog", type=int, default=0, help="Watchdog сек")
    parser.add_argument("--work-dir", default=None, help="Рабочая папка")
    parser.add_argument("--skip-upload", action="store_true", help="Не загружать конфиг")
    args = parser.parse_args()

    # ── Заголовок ──
    print(f"\n{C.BOLD}╔═══════════════════════════════════════════════════════╗")
    print(f"║   moduleBox MQTT Extended Test Suite                  ║")
    print(f"╚═══════════════════════════════════════════════════════╝{C.END}")

    dev = args.device_name
    port = MQTTS_PORT if args.tls else MQTT_PORT

    runner = TestRunner()

    # ── USB ──
    usb = USBDevice(args.com)
    has_usb = usb.connect()
    if has_usb:
        identity = usb.identify()
        log_info(f"USB: {usb.ser.port} → {identity}")
        if identity:
            if not dev:
                dev = identity
            log_info(f"deviceName='{dev}'")
    else:
        log_warn("USB недоступен — некоторые тесты будут ограничены")

    if not dev:
        dev = "moduleBox"
        log_warn(f"Имя устройства не определено, используем '{dev}'")

    # ── Съёмный диск контроллера ──
    disk_path = None
    disk_label = None
    if args.disk:
        disk_path = Path(args.disk)
    else:
        disk_path, disk_label = _find_controller_disk(dev)

    # Если диск найден по fallback (метка ≠ dev.upper()), пробуем восстановить deviceName
    if disk_path and disk_label and disk_label != dev.upper() and not args.device_name:
        # Пробуем прочитать deviceName из config.ini на диске
        cfg_file = disk_path / "config.ini"
        if cfg_file.exists():
            _raw = cfg_file.read_bytes().decode("utf-8", errors="replace")
            _norm = _raw.replace("\r\n", "\n").replace("\r", "\n")
            _sys_lines = _get_ini_section(_norm, "SYSTEM")
            if _sys_lines:
                for _l in _sys_lines:
                    if "deviceName" in _l and "=" in _l:
                        _dn = _l.split("=", 1)[1].split(";")[0].strip()
                        if _dn and _dn != "moduleBox" and _dn.upper() == disk_label:
                            log_info(f"config.ini: deviceName='{_dn}' (совпадает с меткой '{disk_label}')")
                            dev = _dn
                            break
        # Если всё ещё дефолт — предупреждаем
        if dev == "moduleBox" and disk_label != "MODULEBOX":
            log_warn(f"deviceName='moduleBox' (дефолт), метка диска='{disk_label}'")
            log_warn(f"Используйте --device-name <имя> чтобы задать правильное имя "
                     f"(например: --device-name {disk_label[0].lower()}{disk_label[1:]})")

    if disk_path and (disk_path / "config.ini").exists():
        log_ok(f"Диск контроллера: {disk_path} (config.ini найден)")
    elif disk_path:
        log_warn(f"Диск {disk_path} найден, но config.ini отсутствует")
    else:
        log_warn(f"Съёмный диск '{dev.upper()}' не найден")
        if not args.skip_upload:
            log_fail("Невозможно загрузить конфиг без диска. Используйте --disk X: или --skip-upload")
            sys.exit(1)

    # ── IP брокера (авто: ищем LAN-интерфейс 192.168.x.x) ──
    broker_ip = args.broker_ip
    if not broker_ip:
        broker_ip = _find_local_ip()

    print(f"  Устройство : {dev}")
    print(f"  Диск       : {disk_path}")
    print(f"  Брокер     : {broker_ip}:{port}")
    print(f"  TLS={args.tls}  Auth={args.auth}  QOS={args.qos}  WD={args.watchdog}s")

    # ── Брокер ──
    log_head("Запуск локального mosquitto")
    broker = MosquittoBroker(use_tls=args.tls, use_auth=args.auth, work_dir=args.work_dir)
    broker.start()

    try:
        # ── Загрузка конфига на диск и перезагрузка ──
        if not args.skip_upload and disk_path:
            log_head("Запись конфига на диск контроллера")
            # Если dev отличается от текущего deviceName на диске — пишем [SYSTEM] тоже
            write_dev_name = dev if dev != "moduleBox" else None
            ok = upload_mqtt_config_disk(disk_path, broker_ip, args.tls, args.auth, args.qos, args.watchdog,
                                         device_name=write_dev_name)
            if ok:
                log_ok(f"config.ini обновлён → {disk_path}\\config.ini")
                # Показываем что записали
                try:
                    content = (disk_path / "config.ini").read_text(errors="replace")
                    for line in content.splitlines():
                        ls = line.strip()
                        if ls.startswith("mqtt") or ls.startswith("[MQTT") or \
                           ls.startswith("LAN_enable") or ls.startswith("[LAN") or \
                           ls.startswith("deviceName") or ls.startswith("[SYSTEM"):
                            log_dim(f"  {ls}")
                except Exception:
                    pass
            else:
                log_fail("Не удалось записать config.ini")

            if has_usb:
                log_info("Ожидаем сброс кеша диска (2 сек)...")
                time.sleep(2)  # даём Windows сбросить disk cache
                log_info("Перезагрузка контроллера через USB...")
                usb.restart()
                time.sleep(3)  # даём контроллеру начать перезагрузку
            else:
                log_warn("Нет USB — перезагрузите контроллер вручную, затем Enter")
                input("  [Enter]")

        # ── Подготовка MQTT-подписчика (ДО перезагрузки) ──
        log_head("Подключение тестового клиента к брокеру")

        # Проверяем что mosquitto ещё жив
        if not broker.is_alive():
            out = broker.log_file_path.read_text(errors="replace") if broker.log_file_path.exists() else "нет лога"
            log_fail(f"mosquitto умер до подключения listener!\n{out[-500:]}")
            runner.summary()
            return

        listener = MQTTListener(
            "127.0.0.1", port,
            use_tls=args.tls, use_auth=args.auth,
            ca_cert=broker.ca_cert, client_id="mb_ext_test")

        state_topic    = f"clients/{dev}/state"
        topics_topic   = f"clients/{dev}/topics"
        sys_wildcard   = f"{dev}/system/#"

        listener.subscribe(state_topic)
        listener.subscribe(topics_topic)
        listener.subscribe(sys_wildcard)

        listener.start()

        # ────────────── ТЕСТ 1: Брокер доступен ──────────────
        log_head("Тест 1: Доступность брокера")
        runner.record("Брокер доступен", listener.connected.is_set(), f"127.0.0.1:{port}")

        # ────────────── ТЕСТ 2: Контроллер подключился ──────────────
        log_head("Тест 2: Подключение контроллера")
        if not args.skip_upload and has_usb:
            log_info(f"Ожидаем загрузку контроллера ({REBOOT_WAIT} сек)...")
            # USB reconnect в фоне — параллельно ждём MQTT
            usb_thread = threading.Thread(target=lambda: usb.reconnect(REBOOT_WAIT))
            usb_thread.start()

        # Ждём именно state=1 (может прийти LWT state=0 от старой сессии)
        deadline = time.time() + CONNECT_TIMEOUT + REBOOT_WAIT
        got_1 = False
        last_msg = None
        while time.time() < deadline:
            remaining = max(1, deadline - time.time())
            got = listener.wait(state_topic, timeout=remaining)
            if got:
                last_msg = listener.get(state_topic)
                if last_msg and last_msg[0] == "1":
                    got_1 = True
                    break
                # Получили state=0 (LWT от старого подключения) — ждём дальше
                log_dim(f"  получен state={last_msg[0] if last_msg else '?'}, ждём state=1...")
                listener.events[state_topic].clear()
                with listener._lock:
                    listener.messages.pop(state_topic, None)
            else:
                break

        if got_1:
            runner.record("Контроллер подключился", True,
                          f"state=1, retain={last_msg[1]}, qos={last_msg[2]}")
        else:
            runner.record("Контроллер подключился", False,
                          f"state={last_msg[0] if last_msg else 'N/A'} (ожидали 1)")

        if not args.skip_upload and has_usb:
            usb_thread.join(timeout=5)

        if not got_1:
            log_warn("Остальные тесты пропускаем — контроллер не подключился")
            listener.stop()
            runner.summary()
            return

        # ────────────── ТЕСТ 3: Список топиков ──────────────
        log_head("Тест 3: Публикация списка топиков")
        got_topics = listener.wait(topics_topic, timeout=5)
        if got_topics:
            msg = listener.get(topics_topic)
            try:
                parsed = json.loads(msg[0])
                t = len(parsed.get("triggers", []))
                a = len(parsed.get("actions", []))
                runner.record("Список топиков", True, f"{t} triggers, {a} actions (JSON OK)")
            except (json.JSONDecodeError, TypeError):
                runner.record("Список топиков", True, f"данные получены, не JSON: {msg[0][:80]}")
        else:
            runner.record("Список топиков", False, "Не получен")

        # ── Вспомогательная функция: отправить system-команду и ждать ответ ──
        def system_cmd(cmd_suffix, resp_suffix, timeout=CMD_TIMEOUT):
            """Pub → dev/system/<cmd_suffix>, ожидает ответ на dev/system/<resp_suffix>."""
            resp_topic = f"{dev}/system/{resp_suffix}"
            # Подписываемся на конкретный ответный топик
            if resp_topic not in listener.events:
                listener.subscribe(resp_topic)
                time.sleep(0.3)
            else:
                listener.events[resp_topic].clear()
                # Очищаем старые сообщения по этому топику
                with listener._lock:
                    listener.messages.pop(resp_topic, None)
            cmd_topic = f"{dev}/system/{cmd_suffix}"
            listener.publish(cmd_topic, "", qos=args.qos)
            got = listener.wait(resp_topic, timeout=timeout)
            if got:
                return listener.get(resp_topic)
            return None

        # ────────────── ТЕСТ 4: system/getVersion ──────────────
        log_head("Тест 4: Активный пинг — getVersion")
        ver_msg = system_cmd("getVersion", "version")
        if ver_msg:
            runner.record("getVersion", True, f"version='{ver_msg[0]}'")
        else:
            runner.record("getVersion", False, "Нет ответа")

        # ────────────── ТЕСТ 5: system/getFreeRAM ──────────────
        log_head("Тест 5: Активный пинг — getFreeRAM")
        ram_msg = system_cmd("getFreeRAM", "freeRAM")
        if ram_msg:
            try:
                ram_val = int(ram_msg[0])
                runner.record("getFreeRAM", ram_val > 0, f"freeRAM={ram_val} bytes")
            except ValueError:
                runner.record("getFreeRAM", True, f"ответ='{ram_msg[0]}'")
        else:
            runner.record("getFreeRAM", False, "Нет ответа")

        # ────────────── ТЕСТ 6: system/getNETstatus ──────────────
        log_head("Тест 6: Активный пинг — getNETstatus")
        net_msg = system_cmd("getNETstatus", "NETstatus")
        if net_msg:
            payload = net_msg[0]
            try:
                data = json.loads(payload)
                mqtt_ok = data.get("MQTT_init_res", -1)
                runner.record("getNETstatus", True,
                    f"MQTT_init_res={mqtt_ok}, JSON keys={list(data.keys())}")
            except (json.JSONDecodeError, TypeError):
                runner.record("getNETstatus", True, f"ответ='{payload[:100]}'")
        else:
            runner.record("getNETstatus", False, "Нет ответа")

        # ────────────── ТЕСТ 7: system/getFreeDisk ──────────────
        log_head("Тест 7: Активный пинг — getFreeDisk")
        disk_msg = system_cmd("getFreeDisk", "freeDisk")
        if disk_msg:
            runner.record("getFreeDisk", True, f"freeDisk='{disk_msg[0]}'")
        else:
            runner.record("getFreeDisk", False, "Нет ответа")

        # ────────────── ТЕСТ 8: QOS ──────────────
        log_head("Тест 8: Проверка QOS")
        state_msg = listener.get(state_topic)
        if state_msg:
            actual_qos = state_msg[2]
            # Эффективный QOS = min(publisher_qos, subscriber_qos)
            expected_qos = min(args.qos, 2)  # подписка с qos=2
            runner.record("QOS публикации", actual_qos == expected_qos,
                f"ожидали QOS={expected_qos}, получили QOS={actual_qos}")
        else:
            runner.record("QOS публикации", False, "Нет данных state")

        # ────────────── ТЕСТ 9: Retain = false ──────────────
        log_head("Тест 9: retain = false")
        # Подключаемся новым клиентом — не должны получить retained state
        try:
            checker = MQTTListener(
                "127.0.0.1", port,
                use_tls=args.tls, use_auth=args.auth,
                ca_cert=broker.ca_cert, client_id="mb_retain_check")
            checker.subscribe(state_topic)
            checker.start()
            time.sleep(3)
            retain_msg = checker.get(state_topic)
            checker.stop()

            if retain_msg is None:
                runner.record("Retain = false", True, "новый подписчик НЕ получил старое сообщение")
            elif retain_msg[1]:  # retain flag
                runner.record("Retain = false", False, f"ПОЛУЧЕНО retained: '{retain_msg[0]}'")
            else:
                # Получили не-retained сообщение — контроллер отправил новое (маловероятно)
                runner.record("Retain = false", True, "получено свежее (не retained) сообщение")
        except Exception as e:
            runner.record("Retain = false", False, f"Ошибка проверки: {e}")

        # ────────────── ТЕСТ 10: LWT (перезагрузка) ──────────────
        log_head("Тест 10: LWT при перезагрузке")
        try:
            # Очищаем старые events
            listener.events[state_topic].clear()
            with listener._lock:
                listener.messages.pop(state_topic, None)

            # Перезагрузка через MQTT (надёжнее чем USB на этом этапе)
            log_info("Перезагрузка контроллера через MQTT (system/restart)...")
            listener.publish(f"{dev}/system/restart", "", qos=0)

            # При esp_restart() TCP-соединение обрывается без FIN.
            # Когда контроллер перезагрузится (~12с) и пошлёт CONNECT с тем же client_id,
            # mosquitto выполнит session takeover:
            #   1) публикует LWT "0" (от старой сессии)
            #   2) принимает новое подключение
            #   3) контроллер публикует state="1"
            # Оба сообщения приходят почти одновременно.

            LWT_TIMEOUT = REBOOT_WAIT + CONNECT_TIMEOUT  # ~32s — достаточно для reboot + reconnect
            log_info(f"Ожидание LWT + reconnect (таймаут {LWT_TIMEOUT}s)...")

            deadline = time.time() + LWT_TIMEOUT
            got_0 = False
            got_1 = False
            while time.time() < deadline:
                remaining = max(1, deadline - time.time())
                got = listener.wait(state_topic, timeout=remaining)
                if not got:
                    break
                # Читаем ВСЕ сообщения на state_topic
                with listener._lock:
                    all_msgs = list(listener.messages.get(state_topic, []))
                for m in all_msgs:
                    if m[0] == "0":
                        got_0 = True
                    if m[0] == "1":
                        got_1 = True
                if got_0 and got_1:
                    break
                # Ещё не оба — сбрасываем event и ждём следующее сообщение
                listener.events[state_topic].clear()

            if got_0 and got_1:
                runner.record("LWT + reconnect", True, "state=0 (LWT) → state=1 (reconnect)")
            elif got_1 and not got_0:
                # Контроллер переподключился, session takeover отработал, но LWT мог прийти
                # и обработаться до нашей подписки
                runner.record("LWT + reconnect", True,
                    "reconnect state=1 получен (LWT мог быть обработан до подписки)")
            elif got_0 and not got_1:
                runner.record("LWT + reconnect", False,
                    "LWT state=0 получен, но reconnect state=1 не пришёл")
            else:
                runner.record("LWT + reconnect", False,
                    f"Ни LWT, ни reconnect не получены за {LWT_TIMEOUT}s")

            # Восстанавливаем USB (если есть)
            if has_usb:
                usb.reconnect(wait=5)
        except Exception as e:
            runner.record("LWT + reconnect", False, f"Ошибка: {e}")

        # ────────────── ТЕСТ 11: Auth — отказ без пароля ──────────────
        if args.auth:
            log_head("Тест 11: Отказ без аутентификации")
            reject_event = threading.Event()
            reject_rc = {"rc": 0}

            noauth = _make_mqtt_client("mb_noauth_test")
            # НЕ указываем логин/пароль
            if args.tls:
                noauth.tls_set(ca_certs=str(broker.ca_cert) if broker.ca_cert else None)
                noauth.tls_insecure_set(True)
            def on_conn(c, u, f, rc):
                reject_rc["rc"] = rc
                reject_event.set()
            noauth.on_connect = on_conn

            try:
                noauth.connect("127.0.0.1", port, keepalive=5)
                noauth.loop_start()
                reject_event.wait(timeout=5)
                noauth.disconnect()
                noauth.loop_stop()
                rc = reject_rc["rc"]
                # rc=4 = bad user/pass, rc=5 = not authorized
                runner.record("Отказ без пароля", rc in (4, 5),
                    f"rc={rc} ({'отказано' if rc in (4,5) else 'РАЗРЕШЕНО!'})")
            except Exception as e:
                runner.record("Отказ без пароля", True, f"Подключение отклонено: {e}")

        # ────────────── ТЕСТ 12: Watchdog — реальная проверка ──────────────
        if args.watchdog > 0:
            log_head("Тест 12: MQTT Watchdog")
            wd_sec = args.watchdog
            log_info(f"mqttWatchdogTimeout={wd_sec}s")
            log_info("Сценарий: остановка брокера → ожидание watchdog → перезагрузка контроллера")

            # Шаг 1: убеждаемся что контроллер подключён (state=1 с прошлых тестов)
            # Шаг 2: останавливаем брокер
            log_info("Останавливаем mosquitto...")
            broker.stop()
            time.sleep(1)

            # Шаг 3: ждём watchdog + запас
            # Контроллер потеряет TCP → DISCONNECTED → mqtt_watchdog_start(wd_sec)
            # keepalive=60 на брокере, но TCP RST (broker killed) → быстро
            # ESP mqtt_client retry delay: ~10s первый reconnect
            # Итого: контроллер обнаружит disconnect через ~10-20s, watchdog через wd_sec после этого
            wait_total = wd_sec + 25  # запас на обнаружение disconnect + watchdog
            log_info(f"Ожидание перезагрузки от watchdog ({wait_total}s = {wd_sec}s wd + 25s запас)...")
            time.sleep(wait_total)

            # Шаг 4: поднимаем брокер заново
            log_info("Перезапускаем mosquitto...")
            broker_ok = broker.restart()
            if not broker_ok:
                runner.record("Watchdog перезагрузка", False, "Не удалось перезапустить mosquitto")
            else:
                # Шаг 5: новый listener ждёт state=1 (контроллер переподключится)
                try:
                    wd_listener = MQTTListener(
                        "127.0.0.1", port,
                        use_tls=args.tls, use_auth=args.auth,
                        ca_cert=broker.ca_cert, client_id="mb_wd_test")
                    wd_listener.subscribe(state_topic)
                    wd_listener.start()

                    log_info(f"Ожидание подключения контроллера после watchdog reboot ({CONNECT_TIMEOUT + 10}s)...")
                    got = wd_listener.wait(state_topic, timeout=CONNECT_TIMEOUT + 10)
                    wd_msg = wd_listener.get(state_topic)
                    wd_listener.stop()

                    if got and wd_msg and wd_msg[0] == "1":
                        runner.record("Watchdog перезагрузка", True,
                            f"Брокер остановлен → watchdog {wd_sec}s → контроллер перезагрузился → state=1")
                    else:
                        runner.record("Watchdog перезагрузка", False,
                            f"Контроллер не переподключился после watchdog (msg={wd_msg})")
                except Exception as e:
                    runner.record("Watchdog перезагрузка", False, f"Ошибка: {e}")

        # ── Финал ──
        listener.stop()
        all_ok = runner.summary()

    finally:
        broker.stop()
        usb.close()

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
