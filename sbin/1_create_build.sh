#!/bin/bash

cd ../

. script.env

rm -rf build
mkdir -p build

#export ESP_ROM_ELF_DIR=$IDF_PATH
#cmake . -B build -DCMAKE_VERBOSE_MAKEFILE=1
cmake . -B build
