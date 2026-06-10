#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Slot Module Test Suite для moduleBox
======================================

Перебирает модули из manifest.json на SD-карте и для каждого:
  1. Заполняет ВСЕ слоты из поля "slots" манифеста этим модулем
     (mp3Player → только слот 0, в манифесте без "slots" → ошибка).
  2. Пишет config.ini на съёмный диск.
  3. Перезагружает контроллер через USB CDC (system/restart).
  4. Слушает UART-отладчик: ищет ошибки (ESP_LOG_ERROR, panic, watchdog).
  5. При первой ошибке тест останавливается.

После теста config.ini затирается (все слоты = empty).
Бэкап оригинального config.ini сохраняется как config.ini.bak.<timestamp>.

Использование:
  python slot_module_test.py --com COM81 --debug-com COM82 --disk D:
  python slot_module_test.py --com COM81 --debug-com COM82 \\
      --skip-modules audioLAN,opusLAN  # пропустить network-зависимые

Требования:
  pip install pyserial
"""

import argparse
import ctypes
import json
import os
import re
import string
import sys
import threading
import time
from collections import deque
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ОШИБКА: pip install pyserial")
    sys.exit(1)


# ──────────────────── Константы ────────────────────
USB_BAUD = 115200
DEBUG_BAUD = 115200
NUM_OF_SLOTS = 10           # из stateConfig.h

# Тайминги (сек):
REBOOT_GRACE = 2            # пауза после disk write перед system/restart
PORT_REOPEN_DELAY = 1.5     # пауза после restart перед чтением UART
BOOT_TIMEOUT = 30           # макс. ожидание "Load config complite"
STABLE_PERIOD = 5           # сек без ошибок после "Load config complite" = OK

# Маркеры в логе:
LOAD_COMPLETE_MARKER = "Load config complite"
DEFAULT_CONFIG_MARKER = "Load default config complite"
CHECK_MODE_RE = re.compile(r"\[(\d+)\] check mode: '([^']*)'")

# ANSI escape codes (esp_log использует цвета: \x1b[0;31mE (...)\x1b[0m)
ANSI_RE = re.compile(r"\x1b\[[\d;]*[A-Za-z]")

# Шаблоны ошибок (регексы):
ERROR_PATTERNS = [
    (re.compile(r"\bE \(\d+\) [\w_:.]+:"),       "ESP_LOG_ERROR"),
    (re.compile(r"Guru Meditation Error"),       "panic"),
    (re.compile(r"abort\(\) was called"),        "abort"),
    (re.compile(r"task_wdt:"),                   "task_wdt"),
    (re.compile(r"Task watchdog got triggered"), "task_wdt"),
    (re.compile(r"^Backtrace:"),                 "backtrace"),
    (re.compile(r"assert failed:"),              "assert"),
    (re.compile(r"CORRUPT HEAP"),                "heap_corruption"),
    (re.compile(r"LoadProhibited|StoreProhibited|InstrFetchProhibited"), "exception"),
    (re.compile(r"Rebooting\.\.\."),             "panic_reboot"),
]

# Известные "безвредные" ошибки — игнорируем при матче.
# Срабатывают ДО проверки ERROR_PATTERNS.
IGNORE_PATTERNS = [
    re.compile(r"\bE \(\d+\) MAIN: sdcard_init FAIL"),                  # MSC↔SDMMC handover после restart
    re.compile(r"\bE \(\d+\) sdmmc_common: sdmmc_init_ocr"),            # sdmmc init после restart (часть того же handover'а)
    re.compile(r"\bE \(\d+\) vfs_fat_sdmmc: sdmmc_card_init failed"),   # SDMMC card init после restart
    re.compile(r"\bE \(\d+\) gpio: gpio_install_isr_service"),          # повторная установка ISR сервиса несколькими модулями
    re.compile(r"\bE \(\d+\) gpio: gpio_isr_handler_add"),               # порядок инициализации ISR между модулями
]

# Предупреждения — в логе видим как WARN, но тест НЕ падает.
# Обычно это исчерпание периферии (UART, I2C и т.д.) при заполнении всех слотов.
WARNING_PATTERNS = [
    re.compile(r"\bE \(\d+\) [\w_:.]+: .*No free UART driver"),         # кончились UART — типично при слотах>3
    re.compile(r"\bE \(\d+\) pcnt: (pcnt_register_to_group.*no free unit|pcnt_new_unit.*register unit failed)"),  # кончились PCNT unit'ы (4 на ESP32-S3)
    re.compile(r"\bE \(\d+\) ENCODERS: AS5600 not found on slot:"),     # AS5600 физически не подключён к слоту (I2C)
    re.compile(r"\bE \(\d+\) ENCODERS: No free I2C driver for slot:"),  # кончились I2C драйверы (ESP-IDF I2C limit)
    re.compile(r"\bE \(\d+\) mcpwm: .*no free \w+ in group"),           # кончились MCPWM timer/operator (2 группы x 3 на ESP32-S3) — stepper>3
    re.compile(r"\bE \(\d+\) [\w_:.]+: LEDC channels? (has|have) ended"), # кончились LEDC каналы (8 на ESP32-S3) — pwmLeds/button LED при слотах сверх ~2
]

# Модули с особой логикой слотов (если манифест неправильно описывает диапазон).
# Сейчас не используется — манифест уже корректно ограничивает mp3Player/wavPlayer/opusLAN
# полем "slots":"0-0".
SPECIAL_SLOT_RULES = {}


# ──────────────────── Вывод ────────────────────
class C:
    OK = "\033[92m"; FAIL = "\033[91m"; WARN = "\033[93m"
    INFO = "\033[96m"; BOLD = "\033[1m"; DIM = "\033[2m"; END = "\033[0m"

def log_ok(m):   print(f"  {C.OK}[PASS]{C.END}  {m}")
def log_fail(m): print(f"  {C.FAIL}[FAIL]{C.END}  {m}")
def log_info(m): print(f"  {C.INFO}[INFO]{C.END}  {m}")
def log_warn(m): print(f"  {C.WARN}[WARN]{C.END}  {m}")
def log_dim(m):  print(f"  {C.DIM}{m}{C.END}")
def log_head(m): print(f"\n{C.BOLD}{'─'*60}\n  {m}\n{'─'*60}{C.END}")


# ──────────────────── USB CDC (команды) ────────────────────
class CDCDevice:
    """USB CDC порт — отправка system/restart и identify."""

    def __init__(self, port=None):
        self.port = port
        self.ser = None

    def connect(self):
        port = self.port
        if not port:
            for p in serial.tools.list_ports.comports():
                d = (p.description or "").lower()
                if "usb" in d and ("serial" in d or "jtag" in d):
                    port = p.device
                    break
        if not port:
            return False
        try:
            self.ser = serial.Serial(port, USB_BAUD, timeout=3)
            time.sleep(0.5)
            self.ser.reset_input_buffer()
            self.port = port
            return True
        except Exception as e:
            log_warn(f"CDC {port}: {e}")
            return False

    def cmd(self, command, wait=1.0):
        if not self.ser:
            return ""
        try:
            self.ser.reset_input_buffer()
            self.ser.write((command + "\n").encode())
            time.sleep(wait)
            return self.ser.read(self.ser.in_waiting or 2048).decode(errors="replace").strip()
        except Exception:
            return ""

    def identify(self):
        resp = self.cmd("Who are you?", wait=1.0)
        for line in resp.splitlines():
            if "moduleBox:" in line:
                return line.split(":", 1)[1].strip()
        return ""

    def restart(self):
        """Шлёт system/restart. Если порт мёртв (после прошлого reboot
        CDC переподключился) — переоткрывает и пробует ещё раз."""
        ok = self._send_restart()
        if not ok:
            log_dim("  CDC не отвечает, переоткрываем...")
            self.close()
            time.sleep(1.0)
            for _ in range(5):
                if self.connect():
                    break
                time.sleep(0.5)
            self._send_restart()
        # После успешного restart CDC всё равно отвалится — закрываем,
        # переоткроем перед следующим restart.
        self.close()

    def _send_restart(self):
        if not self.ser:
            return False
        try:
            self.ser.reset_input_buffer()
            self.ser.write(b"system/restart\n")
            self.ser.flush()
            time.sleep(0.3)
            return True
        except Exception:
            return False

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None


# ──────────────────── UART-отладчик (логи) ────────────────────
class DebugUART:
    """UART отладочный порт — построчное чтение логов.
    Использует фоновый поток-читатель, чтобы UART буфер ОС не переполнялся
    пока основной поток обрабатывает строки."""

    def __init__(self, port):
        self.port = port
        self.ser = None
        self._buf = ""
        self._lines = deque()        # очередь готовых строк
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def connect(self):
        try:
            self.ser = serial.Serial(self.port, DEBUG_BAUD, timeout=0.1)
            # Увеличиваем приёмный буфер ОС — иначе бурст из ~100 строк теряется.
            try:
                self.ser.set_buffer_size(rx_size=256 * 1024, tx_size=4 * 1024)
            except Exception:
                pass
            time.sleep(0.3)
            self._stop.clear()
            self._thread = threading.Thread(target=self._reader_loop, daemon=True)
            self._thread.start()
            return True
        except Exception as e:
            log_fail(f"Debug UART {self.port}: {e}")
            return False

    def _reader_loop(self):
        """Фоновый поток: непрерывно читает UART и кладёт готовые строки в _lines."""
        while not self._stop.is_set() and self.ser is not None:
            try:
                # Читаем всё что есть в буфере одним блоком, либо ждём 1 байт.
                avail = self.ser.in_waiting
                chunk = self.ser.read(avail if avail > 0 else 1)
            except Exception:
                time.sleep(0.05)
                continue
            if not chunk:
                continue
            text = chunk.decode(errors="replace")
            with self._lock:
                self._buf += text
                while "\n" in self._buf:
                    line, self._buf = self._buf.split("\n", 1)
                    self._lines.append(ANSI_RE.sub("", line).rstrip("\r"))

    def drain(self):
        """Сбрасывает накопленный мусор перед стартом нового цикла."""
        if not self.ser:
            return
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass
        with self._lock:
            self._buf = ""
            self._lines.clear()

    def readline(self, timeout=1.0):
        """Возвращает следующую готовую строку или '' если за timeout не пришло."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if self._lines:
                    return self._lines.popleft()
            time.sleep(0.02)
        return ""

    def close(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)
            self._thread = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None


