#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
UDP Test Suite для moduleBox
============================

Проверяет UDP-интерфейс контроллера: приём команд, исполнение system-команд,
обратную отправку событий, crosslink и поведение портов по умолчанию.

Логика теста — замкнутая петля поверх UDP:
  тестер  --(system/getVersion)-->  контроллер  --(<dev>/system/version:..)-->  тестер
Команды можно слать broadcast'ом (255.255.255.255), поэтому IP устройства
знать заранее не нужно — он вычисляется из адреса-источника ответа.

ПРЕДУСЛОВИЕ (для базового набора без --reconfig):
  контроллер уже настроен и в сети, в config.ini задано:
     [UDP]
     serverAdress = <IP этого ПК>
     serverPort   = <--listen-port, по умолчанию 9000>
     myPort       = <--cmd-port,   по умолчанию 9000>   ; либо не задан (дефолт 9000)
  То есть исходящий UDP контроллера направлен на этот ПК.

С флагом --reconfig (+ --disk/--com) скрипт сам перезапишет секцию [UDP]
на съёмном диске контроллера и перезагрузит его через USB — это позволяет
проверить регрессии #1 (дефолтный порт, работа без myPort) и #5 (невалидный IP).

Использование:
  python udpTest.py                                 # базовый набор, broadcast
  python udpTest.py --device-ip 192.168.1.50        # unicast на конкретный IP
  python udpTest.py --crosslink "remote/btn:1" --crosslink-expect "version"
  python udpTest.py --reconfig --com COM81           # + регрессии с реконфигом

Требования: только стандартная библиотека (socket). Для --reconfig — pyserial.
"""

import argparse
import json
import os
import socket
import sys
import threading
import time
from pathlib import Path

# ──────────── Константы ────────────
DEFAULT_PORT = 9000        # дефолт DEFAULT_UDP_PORT в прошивке (udplink.c)
RECV_TIMEOUT = 6.0         # ожидание ответа на команду, сек
USB_BAUD = 115200
REBOOT_WAIT = 14           # время на перезагрузку контроллера, сек

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False


# ──────────── Терминальные цвета ────────────
class C:
    OK = "\033[92m"; FAIL = "\033[91m"; WARN = "\033[93m"
    INFO = "\033[96m"; BOLD = "\033[1m"; DIM = "\033[2m"; END = "\033[0m"

def log_ok(m):   print(f"  {C.OK}[PASS]{C.END}  {m}")
def log_fail(m): print(f"  {C.FAIL}[FAIL]{C.END}  {m}")
def log_info(m): print(f"  {C.INFO}[INFO]{C.END}  {m}")
def log_warn(m): print(f"  {C.WARN}[WARN]{C.END}  {m}")
def log_head(m): print(f"\n{C.BOLD}{'-'*56}\n  {m}\n{'-'*56}{C.END}")
def log_dim(m):  print(f"  {C.DIM}{m}{C.END}")


def find_local_ip():
    """IP локального LAN-интерфейса. Предпочитаем 192.168.*, затем 10.*,
    исключая docker/VPN-подсети. При сомнениях используйте --broker-ip."""
    candidates = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip.startswith("127.") or ip.startswith("169.254."):
                continue
            candidates.append(ip)
    except Exception:
        pass
    # docker по умолчанию 172.17-172.31 - в самый конец
    for prefix in ["192.168.", "10.", "172."]:
        for ip in candidates:
            if ip.startswith(prefix):
                return ip
    if candidates:
        return candidates[0]
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]; s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def subnet_broadcast(ip):
    """Направленный broadcast /24: 192.168.88.230 -> 192.168.88.255.
    Глобальный 255.255.255.255 уходит в default-route (может быть VPN/docker)."""
    try:
        parts = ip.split(".")
        if len(parts) == 4:
            return ".".join(parts[:3] + ["255"])
    except Exception:
        pass
    return "255.255.255.255"


# ═══════════════════════════════════════════════════
#  UDP-петля: единый сокет для отправки команд и приёма событий
# ═══════════════════════════════════════════════════
class UDPLink:
    def __init__(self, listen_port, cmd_port, device_ip=None, bcast="255.255.255.255"):
        self.listen_port = listen_port
        self.cmd_port = cmd_port
        self.device_ip = device_ip          # None -> broadcast
        self.bcast = bcast                  # направленный broadcast подсети
        self.sock = None
        self.device_name = None
        self._rx = []                        # список (text, src_ip)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("", self.listen_port))
        self.sock.settimeout(0.3)
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        return self

    def _reader(self):
        while not self._stop.is_set():
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            text = data.decode(errors="replace").strip()
            with self._lock:
                self._rx.append((text, addr[0]))
            # Запоминаем имя устройства из первого ответа вида <dev>/system/...
            if self.device_name is None and "/system/" in text:
                self.device_name = text.split("/system/", 1)[0].split("/")[-1]
            if self.device_ip is None and "/system/" in text:
                self.device_ip = addr[0]

    def send(self, payload):
        """Отправляет команду на cmd_port (unicast если known IP, иначе broadcast)."""
        dst = self.device_ip or self.bcast
        self.sock.sendto(payload.encode(), (dst, self.cmd_port))

    def clear(self):
        with self._lock:
            self._rx.clear()

    def wait_for(self, substring, timeout=RECV_TIMEOUT):
        """Ждёт сообщение, содержащее substring. Возвращает (text, src_ip) или None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                for entry in self._rx:
                    if substring in entry[0]:
                        return entry
            time.sleep(0.05)
        return None

    def request(self, cmd, resp_substring, timeout=RECV_TIMEOUT, retries=3):
        """Шлёт команду и ждёт ответ. Повторяет — UDP не гарантирует доставку."""
        for _ in range(retries):
            self.clear()
            self.send(cmd)
            resp = self.wait_for(resp_substring, timeout=timeout / retries + 0.5)
            if resp:
                return resp
        return None

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)
        if self.sock:
            self.sock.close()


