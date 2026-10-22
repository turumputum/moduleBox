// ***************************************************************************
// TITLE
//      retractor - мотор Steadywin GIM8108-8 по CAN, управление скоростью
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "retractor.h"
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"

#include <stdcommand.h>
#include <stdreport.h>

#include <generated_files/gen_retractor.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "RETRACTOR";

// Проверка шины на bus-off, мс
#define BUS_CHECK_MS            500
// Нет heartbeat от мотора дольше этого - связь потеряна, мс (драйвер шлёт его раз в 100 мс)
#define NO_RESPONSE_MS          1500
// Не чаще одного рапорта скорости за этот интервал, мс - оценка скорости
// приходит раз в 10 мс и дрожит около нуля
#define SPEED_REPORT_MIN_MS     100
// Повтор запроса замкнутого контура, если ось сама ушла в IDLE без ошибки
// (например перезагрузка драйвера), мс
#define CLOSED_LOOP_RETRY_MS    1000
// Пока мотор не отвечает - раз в этот интервал пишем в лог состояние шины, мс
#define LINK_LOG_MS             3000
// Сколько кадров с чужим ID показать в логе - дальше молчим, чтобы не залить
#define UNKNOWN_FRAMES_LOG_MAX  20

static const char *bitrate_names[] = { "500k", "1M", "250k", "125k", "100k" };

typedef enum {
    RCMD_setSpeed = 0,
    RCMD_setUpLimit,
    RCMD_setDownLimit,
} RETRACTOR_CMD;

typedef struct {
    // --- опции ---
    int             active_state;
    int             maxCurrent;         // мА, 0 - не трогать настройку драйвера
    int             maxSpeed;           // об/мин
    int             accel;              // об/мин за секунду, 0 - без рампы
    float           velKp;              // Kp регулятора скорости
    float           velKi;              // Ki регулятора скорости
    int             dirSign;            // 1 или -1 (dirInverse)
    uint8_t         node;               // node_id драйвера
    int             bitrate;
    int             refreshPeriod;      // мс
    int             speedReportFlag;

    // --- состояние ---
    int             targetSpeed;        // об/мин, логическое направление
    float           cmdSpeed;           // об/мин после рампы
    int             actualSpeed;        // об/мин из оценки энкодера драйвера
    // Крайние положения: 1 запрещает движение в эту сторону. Сейчас приходят
    // командами, позже их будет выставлять абсолютный энкодер на этой же шине
    int             upLimit;
    int             downLimit;
    int             axisState;          // из heartbeat, -1 пока неизвестно
    uint32_t        axisErrors;         // активные ошибки из heartbeat
    // Авария защёлкивается до action/enable 1: ошибка у ODrive может погаснуть
    // сама (остыл, вернулось напряжение), но молча возобновлять вращение нельзя
    uint32_t        latchedErrors;
    int             busOk;
    int             linkOk;
    int             everRx;
    TickType_t      lastRxTick;
    TickType_t      lastBusCheckTick;
    TickType_t      lastClosedLoopReqTick;
    TickType_t      lastSpeedReportTick;
    TickType_t      lastLinkLogTick;
    int             unknownFramesLogged;
    int             prevSpeedReported;
    const char *    prevFaultReported;

    STDCOMMANDS     cmds;

    int             speedReport;
    int             faultReport;
} retractor_t;