# ──────────────────── Поиск диска контроллера ────────────────────
def find_disk(disk_arg, device_name):
    """Возвращает Path к диску контроллера или None."""
    if disk_arg:
        p = Path(disk_arg)
        if p.exists():
            return p
        log_fail(f"Диск {disk_arg} не существует")
        return None

    if sys.platform != "win32":
        for base in ["/media", "/mnt", "/Volumes"]:
            b = Path(base)
            if not b.exists():
                continue
            for d in b.iterdir():
                if d.is_dir() and (d / "config.ini").exists():
                    return d
        return None

    # Windows: ищем съёмный диск с config.ini
    bitmask = ctypes.windll.kernel32.GetLogicalDrives()
    label = (device_name or "").upper()
    DRIVE_REMOVABLE = 2
    for i, letter in enumerate(string.ascii_uppercase):
        if not (bitmask & (1 << i)):
            continue
        drive = f"{letter}:\\"
        try:
            dt = ctypes.windll.kernel32.GetDriveTypeW(drive)
            if dt != DRIVE_REMOVABLE:
                continue
            cfg = Path(f"{letter}:") / "config.ini"
            if not cfg.exists():
                continue
            vol_buf = ctypes.create_unicode_buffer(261)
            ctypes.windll.kernel32.GetVolumeInformationW(
                drive, vol_buf, 261, None, None, None, None, 0)
            if label and vol_buf.value == label:
                return Path(f"{letter}:")
            # fallback: первый съёмный диск с config.ini
            return Path(f"{letter}:")
        except Exception:
            pass
    return None


