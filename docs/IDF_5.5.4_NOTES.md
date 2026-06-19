# Заметки по миграции на ESP-IDF 5.5.4 / GCC 14

Проектное знание о двух нетривиальных проблемах, всплывших при переходе moduleBox
на ESP-IDF 5.5.4. Держим в git, чтобы не потерять при работе с другой машины.

---

## 1. FATFS: молчаливая порча файлов >64 КБ при множестве мелких write()

В стеке **SDMMC + FATFS IDF v5.5.4** на ESP32-S3 серия мелких `write()` (порядка
десятков вызовов) для файла размером больше ~64 КБ приводит к молчаливой порче:
директория обновляется правильным размером, `fsync`+`close` возвращают 0, но первый
кластер файла на SD-карте остаётся нулями. `read()` сразу после `close()` возвращает NUL.

**Подтверждение:** воспроизведено эмпирически при записи `manifest-3.56.json`
(145 КБ через ~80 вызовов write). Sanity-тесты при 10..65536 байтах одним `write()`
проходят чисто; при разбиении на много мелких write — стабильное повреждение.
Затронут только write path; чтение больших файлов работает нормально.

**Как обходить:**
- Для записи на SD любого файла >64 КБ — собирай весь контент в один буфер в RAM
  (предпочтительно SPIRAM через `heap_caps_malloc(MALLOC_CAP_SPIRAM)`), затем один
  `write(fd, buf, total)`. Это уходит в SDMMC как multi-block transfer и работает.
- При чтении файлов, которые могли быть повреждены прошлым багом, валидируй контент
  по сигнатуре (для JSON — первый байт `{`), не доверяй размеру из `stat()`.
- Если баг починят в новой IDF — workaround всё равно корректен, но можно вернуться
  к streaming write ради экономии RAM.

Эталонная реализация: `_writeManifestoOnce` в `components/manifest/manifest.c`.

---

## 2. GCC 14: латентные ошибки типов, ставшие фатальными

GCC 14 (в составе IDF 5.5.4) повысил `-Wint-conversion` и
`-Wincompatible-pointer-types` до ошибок. Это вскрыло латентные баги типов в коде
самого проекта. Политика: **чинить каждую ошибку по-настоящему, не маскировать
`-Wno-error`.**

Список однажды пропатченных мест (для справки — что именно ломалось):

| Файл | Проблема |
|------|----------|
| accelStepper.c, asyncStepper.c, encoders.c | `pcnt_unit_get_count` / `gptimer_get_raw_count` ждут `int*` / `uint64_t*`, код давал `int32_t` / `unsigned long` |
| st7789/decode_jpeg/decode_jpeg.c | include `esp32/rom/tjpgd.h` -> `esp32s3/rom/tjpgd.h` |
| wav_handle.c | указатель инициализирован из `1` |
| scheduler.c | передан `schedule_entry2_t*` где ждали `schedule_time_t*` (нужно `.time`) |
| ftp.c | `connect()` приведён к `sockaddr_in*` вместо `sockaddr*` |
| WIFI.c, LAN.c | `ip4addr_aton/ntoa` нужен каст к `ip4_addr_t*` (`esp_ip4_addr_t` layout-совместим) |
| CRSF.c, accel.c, distanceSens_tofxxxf_uart.c | `&array` передан где ждали pointer-to-element |
| watchdog.c | 3-й аргумент `xQueueSend` был `NULL`, должен быть `0` |
| rtp_opus_stream.h, rtp_client_stream.h | поле `sock_fd` типа `esp_transport_handle_t`, а хранит сырой int-сокет |
| in_out.c | поле `stateReport` типа `void*`, а держит int report id |
| reporter.c | task-функции объявлены `(void)` вместо `(void *arg)` |

> Заметки — точечный снимок на момент миграции; перед использованием как факта
> сверяйся с текущим кодом.