/*
    Мотор Steadywin GIM8108-8 по CAN, протокол ODrive CANSimple - вращение с заданной скоростью в обе стороны
    Нужен внешний CAN-трансивер - ножка 0 RX, ножка 1 TX
    CAN-контроллер на устройстве один - не больше одного CAN-модуля
    slots: 0-5
*/
void configure_retractor(retractor_t *c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Стартовать выключенным до прихода action/enable 1 - Флаг
    */
    c->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "[retractor_%d] active_state:%d", slot_num, c->active_state);

    /* Предельный ток мотора в мА - 0 оставляет настройку драйвера, По умолчанию 0
    */
    c->maxCurrent = get_option_int_val(slot_num, "maxCurrent", "mA", 0, 0, 60000);
    ESP_LOGD(TAG, "[retractor_%d] maxCurrent:%d mA", slot_num, c->maxCurrent);

    /* Предел скорости в об/мин - команда setSpeed ограничивается им, По умолчанию 100
    */
    c->maxSpeed = get_option_int_val(slot_num, "maxSpeed", "rpm", 100, 1, 3000);
    ESP_LOGD(TAG, "[retractor_%d] maxSpeed:%d rpm", slot_num, c->maxSpeed);

    /* Ускорение в об/мин за секунду - 0 выключает плавный разгон, По умолчанию 0
    */
    c->accel = get_option_int_val(slot_num, "accel", "rpm/s", 0, 0, 100000);
    ESP_LOGD(TAG, "[retractor_%d] accel:%d rpm/s", slot_num, c->accel);

    /* Kp регулятора скорости, По умолчанию 0,5
    */
    c->velKp = get_option_float_val(slot_num, "velKp", 0.5f, 0.0f, 1000.0f);
    ESP_LOGD(TAG, "[retractor_%d] velKp:%f", slot_num, c->velKp);

    /* Ki регулятора скорости - минус 1 это два Kp как в ODrive, По умолчанию минус 1
    */
    c->velKi = get_option_float_val(slot_num, "velKi", -1.0f, -1.0f, 1000.0f);
    if (c->velKi < 0.0f) c->velKi = c->velKp * 2.0f;
    ESP_LOGD(TAG, "[retractor_%d] velKi:%f", slot_num, c->velKi);

    /* Инверсия направления вращения - Флаг
    */
    c->dirSign = get_option_flag_val(slot_num, "dirInverse") ? -1 : 1;
    ESP_LOGD(TAG, "[retractor_%d] dirSign:%d", slot_num, c->dirSign);

    /* CAN ID драйвера 0-63, По умолчанию 1
    */
    c->node = get_option_int_val(slot_num, "motorId", "", 1, 0, 63);
    ESP_LOGD(TAG, "[retractor_%d] node:%d", slot_num, c->node);

    /* Скорость CAN шины, По умолчанию 500k
    */
    c->bitrate = get_option_enum_val(slot_num, "bitrate", "500k", "1M", "250k", "125k", "100k", NULL);
    if (c->bitrate < 0) c->bitrate = RCAN_BITRATE_500K;
    ESP_LOGD(TAG, "[retractor_%d] bitrate:%s", slot_num, bitrate_names[c->bitrate]);

    /* Частота обновления в Гц, По умолчанию 50, максимум 100
    */
    c->refreshPeriod = 1000 / get_option_int_val(slot_num, "refreshRate", "Hz", 50, 1, 100);
    ESP_LOGD(TAG, "[retractor_%d] refreshPeriod:%d ms", slot_num, c->refreshPeriod);

    /* Включить рапорты фактической скорости - Флаг
    */
    c->speedReportFlag = get_option_flag_val(slot_num, "speedReport");

    // Топик-база <deviceName>/retractor_<slot>: trigger - для event, action - чтобы executor доставлял команды
    {
        char t_str[strlen(me_config.deviceName) + strlen("/retractor_0") + 3];
        sprintf(t_str, "%s/retractor_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard topic base:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* === REPORTS === */

    /* Фактическая скорость в об/мин со знаком направления
    */
    c->speedReport = stdreport_register(RPTT_int, slot_num, "rpm", "event/speed");

    /* Авария - none, noResponse, canBusOff, currentLimit, motorOverTemp, dcBusUnderVoltage и др, сброс - action/enable 1
    */
    c->faultReport = stdreport_register(RPTT_string, slot_num, "", "event/fault");

    /* === COMMANDS === */

    /* Задать скорость в об/мин - знак задаёт направление, 0 - стоп с удержанием
    */
    stdcommand_register(&c->cmds, RCMD_setSpeed, "action/setSpeed", PARAMT_int);

    /* Крайнее положение в плюс 0-1 - 1 запрещает вращение с положительной скоростью
    */
    stdcommand_register(&c->cmds, RCMD_setUpLimit, "action/setUpLimit", PARAMT_int);

    /* Крайнее положение в минус 0-1 - 1 запрещает вращение с отрицательной скоростью
    */
    stdcommand_register(&c->cmds, RCMD_setDownLimit, "action/setDownLimit", PARAMT_int);

    /* Включить 1 или выключить 0 - выключенный вал свободен, повторная 1 сбрасывает аварию
    */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

// ---------------------------------------------------------------------------
// ------------------------------ Работа с мотором ---------------------------
// ---------------------------------------------------------------------------

static const char *axis_state_name(int state)
{
    switch (state) {
        case OD_AXIS_STATE_IDLE:        return "idle";
        case OD_AXIS_STATE_CLOSED_LOOP: return "closedLoop";
        case 0:                         return "undefined";
        case 2:                         return "startupSequence";
        case 3:                         return "fullCalibration";
        case 4:                         return "motorCalibration";
        case 6:                         return "encoderIndexSearch";
        case 7:                         return "encoderOffsetCalibration";
        default:                        return "other";
    }
}

/* Перевести ось в режим скорости и замкнутый контур */
static void retractor_motor_start(retractor_t *c, int slot_num)
{
    od_clear_errors(c->node);
    od_set_controller_mode(c->node, OD_CONTROL_MODE_VELOCITY, OD_INPUT_MODE_PASSTHROUGH);
    od_set_input_vel(c->node, 0.0f);
    od_set_axis_state(c->node, OD_AXIS_STATE_CLOSED_LOOP);
    c->latchedErrors = 0;
    c->targetSpeed = 0;
    c->cmdSpeed = 0;
    c->lastClosedLoopReqTick = xTaskGetTickCount();
    ESP_LOGD(TAG, "[retractor_%d] request closed loop", slot_num);
}

/* Мотор появился на шине: старт системы, подключение или перезагрузка драйвера */
static void retractor_on_online(retractor_t *c, int slot_num)
{
    ESP_LOGI(TAG, "[retractor_%d] motor online, node:%d", slot_num, c->node);
    od_request(c->node, OD_CMD_GET_VERSION);
    od_request(c->node, OD_CMD_BUS_VOLTAGE_CURRENT);
    if (c->maxCurrent > 0) {
        // Предел скорости с запасом: ODrive считает ошибкой превышение
        // vel_limit, а под нагрузкой регулятор может кратко его перескочить.
        // Свой предел скорости держит сам модуль (maxSpeed)
        float velLimit = (float)c->maxSpeed * 1.25f / 60.0f;
        od_set_limits(c->node, velLimit, (float)c->maxCurrent / 1000.0f);
        ESP_LOGI(TAG, "[retractor_%d] limits: current %d mA, speed %d rpm",
                 slot_num, c->maxCurrent, (int)lroundf(velLimit * 60.0f));
    }
    // Коэффициенты драйвер не сохраняет - шлём при каждом появлении мотора
    od_set_vel_gains(c->node, c->velKp, c->velKi);
    ESP_LOGI(TAG, "[retractor_%d] vel gains: Kp %.3f Ki %.3f", slot_num, c->velKp, c->velKi);
    if (c->active_state && c->latchedErrors == 0) {
        retractor_motor_start(c, slot_num);
    }
}

static void retractor_on_motor_frame(retractor_t *c, int cmd, const twai_message_t *msg, int slot_num)
{
    switch (cmd) {
        case OD_CMD_HEARTBEAT: {
            c->lastRxTick = xTaskGetTickCount();
            c->everRx = 1;
            uint32_t errors = od_get_u32(msg, 0);
            int state = msg->data[4];
            if (state != c->axisState) {
                ESP_LOGI(TAG, "[retractor_%d] axis state %s -> %s", slot_num,
                         axis_state_name(c->axisState), axis_state_name(state));
                c->axisState = state;
            }
            if (errors != c->axisErrors) {
                c->axisErrors = errors;
                if (errors) {
                    ESP_LOGW(TAG, "[retractor_%d] axis error 0x%08lx %s", slot_num, errors, od_error_name(errors));
                    // Подробности - активные ошибки и причина отключения
                    od_request(c->node, OD_CMD_GET_ERROR);
                } else {
                    ESP_LOGI(TAG, "[retractor_%d] axis error cleared", slot_num);
                }
            }
            if (errors && c->active_state) {
                c->latchedErrors |= errors;
                c->targetSpeed = 0;
            }
            break;
        }

        case OD_CMD_ENCODER_ESTIMATES:
            c->actualSpeed = (int)lroundf(od_get_float(msg, 4) * 60.0f) * c->dirSign;
            break;

        case OD_CMD_GET_ERROR: {
            uint32_t disarm = od_get_u32(msg, 4);
            ESP_LOGW(TAG, "[retractor_%d] active errors 0x%08lx, disarm reason 0x%08lx %s", slot_num,
                     od_get_u32(msg, 0), disarm, od_error_name(disarm));
            break;
        }

        case OD_CMD_GET_VERSION:
            ESP_LOGI(TAG, "[retractor_%d] driver protocol:%d hw:%d.%d.%d fw:%d.%d.%d", slot_num,
                     msg->data[0], msg->data[1], msg->data[2], msg->data[3],
                     msg->data[4], msg->data[5], msg->data[6]);
            break;

        case OD_CMD_BUS_VOLTAGE_CURRENT: {
            int mv = (int)lroundf(od_get_float(msg, 0) * 1000.0f);
            ESP_LOGI(TAG, "[retractor_%d] bus voltage %d mV", slot_num, mv);
            break;
        }

        default:
            break;
    }
}

/* Разбор всего, что пришло по шине за тик */
static void retractor_receive(retractor_t *c, int slot_num)
{
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        int cmd = od_frame_cmd(c->node, &msg);
        if (cmd >= 0) {
            retractor_on_motor_frame(c, cmd, &msg, slot_num);
            continue;
        }
        // Прочие кадры. Сюда встанет абсолютный энкодер крайних положений: по его
        // кадрам (свой ID) выставлять c->upLimit и c->downLimit - так же, как это
        // сейчас делают команды setUpLimit и setDownLimit
        if (c->unknownFramesLogged < UNKNOWN_FRAMES_LOG_MAX) {
            c->unknownFramesLogged++;
            ESP_LOGW(TAG, "[retractor_%d] unknown frame id:0x%lx%s%s dlc:%d data:%02x %02x %02x %02x %02x %02x %02x %02x",
                     slot_num, msg.identifier, msg.extd ? "(ext)" : "", msg.rtr ? "(rtr)" : "", msg.data_length_code,
                     msg.data[0], msg.data[1], msg.data[2], msg.data[3],
                     msg.data[4], msg.data[5], msg.data[6], msg.data[7]);
        }
    }
}

static const char *retractor_fault_string(retractor_t *c)
{
    if (!c->busOk)  return "canBusOff";
    if (!c->linkOk) return "noResponse";
    return od_error_name(c->latchedErrors);
}

// ---------------------------------------------------------------------------
// ---------------------------------- Задача ---------------------------------
// ---------------------------------------------------------------------------

static void retractor_task(void *arg)
{
    int slot_num = (int)(intptr_t)arg;
    retractor_t *c = calloc(1, sizeof(retractor_t));
    STDCOMMAND_PARAMS params = { 0 };

    configure_retractor(c, slot_num);
    c->busOk = 1;
    c->axisState = -1;
    c->prevSpeedReported = INT32_MIN;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    int rx_pin = SLOTS_PIN_MAP[slot_num][0];
    int tx_pin = SLOTS_PIN_MAP[slot_num][1];

    /* nSLEEP драйвера выходов слота - без него выход в высоком импедансе */
    enableSlotDriver(slot_num);

    if (rcan_init(tx_pin, rx_pin, (rcan_bitrate_t)c->bitrate) != ESP_OK) {
        // Контроллер TWAI один: занят другим CAN-модулем или ножки не годятся
        ESP_LOGE(TAG, "[retractor_%d] CAN init failed - task terminated", slot_num);
        waitForWorkPermit(slot_num);
        stdreport_s(c->faultReport, "canInitFailed");
        free(c);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "[retractor_%d] CAN started rx:%d tx:%d bitrate:%s, waiting for motor node:%d",
             slot_num, rx_pin, tx_pin, bitrate_names[c->bitrate], c->node);

    // Заданная скорость держится, пока не придёт другая команда. Контроллер мог
    // перезагрузиться при вращающемся моторе - освобождаем вал. Запуск - когда
    // мотор пришлёт heartbeat (motor online)
    od_set_axis_state(c->node, OD_AXIS_STATE_IDLE);

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, c->active_state);

    TickType_t lastWakeTime = xTaskGetTickCount();
    while (1) {
        TickType_t now = xTaskGetTickCount();

        // --- команды ---
        int cmd = stdcommand_receive(&c->cmds, &params, 0);
        switch (cmd) {
            case -1:
                break;

            case STDCMD_ENABLE:
                if (params.count > 0) {
                    int new_state = params.p[0].i ? 1 : 0;
                    if (new_state) {
                        // Повторная 1 тоже проходит - так сбрасывается авария
                        retractor_motor_start(c, slot_num);
                    } else {
                        od_set_axis_state(c->node, OD_AXIS_STATE_IDLE);
                        c->targetSpeed = 0;
                        c->cmdSpeed = 0;
                    }
                    if (new_state != c->active_state) {
                        c->active_state = new_state;
                        ESP_LOGD(TAG, "[retractor_%d] enable:%d", slot_num, c->active_state);
                        stdreport_enable(slot_num, c->active_state);
                    }
                }
                break;

            case RCMD_setSpeed:
                if (!c->active_state) {
                    ESP_LOGD(TAG, "[retractor_%d] disabled, setSpeed ignored", slot_num);
                    break;
                }
                c->targetSpeed = params.p[0].i;
                if (c->targetSpeed > c->maxSpeed)  c->targetSpeed = c->maxSpeed;
                if (c->targetSpeed < -c->maxSpeed) c->targetSpeed = -c->maxSpeed;
                ESP_LOGD(TAG, "[retractor_%d] setSpeed:%d", slot_num, c->targetSpeed);
                break;

            case RCMD_setUpLimit:
                c->upLimit = params.p[0].i ? 1 : 0;
                ESP_LOGD(TAG, "[retractor_%d] upLimit:%d", slot_num, c->upLimit);
                break;

            case RCMD_setDownLimit:
                c->downLimit = params.p[0].i ? 1 : 0;
                ESP_LOGD(TAG, "[retractor_%d] downLimit:%d", slot_num, c->downLimit);
                break;
        }

        // --- шина и связь с мотором ---
        retractor_receive(c, slot_num);

        if ((now - c->lastBusCheckTick) >= pdMS_TO_TICKS(BUS_CHECK_MS)) {
            c->lastBusCheckTick = now;
            c->busOk = !rcan_check_bus();
        }

        int linkNow = c->everRx && ((now - c->lastRxTick) < pdMS_TO_TICKS(NO_RESPONSE_MS));
        if (linkNow && !c->linkOk) {
            c->linkOk = 1;
            retractor_on_online(c, slot_num);
        } else if (!linkNow && c->linkOk) {
            ESP_LOGW(TAG, "[retractor_%d] motor not responding", slot_num);
            c->targetSpeed = 0;
            c->axisState = -1;
            c->lastLinkLogTick = now;
            rcan_log_status("link lost,");
        }
        c->linkOk = linkNow;

        // Пока связи нет - периодически показываем, что происходит на шине
        if (!c->linkOk && (now - c->lastLinkLogTick) >= pdMS_TO_TICKS(LINK_LOG_MS)) {
            c->lastLinkLogTick = now;
            ESP_LOGW(TAG, "[retractor_%d] no heartbeat from motor node:%d at %s",
                     slot_num, c->node, bitrate_names[c->bitrate]);
            rcan_log_status("no reply,");
        }

        // Ось ушла в IDLE без ошибки (перезагрузка драйвера и т п) - возвращаем контур
        if (c->active_state && c->linkOk && c->latchedErrors == 0 &&
            c->axisState == OD_AXIS_STATE_IDLE &&
            (now - c->lastClosedLoopReqTick) >= pdMS_TO_TICKS(CLOSED_LOOP_RETRY_MS)) {
            retractor_motor_start(c, slot_num);
        }

        // --- крайние положения: в запрещённую сторону не едем ---
        if (c->upLimit && c->targetSpeed > 0)   c->targetSpeed = 0;
        if (c->downLimit && c->targetSpeed < 0) c->targetSpeed = 0;
        if ((c->upLimit && c->cmdSpeed > 0) || (c->downLimit && c->cmdSpeed < 0)) {
            c->cmdSpeed = 0;    // упёрлись - останавливаемся сразу, без рампы
        }

        // --- скорость ---
        // Команда повторяется каждый тик - если в драйвере включён сторожевой
        // таймер оси, он остановит мотор, когда команды перестанут приходить
        int running = c->active_state && c->linkOk && c->busOk &&
                      c->latchedErrors == 0 && c->axisState == OD_AXIS_STATE_CLOSED_LOOP;
        if (running) {
            float target = (float)c->targetSpeed;
            if (c->accel > 0) {
                float step = (float)c->accel * (float)c->refreshPeriod / 1000.0f;
                if (c->cmdSpeed < target) {
                    c->cmdSpeed += step;
                    if (c->cmdSpeed > target) c->cmdSpeed = target;
                } else if (c->cmdSpeed > target) {
                    c->cmdSpeed -= step;
                    if (c->cmdSpeed < target) c->cmdSpeed = target;
                }
            } else {
                c->cmdSpeed = target;
            }
            od_set_input_vel(c->node, c->cmdSpeed * (float)c->dirSign / 60.0f);
        } else {
            c->cmdSpeed = 0;
        }

        // --- рапорты ---
        const char *fault = retractor_fault_string(c);
        if (fault != c->prevFaultReported) {
            c->prevFaultReported = fault;
            stdreport_s(c->faultReport, (char *)fault);
        }

        if (c->speedReportFlag && c->actualSpeed != c->prevSpeedReported &&
            (now - c->lastSpeedReportTick) >= pdMS_TO_TICKS(SPEED_REPORT_MIN_MS)) {
            c->prevSpeedReported = c->actualSpeed;
            c->lastSpeedReportTick = now;
            stdreport_i(c->speedReport, c->actualSpeed);
        }

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(c->refreshPeriod));
    }
}

void start_retractor_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_retractor_%d", slot_num);
    xTaskCreatePinnedToCore(retractor_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES-5, NULL, 1);
    ESP_LOGD(TAG, "retractor task created for slot:%d Heap usage:%lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_retractor()
{
    return manifesto;
}