# ═══════════════════════════════════════════════════
#  Test Runner
# ═══════════════════════════════════════════════════
class TestRunner:
    def __init__(self):
        self.results = []

    def record(self, name, passed, detail=""):
        self.results.append((name, passed, detail))
        (log_ok if passed else log_fail)(f"{name}" + (f"  ->{detail}" if detail else ""))

    def skip(self, name, reason):
        self.results.append((name, None, reason))
        log_warn(f"{name}  ->SKIP ({reason})")

    def summary(self):
        log_head("РЕЗУЛЬТАТЫ")
        passed = sum(1 for _, p, _ in self.results if p is True)
        failed = sum(1 for _, p, _ in self.results if p is False)
        for i, (name, p, detail) in enumerate(self.results, 1):
            mark = f"{C.OK}PASS{C.END}" if p is True else (
                   f"{C.WARN}SKIP{C.END}" if p is None else f"{C.FAIL}FAIL{C.END}")
            print(f"  {i:2d}. [{mark}] {name}")
            if detail:
                print(f"         {C.DIM}{detail}{C.END}")
        color = C.OK if failed == 0 else C.FAIL
        print(f"\n  {C.BOLD}Итого: {color}{passed} pass{C.END}{C.BOLD}, "
              f"{failed} fail, {len(self.results)-passed-failed} skip{C.END}")
        return failed == 0


