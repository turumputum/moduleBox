# COMPONENT SKILL - Правила оформления компонентов moduleBox

Документ описывает стандарты разработки компонентов для проекта moduleBox на основе анализа компонентов `button_leds` и `distanceSens`.

---

## 1. СТРУКТУРА ФАЙЛОВ И ДИРЕКТОРИЙ

### 1.1 Обязательная структура компонента
```
components/
  componentName/
    ├── CMakeLists.txt
    ├── include/
    │   └── componentName.h
    ├── componentName_common.c       (общие функции)
    └── componentName_variant1.c     (варианты реализации)
    └── componentName_variant2.c
```

### 1.2 Примеры из существующих компонентов

**buttonLeds:**
```
buttonLeds/
  ├── CMakeLists.txt
  ├── include/
  │   ├── buttonLeds.h
  │   ├── button_logic.h
  │   └── led_types.h
  ├── button_logic.c
  ├── led_types.c
  ├── button_led.c
  ├── button_smartLed.c
  ├── button_ledRing.c
  ├── button_ledBar.c
  └── button_swiperLed.c
```

**distanceSens:**
```
distanceSens/
  ├── CMakeLists.txt
  ├── include/
  │   └── distanceSens.h
  ├── distanceSens_common.c
  ├── distanceSens_tofxxxf_uart.c
  ├── distanceSens_benewake.c
  ├── distanceSens_hlk2410.c
  └── distanceSens_sr04m.c
```

---

## 2. ЗАГОЛОВОЧНЫЕ ФАЙЛЫ (HEADER FILES)

### 2.1 Структура заголовочного файла

```c
// Защита от множественного включения
#ifndef COMPONENT_NAME_H
#define COMPONENT_NAME_H

// Системные заголовки
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/ledc.h"

// Объявление структур данных
typedef struct {
    uint8_t state;
    uint8_t prevState;
    uint16_t currentPos;
    uint16_t maxVal;
    uint16_t minVal;
    // ...другие поля
    
    // Report IDs
    int mainReport;
} component_context_t;

// Макрос инициализации по умолчанию
#define COMPONENT_DEFAULT() {\
    .state = 0,\
    .prevState = 0,\
    .currentPos = 0,\
    .maxVal = 65535,\
    .minVal = 0,\
}

// Прототипы общих функций
void component_config(component_context_t *ctx, uint8_t slot_num);
void component_report(component_context_t *ctx, uint8_t slot_num);

// Прототипы функций запуска задач
void start_variant1_task(int slot_num);
void start_variant2_task(int slot_num);

#endif // COMPONENT_NAME_H
```

### 2.2 Примеры из компонентов

**distanceSens.h:**
- ✅ Структура `distanceSens_t` с полным набором параметров
- ✅ Макрос `DISTANCE_SENS_DEFAULT()` для инициализации
- ✅ Общие функции: `distanceSens_config()`, `distanceSens_report()`
- ✅ Функции запуска вариантов: `start_tofxxxfuart_task()`, `start_benewakeTOF_task()`

**buttonLeds.h:**
- ✅ Только экспортируемые функции `start_*_task()`
- ✅ Детальные структуры вынесены в отдельные заголовки (`button_logic.h`, `led_types.h`)

---

## 3. СТРУКТУРЫ ДАННЫХ

### 3.1 Правила именования структур

```c
// Основной контекст модуля
typedef struct {
    // Входные параметры
    uint8_t inverse;
    uint16_t debounce_gap;
    
    // Состояние
    uint8_t state;
    uint8_t prevState;
    
    // Таймеры
    TickType_t lastTick;
    TickType_t cooldownTime;
    
    // Команды (если есть)
    STDCOMMANDS cmds;
    
    // Отчеты
    int stateReport;
    int valueReport;
    
    // Аппаратные ресурсы
    ledc_channel_config_t ledc_chan;
} module_context_t;
```

### 3.2 Обязательные поля

**Для модулей с входными данными (датчики):**
- `state` / `prevState` - текущее и предыдущее состояние
- `currentPos` / `prevPos` - текущее и предыдущее значение
- `maxVal` / `minVal` - диапазон значений
- `lastTick` - время последнего обновления
- `debounceGap` - защита от дребезга
- Report ID для stdreport

**Для модулей с выходными данными (исполнительные устройства):**
- `state` - текущее состояние
- `inverse` - инверсия сигнала
- `STDCOMMANDS cmds` - структура команд
- Аппаратные конфигурации (GPIO, LEDC, и т.д.)

### 3.3 Примеры из компонентов

**distanceSens_t:**
```c
typedef struct {
    uint8_t state, prevState;
    uint16_t currentPos, prevPos;
    uint16_t maxVal, minVal;
    uint16_t threshold;
    uint8_t inverse;
    uint16_t deadBand;
    float k;
    TickType_t lastTick;
    TickType_t debounceGap;
    TickType_t cooldownTime;
    ledc_channel_config_t ledc_chan;
    int distanceReport;
} distanceSens_t;
```

**BUTTONCONFIG:**
```c
typedef struct {
    int button_inverse;
    int debounce_gap;
    int event_filter;
    int stateReport;
    int longReport;
    int doubleReport;
    uint16_t longPressTime;
    uint16_t doubleClickTime;
    uint16_t refreshPeriod;
} BUTTONCONFIG;
```

---

## 4. ФУНКЦИЯ КОНФИГУРАЦИИ

### 4.1 Шаблон функции configure

