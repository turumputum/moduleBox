ESP-IDF template app
====================

This is a template application to be used with [Espressif IoT Development Framework](https://github.com/espressif/esp-idf).

Please check [ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for getting started instructions.

*Code in this repository is in the Public Domain (or CC0 licensed, at your option.)
Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.*


gst-launch-1.0 -v wasapi2src device="\\\\\?\\SWD\#MMDEVAPI\#\{0.0.1.00000000\}.\{8a84e721-b228-4e5f-a78a-08ba98a6193a\}\#\{2eef81be-33fa-4800-9670-1cd474972c3f\}" ! audio/x-raw,channels=2,rate=48000 ! audioconvert ! audioresample ! rtpL16pay ! udpsink host=239.0.7.1 port=7777 auto-multicast=true