# ═══════════════════════════════════════════════════
#  Базовый набор тестов (over-the-wire, без реконфига)
# ═══════════════════════════════════════════════════
def run_core_tests(link, runner, args):
    # ── Тест 1: Обнаружение + исполнение команды (петля приём->исполнение->отправка) ──
    log_head("Тест 1: Команда по UDP -> ответ по UDP (getVersion)")
    resp = link.request("system/getVersion", "/system/version:")
    if resp:
        ver = resp[0].split(":", 1)[1] if ":" in resp[0] else "?"
        runner.record("getVersion round-trip", True,
                      f"dev='{link.device_name}', ip={resp[1]}, version='{ver}'")
        log_info(f"Устройство обнаружено: {link.device_name} @ {link.device_ip}")
    else:
        runner.record("getVersion round-trip", False,
                      "нет ответа — проверьте, что [UDP] serverAdress указывает на этот ПК "
                      f"({find_local_ip()}) и serverPort={link.listen_port}")
        # Без петли остальные тесты бессмысленны
        return

    # ── Тест 2: getNETstatus -> JSON, UDP_init_res должен быть 0 (ESP_OK) ──
    log_head("Тест 2: getNETstatus + UDP_init_res")
    resp = link.request("system/getNETstatus", "/system/NETstatus:")
    if resp:
        payload = resp[0].split("NETstatus:", 1)[1]
        try:
            data = json.loads(payload)
            udp_res = data.get("UDP_init_res", -999)
            runner.record("getNETstatus UDP_init_res==0", udp_res == 0,
                          f"UDP_init_res={udp_res}, MQTT={data.get('MQTT_init_res')}, "
                          f"OSC={data.get('OSC_init_res')}")
        except (json.JSONDecodeError, ValueError) as e:
            runner.record("getNETstatus UDP_init_res==0", False, f"невалидный JSON: {e}")
    else:
        runner.record("getNETstatus UDP_init_res==0", False, "нет ответа")

    # ── Тест 3: getFreeRAM -> число > 0 ──
    log_head("Тест 3: getFreeRAM (числовой ответ)")
    resp = link.request("system/getFreeRAM", "/system/freeRAM:")
    if resp:
        try:
            ram = int(resp[0].split("freeRAM:", 1)[1])
            runner.record("getFreeRAM > 0", ram > 0, f"freeRAM={ram} bytes")
        except ValueError:
            runner.record("getFreeRAM > 0", False, f"не число: {resp[0]}")
    else:
        runner.record("getFreeRAM > 0", False, "нет ответа")

    # ── Тест 4: Устойчивость к мусору (не должно ронять задачу) ──
    log_head("Тест 4: Устойчивость к битым/мусорным пакетам")
    for junk in ["", "###no-slash###", "a" * 240, "/leading-slash/x:1", "x/y/z/action/"]:
        link.send(junk)
        time.sleep(0.05)
    # После мусора устройство должно по-прежнему отвечать
    resp = link.request("system/getVersion", "/system/version:")
    runner.record("Жив после мусорных пакетов", resp is not None,
                  "контроллер отвечает после серии битых пакетов" if resp
                  else "нет ответа после мусора (возможно, упала udplink_task)")

    # ── Тест 5: Unicast на вычисленный IP (после broadcast-обнаружения) ──
    log_head("Тест 5: Unicast-доставка команды")
    if link.device_ip:
        saved = link.device_ip
        link.device_ip = saved  # форсируем unicast (а не broadcast)
        resp = link.request("system/getVersion", "/system/version:")
        runner.record("Unicast команда", resp is not None,
                      f"unicast -> {saved}:{link.cmd_port}")
    else:
        runner.skip("Unicast команда", "IP устройства не определён")

    # ── Тест 6: Crosslink (опционально) ──
    if args.crosslink:
        log_head("Тест 6: Crosslink-маршрутизация")
        expect = args.crosslink_expect or "/system/"
        link.clear()
        link.send(args.crosslink)
        resp = link.wait_for(expect, timeout=RECV_TIMEOUT)
        runner.record("Crosslink", resp is not None,
                      f"'{args.crosslink}' -> ожидали '{expect}'" +
                      (f", получили '{resp[0]}'" if resp else " — нет реакции"))


# ═══════════════════════════════════════════════════
#  Опциональный реконфиг через диск + USB (регрессии #1, #5)
# ═══════════════════════════════════════════════════
def _replace_ini_section(text, section, content):
    lines = text.splitlines()
    out = []; skip = False; found = False
    for line in lines:
        s = line.strip()
        if s.startswith(f"[{section}]"):
            skip = True; found = True
            out.append(content.rstrip()); continue
        if skip and s.startswith("["):
            skip = False
        if not skip:
            out.append(line)
    if not found:
        out.append(""); out.append(content.rstrip())
    return "\r\n".join(out) + "\r\n"


