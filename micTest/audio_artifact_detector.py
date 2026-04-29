#!/usr/bin/env python3
"""
Audio Artifact Detector — детектор потерянных пакетов и артефактов в аудиопотоке.

Генерирует тестовый сигнал (синус/меандр) на выход по умолчанию,
одновременно записывает с микрофона и анализирует входящий сигнал
на наличие выпадений (dropouts), щелчков, искажений и прочих аномалий.

Все аномалии логируются в файл с временными метками и длительностью.

Использование:
    python audio_artifact_detector.py <длительность_минут> [--freq 1000] [--wave sine|square] [--threshold 0.3]
"""

import argparse
import datetime
import logging
import math
import os
import sys
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import List, Optional

import numpy as np
import sounddevice as sd


# ─── Типы аномалий ──────────────────────────────────────────────────────────

class AnomalyType(Enum):
    DROPOUT = "DROPOUT"           # тишина / потеря сигнала
    CLICK = "CLICK"               # резкий скачок амплитуды (щелчок)
    LEVEL_DROP = "LEVEL_DROP"     # значительное падение уровня
    LEVEL_SPIKE = "LEVEL_SPIKE"   # значительный всплеск уровня
    FREQ_DRIFT = "FREQ_DRIFT"    # отклонение частоты от ожидаемой
    DISTORTION = "DISTORTION"     # искажение формы сигнала (THD)
    VOLUME_LOW = "VOLUME_LOW"     # громкость ниже допустимого порога (dBFS)
    VOLUME_HIGH = "VOLUME_HIGH"   # громкость выше допустимого порога (dBFS)
    CLIPPING = "CLIPPING"         # клиппинг (сэмплы ≥ порога)


@dataclass
class Anomaly:
    """Одна обнаруженная аномалия."""
    timestamp: float            # время от начала теста (секунды)
    wall_time: str              # реальное время (ISO)
    anomaly_type: AnomalyType
    duration_ms: float          # продолжительность (мс)
    severity: float             # 0..1
    details: str = ""


# ─── Конфигурация ────────────────────────────────────────────────────────────

@dataclass
class Config:
    duration_min: float = 1.0
    sample_rate: int = 48000
    block_size: int = 1024
    channels: int = 1
    frequency: float = 500.0
    wave_type: str = "sine"          # sine | square
    # --- Пороги детекции ---
    dropout_threshold: float = 0.01  # RMS ниже этого → dropout
    click_threshold: float = 1.0     # скачок амплитуды (множитель к среднему)
    level_drop_db: float = -2.0     # падение уровня (dB относительно среднего)
    level_spike_db: float = 2.0     # рост уровня (dB)
    freq_tolerance_pct: float = 5.0  # допуск отклонения частоты (%)
    thd_threshold: float = 0.0       # THD порог (0 = авто-калибровка)
    thd_margin: float = 0.1         # превышение над baseline THD для детекции
    # --- Громкость ---
    volume_low_dbfs: float = -40.0   # порог «слишком тихо» (dBFS)
    volume_high_dbfs: float = -1.0   # порог «слишком громко» (dBFS)
    clipping_threshold: float = 0.99 # |сэмпл| >= этого → клиппинг
    clipping_pct: float = 0.1        # % клиппированных сэмплов в блоке для детекции
    # --- Калибровка ---
    calibration_sec: float = 3.0     # длительность калибровки (секунды)
    # --- Минимальная длительность аномалии для логирования ---
    min_anomaly_duration_ms: float = 20.0
    # --- Файлы ---
    log_dir: str = "."


# ─── Генератор тестового сигнала ─────────────────────────────────────────────

class SignalGenerator:
    """Потоковый генератор тестового сигнала."""

    def __init__(self, config: Config):
        self.config = config
        self.phase = 0.0
        self.amplitude = 0.8  # чтобы не клиппить

    def generate(self, num_frames: int) -> np.ndarray:
        """Генерирует num_frames сэмплов."""
        cfg = self.config
        t = (np.arange(num_frames) + self.phase) / cfg.sample_rate
        if cfg.wave_type == "square":
            signal = self.amplitude * np.sign(np.sin(2 * np.pi * cfg.frequency * t))
        else:
            signal = self.amplitude * np.sin(2 * np.pi * cfg.frequency * t)
        self.phase += num_frames
        return signal.astype(np.float32)


