#!/bin/bash

reset

#valgrind ../.build/manifesto adc1.c Manifest.json

valgrind ../.build/manifesto smartLed.c Manifest.json
