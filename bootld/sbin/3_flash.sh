#!/bin/bash

PORT=USB0
PREFIX=.

if [[ "$PWD" == *"sbin"* ]]; then
  PREFIX=..
fi 

RUNTERM="gtkterm --port /dev/tty$PORT"

PIDOF=`pgrep -f "$RUNTERM"`

if [ ! -z "$PIDOF" ]; then
    kill $PIDOF
fi

   # /home2/ak/opt/esp/tools/python_env/idf4.4_py3.8_env/bin/python /home2/ak/opt/esp/esp-idf/components/esptool_py/esptool/esptool.py -p /dev/tty$PORT -b 460800 --before default_reset --after hard_reset --chip esp32s3  write_flash --flash_mode dio --flash_size detect --flash_freq 80m 0x0 $PREFIX/build/bootloader/bootloader.bin 0x10000 $PREFIX/build/bootldsd.bin
/home/ak/opt/esp/v5/tools/python_env/idf5.1_py3.8_env/bin/python /home2/ak/opt/esp/v5/esp-idf/components/esptool_py/esptool/esptool.py -p /dev/tty$PORT -b 460800 --before default_reset --after hard_reset --chip esp32s3  write_flash --flash_mode dio --flash_size detect --flash_freq 80m 0x0 $PREFIX/build/bootloader/bootloader.bin 0x10000 $PREFIX/build/bootldsd.bin


nohup $RUNTERM > /dev/null &
sleep 1
