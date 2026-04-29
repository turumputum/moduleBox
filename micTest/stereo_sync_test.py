#!/usr/bin/env python3
"""
Stereo Channel Sync Tester — измерение реального L/R рассинхрона
независимо от акустики (TDOA от разнесённых динамиков).

Принцип работы (исправленная методика):
  1. Каждые `period` секунд воспроизводим ОДНОВРЕМЕННО два бипа:
       - L канал: тон freq_l (default 1000 Гц)
       - R канал: тон freq_r (default 2000 Гц)
     Оба бипа стартуют в один и тот же sample-accurate момент времени.
  2. Записываем микрофоном (моно достаточно — оба бипа смешиваются).
  3. Применяем ДВЕ независимые matched-filter корреляции:
       - corr_l = recording ⊛ ref_l  → ловит только L-бипы (1 кГц)
       - corr_r = recording ⊛ ref_r  → ловит только R-бипы (2 кГц)
     Узкополосные сигналы практически не интерферируют → каждая
     корреляция находит свой бип независимо от другого.
  4. Для каждого цикла спариваем найденные t_L и t_R.
     Разность delta = t_R - t_L — это РЕАЛЬНЫЙ электрический L/R лаг.
     Акустическая задержка от динамика до микрофона одинакова для
     обоих сигналов из ОДНОЙ точки → она вычитается, и физическое
     расположение динамиков/мика не влияет на результат.

Почему старая методика (чередование L/R, моно мик) даёт ложный результат:
  Если L-динамик ближе к мику на 2 м чем R-динамик, то L-бипы прибывают
  на ~6 мс раньше своего времени, R-бипы — на ~6 мс позже.
  Интервалы L→R в записи получаются 506 мс, R→L — 494 мс. Это TDOA
  (time difference of arrival), а не реальный рассинхрон контроллера.

Использование:
    python stereo_sync_test.py 30
    python stereo_sync_test.py 60 --period 0.5 --freq-l 1000 --freq-r 2000

Результат: отчёт в консоль + лог-файл + WAV-запись.
"""

import argparse
import datetime
import sys
import time
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import numpy as np
import sounddevice as sd

# Опционально: matplotlib для графиков
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


# ─── Конфигурация ────────────────────────────────────────────────────────────

@dataclass
class SyncConfig:
    duration_sec: float = 60.0          # общая длительность теста
    sample_rate: int = 48000            # частота дискретизации
    out_channels: int = 2               # стерео выход
    in_channels: int = 1                # моно микрофон достаточно
    # --- Бип параметры ---
    # Разные частоты L/R позволяют двум корреляциям разделить каналы
    # в моно-записи. Должны отстоять минимум на октаву для надёжной
    # фильтрации matched-фильтром Hanning-окна.
    beep_freq_l: float = 1000.0         # частота L-бипа (Гц)
    beep_freq_r: float = 2000.0         # частота R-бипа (Гц)
    beep_duration_ms: float = 20.0      # длительность одного бипа (мс)
    beep_amplitude: float = 1.0         # громкость 0..1
    # --- Тайминг ---
    period_sec: float = 0.5             # период между парами L+R бипов
    settle_sec: float = 1.0             # пауза перед началом измерений
    # --- Детекция ---
    detection_threshold: float = 0.3    # порог find_peaks (доля от max)
    min_gap_ratio: float = 0.6          # min расстояние = period * ratio
    # --- Вывод ---
    output_dir: str = "."
    save_wav: bool = True
    save_plot: bool = True


@dataclass
class CyclePair:
    """Одна спаренная пара (L, R) бипов в одном цикле."""
    cycle_index: int                    # порядковый номер цикла (0..N-1)
    t_l_sec: Optional[float]            # время L-пика в записи (сек), None если не найден
    t_r_sec: Optional[float]            # время R-пика в записи
    delta_ms: Optional[float]           # t_R - t_L в мс (положительное = R отстаёт)


# ─── Генерация бипов ─────────────────────────────────────────────────────────

