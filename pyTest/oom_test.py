#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OOM / Heap-Exhaustion Test для moduleBox
=========================================

Гонит контроллер к нехватке кучи реальной тяжёлой конфигурацией слотов
(по умолчанию N слотов button_smartLed с numOfLed диодов) и наблюдает,
ЧТО происходит при OOM: graceful-деградация или panic/reboot-loop.

Опирается на OOM-инструментовку в прошивке (main.c app_main):
  heap_caps_register_failed_alloc_callback -> 'OOM: req=<N> caps=0x<X> fn=<fn>'
Любой провал heap_caps_malloc/calloc/realloc печатает строку ДО возможного
краха, с размером и caps (0x800=INTERNAL, 0x400=SPIRAM, 0x08=DMA, 0x04=8BIT).

Использование:
  # одиночный прогон: 6 слотов button_smartLed по 1000 диодов
  python oom_test.py --com COM14 --debug-com COM4 --disk D: --slots 6 --leds 1000

  # развёртка: наращивать число слотов 1..10, пока не словим OOM/crash
  python oom_test.py --com COM14 --debug-com COM4 --disk D: --leds 1000 --sweep

  # другой модуль-индьюсер
  python oom_test.py --com COM14 --debug-com COM4 --disk D: --module button_swiperLed --slots 8 --leds 1000