# ──────────────────── Парсинг manifest.json ────────────────────
def parse_slots_field(value):
    """Парсит поле "slots": "0-5", "0,2,4" или "0" → список int.
    Возвращает None если поле отсутствует или нераспознано."""
    if value is None:
        return None
    s = str(value).strip()
    if not s:
        return None
    result = set()
    for part in s.split(","):
        part = part.strip()
        if "-" in part:
            try:
                a, b = part.split("-", 1)
                a, b = int(a), int(b)
                for i in range(min(a, b), max(a, b) + 1):
                    if 0 <= i < NUM_OF_SLOTS:
                        result.add(i)
            except ValueError:
                return None
        else:
            try:
                i = int(part)
                if 0 <= i < NUM_OF_SLOTS:
                    result.add(i)
            except ValueError:
                return None
    return sorted(result) if result else None


def find_manifest(disk_path):
    """Ищет файл манифеста: manifest.json или manifest-X.Y.json (последняя версия)."""
    p = Path(disk_path)
    plain = p / "manifest.json"
    if plain.exists():
        return plain
    candidates = sorted(p.glob("manifest-*.json"))
    if candidates:
        return candidates[-1]  # последняя по имени = последняя версия
    return None


def load_manifest(disk_path):
    """Читает файл манифеста из корня диска. Возвращает список modes."""
    mf = find_manifest(disk_path)
    if not mf:
        log_fail(f"manifest*.json не найден на {disk_path}")
        return None
    log_info(f"manifest: {mf.name}")
    try:
        # cp1251 кодировка из прошивки → читаем как latin-1 чтобы не падать на кривых байтах
        raw = mf.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("cp1251", errors="replace")
        data = json.loads(text)
    except Exception as e:
        log_fail(f"Не удалось распарсить {mf.name}: {e}")
        return None

    # Поддержка двух форматов:
    #   1) [ {mode, options, ...}, ... ]              — testManifest.json
    #   2) { "config": [...], "modes": [...] }        — manifest-X.Y.json
    if isinstance(data, list):
        modes = data
    elif isinstance(data, dict) and isinstance(data.get("modes"), list):
        modes = data["modes"]
    else:
        log_fail(f"{mf.name}: ожидался массив или объект с ключом 'modes'")
        return None

    return modes


