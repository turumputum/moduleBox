#!/usr/bin/env python3
"""
Audio Latency Tester — измерение задержки аудиотракта.

Отправляет короткие импульсы (chirp) на выход, слушает микрофон
и замеряет время между отправкой и приёмом сигнала.

Метод: cross-correlation между отправленным и принятым chirp-сигналом.

Использование:
    python latency_test.py <длительность_минут> [--interval 2] [--sample-rate 48000]
"""

import argparse
import datetime
import sys
import time
import threading
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import numpy as np
import sounddevice as sd


# ─── Конфигурация ────────────────────────────────────────────────────────────

@dataclass
class LatencyConfig:
    duration_min: float = 1.0
    sample_rate: int = 48000
    channels: int = 1
    # --- Chirp параметры ---
    chirp_duration_ms: float = 30.0       # длительность chirp-импульса
    chirp_freq_start: float = 500.0       # начальная частота chirp
    chirp_freq_end: float = 6000.0        # конечная частота chirp
    chirp_amplitude: float = 0.8
    # --- Измерение ---
    interval_sec: float = 2.0             # интервал между импульсами
    detection_threshold: float = 0.1      # порог детекции (доля от пика корреляции)
    max_latency_ms: float = 500.0         # макс. ожидаемая задержка
    # --- Файлы ---
    log_dir: str = "."


@dataclass
class LatencyMeasurement:
    """Одно измерение задержки."""
    index: int
    wall_time: str
    elapsed_sec: float
    latency_ms: float
    confidence: float         # 0..1 — уверенность в измерении (пик корреляции)
    valid: bool               # True если измерение надёжное


# ─── Генерация chirp-сигнала ─────────────────────────────────────────────────

def generate_chirp(config: LatencyConfig) -> np.ndarray:
    """Генерирует линейный chirp (свип частоты) для точного определения задержки."""
    num_samples = int(config.chirp_duration_ms / 1000.0 * config.sample_rate)
    t = np.arange(num_samples) / config.sample_rate
    duration = num_samples / config.sample_rate

    # Линейный chirp: частота растёт от f_start до f_end
    f0 = config.chirp_freq_start
    f1 = config.chirp_freq_end
    phase = 2 * np.pi * (f0 * t + (f1 - f0) / (2 * duration) * t ** 2)
    chirp = config.chirp_amplitude * np.sin(phase)

    # Окно Хэннинга для плавных фронтов (без щелчков)
    window = np.hanning(num_samples)
    chirp *= window

    return chirp.astype(np.float32)


# ─── Основной тестер ─────────────────────────────────────────────────────────

