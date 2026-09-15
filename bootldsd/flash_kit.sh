#!/bin/bash
# Полная прошивка устройства комплектом загрузчика (Linux-вариант flash_kit.cmd):
#   0x00000  bootloader.bin       кастомный bootloader (основной проект, bootloader_components/main)
#   0x08000  partition-table.bin  таблица разделов (partitions.csv в корне)
#   0x10000  bootldsd.bin         загрузчик обновлений (bootldsd/loader)
#   0x50000  moduleBox.bin        прошивка
# Использование: ./flash_kit.sh /dev/ttyUSB0 [baud]
cd "$(dirname "$0")"
PORT=${1:?"порт не задан, пример: ./flash_kit.sh /dev/ttyUSB0"}
BAUD=${2:-921600}
for f in bootloader.bin partition-table.bin bootldsd.bin moduleBox.bin; do
  [ -f "$f" ] || { echo "Нет файла $f"; exit 1; }
done
python -m esptool --chip esp32s3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 bootldsd.bin 0x50000 moduleBox.bin