# ──────────────────── Работа с config.ini ────────────────────
def _replace_section(text, section_name, new_content):
    """Заменяет секцию [section_name] в INI-тексте."""
    lines = text.splitlines()
    result = []; skip = False; found = False
    for line in lines:
        s = line.strip()
        if s.startswith(f"[{section_name}]"):
            skip = True; found = True
            result.append(new_content.rstrip())
            continue
        if skip and s.startswith("["):
            skip = False
        if not skip:
            result.append(line)
    if not found:
        result.append("")
        result.append(new_content.rstrip())
    return "\r\n".join(result) + "\r\n"


def write_slots_config(disk_path, slots, mode):
    """Записывает config.ini: указанные слоты = mode, остальные = empty.
    Сохраняет [SYSTEM], [LAN], [MQTT] и др. секции."""
    cfg_file = Path(disk_path) / "config.ini"
    existing = ""
    if cfg_file.exists():
        try:
            raw = cfg_file.read_bytes().decode("utf-8", errors="replace")
            existing = raw.replace("\r\n", "\n").replace("\r", "\n")
        except Exception:
            existing = ""

    text = existing
    for i in range(NUM_OF_SLOTS):
        if i in slots:
            section = f"[SLOT_{i}]\r\nmode = {mode}\r\noptions = \r\ncrosslink = \r\n"
        else:
            section = f"[SLOT_{i}]\r\nmode = empty\r\noptions = \r\ncrosslink = \r\n"
        text = _replace_section(text, f"SLOT_{i}", section)

    try:
        with open(str(cfg_file), "wb") as f:
            f.write(text.encode("utf-8"))
            f.flush()
            os.fsync(f.fileno())
        return True
    except Exception as e:
        log_fail(f"Запись config.ini: {e}")
        return False