```c
void configure_component(component_context_t *ctx, int slot_num) {
    // 1. Инициализация команд (если нужно)
    stdcommand_init(&ctx->cmds, slot_num);
    
    // 2. Чтение опций из конфигурации
    ctx->inverse = get_option_flag_val(slot_num, "inverse");
    ctx->threshold = get_option_int_val(slot_num, "threshold", "", 100, 0, 65535);
    ctx->mode = get_option_enum_val(slot_num, "mode", "auto", "manual", NULL);
    
    // 3. Настройка топиков
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = get_option_string_val(slot_num, "topic", "/default_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
    } else {
        char t_str[strlen(me_config.deviceName) + 20];
        sprintf(t_str, "%s/component_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
    }
    
    // 4. Регистрация отчетов
    ctx->stateReport = stdreport_register(RPTT_int, slot_num, "state", NULL, 0, 1);
    
    // 5. Регистрация команд (если нужно)
    stdcommand_register(&ctx->cmds, CMD_SET_VALUE, NULL, PARAMT_int);
    stdcommand_register(&ctx->cmds, CMD_TOGGLE, "toggle", PARAMT_none);
}
```

### 4.2 Обязательные элементы

1. **Инициализация stdcommand** (для исполнительных модулей)
2. **Чтение всех опций** с помощью `get_option_*_val()`
3. **Настройка топиков** (action_topic или trigger_topic)
4. **Регистрация отчетов** через `stdreport_register()`
5. **Регистрация команд** через `stdcommand_register()`
6. **Логирование** всех важных параметров

### 4.3 Примеры

**button_led (configure_button_led):**
```c
void configure_button_led(PMODULE_CONTEXT ctx, int slot_num) {
    stdcommand_init(&ctx->led.cmds, slot_num);
    
    // Опции кнопки
    ctx->button.button_inverse = get_option_flag_val(slot_num, "buttonInverse");
    ctx->button.debounce_gap = get_option_int_val(slot_num, "buttonDebounceGap", "", 10, 1, 4096);
    
    // Топики
    if (strstr(me_config.slot_options[slot_num], "buttonTopic") != NULL) {
        char * custom_topic = get_option_string_val(slot_num, "buttonTopic", "/button_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
    }
    
    // Отчеты
    ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "state", NULL, 0, 1);
    
    // Команды светодиода
    stdcommand_register(&ctx->led.cmds, LED_CMD_default, NULL, PARAMT_int);
    stdcommand_register(&ctx->led.cmds, LED_CMD_toggleLedState, "toggleLedState", PARAMT_none);
}
```

---

## 5. ГЛАВНАЯ ФУНКЦИЯ ЗАДАЧИ (TASK)

### 5.1 Структура task-функции

```c
static void component_task(void *arg) {
    // 1. Получение номера слота
    int slot_num = *(int*)arg;
    
    // 2. Выделение контекста
    component_context_t ctx = COMPONENT_DEFAULT();
    
    // 3. Конфигурация
    configure_component(&ctx, slot_num);
    
    // 4. Настройка аппаратных ресурсов
    setup_hardware(slot_num, &ctx);
    
    // 5. Создание очереди команд
    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
    // 6. Ожидание разрешения на работу
    waitForWorkPermit(slot_num);
    
    // 7. Основной цикл
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (1) {
        // 7.1 Прием команд
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
        
        // 7.2 Обработка команд
        switch (cmd) {
            case CMD_SET_VALUE:
                handle_set_value(&ctx, params.p[0].i);
                break;
            // ...
        }
        
        // 7.3 Чтение датчиков / Обновление состояния
        read_sensor_data(&ctx, slot_num);
        
        // 7.4 Отправка отчетов
        send_reports(&ctx, slot_num);
        
        // 7.5 Задержка
        vTaskDelayUntil(&lastWakeTime, ctx.refreshPeriod);
    }
}
```

### 5.2 Обязательные элементы task

1. **Получение slot_num** из аргумента
2. **Инициализация контекста**
3. **Вызов configure функции**
4. **Создание command_queue**
5. **Вызов waitForWorkPermit()**
6. **Цикл с vTaskDelayUntil()** для периодического выполнения

### 5.3 Примеры

**button_led_task:**
```c
void button_led_task(void *arg) {
    int slot_num = *(int*)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    
    setup_button_hw(slot_num, ctx);
    configure_button_led(ctx, slot_num);
    
    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
    // Настройка LEDC
    int channel = get_next_ledc_channel();
    ledc_timer_config_t ledc_timer = { /* ... */ };
    ledc_timer_config(&ledc_timer);
    
    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    while (1) {
        // Прием команд
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx->led.cmds, &params, 0);
        
        // Обработка команд
        switch (cmd) { /* ... */ }
        
        // Чтение кнопки
        int button_raw = gpio_get_level(pin_in);
        button_logic_update(&ctx->button, button_state, slot_num, &prev_button_state);
        
        // Обновление LED
        update_led_basic(&ctx->led, &ledc_channel, &currentBright, &appliedBright, &targetBright);
        
        vTaskDelayUntil(&lastWakeTime, ctx->button.refreshPeriod);
    }
}
```

---

## 6. ФУНКЦИЯ ЗАПУСКА ЗАДАЧИ

### 6.1 Шаблон start функции

```c
void start_component_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_component_%d", slot_num);
    xTaskCreate(component_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}
```

### 6.2 Параметры xTaskCreate

- **Имя задачи**: `"task_<component>_<slot_num>"`
- **Стек**: `1024*4` до `1024*5` (зависит от сложности)
- **Приоритет**: `configMAX_PRIORITIES-5` или `12`
- **Аргумент**: `&slot_num`