def _find_controller_disk(device_name):
    label = (device_name or "").upper()
    if sys.platform == "win32":
        import ctypes, string
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        DRIVE_REMOVABLE = 2
        for i, letter in enumerate(string.ascii_uppercase):
            if not (bitmask & (1 << i)):
                continue
            drive = f"{letter}:\\"
            try:
                if ctypes.windll.kernel32.GetDriveTypeW(drive) != DRIVE_REMOVABLE:
                    continue
                if (Path(f"{letter}:") / "config.ini").exists():
                    return Path(f"{letter}:")
            except Exception:
                pass
    else:
        for base in ["/media", "/mnt", "/Volumes"]:
            p = Path(base)
            if p.exists():
                for d in p.iterdir():
                    if (d / "config.ini").exists():
                        return d
    return None


class USBDevice:
    def __init__(self, port=None):
        self.port = port; self.ser = None

    def connect(self, retries=1):
        if not HAS_SERIAL:
            return False
        port = self.port
        if not port:
            for p in serial.tools.list_ports.comports():
                d = (p.description or "").lower()
                if "usb" in d and ("serial" in d or "jtag" in d):
                    port = p.device; break
        if not port:
            return False
        for _ in range(retries):
            try:
                self.ser = serial.Serial(port, USB_BAUD, timeout=2)
                time.sleep(0.5); self.ser.reset_input_buffer()
                self.port = port
                return True
            except Exception:
                time.sleep(1.0)
        return False

    def restart(self):
        """Перезагрузка через USB. CDC отваливается при esp_restart(), поэтому
        переоткрываем порт перед записью и закрываем хэндл после."""
        if self.ser is None:
            self.connect(retries=5)
        try:
            if self.ser:
                self.ser.write(b"system/restart\n"); time.sleep(0.3)
        except Exception as e:
            log_warn(f"restart: порт недоступен ({e}) - возможно, уже перезагружается")
        self.close()

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None


def write_udp_section(disk, server_adress, server_port, my_port):
    """Перезаписывает [UDP] в config.ini. my_port=None -> ключ не пишется (тест дефолта)."""
    sec = f"[UDP]\r\nserverAdress = {server_adress}\r\nserverPort = {server_port}\r\n"
    if my_port is not None:
        sec += f"myPort = {my_port}\r\n"
    cfg = Path(disk) / "config.ini"
    raw = cfg.read_bytes().decode("utf-8", errors="replace") if cfg.exists() else ""
    raw = raw.replace("\r\n", "\n").replace("\r", "\n")
    new = _replace_ini_section(raw, "UDP", sec)
    with open(str(cfg), "wb") as f:
        f.write(new.encode("utf-8")); f.flush(); os.fsync(f.fileno())


