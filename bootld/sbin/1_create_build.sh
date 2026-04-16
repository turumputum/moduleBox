#!/bin/bash

cd ../


. script.env

rm -rf build
mkdir -p build

cmake . -B build -DBOARD=unexpectedmaker_tinys3
#cmake . -B build -DBOARD=unexpectedmaker_tinys3 -DCMAKE_VERBOSE_MAKEFILE=1