### 6.3 Примеры

```c
void start_button_led_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_led_%d", slot_num);
    xTaskCreate(button_led_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}

void start_tofxxxfuart_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_tofxxxfuart_%d", slot_num);
    xTaskCreate(tofxxxfuart_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-10, NULL);
}
```

---

## 7. СИСТЕМА КОМАНД (STDCOMMAND)

### 7.1 Определение команд через enum

```c
typedef enum {
    CMD_default = 0,        // Основная команда без имени
    CMD_toggle,             // Переключение состояния
    CMD_setParam1,          // Установка параметра 1
    CMD_setParam2,          // Установка параметра 2
} COMPONENT_CMD;
```

### 7.2 Регистрация команд

```c
// Основная команда (без имени, только числовое значение)
stdcommand_register(&ctx->cmds, CMD_default, NULL, PARAMT_int);

// Именованная команда без параметров
stdcommand_register(&ctx->cmds, CMD_toggle, "toggle", PARAMT_none);

// Именованная команда с целочисленным параметром
stdcommand_register(&ctx->cmds, CMD_setParam1, "setParam1", PARAMT_int);

// Команда с параметром float
stdcommand_register(&ctx->cmds, CMD_setParam2, "setParam2", PARAMT_float);
```

### 7.3 Прием и обработка команд

```c
STDCOMMAND_PARAMS params = {0};
int cmd = stdcommand_receive(&ctx->cmds, &params, 0);  // 0 = неблокирующий вызов

if (cmd >= 0) {
    ESP_LOGD(TAG, "Slot_%d received cmd:%d", slot_num, cmd);
}

switch (cmd) {
    case CMD_default:
        ctx->state = params.p[0].i;
        break;
        
    case CMD_toggle:
        ctx->state = !ctx->state;
        break;
        
    case CMD_setParam1:
        ctx->param1 = params.p[0].i;
        ESP_LOGD(TAG, "Set param1:%d", ctx->param1);
        break;
}
```

### 7.4 Примеры из button_led

```c
typedef enum {
    LED_CMD_default = 0,
    LED_CMD_toggleLedState,
    LED_CMD_setMinBright,
    LED_CMD_setMaxBright,
    LED_CMD_setFadeTime
} LED_CMD;

stdcommand_register(&ctx->led.cmds, LED_CMD_default, NULL, PARAMT_int);
stdcommand_register(&ctx->led.cmds, LED_CMD_toggleLedState, "toggleLedState", PARAMT_none);
stdcommand_register(&ctx->led.cmds, LED_CMD_setMinBright, "setMinBright", PARAMT_int);
```

---

## 8. СИСТЕМА ОТЧЕТОВ (STDREPORT)

### 8.1 Регистрация отчетов

```c
// Базовый отчет состояния
ctx->stateReport = stdreport_register(
    RPTT_int,           // Тип: int
    slot_num,           // Номер слота
    "state",            // Имя топика
    NULL,               // Подтопик (NULL если не нужен)
    0,                  // Минимальное значение
    1                   // Максимальное значение
);

// Отчет со значением float
ctx->valueReport = stdreport_register(
    RPTT_float,         // Тип: float
    slot_num,
    "value",
    NULL,
    0.0,
    100.0
);

// Отчет с подтопиком
ctx->longPressReport = stdreport_register(
    RPTT_int,
    slot_num,
    "state",
    "longPress",        // Подтопик
    0,
    1
);
```

### 8.2 Отправка отчетов

```c
// Отправка целого числа
stdreport_i(ctx->stateReport, newState);

// Отправка float
stdreport_f(ctx->valueReport, floatValue);
```

### 8.3 Примеры из компонентов

**button_led:**
```c
ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "state", NULL, 0, 1);
ctx->button.longReport = stdreport_register(RPTT_int, slot_num, "state", "longPress", 0, 1);
ctx->button.doubleReport = stdreport_register(RPTT_int, slot_num, "state", "doubleClick", 0, 1);
```

**distanceSens:**
```c
distanceSens->distanceReport = stdreport_register(RPTT_int, slot_num, "distance", NULL, 0, 65535);

// В зависимости от режима
if (distanceSens->flag_float_output == 1) {     
    stdreport_f(distanceSens->distanceReport, f_res);
} else {
    stdreport_i(distanceSens->distanceReport, distanceSens->currentPos);
}
```

### 8.4 ВАЖНОЕ ПРАВИЛО: Регистрация без ветвления

**⚠️ ОБЯЗАТЕЛЬНОЕ ПРАВИЛО:**

✅ **ПРАВИЛЬНО - Регистрируйте ВСЕ отчеты заранее, используйте только нужные:**

```c
// Регистрируем ВСЕ возможные отчеты
ctx->stateReport_0 = stdreport_register(RPTT_int, slot_num, "", topic_0);
ctx->stateReport_1 = stdreport_register(RPTT_int, slot_num, "", topic_1);
ctx->stateReport_combined = stdreport_register(RPTT_int, slot_num, "", topic_main);

// В рабочем цикле используем только нужные в зависимости от режима
if (ctx->logic == INDEPENDENT_MODE) {
    if (changed_0) stdreport_i(ctx->stateReport_0, ctx->stat_0);
    if (changed_1) stdreport_i(ctx->stateReport_1, ctx->stat_1);
} else if (ctx->logic == OR_LOGIC_MODE) {
    stdreport_i(ctx->stateReport_combined, result);
}
```