Требования: pip install pyserial  (тот же venv, что и slot_module_test.py)
ВАЖНО: запускать с PYTHONIOENCODING=utf-8, иначе консоль cp1251 падает на рамках.
"""

import argparse
import re
import sys
import time

# Переиспользуем инфраструктуру slot_module_test.py (тот же каталог)
try:
    from slot_module_test import (
        CDCDevice, DebugUART, find_disk, eject_volume, wait_for_disk,
        _replace_section, C, log_ok, log_fail, log_info, log_warn, log_dim, log_head,
        NUM_OF_SLOTS, REBOOT_GRACE, PORT_REOPEN_DELAY,
        LOAD_COMPLETE_MARKER, DEFAULT_CONFIG_MARKER,
    )
except ImportError as e:
    print(f"ОШИБКА импорта slot_module_test.py (должен лежать рядом): {e}")
    sys.exit(1)

import os
from pathlib import Path


# ──────────────────── Маркеры лога ────────────────────
OOM_RE = re.compile(r"OOM:\s*req=(\d+)\s+caps=0x([0-9a-fA-F]+)\s+fn=(\S+)")
SKIP_RE = re.compile(r"\[(\d+)\] mode '([^']*)' SKIPPED.*low internal RAM (\d+) < (\d+)")
HEAP_BOOT_RE = re.compile(r"free Heap size (\d+)")
LOW_HEAP_RE = re.compile(r"Free heap size is LOW - (\d+)")
WORKING_RE = re.compile(r"Load complite, start working")

PANIC_PATTERNS = [
    (re.compile(r"Guru Meditation Error"),       "panic"),
    (re.compile(r"abort\(\) was called"),        "abort"),
    (re.compile(r"assert failed"),               "assert"),
    (re.compile(r"task_wdt:.*triggered|Task watchdog got triggered"), "task_wdt"),
    (re.compile(r"^Backtrace:"),                 "backtrace"),
    (re.compile(r"CORRUPT HEAP"),                "heap_corruption"),
    (re.compile(r"LoadProhibited|StoreProhibited|InstrFetchProhibited"), "exception"),
    (re.compile(r"Stack canary watchpoint|stack overflow|Unhandled debug exception"), "stack_overflow"),
    (re.compile(r"Rebooting\.\.\."),             "reboot"),
]

CAP_BITS = [
    (0x800, "INTERNAL"), (0x400, "SPIRAM"), (0x08, "DMA"),
    (0x04, "8BIT"), (0x1000, "DEFAULT"), (0x2000, "IRAM8BIT"),
]


def decode_caps(caps_hex):
    try:
        v = int(caps_hex, 16)
    except ValueError:
        return caps_hex
    names = [name for bit, name in CAP_BITS if v & bit]
    return "|".join(names) if names else f"0x{caps_hex}"


# ──────────────────── Запись config.ini ────────────────────
def write_oom_config(disk_path, n_slots, module, options_str):
    """Слоты 0..n_slots-1 = module с options_str, остальные empty.
    Сохраняет [SYSTEM]/[LAN]/[MQTT] и пр."""
    cfg_file = Path(disk_path) / "config.ini"
    existing = ""
    if cfg_file.exists():
        try:
            existing = cfg_file.read_bytes().decode("utf-8", errors="replace")
            existing = existing.replace("\r\n", "\n").replace("\r", "\n")
        except Exception:
            existing = ""
    text = existing
    for i in range(NUM_OF_SLOTS):
        if i < n_slots:
            section = f"[SLOT_{i}]\r\nmode = {module}\r\noptions = {options_str}\r\ncrosslink = \r\n"
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


# ──────────────────── Один прогон ────────────────────
def run_once(cdc, debug, disk_path, n_slots, module, options_str, window):
    """Пишет конфиг, ребутит, слушает UART `window` секунд после старта.
    Возвращает dict с результатом."""
    write_oom_config(disk_path, n_slots, module, options_str)

    time.sleep(REBOOT_GRACE)
    if eject_volume(disk_path):
        log_dim(f"  диск {disk_path} извлечён")
    debug.drain()
    log_info(f"system/restart ({n_slots}x {module} numOfLed={options_str})...")
    cdc.restart()
    time.sleep(PORT_REOPEN_DELAY)

    res = {
        "slots": n_slots, "module": module, "options": options_str,
        "oom_events": [],          # (req, caps_decoded, fn)
        "skips": [],               # (slot, mode, free, threshold) - сработал OOM-guard
        "panic": None,             # (kind, line)
        "booted": False,           # дошли до 'Load complite, start working'
        "heap_boot": None,         # первый 'free Heap size'
        "heap_working": None,      # heap на 'Load complite'
        "low_heap": [],            # значения из 'Free heap size is LOW'
        "lines": 0,
    }

    deadline = time.time() + window
    last_heap = None
    while time.time() < deadline:
        line = debug.readline(timeout=0.3)
        if not line:
            # после booted даём досидеть окно (ловим поздний OOM/crash), иначе ждём
            continue
        res["lines"] += 1

        m = OOM_RE.search(line)
        if m:
            ev = (int(m.group(1)), decode_caps(m.group(2)), m.group(3))
            res["oom_events"].append(ev)
            log_warn(f"  OOM: req={ev[0]}B caps={ev[1]} fn={ev[2]}")
            continue

        sk = SKIP_RE.search(line)
        if sk:
            res["skips"].append((int(sk.group(1)), sk.group(2), int(sk.group(3)), int(sk.group(4))))
            log_warn(f"  GUARD: слот {sk.group(1)} '{sk.group(2)}' пропущен (free {sk.group(3)} < {sk.group(4)})")
            continue

        for pat, kind in PANIC_PATTERNS:
            if pat.search(line):
                res["panic"] = (kind, line.strip())
                log_fail(f"  PANIC[{kind}]: {line.strip()}")
                # паника => дальше пойдёт ребут; даём ещё чуть-чуть и выходим
                deadline = min(deadline, time.time() + 1.5)
                break

        hm = HEAP_BOOT_RE.search(line)
        if hm:
            last_heap = int(hm.group(1))
            if res["heap_boot"] is None:
                res["heap_boot"] = last_heap

        lm = LOW_HEAP_RE.search(line)
        if lm:
            res["low_heap"].append(int(lm.group(1)))

        if WORKING_RE.search(line):
            res["booted"] = True
            res["heap_working"] = last_heap
            log_dim(f"  boot OK, free heap={last_heap}")

    # классификация
    if res["panic"]:
        res["verdict"] = "CRASH"
    elif res["skips"] and res["booted"]:
        res["verdict"] = "GUARDED"          # OOM-guard сработал- слоты пропущены- плата жива
    elif res["oom_events"] and res["booted"]:
        res["verdict"] = "GRACEFUL_OOM"
    elif res["oom_events"] and not res["booted"]:
        res["verdict"] = "OOM_NO_BOOT"
    elif res["booted"]:
        res["verdict"] = "OK"
    else:
        res["verdict"] = "HANG"
    return res


def print_verdict(res):
    v = res["verdict"]
    color = {
        "OK": C.OK, "GUARDED": C.OK, "GRACEFUL_OOM": C.WARN,
        "CRASH": C.FAIL, "OOM_NO_BOOT": C.FAIL, "HANG": C.FAIL,
    }.get(v, C.INFO)
    heap = f"heap boot={res['heap_boot']} work={res['heap_working']}"
    oom = f"{len(res['oom_events'])} OOM" if res["oom_events"] else "no OOM"
    grd = f", {len(res['skips'])} skip" if res["skips"] else ""
    print(f"  {color}[{v}]{C.END} {res['slots']}x {res['module']} "
          f"({res['options']}) — {oom}{grd}, {heap}")
    if res["panic"]:
        print(f"        {C.FAIL}{res['panic'][0]}: {res['panic'][1]}{C.END}")
    if res["oom_events"]:
        # уникальные fn/caps
        seen = {}
        for req, caps, fn in res["oom_events"]:
            seen.setdefault((caps, fn), 0)
            seen[(caps, fn)] += 1
        for (caps, fn), cnt in seen.items():
            print(f"        {C.DIM}OOM x{cnt}: caps={caps} fn={fn}{C.END}")


# ──────────────────── Main ────────────────────
def main():
    ap = argparse.ArgumentParser(description="OOM/heap-exhaustion test для moduleBox",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--com", default=None, help="USB CDC порт (system/restart)")
    ap.add_argument("--debug-com", required=True, help="UART-отладчик (логи)")
    ap.add_argument("--disk", default=None, help="Буква диска контроллера (D:)")
    ap.add_argument("--device-name", default=None)
    ap.add_argument("--module", default="button_smartLed", help="Модуль-индьюсер (по умолчанию button_smartLed)")
    ap.add_argument("--leds", type=int, default=1000, help="numOfLed на слот (по умолчанию 1000)")
    ap.add_argument("--slots", type=int, default=6, help="Сколько слотов набить (одиночный прогон)")
    ap.add_argument("--sweep", action="store_true", help="Наращивать слоты 1..max, пока не словим OOM/CRASH")
    ap.add_argument("--sweep-max", type=int, default=NUM_OF_SLOTS, help="Верх развёртки (по умолчанию 10)")
    ap.add_argument("--window", type=int, default=20, help="Окно прослушки UART после ребута, сек")
    ap.add_argument("--no-cleanup", action="store_true", help="НЕ затирать config.ini в конце")
    args = ap.parse_args()

    options_str = f"numOfLed:{args.leds}"

    print(f"\n{C.BOLD}╔════════════════════════════════════════════════════════╗")
    print(f"║   moduleBox OOM / Heap-Exhaustion Test                 ║")
    print(f"╚════════════════════════════════════════════════════════╝{C.END}")

    cdc = CDCDevice(args.com)
    if not cdc.connect():
        log_fail(f"USB CDC недоступен (--com={args.com})"); sys.exit(1)
    log_ok(f"USB CDC: {cdc.port}")
    dev_name = args.device_name or cdc.identify() or "moduleBox"

    debug = DebugUART(args.debug_com)
    if not debug.connect():
        cdc.close(); sys.exit(1)
    log_ok(f"Debug UART: {args.debug_com}")

    disk_path = find_disk(args.disk, dev_name)
    if not disk_path:
        log_fail("Диск контроллера не найден"); cdc.close(); debug.close(); sys.exit(1)
    log_ok(f"Диск: {disk_path}")

    results = []
    try:
        if args.sweep:
            log_head(f"Развёртка: 1..{args.sweep_max} слотов {args.module} по {args.leds} диодов")
            for n in range(1, args.sweep_max + 1):
                log_head(f"Шаг: {n} слот(ов)")
                res = run_once(cdc, debug, disk_path, n, args.module, options_str, args.window)
                print_verdict(res)
                results.append(res)
                if res["verdict"] in ("CRASH", "OOM_NO_BOOT", "HANG"):
                    log_warn(f"Останов развёртки на {n} слотах: {res['verdict']}")
                    break
        else:
            log_head(f"Прогон: {args.slots}x {args.module} по {args.leds} диодов")
            res = run_once(cdc, debug, disk_path, args.slots, args.module, options_str, args.window)
            print_verdict(res)
            results.append(res)
    finally:
        if not args.no_cleanup:
            log_head("Очистка config.ini")
            # после CRASH устройство в reboot-loop и D: может не смонтироваться -
            # пробуем затереть конфиг с ретраями, проверяя реальный успех записи
            wiped = False
            for attempt in range(5):
                if wait_for_disk(disk_path) and write_oom_config(disk_path, 0, "empty", ""):
                    wiped = True
                    break
                time.sleep(1.0)
            if wiped:
                log_ok("config.ini затёрт")
                time.sleep(REBOOT_GRACE)
                eject_volume(disk_path)
                cdc.restart()
                time.sleep(2)
            else:
                log_fail(f"Диск {disk_path} не вернулся/не записался - конфиг НЕ затёрт. "
                         f"Достань SD и впиши mode=empty вручную, либо power-cycle")
        cdc.close(); debug.close()

    # итог
    log_head("ИТОГ")
    for r in results:
        print_verdict(r)
    crash = next((r for r in results if r["verdict"] in ("CRASH", "OOM_NO_BOOT", "HANG")), None)
    guarded = [r for r in results if r["verdict"] == "GUARDED"]
    graceful = [r for r in results if r["verdict"] == "GRACEFUL_OOM"]
    if crash:
        print(f"\n  {C.FAIL}{C.BOLD}OOM приводит к НЕ-graceful отказу: "
              f"{crash['verdict']} на {crash['slots']} слотах{C.END}")
        sys.exit(1)
    elif guarded:
        n = min(r['slots'] for r in guarded)
        print(f"\n  {C.OK}{C.BOLD}OOM-guard сработал (с {n} слотов слоты пропускаются), "
              f"плата устойчива - крашей нет{C.END}")
        sys.exit(0)
    elif graceful:
        print(f"\n  {C.WARN}{C.BOLD}OOM обработан gracefully "
              f"(min слотов с OOM: {min(r['slots'] for r in graceful)}){C.END}")
        sys.exit(0)
    else:
        print(f"\n  {C.OK}{C.BOLD}OOM не достигнут на заданной нагрузке "
              f"(увеличь --slots/--leds или --sweep){C.END}")
        sys.exit(0)


if __name__ == "__main__":
    main()
