#!/bin/bash

reset

#valgrind ../.build/manifesto adc1.c Manifest.json
make -C ../
valgrind ../.build/manifesto ../../components/me_slot_config/smartLed.c Manifest.json no

echo -e "\n\n\n@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n"

jq . Manifest.json