❌ **НЕПРАВИЛЬНО - Избегайте ветвления при регистрации:**

```c
// ПЛОХО! Ветвление при регистрации отчетов
if (ctx->logic == INDEPENDENT_MODE) {
    ctx->stateReport_0 = stdreport_register(RPTT_int, slot_num, "", topic_0);
    ctx->stateReport_1 = stdreport_register(RPTT_int, slot_num, "", topic_1);
} else {
    ctx->stateReport_combined = stdreport_register(RPTT_int, slot_num, "", topic_main);
}
```

**Почему это важно:**
1. Система генерации манифестов анализирует вызовы `stdreport_register()` статически
2. Ветвление при регистрации усложняет автоматическое создание документации
3. Регистрация всех отчетов занимает минимум памяти, но делает код понятнее
4. Неиспользуемые отчеты не создают нагрузку - они просто не вызываются

**Примеры логики работы с многоканальными входами:**

**OR логика (любой вход = 1 → выход = 1):**
```c
// При изменении любого входа пересчитываем результат
int current_result = (ctx->stat_0 || ctx->stat_1) ? 1 : 0;
int prev_result = (ctx->prevState_0 || ctx->prevState_1) ? 1 : 0;

// Отчет только при изменении результата
if (current_result != prev_result) {
    stdreport_i(ctx->stateReport_combined, current_result);
}
```

**AND логика (все входы = 1 → выход = 1):**
```c
// Результат = 1 только когда ВСЕ входы равны 1
int current_result = (ctx->stat_0 && ctx->stat_1) ? 1 : 0;
int prev_result = (ctx->prevState_0 && ctx->prevState_1) ? 1 : 0;

// Отчет только при изменении результата
if (current_result != prev_result) {
    stdreport_i(ctx->stateReport_combined, current_result);
}
```

---

## 9. ЧТЕНИЕ ОПЦИЙ КОНФИГУРАЦИИ

### 9.1 Функции get_option_*_val

```c
// Флаг (проверка наличия опции)
int inverse = get_option_flag_val(slot_num, "inverse");

// Целое число с диапазоном
int threshold = get_option_int_val(
    slot_num,
    "threshold",        // Имя опции
    "",                 // Единицы измерения (для документации)
    100,                // Значение по умолчанию
    0,                  // Минимум
    65535               // Максимум
);

// Перечисление (enum)
int mode = get_option_enum_val(
    slot_num,
    "mode",
    "auto",            // Значение 0
    "manual",          // Значение 1
    "disabled",        // Значение 2
    NULL               // Завершающий NULL
);

// Строка
char* topic = get_option_string_val(
    slot_num,
    "topic",
    "/default_topic"    // Значение по умолчанию
);
```

### 9.2 Примеры из компонентов

**button_led:**
```c
ctx->button.button_inverse = get_option_flag_val(slot_num, "buttonInverse");
ctx->button.debounce_gap = get_option_int_val(slot_num, "buttonDebounceGap", "", 10, 1, 4096);
ctx->button.longPressTime = get_option_int_val(slot_num, "longPressTime", "ms", 0, 0, 65535);
ctx->led.maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
ctx->led.ledMode = get_option_enum_val(slot_num, "ledMode", "none", "flash", "glitch", NULL);
```

**distanceSens:**
```c
distanceSens->inverse = get_option_flag_val(slot_num, "inverse");
distanceSens->threshold = get_option_int_val(slot_num, "threshold", "", 0, 0, 65535);
distanceSens->deadBand = get_option_int_val(slot_num, "deadBand", "", 0, 0, 65535);
distanceSens->k = get_option_float_val(slot_num, "k", "", 1.0, 0.0, 1.0);
```

---

## 10. ОБРАБОТКА ПРЕРЫВАНИЙ И ОЧЕРЕДЕЙ

### 10.1 Настройка прерываний GPIO

```c
void setup_button_hw(int slot_num, PMODULE_CONTEXT ctx) {
    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    
    // Настройка GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << pin_in),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    
    // Установка обработчика прерываний
    gpio_isr_handler_add(pin_in, slot_interrupt_handler, (void*)&ctx->isrCfgs[0]);
}
```

### 10.2 Обработка очереди прерываний

```c
uint8_t msg;
if (xQueueReceive(me_state.interrupt_queue[slot_num], &msg, 0) == pdPASS) {
    // Защита от дребезга
    if (ctx->button.debounce_gap > 0) {
        vTaskDelay(ctx->button.debounce_gap);
    }
    
    // Повторное чтение состояния
    button_raw = gpio_get_level(pin_in);
}
```

---

## 11. РАБОТА С АППАРАТНЫМИ РЕСУРСАМИ

### 11.1 LEDC (PWM для светодиодов)

```c
// Получение свободного канала
int channel = get_next_ledc_channel();

// Конфигурация таймера
ledc_timer_config_t ledc_timer = {
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .duty_resolution  = LEDC_TIMER_8_BIT,
    .timer_num        = LEDC_TIMER_0,
    .freq_hz          = 4000,
    .clk_cfg          = LEDC_AUTO_CLK
};
ledc_timer_config(&ledc_timer);

// Конфигурация канала
ledc_channel_config_t ledc_channel = {
    .speed_mode     = LEDC_LOW_SPEED_MODE,
    .channel        = (ledc_channel_t)channel,
    .timer_sel      = LEDC_TIMER_0,
    .intr_type      = LEDC_INTR_DISABLE,
    .gpio_num       = pin_out,
    .duty           = 0,
    .hpoint         = 0
};
ledc_channel_config(&ledc_channel);

// Установка яркости
ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, brightness);
ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
```