def wipe_config(disk_path):
    """Затирает все слоты в config.ini (mode=empty)."""
    return write_slots_config(disk_path, set(), "")


def wait_for_disk(disk_path, timeout=15):
    """Ждёт пока config.ini снова появится на диске (после MSC re-mount)."""
    cfg = Path(disk_path) / "config.ini"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if cfg.exists():
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def eject_volume(disk_path):
    """Программно извлекает том на Windows — заставляет ОС отпустить SD,
    чтобы контроллер мог взять его через SDMMC после ребута.
    Возвращает True если ОС подтвердила dismount."""
    if sys.platform != "win32":
        return False

    letter = str(disk_path).rstrip("\\/").rstrip(":")[-1]
    if not letter.isalpha():
        return False

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    FSCTL_LOCK_VOLUME = 0x00090018
    FSCTL_DISMOUNT_VOLUME = 0x00090020
    FSCTL_UNLOCK_VOLUME = 0x0009001C

    path = f"\\\\.\\{letter}:"
    h = ctypes.windll.kernel32.CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None, OPEN_EXISTING, 0, None)
    if h == INVALID_HANDLE_VALUE or h == -1 or h is None:
        return False
    try:
        bytes_returned = ctypes.c_ulong(0)
        # Lock — может потребовать несколько попыток если кто-то держит volume
        for _ in range(5):
            ok = ctypes.windll.kernel32.DeviceIoControl(
                h, FSCTL_LOCK_VOLUME, None, 0, None, 0,
                ctypes.byref(bytes_returned), None)
            if ok:
                break
            time.sleep(0.3)
        # Dismount даже если lock не удался — попробуем
        ok_dis = ctypes.windll.kernel32.DeviceIoControl(
            h, FSCTL_DISMOUNT_VOLUME, None, 0, None, 0,
            ctypes.byref(bytes_returned), None)
        # Снимаем lock — иначе пока handle не закрыт, volume залочен
        ctypes.windll.kernel32.DeviceIoControl(
            h, FSCTL_UNLOCK_VOLUME, None, 0, None, 0,
            ctypes.byref(bytes_returned), None)
        return bool(ok_dis)
    finally:
        ctypes.windll.kernel32.CloseHandle(h)


# ──────────────────── Анализ лога ────────────────────
def match_error(line):
    """Возвращает (True, тип_ошибки) если строка — ошибка, иначе (False, None).
    Строки из IGNORE_PATTERNS и WARNING_PATTERNS пропускаются (не считаются fail)."""
    for pat in IGNORE_PATTERNS:
        if pat.search(line):
            return False, None
    for pat in WARNING_PATTERNS:
        if pat.search(line):
            return False, None
    for pat, kind in ERROR_PATTERNS:
        if pat.search(line):
            return True, kind
    return False, None


def match_warning(line):
    """Возвращает True если строка — известное предупреждение (не fail, но видим)."""
    for pat in WARNING_PATTERNS:
        if pat.search(line):
            return True
    return False