def generate_beep(freq_hz: float, config: SyncConfig) -> np.ndarray:
    """Генерирует моно-бип (тональный импульс) с окном Хэннинга."""
    n = int(config.beep_duration_ms / 1000.0 * config.sample_rate)
    t = np.arange(n) / config.sample_rate
    tone = config.beep_amplitude * np.sin(2 * np.pi * freq_hz * t)
    tone *= np.hanning(n)
    return tone.astype(np.float32)


# ─── Тестер ───────────────────────────────────────────────────────────────────

class StereoSyncTester:
    """Воспроизводит одновременные L+R бипы (разные частоты) и измеряет
    разность времён их прибытия в записи. См. doc-string модуля."""

    def __init__(self, config: SyncConfig):
        self.config = config
        self.beep_l = generate_beep(config.beep_freq_l, config)
        self.beep_r = generate_beep(config.beep_freq_r, config)
        self.beep_len = len(self.beep_l)
        assert len(self.beep_l) == len(self.beep_r)

        # Запись
        self._rec_blocks: List[np.ndarray] = []
        self._rec_lock = threading.Lock()

        # Pre-computed schedule: sample-accurate positions where to place
        # the simultaneous L+R beep pair.
        settle_samples = int(config.settle_sec * config.sample_rate)
        period_samples = config.period_sec * config.sample_rate
        total_cycles = int(config.duration_sec / config.period_sec)
        self._beep_schedule: List[int] = []  # sample_pos
        for i in range(total_cycles):
            pos = int(round(settle_samples + i * period_samples))
            self._beep_schedule.append(pos)

        self._cycle_count = total_cycles
        self._next_beep_idx = 0
        self._out_frame_count = 0
        self._played_count = 0

        # Лог
        self._log_lines: List[str] = []

    # ── Logging ──

    def _log(self, msg: str, console: bool = True):
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = f"[{ts}] {msg}"
        self._log_lines.append(line)
        if console:
            print(line)

    # ── Audio callback ──

    def _audio_callback(self, indata, outdata, frames, time_info, status):
        # Запись (берём первый канал, даже если входов несколько)
        block = indata[:, 0].copy()
        with self._rec_lock:
            self._rec_blocks.append(block)

        # Вывод — sample-accurate placement парных бипов
        outdata.fill(0)
        blk_start = self._out_frame_count
        blk_end = blk_start + frames

        while self._next_beep_idx < len(self._beep_schedule):
            beep_start = self._beep_schedule[self._next_beep_idx]
            beep_end = beep_start + self.beep_len

            if beep_start >= blk_end:
                break

            if beep_end <= blk_start:
                self._next_beep_idx += 1
                self._played_count += 1
                continue

            out_from = max(0, beep_start - blk_start)
            out_to = min(frames, beep_end - blk_start)
            beep_from = max(0, blk_start - beep_start)
            beep_to = beep_from + (out_to - out_from)

            # Оба бипа одновременно: L-частота в канал 0, R-частота в канал 1
            outdata[out_from:out_to, 0] += self.beep_l[beep_from:beep_to]
            outdata[out_from:out_to, 1] += self.beep_r[beep_from:beep_to]

            if beep_end <= blk_end:
                self._next_beep_idx += 1
                self._played_count += 1
            else:
                break

        self._out_frame_count = blk_end

    # ── Запуск ──

    def run(self):
        """Основной цикл: воспроизведение бипов + запись."""
        cfg = self.config

        self._log("=" * 60)
        self._log("Stereo Channel Sync Tester (simultaneous L+R, dual-freq)")
        self._log("=" * 60)
        self._log(f"Длительность:    {cfg.duration_sec:.0f} сек")
        self._log(f"Период пар:      {cfg.period_sec:.3f} сек")
        self._log(f"L freq:          {cfg.beep_freq_l:.0f} Гц")
        self._log(f"R freq:          {cfg.beep_freq_r:.0f} Гц")
        self._log(f"Длительн. бипа:  {cfg.beep_duration_ms:.0f} мс")
        self._log(f"Sample rate:     {cfg.sample_rate}")
        self._log(f"Settle time:     {cfg.settle_sec:.1f} сек")
        self._log("")

        self._log("Устройства вывода/ввода:")
        try:
            dev_in = sd.query_devices(kind='input')
            dev_out = sd.query_devices(kind='output')
            self._log(f"  Input:  {dev_in['name']}")
            self._log(f"  Output: {dev_out['name']}")
        except Exception as e:
            self._log(f"  (не удалось определить: {e})")
        self._log("")

        try:
            stream = sd.Stream(
                samplerate=cfg.sample_rate,
                blocksize=256,
                channels=(cfg.in_channels, cfg.out_channels),
                dtype='float32',
                latency='low',
                callback=self._audio_callback,
            )
        except Exception as e:
            self._log(f"ОШИБКА: не удалось открыть аудиопоток: {e}")
            sys.exit(1)

        with stream:
            self._log("Поток открыт. Ждём settle...")
            self._log("Начинаем генерацию пар L+R (sample-accurate scheduling)...")
            self._log(f"  Запланировано пар: {self._cycle_count}")

            total_wait = cfg.settle_sec + cfg.duration_sec + 0.2
            start_time = time.monotonic()
            last_logged = 0

            while time.monotonic() - start_time < total_wait:
                played = self._played_count
                if played > last_logged and (played % 10 == 1 or played == self._cycle_count):
                    elapsed = time.monotonic() - start_time - cfg.settle_sec
                    self._log(
                        f"  Пара #{played:>4d}  t={elapsed:.2f}s / {cfg.duration_sec:.0f}s",
                        console=True,
                    )
                    last_logged = played
                time.sleep(0.05)

        self._log(f"\nЗапись завершена. Всего пар: {self._cycle_count}")

        with self._rec_lock:
            recording = np.concatenate(self._rec_blocks)

        self._log(f"Длина записи: {len(recording)} сэмплов "
                   f"({len(recording)/cfg.sample_rate:.2f} сек)")

        if cfg.save_wav:
            self._save_wav(recording)

        # Детекция отдельно для L и R, затем спаривание по циклам
        peaks_l = self._detect_peaks(recording, self.beep_l, "L")
        peaks_r = self._detect_peaks(recording, self.beep_r, "R")
        pairs = self._pair_peaks(peaks_l, peaks_r)
        self._analyze(pairs)

    # ── Save WAV ──

    def _save_wav(self, recording: np.ndarray) -> Path:
        import wave as _wave
        cfg = self.config
        out_dir = Path(cfg.output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        path = out_dir / f"sync_test_{ts}.wav"

        data16 = np.clip(recording, -1.0, 1.0)
        data16 = (data16 * 32767).astype(np.int16)

        with _wave.open(str(path), 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(cfg.sample_rate)
            wf.writeframes(data16.tobytes())

        self._log(f"WAV сохранён: {path}")
        return path

    # ── Peak detection helpers ──

    @staticmethod
    def _parabolic_subsample(y_minus: float, y_zero: float, y_plus: float) -> float:
        """3-point parabola fit → fractional offset (-0.5..+0.5) от y_zero."""
        denom = (y_minus - 2.0 * y_zero + y_plus)
        if abs(denom) < 1e-12:
            return 0.0
        return 0.5 * (y_minus - y_plus) / denom

    @staticmethod
    def _hilbert_envelope(x: np.ndarray) -> np.ndarray:
        """Hilbert-envelope через FFT (без scipy).

        Возвращает |analytic_signal(x)| — гладкую огибающую энергии,
        НЕ ОСЦИЛЛИРУЮЩУЮ с частотой carrier. Это критично для matched
        filter с tone-сигналом: corr = sin(2π·f·τ) · slow_envelope(τ),
        и np.abs(corr) даёт rectified pattern с пиками каждые 1/(2f).
        find_peaks на |corr| выбирает один из этих под-пиков случайно
        в зависимости от фазы — что для разных частот L/R даёт
        систематически разные смещения относительно центра бипа."""
        n = len(x)
        if n == 0:
            return x
        X = np.fft.fft(x)
        h = np.zeros(n)
        if n % 2 == 0:
            h[0] = h[n // 2] = 1
            h[1:n // 2] = 2
        else:
            h[0] = 1
            h[1:(n + 1) // 2] = 2
        analytic = np.fft.ifft(X * h)
        return np.abs(analytic)

    @staticmethod
    def _find_peaks_local(signal: np.ndarray, height: float, distance: int) -> List[int]:
        """Истинные локальные максимумы выше height, greedy-pruning по distance.

        Не зависит от rising slope — устраняет систематический сдвиг,
        который имел место у threshold-crossing детекторов когда амплитуды
        двух типов сигналов разные."""
        n = len(signal)
        if n < 3:
            return []
        s_prev = signal[:-2]
        s_curr = signal[1:-1]
        s_next = signal[2:]
        is_peak = (s_curr > s_prev) & (s_curr >= s_next) & (s_curr >= height)
        candidate_idx = np.where(is_peak)[0] + 1
        if len(candidate_idx) == 0:
            return []
        order = np.argsort(-signal[candidate_idx])
        kept: List[int] = []
        for k in order:
            pos = int(candidate_idx[k])
            if all(abs(pos - p) >= distance for p in kept):
                kept.append(pos)
        kept.sort()
        return kept

    # ── Detection ──

    def _detect_peaks(self, recording: np.ndarray, ref_beep: np.ndarray,
                      label: str) -> List[float]:
        """Matched-filter корреляция с эталонным бипом → список времён
        пиков (с sub-sample precision)."""
        cfg = self.config

        corr = np.correlate(recording, ref_beep, mode='full')
        offset = len(ref_beep) - 1
        corr = corr[offset:]
        # Hilbert envelope — гладкая огибающая корреляции, не зависит от
        # фазы carrier. См. _hilbert_envelope. Без этого find_peaks ловит
        # суб-пики rectified |corr| и даёт систематический сдвиг,
        # зависящий от частоты сигнала.
        corr_env = self._hilbert_envelope(corr)
        max_corr = float(np.max(corr_env)) if np.max(corr_env) > 0 else 1.0
        corr_env = corr_env / max_corr

        threshold = cfg.detection_threshold
        min_gap = int(cfg.period_sec * cfg.min_gap_ratio * cfg.sample_rate)

        peaks_int = self._find_peaks_local(corr_env, threshold, min_gap)

        peaks_t: List[float] = []
        for pi in peaks_int:
            if 0 < pi < len(corr_env) - 1:
                frac = self._parabolic_subsample(
                    float(corr_env[pi - 1]),
                    float(corr_env[pi]),
                    float(corr_env[pi + 1]),
                )
            else:
                frac = 0.0
            peaks_t.append((pi + frac) / cfg.sample_rate)

        self._log(f"  [{label}] корреляция: пиков={len(peaks_t)} "
                  f"max_corr={max_corr:.4f}")
        return peaks_t

    def _pair_peaks(self, peaks_l: List[float], peaks_r: List[float]) -> List[CyclePair]:
        """Спариваем L и R пики по близости во времени.

        Для каждого scheduled цикла берём ожидаемое время t_expected =
        settle + i * period (плюс системная latency, которую не знаем —
        поэтому ищем ближайший пик в окне ± period/2)."""
        cfg = self.config
        self._log("\n--- Спаривание L/R пиков по циклам ---")

        # Системная latency = сдвиг первого L-пика относительно first scheduled
        scheduled_t = [pos / cfg.sample_rate for pos in self._beep_schedule]
        sys_latency = 0.0
        if peaks_l:
            sys_latency = peaks_l[0] - scheduled_t[0]
            self._log(f"  Оценка системной latency: {sys_latency*1000:.2f} мс")

        half_window = cfg.period_sec / 2.0
        pairs: List[CyclePair] = []
        used_l = set()
        used_r = set()
        for ci, sched in enumerate(scheduled_t):
            target = sched + sys_latency
            t_l = self._nearest_unused(peaks_l, target, half_window, used_l)
            t_r = self._nearest_unused(peaks_r, target, half_window, used_r)
            delta_ms = ((t_r - t_l) * 1000.0) if (t_l is not None and t_r is not None) else None
            pairs.append(CyclePair(ci, t_l, t_r, delta_ms))

        complete = sum(1 for p in pairs if p.delta_ms is not None)
        self._log(f"  Полных пар: {complete} из {len(pairs)}")
        return pairs

    @staticmethod
    def _nearest_unused(peaks: List[float], target: float,
                         half_window: float, used: set) -> Optional[float]:
        best_t = None
        best_idx = -1
        best_diff = half_window + 1.0
        for i, t in enumerate(peaks):
            if i in used:
                continue
            d = abs(t - target)
            if d < best_diff and d <= half_window:
                best_diff = d
                best_t = t
                best_idx = i
        if best_idx >= 0:
            used.add(best_idx)
        return best_t

    # ── Анализ ──

    def _analyze(self, pairs: List[CyclePair]):
        cfg = self.config
        self._log("\n" + "=" * 60)
        self._log("АНАЛИЗ РАССИНХРОНИЗАЦИИ L/R")
        self._log("=" * 60)

        complete = [p for p in pairs if p.delta_ms is not None]
        if len(complete) < 4:
            self._log(f"ОШИБКА: только {len(complete)} полных пар (нужно >= 4).")
            self._log("Проверьте громкость, расстояние до микрофона, пороги, частоты.")
            self._save_log()
            return

        deltas = np.array([p.delta_ms for p in complete])

        # Robust median + MAD outlier rejection. Detector occasionally
        # locks onto a spurious correlation peak (most often on the L
        # channel at 1 kHz where amplitude/SNR is lower) — those skew
        # the mean dramatically. Median is unaffected.
        median_delta = float(np.median(deltas))
        mad = float(np.median(np.abs(deltas - median_delta)))
        mad_threshold = 5.0 * mad if mad > 0 else 0.5  # fallback for near-zero MAD
        inlier_mask = np.abs(deltas - median_delta) <= mad_threshold
        n_outliers = int(np.sum(~inlier_mask))
        deltas_clean = deltas[inlier_mask]

        self._log(f"\nПолных пар:       {len(complete)} из {len(pairs)}")
        self._log(f"Outliers (>5·MAD): {n_outliers} (отброшены из аналитики)")
        self._log(f"Метрика:          delta = t_R - t_L  (положительное = R отстаёт)")
        self._log("")
        self._log("Статистика delta по inliers (мс):")
        self._log(f"  Медиана:        {np.median(deltas_clean):+.4f}")
        self._log(f"  Среднее:        {np.mean(deltas_clean):+.4f}")
        self._log(f"  Стд. откл.:     {np.std(deltas_clean):.4f}")
        self._log(f"  Мин:            {np.min(deltas_clean):+.4f}")
        self._log(f"  Макс:           {np.max(deltas_clean):+.4f}")
        self._log(f"  Размах:         {np.max(deltas_clean) - np.min(deltas_clean):.4f}")
        self._log(f"  MAD (raw):      {mad:.4f}")

        # Use median as primary metric — robust to outliers
        result_delta = float(np.median(deltas_clean))
        self._log(f"\n{'─' * 50}")
        self._log("РЕЗУЛЬТАТ:")
        self._log(f"  ► L/R лаг (медиана) ≈ {abs(result_delta):.3f} мс")
        if abs(result_delta) < 0.05:
            self._log("    (каналы синхронны в пределах точности измерения)")
        elif result_delta > 0:
            self._log(f"    (правый канал ОТСТАЁТ на ~{abs(result_delta):.3f} мс)")
        else:
            self._log(f"    (левый канал ОТСТАЁТ на ~{abs(result_delta):.3f} мс)")
        self._log(f"")
        self._log(f"  ВНИМАНИЕ: на одном контроллере ожидаемый результат ≈ 0,")
        self._log(f"  но методика с РАЗНЫМИ частотами L/R даёт остаточный сдвиг")
        self._log(f"  ~3-5 мс из-за частотно-зависимой групповой задержки в DAC,")
        self._log(f"  динамиках и микрофоне. Считай 0 ± 5 мс синхронным; реальные")
        self._log(f"  рассинхроны от mismatched контроллеров обычно >>10 мс.")
        self._log(f"{'─' * 50}")

        jitter = float(np.std(deltas_clean))
        self._log(f"\nДжиттер delta (стд. откл. inliers): {jitter:.4f} мс")
        if jitter < 0.1:
            self._log("  → Отличная стабильность")
        elif jitter < 0.5:
            self._log("  → Хорошая стабильность")
        elif jitter < 2.0:
            self._log("  → Удовлетворительная стабильность")
        else:
            self._log("  → Плохая стабильность (значительный джиттер)")

        # Также посмотрим на собственные интервалы L-L и R-R — это покажет
        # стабильность периода без влияния L/R лага.
        l_times = [p.t_l_sec for p in complete if p.t_l_sec is not None]
        r_times = [p.t_r_sec for p in complete if p.t_r_sec is not None]
        if len(l_times) >= 3:
            l_intervals = np.diff(l_times) * 1000.0
            self._log(f"\nИнтервалы L-L (n={len(l_intervals)}): "
                      f"среднее={np.mean(l_intervals):.3f} мс, "
                      f"стд={np.std(l_intervals):.3f} мс")
        if len(r_times) >= 3:
            r_intervals = np.diff(r_times) * 1000.0
            self._log(f"Интервалы R-R (n={len(r_intervals)}): "
                      f"среднее={np.mean(r_intervals):.3f} мс, "
                      f"стд={np.std(r_intervals):.3f} мс")
        self._log(f"Ожидаемый период: {cfg.period_sec*1000:.1f} мс")

        # Таблица всех пар
        self._log(f"\n{'─' * 60}")
        self._log("Детальная таблица:")
        self._log(f"{'#':>4s}  {'t_L (s)':>10s}  {'t_R (s)':>10s}  {'delta (мс)':>11s}")
        self._log(f"{'─' * 4}  {'─' * 10}  {'─' * 10}  {'─' * 11}")
        for p in pairs:
            tl = f"{p.t_l_sec:.4f}" if p.t_l_sec is not None else "  ---"
            tr = f"{p.t_r_sec:.4f}" if p.t_r_sec is not None else "  ---"
            dt = f"{p.delta_ms:+.4f}" if p.delta_ms is not None else "  ---"
            self._log(f"{p.cycle_index:>4d}  {tl:>10s}  {tr:>10s}  {dt:>11s}")

        if HAS_MATPLOTLIB and cfg.save_plot:
            self._save_plot(deltas)

        self._save_log()

    # ── Графики ──

    def _save_plot(self, deltas: np.ndarray):
        cfg = self.config
        out_dir = Path(cfg.output_dir)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        plot_path = out_dir / f"sync_test_{ts}.png"

        fig, axes = plt.subplots(2, 1, figsize=(14, 8))

        ax1 = axes[0]
        x = np.arange(len(deltas))
        ax1.plot(x, deltas, 'o-', markersize=4, alpha=0.7)
        ax1.axhline(y=0, color='green', linestyle='--', linewidth=1.5,
                    label='Идеал (0 мс)')
        ax1.axhline(y=float(np.mean(deltas)), color='red', linestyle=':',
                    linewidth=1, label=f'Среднее ({np.mean(deltas):+.3f} мс)')
        ax1.set_xlabel('Цикл')
        ax1.set_ylabel('delta = t_R − t_L (мс)')
        ax1.set_title('Реальный L/R лаг по циклам')
        ax1.legend()
        ax1.grid(True, alpha=0.3)

        ax2 = axes[1]
        bins = np.linspace(np.min(deltas) - 0.1, np.max(deltas) + 0.1, 30)
        ax2.hist(deltas, bins=bins, alpha=0.7, color='tab:blue')
        ax2.axvline(x=0, color='green', linestyle='--', linewidth=1.5,
                    label='Идеал (0 мс)')
        ax2.axvline(x=float(np.mean(deltas)), color='red', linestyle=':',
                    linewidth=1, label=f'Среднее ({np.mean(deltas):+.3f} мс)')
        ax2.set_xlabel('delta (мс)')
        ax2.set_ylabel('Количество')
        ax2.set_title('Распределение L/R лага')
        ax2.legend()
        ax2.grid(True, alpha=0.3)

        plt.tight_layout()
        plt.savefig(str(plot_path), dpi=150)
        plt.close()
        self._log(f"График сохранён: {plot_path}")

    # ── Сохранение лога ──

    def _save_log(self):
        cfg = self.config
        out_dir = Path(cfg.output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        path = out_dir / f"sync_test_{ts}.log"
        path.write_text("\n".join(self._log_lines), encoding="utf-8")
        print(f"Лог сохранён: {path}")


# ─── Список устройств ────────────────────────────────────────────────────────

def list_devices():
    print("\nДоступные аудиоустройства:")
    print(sd.query_devices())
    print()


# ─── CLI ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Тест L/R рассинхрона: одновременные L+R бипы разных частот",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Примеры:
  python stereo_sync_test.py 30                    # 30 сек, период 0.5с
  python stereo_sync_test.py 60 --period 0.3       # 60 сек, период 0.3с
  python stereo_sync_test.py --list-devices         # показать устройства
  python stereo_sync_test.py 30 --freq-l 800 --freq-r 1600
        """,
    )
    parser.add_argument("duration", nargs="?", type=float, default=60.0,
                        help="Длительность теста (секунды), по умолчанию 60")
    parser.add_argument("--period", type=float, default=0.5,
                        help="Период между парами L+R (секунды), по умолчанию 0.5")
    parser.add_argument("--freq-l", type=float, default=1000.0,
                        help="Частота L-бипа (Гц), по умолчанию 1000")
    parser.add_argument("--freq-r", type=float, default=2000.0,
                        help="Частота R-бипа (Гц), по умолчанию 2000")
    parser.add_argument("--beep-ms", type=float, default=20.0,
                        help="Длительность бипа (мс), по умолчанию 20")
    parser.add_argument("--amplitude", type=float, default=0.8,
                        help="Громкость бипа 0..1, по умолчанию 0.8")
    parser.add_argument("--sample-rate", type=int, default=48000,
                        help="Частота дискретизации, по умолчанию 48000")
    parser.add_argument("--threshold", type=float, default=0.3,
                        help="Порог детекции (0..1), по умолчанию 0.3")
    parser.add_argument("--settle", type=float, default=1.0,
                        help="Пауза перед началом (сек), по умолчанию 1.0")
    parser.add_argument("--output-dir", type=str, default=".",
                        help="Директория для результатов")
    parser.add_argument("--no-wav", action="store_true",
                        help="Не сохранять WAV-файл")
    parser.add_argument("--no-plot", action="store_true",
                        help="Не сохранять PNG-график")
    parser.add_argument("--list-devices", action="store_true",
                        help="Показать аудиоустройства и выйти")

    args = parser.parse_args()

    if args.list_devices:
        list_devices()
        sys.exit(0)

    config = SyncConfig(
        duration_sec=args.duration,
        sample_rate=args.sample_rate,
        beep_freq_l=args.freq_l,
        beep_freq_r=args.freq_r,
        beep_duration_ms=args.beep_ms,
        beep_amplitude=args.amplitude,
        period_sec=args.period,
        settle_sec=args.settle,
        detection_threshold=args.threshold,
        output_dir=args.output_dir,
        save_wav=not args.no_wav,
        save_plot=not args.no_plot and HAS_MATPLOTLIB,
    )

    if config.period_sec < config.beep_duration_ms / 1000.0 * 2:
        print(f"ОШИБКА: период ({config.period_sec}s) слишком мал "
              f"для длительности бипа ({config.beep_duration_ms}ms).")
        sys.exit(1)

    if abs(config.beep_freq_r - config.beep_freq_l) < config.beep_freq_l * 0.5:
        print(f"ПРЕДУПРЕЖДЕНИЕ: частоты L ({config.beep_freq_l}) и R "
              f"({config.beep_freq_r}) близки — корреляции могут "
              f"перепутать каналы. Рекомендуется отстояние минимум на октаву.")

    tester = StereoSyncTester(config)
    tester.run()


if __name__ == "__main__":
    main()