class LatencyTester:
    """Измеряет задержку аудиотракта методом cross-correlation."""

    def __init__(self, config: LatencyConfig):
        self.config = config
        self.chirp = generate_chirp(config)
        self.measurements: List[LatencyMeasurement] = []

        # Буферы для записи
        self._recording: List[np.ndarray] = []
        self._recording_lock = threading.Lock()

        # Управление импульсами
        self._chirp_pending = False
        self._chirp_position = 0
        self._chirp_lock = threading.Lock()

        # Лог
        self._log_file: Optional[Path] = None
        self._log_lines: List[str] = []

    def _log(self, msg: str, console: bool = True):
        self._log_lines.append(msg)
        if console:
            print(msg)

    def _setup_log(self):
        log_dir = Path(self.config.log_dir)
        log_dir.mkdir(parents=True, exist_ok=True)
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        self._log_file = log_dir / f"latency_log_{timestamp}.txt"

    def _save_log(self):
        if self._log_file:
            self._log_file.write_text("\n".join(self._log_lines), encoding="utf-8")

    def _trigger_chirp(self):
        """Запланировать отправку chirp-импульса."""
        with self._chirp_lock:
            self._chirp_pending = True
            self._chirp_position = 0

    def _audio_callback(self, indata, outdata, frames, time_info, status):
        """Callback для полнодуплексного потока."""
        if status:
            pass  # игнорируем xrun'ы в логе, они отдельно обрабатываются

        # Запись входа
        in_block = indata[:, 0].copy()
        with self._recording_lock:
            self._recording.append(in_block)

        # Вывод chirp или тишина
        outdata.fill(0)
        with self._chirp_lock:
            if self._chirp_pending:
                remaining = len(self.chirp) - self._chirp_position
                to_copy = min(remaining, frames)
                outdata[:to_copy, 0] = self.chirp[self._chirp_position:self._chirp_position + to_copy]
                self._chirp_position += to_copy
                if self._chirp_position >= len(self.chirp):
                    self._chirp_pending = False

    def _find_chirp_in_recording(self, recording: np.ndarray,
                                  search_start: int, search_end: int) -> Optional[tuple]:
        """
        Ищет chirp в записи методом cross-correlation.
        Возвращает (offset_samples, confidence) или None.
        """
        if search_end <= search_start or search_end > len(recording):
            return None

        segment = recording[search_start:search_end]
        if len(segment) < len(self.chirp):
            return None

        # Нормализованная кросс-корреляция
        chirp = self.chirp
        chirp_norm = np.sqrt(np.sum(chirp ** 2))
        if chirp_norm < 1e-10:
            return None

        # Используем scipy-стиль корреляции через FFT для скорости
        n = len(segment)
        m = len(chirp)
        # Корреляция через свёртку
        corr = np.correlate(segment, chirp, mode='valid')

        if len(corr) == 0:
            return None

        # Нормализация: делим на энергию скользящего окна
        seg_sq = segment ** 2
        cum_sq = np.cumsum(seg_sq)
        window_energy = np.zeros(len(corr))
        window_energy[0] = cum_sq[m - 1]
        window_energy[1:] = cum_sq[m:m + len(corr) - 1] - cum_sq[:len(corr) - 1]
        window_rms = np.sqrt(np.maximum(window_energy, 1e-20))

        norm_corr = corr / (window_rms * chirp_norm)

        peak_idx = np.argmax(np.abs(norm_corr))
        confidence = float(np.abs(norm_corr[peak_idx]))

        if confidence < self.config.detection_threshold:
            return None

        return (search_start + peak_idx, confidence)

    def _measure_latency(self, chirp_send_sample: int, recording: np.ndarray) -> Optional[LatencyMeasurement]:
        """Измеряет задержку для одного chirp-импульса."""
        cfg = self.config
        max_latency_samples = int(cfg.max_latency_ms / 1000.0 * cfg.sample_rate)

        # Ищем chirp после момента отправки
        search_start = chirp_send_sample
        search_end = min(chirp_send_sample + max_latency_samples + len(self.chirp),
                         len(recording))

        result = self._find_chirp_in_recording(recording, search_start, search_end)
        if result is None:
            return None

        offset_samples, confidence = result
        latency_samples = offset_samples - chirp_send_sample
        latency_ms = latency_samples / cfg.sample_rate * 1000.0

        return LatencyMeasurement(
            index=len(self.measurements) + 1,
            wall_time=datetime.datetime.now().isoformat(timespec="milliseconds"),
            elapsed_sec=0,
            latency_ms=round(latency_ms, 2),
            confidence=round(confidence, 3),
            valid=confidence > 0.3,
        )

    def run(self):
        """Запуск полного теста."""
        cfg = self.config
        total_seconds = cfg.duration_min * 60.0

        self._setup_log()

        self._log(f"{'=' * 60}")
        self._log(f"  Audio Latency Tester")
        self._log(f"  Chirp: {cfg.chirp_freq_start:.0f}-{cfg.chirp_freq_end:.0f} Гц, "
                  f"{cfg.chirp_duration_ms:.0f} мс")
        self._log(f"  Интервал: {cfg.interval_sec:.1f} сек")
        self._log(f"  Длительность: {cfg.duration_min} мин")
        self._log(f"  Sample rate: {cfg.sample_rate} Гц")
        self._log(f"{'=' * 60}")
        self._log("")

        # Устройства
        self._log(f"Устройство вывода: {sd.query_devices(kind='output')['name']}")
        self._log(f"Устройство ввода:  {sd.query_devices(kind='input')['name']}")
        self._log("")

        self._log("Доступные аудиоустройства:")
        self._log(str(sd.query_devices()))
        self._log("")

        # Запуск потока
        block_size = 256  # маленький для точности
        chirp_send_times: List[int] = []  # sample positions когда отправили chirp
        total_samples = [0]  # mutable в closure

        def callback(indata, outdata, frames, time_info, status):
            in_block = indata[:, 0].copy()
            with self._recording_lock:
                self._recording.append(in_block)

            outdata.fill(0)
            with self._chirp_lock:
                if self._chirp_pending:
                    remaining = len(self.chirp) - self._chirp_position
                    to_copy = min(remaining, frames)
                    outdata[:to_copy, 0] = self.chirp[self._chirp_position:self._chirp_position + to_copy]
                    if self._chirp_position == 0:
                        chirp_send_times.append(total_samples[0])
                    self._chirp_position += to_copy
                    if self._chirp_position >= len(self.chirp):
                        self._chirp_pending = False

            total_samples[0] += frames

        try:
            with sd.Stream(
                samplerate=cfg.sample_rate,
                blocksize=block_size,
                channels=cfg.channels,
                dtype='float32',
                callback=callback,
            ):
                start = time.time()
                next_chirp = start + 1.0  # первый chirp через 1 секунду (пропустить начало)
                measurement_idx = 0

                self._log(f"Тест запущен. Ожидание {cfg.interval_sec} сек между импульсами...\n")

                while time.time() - start < total_seconds:
                    now = time.time()

                    # Отправляем chirp по расписанию
                    if now >= next_chirp:
                        self._trigger_chirp()
                        next_chirp = now + cfg.interval_sec
                        measurement_idx += 1

                    # Анализируем результаты с задержкой (ждём чтобы ответ успел прийти)
                    # Берём запись и ищем корреляцию
                    if chirp_send_times and len(chirp_send_times) > len(self.measurements):
                        # Ждём max_latency после отправки, потом анализируем
                        send_sample = chirp_send_times[len(self.measurements)]
                        wait_samples = send_sample + int(cfg.max_latency_ms / 1000.0 * cfg.sample_rate) + len(self.chirp) * 2
                        if total_samples[0] > wait_samples:
                            # Собираем запись
                            with self._recording_lock:
                                recording = np.concatenate(self._recording)

                            m = self._measure_latency(send_sample, recording)
                            if m:
                                m.elapsed_sec = round(time.time() - start, 1)
                                m.index = len(self.measurements) + 1
                                self.measurements.append(m)

                                status = "OK" if m.valid else "LOW CONFIDENCE"
                                line = (
                                    f"  #{m.index:3d} | "
                                    f"T+{m.elapsed_sec:6.1f}с | "
                                    f"Задержка: {m.latency_ms:7.2f} мс | "
                                    f"Уверенность: {m.confidence:.3f} | "
                                    f"{status}"
                                )
                                self._log(line)
                            else:
                                self.measurements.append(LatencyMeasurement(
                                    index=len(self.measurements) + 1,
                                    wall_time=datetime.datetime.now().isoformat(timespec="milliseconds"),
                                    elapsed_sec=round(time.time() - start, 1),
                                    latency_ms=-1,
                                    confidence=0,
                                    valid=False,
                                ))
                                self._log(
                                    f"  #{len(self.measurements):3d} | "
                                    f"T+{time.time() - start:6.1f}с | "
                                    f"Chirp НЕ НАЙДЕН в записи"
                                )

                    # Прогресс
                    elapsed = time.time() - start
                    pct = elapsed / total_seconds * 100.0
                    valid_count = sum(1 for m in self.measurements if m.valid)
                    sys.stdout.write(
                        f"\rПрогресс: {pct:5.1f}% | "
                        f"Измерений: {len(self.measurements)} (валидных: {valid_count})"
                    )
                    sys.stdout.flush()

                    time.sleep(0.05)

        except KeyboardInterrupt:
            self._log("\n\nТест прерван (Ctrl+C).")
        except Exception as e:
            self._log(f"\nОшибка: {e}")
            raise

        # Итоги
        self._print_summary()

        # Сохраняем
        self._save_log()
        self._log(f"\nЛог сохранён: {self._log_file.resolve()}")
        self._save_log()  # ещё раз чтобы включить последнюю строку

    def _print_summary(self):
        """Итоговая статистика."""
        valid = [m for m in self.measurements if m.valid and m.latency_ms >= 0]
        invalid = [m for m in self.measurements if not m.valid or m.latency_ms < 0]

        self._log(f"\n\n{'=' * 60}")
        self._log("=== ИТОГИ ТЕСТА ЗАДЕРЖКИ ===")
        self._log(f"Всего измерений: {len(self.measurements)}")
        self._log(f"Валидных: {len(valid)}")
        self._log(f"Невалидных / не найдено: {len(invalid)}")

        if valid:
            latencies = [m.latency_ms for m in valid]
            arr = np.array(latencies)

            self._log(f"\n--- Статистика задержки (мс) ---")
            self._log(f"  Минимум:     {np.min(arr):8.2f} мс")
            self._log(f"  Максимум:    {np.max(arr):8.2f} мс")
            self._log(f"  Среднее:     {np.mean(arr):8.2f} мс")
            self._log(f"  Медиана:     {np.median(arr):8.2f} мс")
            self._log(f"  Std (σ):     {np.std(arr):8.2f} мс")
            self._log(f"  P95:         {np.percentile(arr, 95):8.2f} мс")
            self._log(f"  P99:         {np.percentile(arr, 99):8.2f} мс")
            self._log(f"  Jitter (σ):  {np.std(arr):8.2f} мс")

            # Распределение
            self._log(f"\n--- Распределение ---")
            ranges = [(0, 5), (5, 10), (10, 20), (20, 50), (50, 100), (100, 200), (200, 500)]
            for lo, hi in ranges:
                count = sum(1 for v in latencies if lo <= v < hi)
                if count > 0:
                    bar = "█" * int(count / len(latencies) * 40)
                    pct = count / len(latencies) * 100
                    self._log(f"  {lo:3d}-{hi:3d} мс: {count:4d} ({pct:5.1f}%) {bar}")

            # Тренд: первая vs последняя четверть
            if len(latencies) >= 8:
                q = len(latencies) // 4
                first_q = np.mean(latencies[:q])
                last_q = np.mean(latencies[-q:])
                drift = last_q - first_q
                self._log(f"\n--- Тренд ---")
                self._log(f"  Первая четверть (avg): {first_q:.2f} мс")
                self._log(f"  Последняя четверть:    {last_q:.2f} мс")
                self._log(f"  Дрейф:                 {drift:+.2f} мс")

            # Выбросы (> 2σ от среднего)
            mean = np.mean(arr)
            std = np.std(arr)
            outliers = [m for m in valid if abs(m.latency_ms - mean) > 2 * std]
            if outliers:
                self._log(f"\n--- Выбросы (>2σ) ---")
                for m in outliers[:10]:
                    self._log(f"  #{m.index}: {m.latency_ms:.2f} мс (T+{m.elapsed_sec:.1f}с)")

        else:
            self._log("\nНет валидных измерений. Проверьте подключение аудио.")

        self._log("=" * 60)