def watch_log(debug, mode, slots, boot_timeout=BOOT_TIMEOUT, stable_period=STABLE_PERIOD):
    """Слушает debug UART после reboot. Проверяет, что:
       - в логе нет ошибок (фильтр по IGNORE_PATTERNS),
       - выполнилось init_slots ('Load config complite'),
       - все ожидаемые слоты попали в init_slots ('[N] check mode: 'X'').

       Возвращает (status, message, log_lines, warnings), где status:
         'ok'                  — всё хорошо,
         'error'               — нашли ошибку (стоп теста),
         'config_not_applied'  — конфиг не применился (retry),
         'timeout'             — таймаут (retry).
    """
    log_lines = []
    warnings = []
    deadline_boot = time.time() + boot_timeout
    seen_load_complete = False
    stable_until = None
    saw_default = False
    expected = set(slots)
    seen_check_mode = {}    # slot_num -> mode_seen

    while True:
        line = debug.readline(timeout=0.3)
        if line:
            log_lines.append(line)

            if match_warning(line):
                warnings.append(line.strip())
                log_warn(f"  WARN: {line.strip()}")

            is_err, kind = match_error(line)
            if is_err:
                return "error", f"{kind}: {line.strip()}", log_lines, warnings

            if DEFAULT_CONFIG_MARKER in line:
                saw_default = True

            m = CHECK_MODE_RE.search(line)
            if m:
                slot_idx = int(m.group(1))
                seen_mode = m.group(2)
                seen_check_mode[slot_idx] = seen_mode

            # 'Load config complite' — но НЕ 'Load default config complite'
            if LOAD_COMPLETE_MARKER in line and DEFAULT_CONFIG_MARKER not in line:
                seen_load_complete = True
                stable_until = time.time() + stable_period
                log_dim(f"  [{mode}] init_slots завершено, стабилизация {stable_period}с...")

        now = time.time()
        if seen_load_complete and now >= stable_until:
            # Проверяем, что все ожидаемые слоты увидели правильный mode
            missing = expected - set(seen_check_mode.keys())
            wrong = {s: seen_check_mode[s] for s in expected & set(seen_check_mode.keys())
                     if seen_check_mode[s] != mode}
            if missing or wrong:
                detail_parts = []
                if missing:
                    detail_parts.append(f"не увидели check mode для слотов {sorted(missing)}")
                if wrong:
                    detail_parts.append(f"неверный mode: {wrong}")
                return "config_not_applied", "; ".join(detail_parts), log_lines, warnings
            return "ok", None, log_lines, warnings

        if not seen_load_complete and now >= deadline_boot:
            if saw_default:
                return "config_not_applied", \
                       f"timeout {boot_timeout}s, виден только default config (SD не смонтировался)", \
                       log_lines, warnings
            return "timeout", \
                   f"timeout {boot_timeout}s — не было '{LOAD_COMPLETE_MARKER}'", \
                   log_lines, warnings


# ──────────────────── Test Runner ────────────────────
class TestResult:
    def __init__(self):
        self.results = []  # list of (mode, slots, ok, detail)

    def record(self, mode, slots, ok, detail):
        self.results.append({"mode": mode, "slots": slots, "ok": ok, "detail": detail})

    def summary(self):
        log_head("РЕЗУЛЬТАТЫ")
        passed = sum(1 for r in self.results if r["ok"])
        total = len(self.results)
        for r in self.results:
            mark = f"{C.OK}PASS{C.END}" if r["ok"] else f"{C.FAIL}FAIL{C.END}"
            slots_str = ",".join(str(s) for s in r["slots"]) if r["slots"] else "—"
            print(f"  [{mark}] {r['mode']:20s} slots=[{slots_str}]")
            if r["detail"]:
                print(f"            {C.DIM}{r['detail']}{C.END}")
        color = C.OK if passed == total else C.FAIL
        print(f"\n  {C.BOLD}Итого: {color}{passed}/{total}{C.END}{C.BOLD} модулей пройдено{C.END}")
        return passed == total


# ──────────────────── Дамп лога в файл ────────────────────
def dump_log(out_dir, mode, slots, log_lines):
    out_dir.mkdir(parents=True, exist_ok=True)
    slots_str = "_".join(str(s) for s in slots) if slots else "noslots"
    fn = out_dir / f"{mode}_slots_{slots_str}.log"
    try:
        fn.write_text("\n".join(log_lines), encoding="utf-8")
        log_dim(f"  лог сохранён: {fn}")
    except Exception as e:
        log_warn(f"Не удалось сохранить лог: {e}")


