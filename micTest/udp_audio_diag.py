#!/usr/bin/env python3
"""
UDP Audio Diagnostic — диагностика аудиоданных от TouchDesigner.

Перехватывает UDP-пакеты с сырым PCM (F32LE), анализирует содержимое
и сохраняет как WAV для прослушивания.

Использование:
    python udp_audio_diag.py [--port 15004] [--seconds 5] [--rate 44100]

Скрипт покажет:
  - Размеры пакетов (сколько сэмплов в каждом)
  - Диапазон значений float (мин/макс)
  - Частоту принимаемых пакетов (пакетов/сек)
  - Спектральный анализ (основная частота)
  - Сохранит WAV-файл для прослушивания
"""

import argparse
import socket
import struct
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np

try:
    import scipy.signal
    HAS_SCIPY = True
except ImportError:
    HAS_SCIPY = False


def receive_udp_audio(port: int, seconds: float, rate: int) -> dict:
    """Принимает UDP-пакеты и собирает статистику."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(2.0)

    print(f"[*] Слушаю UDP порт {port} в течение {seconds} сек...")
    print(f"    Ожидаемый формат: F32LE, rate={rate}Hz")
    print()

    all_samples = []
    packet_sizes = []
    packet_times = []
    raw_packets = []
    start = time.time()

    try:
        while time.time() - start < seconds:
            try:
                data, addr = sock.recvfrom(65536)
            except socket.timeout:
                print("  [!] Нет данных (timeout 2 сек)... проверьте, что TouchDesigner вещает")
                continue

            now = time.time()
            packet_times.append(now)
            packet_sizes.append(len(data))
            raw_packets.append(data)

            # Распаковываем как float32 LE
            n_floats = len(data) // 4
            if len(data) % 4 != 0:
                print(f"  [!] Пакет {len(packet_sizes)}: размер {len(data)} не кратен 4!")
            samples = struct.unpack(f'<{n_floats}f', data[:n_floats * 4])
            all_samples.extend(samples)

    except KeyboardInterrupt:
        print("\n[*] Прервано пользователем")
    finally:
        sock.close()

    elapsed = time.time() - start
    return {
        'samples': np.array(all_samples, dtype=np.float32),
        'packet_sizes': packet_sizes,
        'packet_times': packet_times,
        'raw_packets': raw_packets,
        'elapsed': elapsed,
        'rate': rate,
    }


def analyze_packets(data: dict):
    """Анализирует принятые пакеты."""
    sizes = data['packet_sizes']
    times = data['packet_times']
    samples = data['samples']
    rate = data['rate']

    print("=" * 60)
    print("  СТАТИСТИКА ПАКЕТОВ")
    print("=" * 60)

    if not sizes:
        print("  Ни одного пакета не получено!")
        return

    print(f"  Всего пакетов:        {len(sizes)}")
    print(f"  Всего сэмплов:        {len(samples)}")
    print(f"  Время записи:         {data['elapsed']:.2f} сек")

    # Размеры пакетов
    size_counts = Counter(sizes)
    print(f"\n  Размеры пакетов (байт → кол-во):")
    for sz, cnt in sorted(size_counts.items()):
        n_samples = sz // 4
        print(f"    {sz} байт ({n_samples} сэмплов): {cnt} пакетов")

    # Частота пакетов
    if len(times) > 1:
        intervals = np.diff(times)
        print(f"\n  Частота пакетов:")
        print(f"    Средний интервал:   {np.mean(intervals)*1000:.2f} мс")
        print(f"    Мин. интервал:      {np.min(intervals)*1000:.2f} мс")
        print(f"    Макс. интервал:     {np.max(intervals)*1000:.2f} мс")
        print(f"    Пакетов/сек:        {1.0/np.mean(intervals):.1f}")

    # Ожидаемое количество сэмплов
    expected_samples = int(data['elapsed'] * rate)
    actual_samples = len(samples)
    ratio = actual_samples / expected_samples if expected_samples > 0 else 0
    print(f"\n  Ожидаемо сэмплов (при {rate}Hz): {expected_samples}")
    print(f"  Фактически сэмплов:              {actual_samples}")
    print(f"  Соотношение:                      {ratio:.3f} ({ratio*100:.1f}%)")
    if ratio < 0.9:
        print(f"  [!] ПОТЕРЯ ДАННЫХ: приходит только {ratio*100:.1f}% сэмплов!")
        print(f"      Возможно TD шлёт не все сэмплы или разрывы в потоке.")


def analyze_audio(data: dict):
    """Анализирует аудиоданные."""
    samples = data['samples']
    rate = data['rate']

    print()
    print("=" * 60)
    print("  АНАЛИЗ АУДИО")
    print("=" * 60)

    if len(samples) == 0:
        print("  Нет данных для анализа!")
        return

    print(f"  Диапазон значений:    [{np.min(samples):.6f} .. {np.max(samples):.6f}]")
    print(f"  Среднее:              {np.mean(samples):.6f}")
    print(f"  RMS:                  {np.sqrt(np.mean(samples**2)):.6f}")
    print(f"  Пик (абс.):          {np.max(np.abs(samples)):.6f}")

    # Проверка на NaN/Inf
    nan_count = np.sum(np.isnan(samples))
    inf_count = np.sum(np.isinf(samples))
    if nan_count > 0:
        print(f"  [!] NaN значений:     {nan_count}")
    if inf_count > 0:
        print(f"  [!] Inf значений:     {inf_count}")

    # Проверка на тишину
    if np.max(np.abs(samples)) < 1e-6:
        print("  [!] ТИШИНА: все значения ~0")
        return

    # Проверка на выход за [-1, 1]
    over = np.sum(np.abs(samples) > 1.0)
    if over > 0:
        print(f"  [!] Значений вне [-1,1]: {over} ({over/len(samples)*100:.2f}%)")
        print(f"      Макс. абс. значение: {np.max(np.abs(samples)):.4f}")

    # FFT анализ — определение основной частоты
    if len(samples) >= rate // 10:  # минимум 100мс данных
        # Берём кусок из середины для анализа
        chunk_size = min(len(samples), rate * 2)  # макс 2 сек
        start = max(0, len(samples) // 2 - chunk_size // 2)
        chunk = samples[start:start + chunk_size]

        # Убираем DC offset
        chunk = chunk - np.mean(chunk)

        # FFT
        fft = np.fft.rfft(chunk)
        freqs = np.fft.rfftfreq(len(chunk), 1.0 / rate)
        magnitudes = np.abs(fft)

        # Основная частота (исключаем DC)
        magnitudes[0] = 0
        peak_idx = np.argmax(magnitudes)
        peak_freq = freqs[peak_idx]
        peak_mag = magnitudes[peak_idx]
        total_mag = np.sum(magnitudes)

        # THD (Total Harmonic Distortion) — доля энергии в гармониках
        # Если это чистый синус, THD должен быть < 1%
        fundamental_band = max(1, int(peak_idx * 0.05))
        fundamental_energy = np.sum(magnitudes[max(0,peak_idx-fundamental_band):peak_idx+fundamental_band+1]**2)
        total_energy = np.sum(magnitudes**2)
        thd = np.sqrt(max(0, total_energy - fundamental_energy)) / np.sqrt(fundamental_energy) * 100 if fundamental_energy > 0 else 0

        print(f"\n  Спектральный анализ (FFT):")
        print(f"    Основная частота:   {peak_freq:.1f} Hz")
        print(f"    Пик магнитуда:      {peak_mag:.1f}")
        print(f"    Доля пика:          {peak_mag/total_mag*100:.1f}%")
        print(f"    THD:                {thd:.1f}%")

        if peak_freq > 200 and peak_freq < 240:
            print(f"    [OK] Похоже на синус ~220 Hz!")
        elif thd > 50:
            print(f"    [!] THD > 50% — это НЕ чистый тон, похоже на шум")
        
        # Топ-5 частот
        top_indices = np.argsort(magnitudes)[-5:][::-1]
        print(f"\n    Топ-5 частот:")
        for i, idx in enumerate(top_indices):
            print(f"      {i+1}. {freqs[idx]:8.1f} Hz  (магнитуда: {magnitudes[idx]:.1f})")

    # Проверка на «белый шум» vs тон
    # Белый шум имеет плоский спектр, тон — один пик
    if len(samples) > 1000:
        # Автокорреляция — у синуса будет чёткий пик
        max_lag = min(len(samples) // 2, rate // 50)  # до 20мс
        autocorr = np.correlate(samples[:max_lag*2], samples[:max_lag*2], 'full')
        autocorr = autocorr[len(autocorr)//2:]  # только положительные лаги
        autocorr /= autocorr[0]  # нормализация

        # Ищем второй пик (первый всегда при lag=0)
        if HAS_SCIPY:
            peaks, _ = scipy.signal.find_peaks(autocorr, height=0.3, distance=rate//1000)
            if len(peaks) > 0:
                period_samples = peaks[0]
                detected_freq = rate / period_samples
                print(f"\n    Автокорреляция:")
                print(f"      Период:           {period_samples} сэмплов")
                print(f"      Частота:           {detected_freq:.1f} Hz")
                print(f"      Корреляция:       {autocorr[peaks[0]]:.3f}")
            else:
                print(f"\n    Автокорреляция: периодичность НЕ обнаружена → это шум")
        

def analyze_first_bytes(data: dict):
    """Показывает первые байты для проверки формата."""
    raw = data['raw_packets']
    if not raw:
        return

    print()
    print("=" * 60)
    print("  ПЕРВЫЕ ПАКЕТЫ (сырые данные)")
    print("=" * 60)

    for i, pkt in enumerate(raw[:3]):
        print(f"\n  Пакет {i+1}: {len(pkt)} байт")

        # Показать первые 16 float32
        n = min(16, len(pkt) // 4)
        floats = struct.unpack(f'<{n}f', pkt[:n*4])
        print(f"    Как F32LE: {', '.join(f'{v:.6f}' for v in floats[:8])}")
        if n > 8:
            print(f"              {', '.join(f'{v:.6f}' for v in floats[8:16])}")

        # Показать как S16LE для сравнения
        n16 = min(16, len(pkt) // 2)
        int16s = struct.unpack(f'<{n16}h', pkt[:n16*2])
        print(f"    Как S16LE: {', '.join(str(v) for v in int16s[:8])}")

        # Hex dump первых 32 байт
        hex_bytes = ' '.join(f'{b:02x}' for b in pkt[:32])
        print(f"    Hex:       {hex_bytes}")


def save_wav(data: dict, filename: str):
    """Сохраняет принятые данные как WAV."""
    import wave
    samples = data['samples']
    rate = data['rate']

    if len(samples) == 0:
        print("  Нет данных для сохранения!")
        return

    # Сохраняем как 32-bit float WAV
    filepath = Path(filename)
    print(f"\n  Сохраняю WAV: {filepath.absolute()}")
    print(f"    Формат: float32, {rate}Hz, моно")
    print(f"    Длительность: {len(samples)/rate:.2f} сек")

    # Нормализация (если есть значения вне [-1, 1])
    peak = np.max(np.abs(samples))
    if peak > 1.0:
        print(f"    [!] Нормализация: пик {peak:.4f} → 1.0")
        samples_norm = samples / peak
    else:
        samples_norm = samples

    # Сохраняем как 16-bit WAV (для совместимости)
    samples_16 = (samples_norm * 32767).clip(-32768, 32767).astype(np.int16)

    with wave.open(str(filepath), 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(rate)
        wf.writeframes(samples_16.tobytes())

    print(f"    Размер: {filepath.stat().st_size / 1024:.1f} KB")
    print(f"    Файл можно открыть в Audacity или любом аудиоплеере")


def test_interpretation(data: dict):
    """Пробует интерпретировать данные как разные форматы и ищет синус."""
    raw = data['raw_packets']
    rate = data['rate']

    if not raw or len(raw) < 10:
        return

    print()
    print("=" * 60)
    print("  ТЕСТ ИНТЕРПРЕТАЦИИ ФОРМАТА")
    print("=" * 60)

    # Собираем все сырые байты
    all_bytes = b''.join(raw)
    
    formats = {
        'F32LE': ('<f', 4),
        'F32BE': ('>f', 4),
        'S16LE': ('<h', 2),
        'S16BE': ('>h', 2),
        'S32LE': ('<i', 4),
        'S32BE': ('>i', 4),
        'S24LE (padded to 32)': ('<i', 4),  # approximate
    }

    for fmt_name, (pack_fmt, sample_size) in formats.items():
        n = min(len(all_bytes) // sample_size, rate * 2)
        if n < rate // 10:
            continue

        try:
            samples = np.array(
                struct.unpack(f'{pack_fmt[0]}{n}{pack_fmt[1]}', all_bytes[:n * sample_size]),
                dtype=np.float64
            )

            # Нормализация
            if 'S16' in fmt_name:
                samples /= 32768.0
            elif 'S32' in fmt_name or 'S24' in fmt_name:
                samples /= 2147483648.0

            # FFT на chunk из середины
            chunk_size = min(len(samples), rate)
            chunk = samples[:chunk_size] - np.mean(samples[:chunk_size])
            
            if np.max(np.abs(chunk)) < 1e-10:
                continue

            fft = np.fft.rfft(chunk)
            freqs = np.fft.rfftfreq(len(chunk), 1.0 / rate)
            magnitudes = np.abs(fft)
            magnitudes[0] = 0
            
            peak_idx = np.argmax(magnitudes)
            peak_freq = freqs[peak_idx]
            peak_mag = magnitudes[peak_idx]
            total_mag = np.sum(magnitudes)
            dominance = peak_mag / total_mag * 100 if total_mag > 0 else 0

            marker = ""
            if 200 < peak_freq < 240 and dominance > 20:
                marker = " ✓ СИНУС 220Hz!"
            elif dominance > 30:
                marker = " ~ тональный сигнал"

            print(f"  {fmt_name:25s}: пик {peak_freq:7.1f} Hz, доминантность {dominance:5.1f}%{marker}")

        except Exception as e:
            print(f"  {fmt_name:25s}: ошибка — {e}")


def main():
    parser = argparse.ArgumentParser(description='UDP Audio Diagnostic')
    parser.add_argument('--port', type=int, default=15004, help='UDP порт (default: 15004)')
    parser.add_argument('--seconds', type=float, default=5.0, help='Длительность записи в сек (default: 5)')
    parser.add_argument('--rate', type=int, default=44100, help='Sample rate (default: 44100)')
    parser.add_argument('--output', type=str, default='udp_capture.wav', help='Имя выходного WAV файла')
    args = parser.parse_args()

    print()
    print("╔══════════════════════════════════════════╗")
    print("║   UDP Audio Diagnostic — AntiDante       ║")
    print("╚══════════════════════════════════════════╝")
    print()

    # ВАЖНО: остановите поток в AntiDante перед запуском!
    print("[!] ВАЖНО: остановите поток в AntiDante (или используйте другой порт),")
    print("    чтобы этот скрипт мог захватить данные на порту", args.port)
    print()

    data = receive_udp_audio(args.port, args.seconds, args.rate)

    analyze_packets(data)
    analyze_audio(data)
    analyze_first_bytes(data)
    test_interpretation(data)

    if len(data['samples']) > 0:
        save_wav(data, args.output)

    print()
    print("=" * 60)
    print("  РЕКОМЕНДАЦИИ")
    print("=" * 60)

    if len(data['samples']) == 0:
        print("  • Данные не получены. Проверьте:")
        print("    1. TouchDesigner вещает на порт", args.port)
        print("    2. Адрес назначения = 127.0.0.1")
        print("    3. Firewall не блокирует UDP")
    else:
        samples = data['samples']
        peak = np.max(np.abs(samples))
        
        # Quick check for noise vs tone
        chunk = samples[:min(len(samples), args.rate)]
        chunk = chunk - np.mean(chunk)
        if np.max(np.abs(chunk)) > 1e-10:
            fft = np.fft.rfft(chunk)
            freqs = np.fft.rfftfreq(len(chunk), 1.0 / args.rate)
            magnitudes = np.abs(fft)
            magnitudes[0] = 0
            peak_idx = np.argmax(magnitudes)
            peak_freq = freqs[peak_idx]
            dominance = magnitudes[peak_idx] / np.sum(magnitudes) * 100

            if dominance > 20 and 200 < peak_freq < 240:
                print("  ✓ Данные от TouchDesigner содержат синус ~220Hz")
                print("    Проблема скорее всего в GStreamer пайплайне AntiDante")
                print("    или на стороне приёмника.")
            elif dominance < 5:
                print("  ✗ Данные от TouchDesigner — это ШУМ, а не синус!")
                print("    Проблема на стороне TouchDesigner:")
                print("    • Проверьте генератор сигнала в TD")
                print("    • Убедитесь что ChOp -> audioOut настроен правильно")
            else:
                print(f"  ? Данные содержат тон {peak_freq:.0f}Hz (доминантность {dominance:.0f}%)")
                print("    Не похоже на чистый синус 220Hz")

        expected = int(data['elapsed'] * args.rate)
        actual = len(samples)
        if actual < expected * 0.8:
            print(f"  [!] Получено только {actual/expected*100:.0f}% ожидаемых сэмплов")
            print("      TD может отправлять данные реже чем нужно (60fps фреймрейт?)")
            
    print()


if __name__ == '__main__':
    main()