### 11.2 UART для датчиков

```c
uart_config_t uart_config = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
};

uart_param_config(uart_num, &uart_config);
uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);
```

### 11.3 RMT для адресных светодиодов

```c
rmt_led_heap_t *rmt_heap = calloc(1, sizeof(rmt_led_heap_t));
*rmt_heap = RMT_LED_HEAP_DEFAULT();

rmt_heap->tx_chan_config.gpio_num = pin_out;
rmt_new_tx_channel(&rmt_heap->tx_chan_config, &rmt_heap->led_chan);
rmt_new_led_strip_encoder(&rmt_heap->encoder_config, &rmt_heap->led_encoder);
rmt_enable(rmt_heap->led_chan);

// Отправка данных
rmt_transmit(rmt_heap->led_chan, rmt_heap->led_encoder, 
             pixel_data, pixel_size, &rmt_heap->tx_config);
```

---

## 12. ЛОГИРОВАНИЕ И ОТЛАДКА

### 12.1 Настройка уровня логирования

```c
static const char *TAG = "COMPONENT_NAME";
#undef  LOG_LOCAL_LEVEL 
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
```

### 12.2 Использование логов

```c
ESP_LOGE(TAG, "Error: %s", error_message);      // Ошибка
ESP_LOGW(TAG, "Warning: %s", warning_message);  // Предупреждение
ESP_LOGI(TAG, "Info: %s", info_message);        // Информация
ESP_LOGD(TAG, "Debug: value=%d", value);        // Отладка
```

### 12.3 Рекомендации

- Логировать все изменения конфигурации
- Логировать критические события
- Использовать DEBUG уровень для детальной отладки
- Добавлять slot_num в сообщения для идентификации

---

## 13. CMakeLists.txt

### 13.1 Шаблон CMakeLists.txt

```cmake
# Поиск всех файлов вариантов модулей
file(GLOB_RECURSE MODULE_FILES 
    "component_variant1.c"
    "component_variant2.c" 
    "component_variant3.c"
)

# Подключение системы генерации манифестов
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

# Регистрация компонента
idf_component_register(
    SRCS
        "component_common.c"
        ${GENERATED_FILES}
        ${MODULE_FILES}
    INCLUDE_DIRS 
        "include" 
        ${CMAKE_CURRENT_BINARY_DIR}
    REQUIRES 
        "stateConfig" 
        "reporter" 
        "me_slot_config"
        # другие зависимости
)
```

### 13.2 Примеры из компонентов

**buttonLeds:**
```cmake
file(GLOB_RECURSE MODULE_FILES 
    "button_led.c" 
    "button_ledBar.c" 
    "button_ledRing.c" 
    "button_smartLed.c" 
    "button_swiperLed.c"
)

idf_component_register(
    SRCS
        "button_logic.c"
        "led_types.c"
        ${GENERATED_FILES}
        ${MODULE_FILES}
    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
    REQUIRES "me_slot_config" "stateConfig" "reporter" "rgbHsv" "arsenal"
)
```

**distanceSens:**
```cmake
file(GLOB_RECURSE MODULE_FILES 
    "distanceSens_tofxxxf_uart.c"
    "distanceSens_benewake.c" 
    "distanceSens_hlk2410.c" 
    "distanceSens_sr04m.c"
)

idf_component_register(
    SRCS
        "distanceSens_common.c"
        ${GENERATED_FILES}
        ${MODULE_FILES}
    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
    REQUIRES stateConfig
)
```

---

## 14. ОБЩИЕ ПАТТЕРНЫ И BEST PRACTICES

### 14.1 Разделение логики

✅ **Правильно:**
- Общие функции в `_common.c`
- Каждый вариант в отдельном файле
- Структуры и типы в заголовочных файлах

❌ **Неправильно:**
- Весь код в одном файле
- Дублирование кода между вариантами

### 14.2 Управление памятью

✅ **Правильно:**
```c
component_context_t *ctx = calloc(1, sizeof(component_context_t));
// или
component_context_t ctx = COMPONENT_DEFAULT();
```

❌ **Неправильно:**
```c
component_context_t ctx; // Неинициализированная память
```

### 14.3 Обработка ошибок

✅ **Правильно:**
```c
int channel = get_next_ledc_channel();
if (channel < 0) {
    ESP_LOGE(TAG, "No free LEDC channel");
    free(ctx);
    vTaskDelete(NULL);
    return;
}
```

### 14.4 Периодичность задач

```c
TickType_t lastWakeTime = xTaskGetTickCount();
while (1) {
    // Работа...
    vTaskDelayUntil(&lastWakeTime, refreshPeriod);
}
```

### 14.5 Cooldown и Debounce

**Debounce (защита от дребезга):**
```c
if (xTaskGetTickCount() - ctx->lastTick > ctx->debounceGap) {
    // Обработка события
    ctx->lastTick = xTaskGetTickCount();
}
```

**Cooldown (период "перезарядки"):**
```c
if (ctx->inCooldown) {
    if (xTaskGetTickCount() - ctx->cooldownStartTick >= ctx->cooldownTime) {
        ctx->inCooldown = 0;  // Cooldown завершен
    } else {
        return;  // Еще в cooldown
    }
}

// Запуск cooldown
if (newState == 1 && ctx->cooldownTime > 0) {
    ctx->inCooldown = 1;
    ctx->cooldownStartTick = xTaskGetTickCount();
}
```

---

## 15. КОНТРОЛЬНЫЙ СПИСОК (CHECKLIST)