# ──────────────────── Main ────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Slot Module Test Suite для moduleBox",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--com", default=None,
                        help="COM-порт USB CDC (для system/restart). Авто-поиск если не задан.")
    parser.add_argument("--debug-com", required=True,
                        help="COM-порт UART-отладчика (для чтения логов).")
    parser.add_argument("--disk", default=None,
                        help="Буква диска контроллера (D:). Авто-поиск если не задан.")
    parser.add_argument("--device-name", default=None,
                        help="deviceName для поиска диска.")
    parser.add_argument("--skip-modules", default="",
                        help="Список модулей через запятую, которые пропустить.")
    parser.add_argument("--only-modules", default="",
                        help="Тестировать ТОЛЬКО эти модули (через запятую).")
    parser.add_argument("--skip-first", type=int, default=0,
                        help="Пропустить первые N модулей по порядку из манифеста.")
    parser.add_argument("--from", dest="start_from", default=None,
                        help="Начать с модуля с этим именем (пропустив всё перед ним).")
    parser.add_argument("--boot-timeout", type=int, default=BOOT_TIMEOUT,
                        help=f"Таймаут на 'Load config complite' (сек, по умолчанию {BOOT_TIMEOUT}).")
    parser.add_argument("--stable-period", type=int, default=STABLE_PERIOD,
                        help=f"Окно тишины после загрузки (сек, по умолчанию {STABLE_PERIOD}).")
    parser.add_argument("--log-dir", default="slot_test_logs",
                        help="Папка для дампа логов при FAIL.")
    parser.add_argument("--no-cleanup", action="store_true",
                        help="НЕ затирать config.ini в конце теста.")
    parser.add_argument("--retries", type=int, default=3,
                        help="Сколько раз повторить ребут если SD не смонтировался (по умолчанию 3).")
    args = parser.parse_args()

    skip_modules = {s.strip() for s in args.skip_modules.split(",") if s.strip()}
    only_modules = {s.strip() for s in args.only_modules.split(",") if s.strip()}

    print(f"\n{C.BOLD}╔══════════════════════════════════════════════════════════╗")
    print(f"║   moduleBox Slot Module Test Suite                       ║")
    print(f"╚══════════════════════════════════════════════════════════╝{C.END}")

    # ── Подключение CDC ──
    cdc = CDCDevice(args.com)
    if not cdc.connect():
        log_fail(f"USB CDC недоступен (--com={args.com})")
        sys.exit(1)
    log_ok(f"USB CDC: {cdc.port}")

    dev_name = args.device_name or cdc.identify() or "moduleBox"
    log_info(f"deviceName='{dev_name}'")

    # ── Подключение Debug UART ──
    debug = DebugUART(args.debug_com)
    if not debug.connect():
        cdc.close()
        sys.exit(1)
    log_ok(f"Debug UART: {args.debug_com}")

    # ── Поиск диска ──
    disk_path = find_disk(args.disk, dev_name)
    if not disk_path:
        log_fail("Диск контроллера не найден")
        cdc.close(); debug.close()
        sys.exit(1)
    log_ok(f"Диск: {disk_path}")

    # ── Манифест ──
    modules = load_manifest(disk_path)
    if modules is None:
        cdc.close(); debug.close()
        sys.exit(1)
    log_ok(f"manifest.json: {len(modules)} модулей")

    runner = TestResult()
    log_dir = Path(args.log_dir)
    stop_test = False

    # ── Применяем --skip-first / --from до фильтра ──
    if args.start_from:
        start_idx = next((i for i, m in enumerate(modules)
                          if m.get("mode", "").strip() == args.start_from), None)
        if start_idx is None:
            log_fail(f"--from {args.start_from}: модуль не найден в манифесте")
            cdc.close(); debug.close()
            sys.exit(1)
        modules = modules[start_idx:]
        log_info(f"--from {args.start_from}: начинаем с модуля #{start_idx}")
    elif args.skip_first > 0:
        modules = modules[args.skip_first:]
        log_info(f"--skip-first {args.skip_first}: пропустили первые {args.skip_first} модулей")

    # ── Цикл по модулям ──
    try:
        for module in modules:
            mode = module.get("mode", "").strip()
            if not mode:
                continue
            if only_modules and mode not in only_modules:
                continue
            if mode in skip_modules:
                log_dim(f"  [{mode}] пропущен (--skip-modules)")
                continue
            if mode == "empty" or mode == "SD_card":
                continue

            # Определение слотов
            if mode in SPECIAL_SLOT_RULES:
                slots = SPECIAL_SLOT_RULES[mode]
                slots_src = "spec"
            else:
                slots_field = module.get("slots")
                slots = parse_slots_field(slots_field)
                slots_src = f"manifest='{slots_field}'"

            if not slots:
                runner.record(mode, [], False,
                              f"no 'slots' field in manifest (поле обязательно)")
                stop_test = True
                log_fail(f"[{mode}] поле 'slots' отсутствует или некорректно — стоп")
                break

            log_head(f"Тестируем: {mode}  слоты={slots}  ({slots_src})")

            # Запись config.ini
            if not write_slots_config(disk_path, set(slots), mode):
                runner.record(mode, slots, False, "не удалось записать config.ini")
                stop_test = True
                break

            # Цикл retry: если SD не смонтировался / config не применился — пробуем ещё раз.
            status = None
            err_msg = None
            log_lines = []
            warnings = []
            for attempt in range(1, args.retries + 1):
                # Даём ОС сбросить кеш и извлекаем volume — чтобы Windows отпустил SD.
                time.sleep(REBOOT_GRACE)
                if eject_volume(disk_path):
                    log_dim(f"  диск {disk_path} извлечён (ОС отпустила SD)")
                else:
                    log_dim(f"  eject не удался")

                debug.drain()
                if attempt == 1:
                    log_info(f"system/restart...")
                else:
                    log_warn(f"retry {attempt}/{args.retries}: system/restart...")
                cdc.restart()
                time.sleep(PORT_REOPEN_DELAY)

                status, err_msg, log_lines, warnings = watch_log(
                    debug, mode, slots,
                    boot_timeout=args.boot_timeout,
                    stable_period=args.stable_period)

                if status == "ok":
                    detail = f"{len(log_lines)} строк лога"
                    if attempt > 1:
                        detail += f", retry={attempt}"
                    if warnings:
                        detail += f", warnings={len(warnings)}"
                    runner.record(mode, slots, True, detail)
                    log_ok(f"[{mode}] OK"
                           + (f" (с {attempt}-й попытки)" if attempt > 1 else "")
                           + (f", warnings: {len(warnings)}" if warnings else ""))
                    break

                if status == "error":
                    # Реальная ошибка — не retry'им, останавливаем тест.
                    break

                # status == 'config_not_applied' / 'timeout' → retry
                log_warn(f"  попытка {attempt}: {err_msg}")

            if status == "ok":
                continue

            # Все retry истрачены или была реальная ошибка
            runner.record(mode, slots, False,
                          f"{err_msg} (после {attempt} попыток)" if status != "error" else err_msg)
            dump_log(log_dir, mode, slots, log_lines)
            log_fail(f"[{mode}] {err_msg}")
            # Хвост лога в консоль
            tail = log_lines[-50:] if len(log_lines) > 50 else log_lines
            print(f"\n  {C.BOLD}─── Лог отладчика ({len(log_lines)} строк, "
                  f"показано {len(tail)}) ───{C.END}")
            if not tail:
                print(f"  {C.WARN}(UART молчит — нет ни одной строки){C.END}")
            else:
                for ln in tail:
                    print(f"  {C.DIM}│{C.END} {ln}")
            print(f"  {C.BOLD}─── конец лога ───{C.END}\n")
            stop_test = True
            break

    finally:
        # Затираем конфиг (если не --no-cleanup)
        if not args.no_cleanup:
            log_head("Очистка config.ini")
            log_dim(f"  ждём пока диск {disk_path} снова появится...")
            if wait_for_disk(disk_path):
                if wipe_config(disk_path):
                    log_ok("config.ini затёрт (все слоты = empty)")
                else:
                    log_fail("Не удалось затереть config.ini")
                time.sleep(REBOOT_GRACE)
                eject_volume(disk_path)
                cdc.restart()
                time.sleep(2)
            else:
                log_fail(f"Диск {disk_path} не вернулся за 15с — конфиг не затёрт")

        cdc.close()
        debug.close()

    all_ok = runner.summary()
    if stop_test:
        print(f"\n  {C.WARN}Тест остановлен на первой ошибке.{C.END}")
    sys.exit(0 if all_ok and not stop_test else 1)


if __name__ == "__main__":
    main()