# ─── Анализатор входного сигнала ─────────────────────────────────────────────

class SignalAnalyzer:
    """Анализирует блоки входного сигнала и детектирует аномалии."""

    def __init__(self, config: Config):
        self.config = config
        self.anomalies: List[Anomaly] = []
        self._lock = threading.Lock()

        # Скользящие метрики
        self._rms_history: List[float] = []
        self._rms_avg: float = 0.0
        self._prev_block: Optional[np.ndarray] = None
        self._block_index: int = 0
        self._start_time: float = 0.0

        # Состояние трекинга dropout
        self._in_dropout = False
        self._dropout_start: float = 0.0

        # Калибровка
        calibration_blocks = int(config.calibration_sec * config.sample_rate / config.block_size)
        self._warmup_blocks = max(calibration_blocks, 30)
        self._thd_samples: List[float] = []  # THD значения за калибровку
        self._baseline_thd: float = 0.0
        self._calibrated = False

        # Агрегация непрерывных аномалий одного типа
        self._ongoing: dict = {}  # AnomalyType -> {start_time, start_wall, block_count, total_severity, last_details}

        # Статистика громкости (dBFS)
        self._volume_dbfs_history: List[float] = []
        self._volume_min_dbfs: float = 0.0
        self._volume_max_dbfs: float = -120.0
        self._volume_sum_dbfs: float = 0.0
        self._volume_count: int = 0
        self._peak_sample: float = 0.0

    def set_start_time(self, t: float):
        self._start_time = t

    def _elapsed(self) -> float:
        return time.time() - self._start_time

    def _block_time(self) -> float:
        """Время начала текущего блока (от старта теста)."""
        return self._block_index * self.config.block_size / self.config.sample_rate

    def _start_or_continue_anomaly(self, atype: AnomalyType,
                                    severity: float, details: str = ""):
        """Начинает новую или продолжает текущую серию аномалий этого типа."""
        if atype not in self._ongoing:
            self._ongoing[atype] = {
                'start_time': self._block_time(),
                'start_wall': datetime.datetime.now().isoformat(timespec="milliseconds"),
                'block_count': 1,
                'total_severity': severity,
                'last_details': details,
            }
        else:
            self._ongoing[atype]['block_count'] += 1
            self._ongoing[atype]['total_severity'] += severity
            self._ongoing[atype]['last_details'] = details

    def _end_anomaly(self, atype: AnomalyType):
        """Завершает текущую серию аномалий, создаёт одну агрегированную запись."""
        if atype not in self._ongoing:
            return
        info = self._ongoing.pop(atype)
        block_dur_ms = self.config.block_size / self.config.sample_rate * 1000.0
        total_dur = info['block_count'] * block_dur_ms
        avg_severity = info['total_severity'] / info['block_count']
        if total_dur < self.config.min_anomaly_duration_ms:
            return
        a = Anomaly(
            timestamp=info['start_time'],
            wall_time=info['start_wall'],
            anomaly_type=atype,
            duration_ms=round(total_dur, 2),
            severity=round(min(avg_severity, 1.0), 3),
            details=f"{info['last_details']} (непрерывно {info['block_count']} блоков)",
        )
        with self._lock:
            self.anomalies.append(a)
        return a

    def _add_anomaly(self, atype: AnomalyType, duration_ms: float,
                     severity: float, details: str = ""):
        """Одиночная аномалия (без агрегации, напр. click)."""
        a = Anomaly(
            timestamp=self._block_time(),
            wall_time=datetime.datetime.now().isoformat(timespec="milliseconds"),
            anomaly_type=atype,
            duration_ms=round(duration_ms, 2),
            severity=round(min(severity, 1.0), 3),
            details=details,
        )
        with self._lock:
            self.anomalies.append(a)
        return a

    def finalize(self):
        """Завершает все незакрытые серии аномалий."""
        for atype in list(self._ongoing.keys()):
            self._end_anomaly(atype)

    @property
    def baseline_thd(self) -> float:
        return self._baseline_thd

    @property
    def calibrated(self) -> bool:
        return self._calibrated

    # --- RMS ---
    @staticmethod
    def _rms(block: np.ndarray) -> float:
        return float(np.sqrt(np.mean(block ** 2)))

    # --- Основная частота через автокорреляцию ---
    def _estimate_frequency(self, block: np.ndarray) -> Optional[float]:
        if len(block) < 64:
            return None
        corr = np.correlate(block, block, mode='full')
        corr = corr[len(corr) // 2:]
        # Ищем первый минимум, затем первый максимум после него
        d = np.diff(corr)
        zero_crossings = np.where(np.diff(np.sign(d)))[0]
        if len(zero_crossings) < 2:
            return None
        # Первый максимум после первого минимума
        first_min_idx = zero_crossings[0]
        peaks_after = zero_crossings[1:]
        if len(peaks_after) == 0:
            return None
        peak_idx = peaks_after[0] + 1
        if peak_idx <= 0:
            return None
        return self.config.sample_rate / peak_idx

    # --- THD (Total Harmonic Distortion) ---
    def _estimate_thd(self, block: np.ndarray) -> float:
        if len(block) < 256:
            return 0.0
        N = len(block)
        window = np.hanning(N)
        spectrum = np.abs(np.fft.rfft(block * window))
        freqs = np.fft.rfftfreq(N, 1.0 / self.config.sample_rate)

        fund_freq = self.config.frequency
        # Ищем фундаментальную
        fund_bin = int(round(fund_freq * N / self.config.sample_rate))
        if fund_bin <= 0 or fund_bin >= len(spectrum):
            return 0.0

        # Окно ±2 бина вокруг фундаментальной
        lo, hi = max(0, fund_bin - 2), min(len(spectrum), fund_bin + 3)
        fund_power = np.sum(spectrum[lo:hi] ** 2)

        # Гармоники 2..6
        harm_power = 0.0
        for h in range(2, 7):
            hbin = int(round(h * fund_freq * N / self.config.sample_rate))
            if hbin >= len(spectrum):
                break
            hlo, hhi = max(0, hbin - 2), min(len(spectrum), hbin + 3)
            harm_power += np.sum(spectrum[hlo:hhi] ** 2)

        if fund_power < 1e-12:
            return 0.0
        return float(np.sqrt(harm_power / fund_power))

    # --- Основной метод анализа блока ---
    def analyze_block(self, block: np.ndarray):
        """Анализирует один блок входного сигнала."""
        cfg = self.config
        rms = self._rms(block)
        self._rms_history.append(rms)
        # Храним последние 200 значений для статистики
        if len(self._rms_history) > 200:
            self._rms_history = self._rms_history[-200:]
        self._rms_avg = float(np.mean(self._rms_history))

        block_duration_ms = cfg.block_size / cfg.sample_rate * 1000.0
        warming_up = self._block_index < self._warmup_blocks

        # Собираем THD во время калибровки
        if warming_up and cfg.wave_type == "sine" and rms > 0.01:
            thd = self._estimate_thd(block)
            if thd > 0:
                self._thd_samples.append(thd)

        # Завершаем калибровку
        if self._block_index == self._warmup_blocks and not self._calibrated:
            self._calibrated = True
            if self._thd_samples:
                self._baseline_thd = float(np.mean(self._thd_samples))
                thd_std = float(np.std(self._thd_samples))
                # Если порог не задан вручную, ставим baseline + margin
                if cfg.thd_threshold <= 0:
                    cfg.thd_threshold = self._baseline_thd + cfg.thd_margin

        # 1) Dropout: RMS ниже порога
        if rms < cfg.dropout_threshold:
            if not self._in_dropout:
                self._in_dropout = True
                self._dropout_start = self._block_time()
        else:
            if self._in_dropout:
                dur = (self._block_time() - self._dropout_start) * 1000.0
                if dur >= cfg.min_anomaly_duration_ms and not warming_up:
                    self._add_anomaly(
                        AnomalyType.DROPOUT, dur, min(dur / 100.0, 1.0),
                        f"RMS упал ниже {cfg.dropout_threshold:.3f}")
                self._in_dropout = False

        if not warming_up and self._rms_avg > 0.01:
            # 2) Click: межблочный скачок
            if self._prev_block is not None:
                junction = np.abs(block[0] - self._prev_block[-1])
                avg_amplitude = self._rms_avg * 1.414
                if avg_amplitude > 1e-6 and junction > cfg.click_threshold * avg_amplitude:
                    self._add_anomaly(
                        AnomalyType.CLICK, 0.02,  # примерно 1 сэмпл
                        min(junction / (cfg.click_threshold * avg_amplitude), 1.0),
                        f"Скачок амплитуды: {junction:.4f} (порог {cfg.click_threshold * avg_amplitude:.4f})")

            # 3) Внутриблочные клики — резкие перепады внутри блока
            diffs = np.abs(np.diff(block))
            avg_amplitude = self._rms_avg * 1.414
            if avg_amplitude > 1e-6:
                click_mask = diffs > cfg.click_threshold * avg_amplitude
                num_clicks = int(np.sum(click_mask))
                if num_clicks > 0:
                    # Не считаем клики в меандре (для square wave перепады нормальны)
                    if cfg.wave_type != "square" or num_clicks > (cfg.frequency * cfg.block_size / cfg.sample_rate * 2 + 4):
                        click_dur = num_clicks / cfg.sample_rate * 1000.0
                        self._add_anomaly(
                            AnomalyType.CLICK, click_dur,
                            min(float(np.max(diffs)) / (cfg.click_threshold * avg_amplitude), 1.0),
                            f"Обнаружено {num_clicks} резких перепадов внутри блока")

            # 4) Level drop / spike (с агрегацией)
            if self._rms_avg > 0.01:
                db = 20.0 * np.log10(max(rms, 1e-10) / max(self._rms_avg, 1e-10))
                if db < cfg.level_drop_db:
                    self._start_or_continue_anomaly(
                        AnomalyType.LEVEL_DROP,
                        min(abs(db) / 40.0, 1.0),
                        f"Уровень: {db:.1f} dB относительно среднего")
                else:
                    self._end_anomaly(AnomalyType.LEVEL_DROP)

                if db > cfg.level_spike_db:
                    self._start_or_continue_anomaly(
                        AnomalyType.LEVEL_SPIKE,
                        min(db / 40.0, 1.0),
                        f"Уровень: +{db:.1f} dB относительно среднего")
                else:
                    self._end_anomaly(AnomalyType.LEVEL_SPIKE)

            # 5) Frequency drift (с агрегацией)
            est_freq = self._estimate_frequency(block)
            if est_freq is not None and est_freq > 20:
                deviation_pct = abs(est_freq - cfg.frequency) / cfg.frequency * 100.0
                if deviation_pct > cfg.freq_tolerance_pct:
                    self._start_or_continue_anomaly(
                        AnomalyType.FREQ_DRIFT,
                        min(deviation_pct / 50.0, 1.0),
                        f"Частота {est_freq:.1f} Гц (ожидалось {cfg.frequency:.0f} Гц, отклонение {deviation_pct:.1f}%)")
                else:
                    self._end_anomaly(AnomalyType.FREQ_DRIFT)

            # 6) THD (с агрегацией, относительно baseline)
            if cfg.wave_type == "sine":
                thd = self._estimate_thd(block)
                effective_threshold = cfg.thd_threshold
                if thd > effective_threshold:
                    self._start_or_continue_anomaly(
                        AnomalyType.DISTORTION,
                        min((thd - self._baseline_thd) / 0.3, 1.0),
                        f"THD = {thd:.3f} (baseline {self._baseline_thd:.3f}, порог {effective_threshold:.3f})")
                else:
                    self._end_anomaly(AnomalyType.DISTORTION)

            # 7) Громкость — слишком тихо / слишком громко (с агрегацией)
            dbfs = 20.0 * np.log10(max(rms, 1e-10))
            self._volume_dbfs_history.append(dbfs)
            if len(self._volume_dbfs_history) > 200:
                self._volume_dbfs_history = self._volume_dbfs_history[-200:]
            self._volume_min_dbfs = min(self._volume_min_dbfs, dbfs)
            self._volume_max_dbfs = max(self._volume_max_dbfs, dbfs)
            self._volume_sum_dbfs += dbfs
            self._volume_count += 1

            peak = float(np.max(np.abs(block)))
            self._peak_sample = max(self._peak_sample, peak)

            if dbfs < cfg.volume_low_dbfs:
                self._start_or_continue_anomaly(
                    AnomalyType.VOLUME_LOW,
                    min(abs(dbfs - cfg.volume_low_dbfs) / 20.0, 1.0),
                    f"Громкость {dbfs:.1f} dBFS (порог {cfg.volume_low_dbfs:.1f} dBFS)")
            else:
                self._end_anomaly(AnomalyType.VOLUME_LOW)

            if dbfs > cfg.volume_high_dbfs:
                self._start_or_continue_anomaly(
                    AnomalyType.VOLUME_HIGH,
                    min((dbfs - cfg.volume_high_dbfs) / 3.0, 1.0),
                    f"Громкость {dbfs:.1f} dBFS (порог {cfg.volume_high_dbfs:.1f} dBFS)")
            else:
                self._end_anomaly(AnomalyType.VOLUME_HIGH)

            # 8) Клиппинг — процент сэмплов >= порога
            clipped = np.sum(np.abs(block) >= cfg.clipping_threshold)
            clipped_pct = clipped / len(block) * 100.0
            if clipped_pct >= cfg.clipping_pct:
                self._start_or_continue_anomaly(
                    AnomalyType.CLIPPING,
                    min(clipped_pct / 10.0, 1.0),
                    f"Клиппинг: {clipped_pct:.2f}% сэмплов >= {cfg.clipping_threshold}")
            else:
                self._end_anomaly(AnomalyType.CLIPPING)

        self._prev_block = block.copy()
        self._block_index += 1

    # --- Геттеры статистики громкости ---
    @property
    def volume_min_dbfs(self) -> float:
        return self._volume_min_dbfs

    @property
    def volume_max_dbfs(self) -> float:
        return self._volume_max_dbfs

    @property
    def volume_avg_dbfs(self) -> float:
        return self._volume_sum_dbfs / max(self._volume_count, 1)

    @property
    def peak_sample(self) -> float:
        return self._peak_sample

    def get_anomalies(self) -> List[Anomaly]:
        with self._lock:
            return list(self.anomalies)


# ─── Главный движок теста ────────────────────────────────────────────────────

class ArtifactDetector:
    """Запускает генерацию и запись, анализирует и логирует."""

    def __init__(self, config: Config):
        self.config = config
        self.generator = SignalGenerator(config)
        self.analyzer = SignalAnalyzer(config)
        self._running = False
        self._anomaly_count = 0
        self._logger = self._setup_logger()

    def _setup_logger(self) -> logging.Logger:
        log_dir = Path(self.config.log_dir)
        log_dir.mkdir(parents=True, exist_ok=True)

        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = log_dir / f"artifact_log_{timestamp}.txt"

        logger = logging.getLogger("artifact_detector")
        logger.setLevel(logging.DEBUG)

        # Файловый хендлер
        fh = logging.FileHandler(str(log_file), encoding="utf-8")
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(logging.Formatter("%(message)s"))

        # Консольный хендлер
        ch = logging.StreamHandler(sys.stdout)
        ch.setLevel(logging.INFO)
        ch.setFormatter(logging.Formatter("%(message)s"))

        logger.addHandler(fh)
        logger.addHandler(ch)

        logger.info(f"=== Audio Artifact Detector ===")
        logger.info(f"Лог: {log_file.resolve()}")
        logger.info(f"Дата: {datetime.datetime.now().isoformat(timespec='seconds')}")
        logger.info(f"Длительность: {self.config.duration_min} мин")
        logger.info(f"Частота дискретизации: {self.config.sample_rate} Гц")
        logger.info(f"Размер блока: {self.config.block_size}")
        logger.info(f"Тестовый сигнал: {self.config.wave_type} @ {self.config.frequency} Гц")
        logger.info(f"Калибровка: {self.config.calibration_sec} сек")
        thd_mode = f"авто (baseline + {self.config.thd_margin})" if self.config.thd_threshold <= 0 else f"фикс. {self.config.thd_threshold}"
        logger.info(f"Пороги: dropout_rms<{self.config.dropout_threshold}, "
                     f"click_mult>{self.config.click_threshold}, "
                     f"level_drop<{self.config.level_drop_db}dB, "
                     f"level_spike>{self.config.level_spike_db}dB, "
                     f"freq_tol={self.config.freq_tolerance_pct}%, "
                     f"thd: {thd_mode}")
        logger.info(f"Громкость: low<{self.config.volume_low_dbfs} dBFS, "
                     f"high>{self.config.volume_high_dbfs} dBFS, "
                     f"clip>={self.config.clipping_threshold} ({self.config.clipping_pct}%)")
        logger.info("=" * 60)
        logger.info("")

        self._log_file = log_file
        self._file_handler = fh
        return logger

    def _format_anomaly(self, a: Anomaly) -> str:
        return (
            f"[{a.wall_time}] "
            f"T+{a.timestamp:8.3f}s | "
            f"{a.anomaly_type.value:14s} | "
            f"длительность: {a.duration_ms:8.2f} мс | "
            f"severity: {a.severity:.3f} | "
            f"{a.details}")

    def _log_anomaly(self, a: Anomaly):
        self._anomaly_count += 1
        line = self._format_anomaly(a)
        self._logger.warning(line)

    def _log_to_file(self, msg: str):
        """Пишет только в файл, минуя консольный хендлер."""
        record = logging.LogRecord(
            name="artifact_detector", level=logging.WARNING,
            pathname="", lineno=0, msg=msg, args=(), exc_info=None)
        self._file_handler.emit(record)

    def _audio_callback(self, indata, outdata, frames, time_info, status):
        """Обратный вызов sounddevice для полнодуплексного потока."""
        if status:
            self._logger.debug(f"Stream status: {status}")

        # Генерируем выходной сигнал
        out_signal = self.generator.generate(frames)
        outdata[:, 0] = out_signal
        if self.config.channels > 1:
            for c in range(1, outdata.shape[1]):
                outdata[:, c] = out_signal

        # Анализируем входной (микрофон) — берём первый канал
        in_block = indata[:, 0].copy()
        self.analyzer.analyze_block(in_block)

    def run(self):
        """Запуск полного теста."""
        cfg = self.config
        total_seconds = cfg.duration_min * 60.0

        self._logger.info(f"Запуск теста: {cfg.duration_min} мин ({total_seconds:.0f} сек)")
        self._logger.info(f"Устройство вывода: {sd.query_devices(kind='output')['name']}")
        self._logger.info(f"Устройство ввода:  {sd.query_devices(kind='input')['name']}")
        self._logger.info("")

        self.analyzer.set_start_time(time.time())
        prev_anom_count = 0
        calibration_logged = False

        try:
            with sd.Stream(
                samplerate=cfg.sample_rate,
                blocksize=cfg.block_size,
                channels=cfg.channels,
                dtype='float32',
                callback=self._audio_callback,
            ):
                self._running = True
                start = time.time()

                while time.time() - start < total_seconds:
                    time.sleep(0.5)

                    # Собираем всё, что нужно вывести перед прогрессом
                    messages = []

                    # Логируем результат калибровки
                    if not calibration_logged and self.analyzer.calibrated:
                        calibration_logged = True
                        messages.append(
                            f"Калибровка завершена: baseline THD = {self.analyzer.baseline_thd:.4f}, "
                            f"порог THD = {cfg.thd_threshold:.4f}")

                    # Логируем новые аномалии
                    anomalies = self.analyzer.get_anomalies()
                    for a in anomalies[prev_anom_count:]:
                        self._anomaly_count += 1
                        msg = self._format_anomaly(a)
                        messages.append(msg)
                    prev_anom_count = len(anomalies)

                    # Прогресс
                    elapsed = time.time() - start
                    pct = elapsed / total_seconds * 100.0
                    vol_str = ""
                    if self.analyzer._volume_count > 0:
                        cur_dbfs = self.analyzer._volume_dbfs_history[-1] if self.analyzer._volume_dbfs_history else -120.0
                        vol_str = f" | Vol: {cur_dbfs:.1f} dBFS (pk {self.analyzer.peak_sample:.3f})"
                    progress = (
                        f"Прогресс: {pct:5.1f}% | "
                        f"Время: {elapsed:.0f}/{total_seconds:.0f}с | "
                        f"Аномалий: {self._anomaly_count}{vol_str}")

                    # Очищаем строку прогресса, печатаем сообщения, затем новый прогресс
                    sys.stdout.write("\r" + " " * 120 + "\r")
                    if messages:
                        for msg in messages:
                            sys.stdout.write(msg + "\n")
                            self._log_to_file(msg)  # только в файл
                    sys.stdout.write(progress)
                    sys.stdout.flush()

                self._running = False

        except KeyboardInterrupt:
            self._logger.info("\n\nТест прерван пользователем (Ctrl+C).")
        except Exception as e:
            self._logger.error(f"\nОшибка: {e}")
            raise

        # Завершаем незакрытые серии
        self.analyzer.finalize()

        # Финальные аномалии
        sys.stdout.write("\r" + " " * 80 + "\r")
        sys.stdout.flush()
        anomalies = self.analyzer.get_anomalies()
        for a in anomalies[prev_anom_count:]:
            self._log_anomaly(a)

        self._print_summary(anomalies, time.time() - start if 'start' in dir() else 0)

    def _print_summary(self, anomalies: List[Anomaly], elapsed: float):
        """Итоговый отчёт."""
        self._logger.info("\n")
        self._logger.info("=" * 60)
        self._logger.info("=== ИТОГИ ТЕСТА ===")
        self._logger.info(f"Длительность: {elapsed:.1f} сек")
        self._logger.info(f"Baseline THD: {self.analyzer.baseline_thd:.4f}")
        self._logger.info(f"Порог THD: {self.config.thd_threshold:.4f}")
        self._logger.info(f"")
        self._logger.info(f"--- Статистика громкости ---")
        self._logger.info(f"  Мин. громкость:  {self.analyzer.volume_min_dbfs:.1f} dBFS")
        self._logger.info(f"  Макс. громкость: {self.analyzer.volume_max_dbfs:.1f} dBFS")
        self._logger.info(f"  Средняя:         {self.analyzer.volume_avg_dbfs:.1f} dBFS")
        self._logger.info(f"  Пиковый сэмпл:  {self.analyzer.peak_sample:.4f} ({20.0 * np.log10(max(self.analyzer.peak_sample, 1e-10)):.1f} dBFS)")
        self._logger.info(f"")
        self._logger.info(f"Всего аномалий: {len(anomalies)}")

        if anomalies:
            # Группируем по типу
            by_type: dict = {}
            for a in anomalies:
                by_type.setdefault(a.anomaly_type.value, []).append(a)

            self._logger.info("\nПо типам:")
            for atype, items in sorted(by_type.items()):
                total_dur = sum(i.duration_ms for i in items)
                avg_sev = sum(i.severity for i in items) / len(items)
                self._logger.info(
                    f"  {atype:14s}: {len(items):5d} шт, "
                    f"суммарная длительность: {total_dur:10.2f} мс, "
                    f"средняя severity: {avg_sev:.3f}")

            # Top-5 самых длинных
            top = sorted(anomalies, key=lambda a: a.duration_ms, reverse=True)[:5]
            self._logger.info("\nТоп-5 самых длинных аномалий:")
            for i, a in enumerate(top, 1):
                self._logger.info(
                    f"  {i}. [{a.wall_time}] {a.anomaly_type.value}: "
                    f"{a.duration_ms:.2f} мс — {a.details}")
        else:
            self._logger.info("Аномалий не обнаружено. Сигнал чистый.")

        self._logger.info("=" * 60)
        self._logger.info(f"Лог сохранён: {self._log_file.resolve()}")

# ─── CLI ─────────────────────────────────────────────────────────────────────

def parse_args() -> Config:
    parser = argparse.ArgumentParser(
        description="Audio Artifact Detector — детектор потерь и артефактов в аудиопотоке",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Примеры:
  python audio_artifact_detector.py 5                    # 5 минут, синус 1000 Гц
  python audio_artifact_detector.py 10 --wave square     # 10 минут, меандр
  python audio_artifact_detector.py 2 --freq 440         # 2 минуты, 440 Гц
        """
    )

    parser.add_argument(
        "duration", type=float,
        help="Длительность теста в минутах")

    parser.add_argument(
        "--freq", type=float, default=200.0,
        help="Частота тестового сигнала в Гц (по умолчанию: 200)")

    parser.add_argument(
        "--wave", choices=["sine", "square"], default="sine",
        help="Тип волны: sine (синус) или square (меандр)")

    parser.add_argument(
        "--sample-rate", type=int, default=48000,
        help="Частота дискретизации (по умолчанию: 48000)")

    parser.add_argument(
        "--block-size", type=int, default=1024,
        help="Размер блока обработки (по умолчанию: 1024)")

    parser.add_argument(
        "--dropout-threshold", type=float, default=0.02,
        help="Порог RMS для детекции dropout (по умолчанию: 0.02)")

    parser.add_argument(
        "--click-threshold", type=float, default=5.0,
        help="Множитель для детекции кликов (по умолчанию: 5.0)")

    parser.add_argument(
        "--thd-threshold", type=float, default=0.0,
        help="Порог THD для детекции искажений (0 = авто-калибровка)")

    parser.add_argument(
        "--thd-margin", type=float, default=0.15,
        help="Превышение над baseline THD для детекции (по умолчанию: 0.15)")

    parser.add_argument(
        "--volume-low", type=float, default=-40.0,
        help="Порог 'слишком тихо' в dBFS (по умолчанию: -40)")

    parser.add_argument(
        "--volume-high", type=float, default=-1.0,
        help="Порог 'слишком громко' в dBFS (по умолчанию: -1)")

    parser.add_argument(
        "--clipping-threshold", type=float, default=0.99,
        help="Порог |сэмпла| для детекции клиппинга (по умолчанию: 0.99)")

    parser.add_argument(
        "--clipping-pct", type=float, default=0.1,
        help="Мин. процент клиппированных сэмплов в блоке (по умолчанию: 0.1)")

    parser.add_argument(
        "--calibration-sec", type=float, default=3.0,
        help="Длительность калибровки в секундах (по умолчанию: 3)")

    parser.add_argument(
        "--list-devices", action="store_true",
        help="Показать доступные аудиоустройства и выйти")

    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        sys.exit(0)

    config = Config(
        duration_min=args.duration,
        sample_rate=args.sample_rate,
        block_size=args.block_size,
        frequency=args.freq,
        wave_type=args.wave,
        dropout_threshold=args.dropout_threshold,
        click_threshold=args.click_threshold,
        thd_threshold=args.thd_threshold,
        thd_margin=args.thd_margin,
        volume_low_dbfs=args.volume_low,
        volume_high_dbfs=args.volume_high,
        clipping_threshold=args.clipping_threshold,
        clipping_pct=args.clipping_pct,
        calibration_sec=args.calibration_sec,
        log_dir=str(Path(__file__).parent),
    )

    return config


def main():
    config = parse_args()

    print(f"\n{'=' * 60}")
    print(f"  Audio Artifact Detector")
    print(f"  Длительность: {config.duration_min} мин")
    print(f"  Мин. длительность аномалии: {config.min_anomaly_duration_ms} мс")
    print(f"{'=' * 60}\n")

    # Показываем устройства
    print("Доступные аудиоустройства:")
    print(sd.query_devices())
    print()

    detector = ArtifactDetector(config)
    detector.run()


if __name__ == "__main__":
    main()