При создании нового компонента убедитесь:

### Структура файлов
- [ ] Создана папка `components/componentName/`
- [ ] Создана папка `components/componentName/include/`
- [ ] Есть файл `componentName.h`
- [ ] Есть файл `componentName_common.c`
- [ ] Есть файлы вариантов `componentName_variant.c`
- [ ] Есть файл `CMakeLists.txt`

### Заголовочный файл
- [ ] Защита от множественного включения (#ifndef/#define/#endif)
- [ ] Определена структура контекста
- [ ] Определен макрос инициализации по умолчанию
- [ ] Объявлены прототипы всех публичных функций

### Функция конфигурации
- [ ] Инициализация stdcommand (если нужно)
- [ ] Чтение всех опций через get_option_*_val()
- [ ] Настройка топиков (action/trigger)
- [ ] Регистрация отчетов через stdreport_register()
- [ ] Регистрация команд через stdcommand_register()
- [ ] Логирование всех параметров

### Task функция
- [ ] Получение slot_num из аргумента
- [ ] Инициализация контекста
- [ ] Вызов configure функции
- [ ] Создание command_queue
- [ ] Вызов waitForWorkPermit()
- [ ] Основной цикл с vTaskDelayUntil()
- [ ] Обработка команд через stdcommand_receive()
- [ ] Отправка отчетов через stdreport_*()

### Start функция
- [ ] Создание уникального имени задачи
- [ ] Правильный размер стека (1024*4 - 1024*5)
- [ ] Передача &slot_num в качестве аргумента

### CMakeLists.txt
- [ ] Использование file(GLOB_RECURSE MODULE_FILES ...)
- [ ] Подключение CMakeManifest.txt
- [ ] Включение всех зависимостей в REQUIRES

### Общее
- [ ] Все переменные инициализированы
- [ ] Обработка ошибок (проверка NULL, return codes)
- [ ] Логирование с правильным уровнем
- [ ] Освобождение ресурсов при ошибках
- [ ] Комментарии для сложных участков кода

---

## 16. ПРИМЕРЫ ТИПОВЫХ МОДУЛЕЙ

### 16.1 Модуль с кнопкой и светодиодом (button_led)

**Особенности:**
- Два независимых потока логики (кнопка + светодиод)
- Прерывания для кнопки
- PWM для светодиода
- События: нажатие, длинное нажатие, двойной клик
- Плавное изменение яркости

### 16.2 Модуль датчика расстояния (distanceSens)

**Особенности:**
- Общая структура `distanceSens_t`
- Множество вариантов датчиков (UART, I2C, ультразвук)
- Режимы работы: пороговый / аналоговый
- Фильтрация и сглаживание
- Cooldown механизм
- Визуальная индикация через LED

### 16.3 Модуль выходов (out_2ch, out_3ch)

**Особенности:**
- Массивы для управления несколькими каналами
- Инверсия на канал
- Команды: установка, переключение, импульс
- Таймеры для импульсов

---

## 17. СИСТЕМА ГЕНЕРАЦИИ МАНИФЕСТОВ

### 17.1 Общая концепция

Каждый файл варианта модуля должен иметь:
1. **В начале файла** - включение сгенерированного заголовка с манифестом
2. **В конце файла** - функцию экспорта манифеста

Система автоматически генерирует манифест на основе специальных комментариев в коде, которые затем используются для создания документации и web-интерфейса конфигурации.

### 17.2 Включение сгенерированного заголовка

**В начале каждого файла варианта модуля добавить:**

```c
#include <generated_files/gen_FILENAME.h>
```

где `FILENAME` - имя файла без расширения `.c`

**Примеры:**

```c
// В файле button_smartLed.c
#include <generated_files/gen_button_smartLed.h>

// В файле distanceSens_tofxxxf_uart.c
#include <generated_files/gen_distanceSens_tofxxxf_uart.h>

// В файле button_led.c
#include <generated_files/gen_button_led.h>
```

### 17.3 Функция экспорта манифеста

**В конце каждого файла варианта модуля добавить:**

```c
const char * get_manifest_FILENAME()
{
    return manifesto;
}
```

где `FILENAME` - имя файла без расширения `.c`

**Примеры:**

```c
// В файле button_smartLed.c
const char * get_manifest_button_smartLed()
{
    return manifesto;
}

// В файле distanceSens_tofxxxf_uart.c
const char * get_manifest_distanceSens_tofxxxf_uart()
{
    return manifesto;
}

// В файле outputs.c
const char * get_manifest_outputs()
{
    return manifesto;
}
```

### 17.4 Процесс генерации манифестов

Система сборки автоматически:

1. **Сканирует** все файлы из `MODULE_FILES` в CMakeLists.txt
2. **Вызывает** утилиту `manifesto.exe` для каждого файла
3. **Генерирует** файл `generated_files/gen_FILENAME.h` с переменной `manifesto`
4. **Собирает** все манифесты в единый файл `manifest.json` на SD-карте

**Процесс описан в CMakeManifest.txt:**

```cmake
foreach(SOURCE_FILE ${MODULE_FILES})
    get_filename_component(FILE_NAME "${SOURCE_FILE}" NAME_WE)
    set(GENERATED_OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated_files/gen_${FILE_NAME}.h")
    
    add_custom_command(
        OUTPUT "${GENERATED_OUTPUT_FILE}"
        COMMAND manifesto ARGS "${SOURCE_FILE}" "${GENERATED_OUTPUT_FILE}"
        DEPENDS "${SOURCE_FILE}"
        COMMENT "Generating manifest of ${SOURCE_FILE}"
    )
    
    list(APPEND GENERATED_FILES "${GENERATED_OUTPUT_FILE}")
endforeach()
```

### 17.5 Формат комментариев для генерации манифеста

Манифест генерируется из **специальных комментариев** в коде:

#### Описание модуля
```c
/* 
    Модуль кнопка со смарт-светодиодами
*/
void configure_button_smartLed(PMODULE_CONTEXT ctx, int slot_num)
{
    // ...
}
```

#### Описание опций конфигурации
```c
/* Количество светодиодов
*/
ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);

/* Флаг инвертирует значение яркости 
*/
ctx->led.inverse = get_option_flag_val(slot_num, "ledInverse");

/* Скорость изменения яркости в миллисекундах
*/
ctx->led.fadeTime = get_option_int_val(slot_num, "fadeTime", "ms", 1000, 10, 10000);
```

#### Описание команд
```c
/* Числовое значение.
   задаёт текущее состояние светодиода (вкл/выкл)
*/
stdcommand_register(&ctx->led.cmds, SMARTLED_default, NULL, PARAMT_int);

/* Команда меняет текущее состояние светодиода на противоположное
*/
stdcommand_register(&ctx->led.cmds, SMARTLED_toggleLedState, "toggleLedState", PARAMT_none);

/* Команда задает цвет подсветки
пример "moduleBox/ledRing_0/setRGB:255 0 0" установить красный цвет
*/
stdcommand_register(&ctx->led.cmds, SMARTLED_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);
```

#### Описание отчетов
```c
/* Рапортует при изменении состояния кнопки
*/
ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "state", NULL, 0, 1);

/* Рапортует текущее значение расстояния
В режиме threshold отправляет 0/1
В режиме аналогового датчика отправляет расстояние в мм или float (0.0-1.0)
*/
distanceSens->distanceReport = stdreport_register(RPTT_int, slot_num, "mm", "distance", 0, distanceSens->maxVal);
```

### 17.6 Регистрация в системе манифестов

**В файле manifest.c автоматически создаются:**

```c
// Объявления функций
#define MODDEF(a) extern const char * get_manifest_##a();
MODULE_FUNCTIONS_DEFS

// Массив указателей на функции
#define MOD(a) get_manifest_##a
typedef const char * (*GET_MANIFEST_FUNC)();

static GET_MANIFEST_FUNC funcs[] = 
{
    MODULE_FUNCTIONS  // Автоматически подставляется из CMake
    NULL
};
```

**Макросы MODULE_FUNCTIONS и MODULE_FUNCTIONS_DEFS формируются в CMake:**

```cmake
string(APPEND LOCAL_MF "MOD(${FILE_BASENAME}),")
string(APPEND LOCAL_MFDEF "MODDEF(${FILE_BASENAME}) ")

SET(MODULE_FUNCTIONS  "${MODULE_FUNCTIONS} ${LOCAL_MF}" CACHE INTERNAL "MODULE_FUNCTIONS")
SET(MODULE_FUNCTIONS_DEFS  "${MODULE_FUNCTIONS_DEFS} ${LOCAL_MFDEF}" CACHE INTERNAL "MODULE_FUNCTIONS_DEFS")
```

### 17.7 Полный пример структуры файла

```c
// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

// Системные заголовки
#include "componentName.h"
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "stateConfig.h"
#include "stdcommand.h"
#include "stdreport.h"

// ОБЯЗАТЕЛЬНО: Включение сгенерированного манифеста
#include <generated_files/gen_component_variant.h>

static const char *TAG = "COMPONENT_NAME";
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// ---------------------------------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// ---------------------------------------------------------------------------

/* 
    Модуль описание модуля
*/
void configure_component_variant(component_context_t *ctx, int slot_num)
{
    /* Описание параметра 1
    */
    ctx->param1 = get_option_int_val(slot_num, "param1", "", 100, 0, 1000);
    
    /* Описание параметра 2
    */
    ctx->param2 = get_option_flag_val(slot_num, "param2");
    
    /* Регистрация команды
    */
    stdcommand_register(&ctx->cmds, CMD_DEFAULT, NULL, PARAMT_int);
    
    /* Регистрация отчета
    */
    ctx->report = stdreport_register(RPTT_int, slot_num, "value", NULL, 0, 100);
}

void component_variant_task(void *arg)
{
    int slot_num = *(int*)arg;
    component_context_t ctx = COMPONENT_DEFAULT();
    
    configure_component_variant(&ctx, slot_num);
    
    // ...остальная логика...
    
    while (1) {
        // Основной цикл
        vTaskDelayUntil(&lastWakeTime, refreshPeriod);
    }
}

void start_component_variant_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_component_variant_%d", slot_num);
    xTaskCreate(component_variant_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}

// ОБЯЗАТЕЛЬНО: Функция экспорта манифеста
const char * get_manifest_component_variant()
{
    return manifesto;
}
```

### 17.8 Проверка корректности манифеста

После сборки проекта проверьте:

1. **Наличие сгенерированных файлов:**
   ```
   build/esp-idf/componentName/generated_files/gen_variant.h
   ```

2. **Содержимое сгенерированного файла:**
   ```c
   // Должна быть объявлена строковая константа manifesto
   static const char manifesto[] = "{ \"mode\": \"variant\", ... }";
   ```

3. **Файл manifest.json на SD-карте** после первого запуска устройства

### 17.9 Типичные ошибки

❌ **Забыли включить заголовок:**
```c
// Ошибка компиляции: 'manifesto' undeclared
const char * get_manifest_button_smartLed()
{
    return manifesto;  // ERROR!
}
```

✅ **Правильно:**
```c
#include <generated_files/gen_button_smartLed.h>

const char * get_manifest_button_smartLed()
{
    return manifesto;  // OK
}
```

❌ **Неправильное имя функции:**
```c
// Файл: button_smartLed.c
const char * get_manifest_button_smart_led()  // Неправильно! Подчеркивание вместо правильного имени
{
    return manifesto;
}
```

✅ **Правильно:**
```c
// Файл: button_smartLed.c
const char * get_manifest_button_smartLed()  // Имя файла без расширения
{
    return manifesto;
}
```

❌ **Файл не добавлен в MODULE_FILES:**
```cmake
file(GLOB_RECURSE MODULE_FILES 
    "component_variant1.c"
    # "component_variant2.c"  # Закомментирован - манифест не будет сгенерирован!
)
```

✅ **Правильно:**
```cmake
file(GLOB_RECURSE MODULE_FILES 
    "component_variant1.c"
    "component_variant2.c"  # Включен в сборку
)
```

---

## 18. КОНТРОЛЬНЫЙ СПИСОК (CHECKLIST) - ОБНОВЛЕННЫЙ

При создании нового компонента убедитесь:

### Структура файлов
- [ ] Создана папка `components/componentName/`
- [ ] Создана папка `components/componentName/include/`
- [ ] Есть файл `componentName.h`
- [ ] Есть файл `componentName_common.c`
- [ ] Есть файлы вариантов `componentName_variant.c`
- [ ] Есть файл `CMakeLists.txt`

### Манифесты (НОВОЕ)
- [ ] В начале каждого файла варианта добавлен `#include <generated_files/gen_FILENAME.h>`
- [ ] В конце каждого файла варианта добавлена функция `get_manifest_FILENAME()`
- [ ] Имя в функции манифеста точно соответствует имени файла (без .c)
- [ ] Все опции конфигурации имеют комментарии с описанием
- [ ] Все команды stdcommand имеют комментарии с описанием
- [ ] Все отчеты stdreport имеют комментарии с описанием
- [ ] Файлы вариантов включены в MODULE_FILES в CMakeLists.txt

### Заголовочный файл
- [ ] Защита от множественного включения (#ifndef/#define/#endif)
- [ ] Определена структура контекста
- [ ] Определен макрос инициализации по умолчанию
- [ ] Объявлены прототипы всех публичных функций

### Функция конфигурации
- [ ] Инициализация stdcommand (если нужно)
- [ ] Чтение всех опций через get_option_*_val()
- [ ] Настройка топиков (action/trigger)
- [ ] Регистрация отчетов через stdreport_register()
- [ ] Регистрация команд через stdcommand_register()
- [ ] Логирование всех параметров

### Task функция
- [ ] Получение slot_num из аргумента
- [ ] Инициализация контекста
- [ ] Вызов configure функции
- [ ] Создание command_queue
- [ ] Вызов waitForWorkPermit()
- [ ] Основной цикл с vTaskDelayUntil()
- [ ] Обработка команд через stdcommand_receive()
- [ ] Отправка отчетов через stdreport_*()

### Start функция
- [ ] Создание уникального имени задачи
- [ ] Правильный размер стека (1024*4 - 1024*5)
- [ ] Передача &slot_num в качестве аргумента

### CMakeLists.txt
- [ ] Использование file(GLOB_RECURSE MODULE_FILES ...)
- [ ] Подключение CMakeManifest.txt
- [ ] Включение всех зависимостей в REQUIRES

### Общее
- [ ] Все переменные инициализированы
- [ ] Обработка ошибок (проверка NULL, return codes)
- [ ] Логирование с правильным уровнем
- [ ] Освобождение ресурсов при ошибках
- [ ] Комментарии для сложных участков кода

---

## 19. ПРИМЕРЫ ТИПОВЫХ МОДУЛЕЙ

### 19.1 Модуль с кнопкой и светодиодом (button_led)

**Особенности:**
- Два независимых потока логики (кнопка + светодиод)
- Прерывания для кнопки
- PWM для светодиода
- События: нажатие, длинное нажатие, двойной клик
- Плавное изменение яркости

### 19.2 Модуль датчика расстояния (distanceSens)

**Особенности:**
- Общая структура `distanceSens_t`
- Множество вариантов датчиков (UART, I2C, ультразвук)
- Режимы работы: пороговый / аналоговый
- Фильтрация и сглаживание
- Cooldown механизм
- Визуальная индикация через LED

### 19.3 Модуль выходов (out_2ch, out_3ch)

**Особенности:**
- Массивы для управления несколькими каналами
- Инверсия на канал
- Команды: установка, переключение, импульс
- Таймеры для импульсов

---

## 20. ЗАКЛЮЧЕНИЕ

Данный документ описывает стандарты разработки компонентов для проекта moduleBox, основанные на анализе существующих компонентов `button_leds` и `distanceSens`.

**Ключевые принципы:**
1. **Модульность** - разделение на варианты и общую логику
2. **Конфигурируемость** - все параметры через get_option_*_val()
3. **Стандартизация** - использование stdcommand и stdreport
4. **Надежность** - обработка ошибок и управление ресурсами
5. **Документированность** - комментарии и логирование
6. **Автоматизация** - система генерации манифестов из комментариев

При создании нового компонента используйте этот документ как руководство и контрольный список.