def run_reconfig_tests(runner, args):
    log_head("РЕГРЕССИОННЫЕ ТЕСТЫ С РЕКОНФИГОМ (диск + USB)")
    my_ip = args.broker_ip or find_local_ip()
    log_info(f"IP этого ПК: {my_ip}, listen-port: {args.listen_port}")

    usb = USBDevice(args.com)
    if not usb.connect():
        runner.skip("Реконфиг", "USB CDC недоступен (нужен для перезагрузки/лога)")
        return
    log_info(f"USB: {usb.port}")

    disk = Path(args.disk) if args.disk else _find_controller_disk(args.device_name)
    if not disk or not (disk / "config.ini").exists():
        runner.skip("Реконфиг", "съёмный диск контроллера с config.ini не найден (--disk X:)")
        usb.close(); return

    log_info(f"Диск контроллера: {disk}")

    bcast = subnet_broadcast(my_ip)

    def reboot_and_probe(server_adress, my_port):
        """Пишет [UDP], перезагружает контроллер, опрашивает петлю getVersion."""
        write_udp_section(disk, server_adress, args.listen_port, my_port)
        time.sleep(2)                       # сброс кеша диска на USB-носитель
        usb.restart()                       # переоткрывает порт, шлёт restart, закрывает
        log_info(f"Ожидание перезагрузки + подъёма сети (до {REBOOT_WAIT + 25}s)...")
        time.sleep(REBOOT_WAIT)
        link = UDPLink(args.listen_port, args.cmd_port, bcast=bcast).start()
        try:
            deadline = time.time() + 25     # LAN/DHCP может подниматься небыстро
            while time.time() < deadline:
                resp = link.request("system/getVersion", "/system/version:",
                                    timeout=3, retries=1)
                if resp:
                    return resp
            return None
        finally:
            link.stop()

    try:
        # ── Baseline: с явным myPort (работает и на старой прошивке) ──
        # Отделяет проблемы сети/харнесса от незакрытой регрессии #1.
        log_head("Baseline: serverAdress + myPort (проверка сети)")
        resp = reboot_and_probe(my_ip, my_port=args.cmd_port)
        runner.record("Baseline UDP (с myPort)", resp is not None,
                      f"ответ с {resp[1]}" if resp
                      else "нет ответа - проверьте подсеть/что прошивка вообще с UDP")

        # ── Регрессия #1: outbound работает БЕЗ myPort (дефолтный порт 9000) ──
        log_head("Регрессия #1: исходящий UDP без myPort")
        resp = reboot_and_probe(my_ip, my_port=None)
        runner.record("Outbound без myPort (дефолт 9000)", resp is not None,
                      f"контроллер ответил с {resp[1]}, хотя myPort не задан" if resp
                      else "ответа нет - регрессия #1 не закрыта (нужна НОВАЯ прошивка)")

        # ── Регрессия #5: невалидный serverAdress по UDP не верифицируется ──
        # При невалидном IP исходящий тракт отключается (по дизайну), поэтому
        # ответа по UDP не будет; варнинг смотрится в консоли idf.py monitor.
        runner.skip("Варнинг на невалидный IP (#5)",
                    "не проверяется по UDP (исходящий выключен); ищите 'not a valid IPv4' в idf.py monitor")

    finally:
        # ── Восстанавливаем рабочую конфигурацию ──
        log_info("Восстановление [UDP]: serverAdress->этот ПК, myPort->дефолт")
        write_udp_section(disk, my_ip, args.listen_port, args.cmd_port)
        time.sleep(2)
        usb.restart()
        usb.close()


# ═══════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════
def main():
    parser = argparse.ArgumentParser(
        description="UDP Test Suite для moduleBox",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--listen-port", type=int, default=DEFAULT_PORT,
                        help=f"порт, на котором тестер принимает события (= device serverPort), деф. {DEFAULT_PORT}")
    parser.add_argument("--cmd-port", type=int, default=DEFAULT_PORT,
                        help=f"порт, на который шлём команды (= device myPort), деф. {DEFAULT_PORT}")
    parser.add_argument("--device-ip", default=None,
                        help="IP контроллера (если не задан — команды шлются broadcast'ом)")
    parser.add_argument("--crosslink", default=None,
                        help="строка crosslink-события для теста, напр. 'remote/btn:1'")
    parser.add_argument("--crosslink-expect", default=None,
                        help="подстрока, которую ждём в ответе на crosslink")
    parser.add_argument("--reconfig", action="store_true",
                        help="регрессии #1/#5 с реконфигом через диск+USB")
    parser.add_argument("--disk", default=None, help="буква диска контроллера (напр. D:)")
    parser.add_argument("--com", default=None, help="COM-порт USB CDC")
    parser.add_argument("--device-name", default=None, help="deviceName (для поиска диска)")
    parser.add_argument("--broker-ip", default=None, help="IP этого ПК (авто)")
    args = parser.parse_args()

    # Консоль может быть в cp1251 - не роняемся на символах вне кодировки
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass

    print(f"\n{C.BOLD}{'='*57}")
    print(f"   moduleBox UDP Test Suite")
    print(f"{'='*57}{C.END}")
    print(f"  Этот ПК      : {find_local_ip()}")
    print(f"  Listen port  : {args.listen_port}   (= device [UDP] serverPort)")
    print(f"  Cmd port     : {args.cmd_port}   (= device [UDP] myPort)")
    print(f"  Device IP    : {args.device_ip or 'broadcast (255.255.255.255)'}")

    runner = TestRunner()

    bcast = subnet_broadcast(args.broker_ip or find_local_ip())
    log_head("Базовый набор (over-the-wire)")
    link = UDPLink(args.listen_port, args.cmd_port, args.device_ip, bcast).start()
    try:
        run_core_tests(link, runner, args)
    finally:
        link.stop()

    if args.reconfig:
        run_reconfig_tests(runner, args)

    all_ok = runner.summary()
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