# ─── CLI ─────────────────────────────────────────────────────────────────────

def parse_args() -> LatencyConfig:
    parser = argparse.ArgumentParser(
        description="Audio Latency Tester — измерение задержки аудиотракта",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Примеры:
  python latency_test.py 1                    # 1 минута
  python latency_test.py 5 --interval 3       # 5 минут, импульс каждые 3 сек
  python latency_test.py 2 --sample-rate 44100
  python latency_test.py 0.5 --interval 1     # 30 сек, каждую секунду
        """
    )

    parser.add_argument(
        "duration", type=float,
        help="Длительность теста в минутах")

    parser.add_argument(
        "--interval", type=float, default=2.0,
        help="Интервал между импульсами в секундах (по умолчанию: 2)")

    parser.add_argument(
        "--sample-rate", type=int, default=48000,
        help="Частота дискретизации (по умолчанию: 48000)")

    parser.add_argument(
        "--chirp-duration", type=float, default=30.0,
        help="Длительность chirp-импульса в мс (по умолчанию: 30)")

    parser.add_argument(
        "--max-latency", type=float, default=500.0,
        help="Максимальная ожидаемая задержка в мс (по умолчанию: 500)")

    parser.add_argument(
        "--list-devices", action="store_true",
        help="Показать доступные аудиоустройства и выйти")

    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        sys.exit(0)

    return LatencyConfig(
        duration_min=args.duration,
        sample_rate=args.sample_rate,
        interval_sec=args.interval,
        chirp_duration_ms=args.chirp_duration,
        max_latency_ms=args.max_latency,
        log_dir=str(Path(__file__).parent),
    )


def main():
    config = parse_args()
    tester = LatencyTester(config)
    tester.run()


if __name__ == "__main__":
    main()
