// ***************************************************************************
// TITLE
//      retractor - CAN шина (TWAI) и протокол ODrive CANSimple
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <string.h>
#include "retractor.h"
#include "freertos/task.h"
#include "esp_log.h"

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "RETRACTOR_CAN";

// ---------------------------------------------------------------------------
// --------------------------------- CAN шина --------------------------------
// ---------------------------------------------------------------------------

esp_err_t rcan_init(int tx_pin, int rx_pin, rcan_bitrate_t bitrate)
{
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NORMAL);
    // Драйвер сам шлёт оценки энкодера раз в 10 мс, плюс запас под кадры
    // будущего энкодера на той же линии
    g_config.rx_queue_len = 32;
    g_config.tx_queue_len = 8;

    twai_timing_config_t t_config;
    switch (bitrate) {
        case RCAN_BITRATE_1M:   t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();   break;
        case RCAN_BITRATE_250K: t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); break;
        case RCAN_BITRATE_125K: t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(); break;
        case RCAN_BITRATE_100K: t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_100KBITS(); break;
        case RCAN_BITRATE_500K:
        default:                t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); break;
    }
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        // ESP_ERR_INVALID_STATE - контроллер уже занят другим CAN-модулем
        ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return err;
    }
    ESP_LOGD(TAG, "TWAI started tx:%d rx:%d bitrate:%d", tx_pin, rx_pin, bitrate);
    return ESP_OK;
}

esp_err_t rcan_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    twai_message_t msg = {0};
    msg.identifier = id;
    msg.data_length_code = len;
    memcpy(msg.data, data, len);
    // Без ожидания: если кадр никто не подтверждает, очередь передачи забивается,
    // и блокировать рабочий цикл из-за этого нельзя
    return twai_transmit(&msg, 0);
}

esp_err_t rcan_send_rtr(uint32_t id, uint8_t len)
{
    twai_message_t msg = {0};
    msg.identifier = id;
    msg.rtr = 1;
    msg.data_length_code = len;
    return twai_transmit(&msg, 0);
}

int rcan_check_bus(void)
{
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return 1;
    }
    switch (st.state) {
        case TWAI_STATE_RUNNING:
            return 0;
        case TWAI_STATE_BUS_OFF:
            ESP_LOGW(TAG, "bus-off, initiate recovery");
            twai_initiate_recovery();
            return 1;
        case TWAI_STATE_STOPPED:
            // восстановление завершено - контроллер надо запустить заново
            ESP_LOGW(TAG, "bus recovered, restart");
            twai_start();
            return 1;
        default:    // RECOVERING
            return 1;
    }
}

void rcan_log_status(const char *prefix)
{
    static const char *states[] = { "stopped", "running", "busOff", "recovering" };
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        ESP_LOGW(TAG, "%s bus status unavailable", prefix);
        return;
    }
    // Подсказка по счётчикам: растёт tec и txFailed при нуле rx - кадр никто не
    // подтверждает (нет второго узла, обрыв, нет питания мотора, нет терминатора).
    // Растёт rec и busErr - скорость шины не совпадает или перепутаны CANH-CANL
    ESP_LOGW(TAG, "%s bus:%s tec:%lu rec:%lu txQueue:%lu rxQueue:%lu txFailed:%lu rxMissed:%lu busErr:%lu",
             prefix, (st.state <= TWAI_STATE_RECOVERING) ? states[st.state] : "?",
             st.tx_error_counter, st.rx_error_counter, st.msgs_to_tx, st.msgs_to_rx,
             st.tx_failed_count, st.rx_missed_count, st.bus_error_count);
}

// ---------------------------------------------------------------------------
// --------------------------------- ODrive ----------------------------------
// ---------------------------------------------------------------------------

static uint32_t od_id(uint8_t node, uint8_t cmd)
{
    return ((uint32_t)node << 5) | cmd;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void put_float(uint8_t *p, float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    put_u32(p, v);
}

esp_err_t od_request(uint8_t node, uint8_t cmd)
{
    // Все Get_ сообщения, которые мы запрашиваем, длиной 8
    return rcan_send_rtr(od_id(node, cmd), 8);
}

esp_err_t od_set_axis_state(uint8_t node, uint32_t state)
{
    uint8_t d[8] = {0};
    put_u32(&d[0], state);
    return rcan_send(od_id(node, OD_CMD_SET_AXIS_STATE), d, 8);
}

esp_err_t od_set_controller_mode(uint8_t node, uint32_t control_mode, uint32_t input_mode)
{
    uint8_t d[8];
    put_u32(&d[0], control_mode);
    put_u32(&d[4], input_mode);
    return rcan_send(od_id(node, OD_CMD_SET_CONTROLLER_MODE), d, 8);
}

esp_err_t od_set_input_vel(uint8_t node, float vel)
{
    uint8_t d[8];
    put_float(&d[0], vel);
    put_float(&d[4], 0.0f);     // момент упреждения не используем
    return rcan_send(od_id(node, OD_CMD_SET_INPUT_VEL), d, 8);
}

esp_err_t od_set_limits(uint8_t node, float vel_limit, float current_limit)
{
    uint8_t d[8];
    put_float(&d[0], vel_limit);
    put_float(&d[4], current_limit);
    return rcan_send(od_id(node, OD_CMD_SET_LIMITS), d, 8);
}

esp_err_t od_clear_errors(uint8_t node)
{
    uint8_t d[1] = { 0 };       // identify 0 - без мигания светодиодом
    return rcan_send(od_id(node, OD_CMD_CLEAR_ERRORS), d, 1);
}

esp_err_t od_set_vel_gains(uint8_t node, float kp, float ki)
{
    uint8_t d[8];
    put_float(&d[0], kp);
    put_float(&d[4], ki);
    return rcan_send(od_id(node, OD_CMD_SET_VEL_GAINS), d, 8);
}

int od_frame_cmd(uint8_t node, const twai_message_t *msg)
{
    if (msg->extd || msg->rtr || (msg->identifier >> 5) != node) {
        return -1;
    }
    return msg->identifier & 0x1F;
}

uint32_t od_get_u32(const twai_message_t *msg, int offset)
{
    return (uint32_t)msg->data[offset] |
           ((uint32_t)msg->data[offset + 1] << 8) |
           ((uint32_t)msg->data[offset + 2] << 16) |
           ((uint32_t)msg->data[offset + 3] << 24);
}

float od_get_float(const twai_message_t *msg, int offset)
{
    uint32_t v = od_get_u32(msg, offset);
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

const char *od_error_name(uint32_t errors)
{
    // Порядок - по важности для пользователя, а не по номеру бита
    static const struct { uint32_t bit; const char *name; } names[] = {
        { 0x00000400, "dcBusOverCurrent" },
        { 0x00001000, "currentLimit" },
        { 0x00002000, "motorOverTemp" },
        { 0x00004000, "inverterOverTemp" },
        { 0x00000100, "dcBusOverVoltage" },
        { 0x00000200, "dcBusUnderVoltage" },
        { 0x00000800, "overRegenCurrent" },
        { 0x00008000, "velocityLimit" },
        { 0x00010000, "positionLimit" },
        { 0x04000000, "spinout" },
        { 0x00000020, "drvFault" },
        { 0x01000000, "watchdog" },
        { 0x02000000, "estop" },
        { 0x08000000, "brakeResistor" },
        { 0x10000000, "thermistor" },
        { 0x40000000, "calibration" },
        { 0x00000010, "badConfig" },
        { 0x00000008, "missingEstimate" },
        { 0x00000040, "missingInput" },
        { 0x00000004, "timing" },
        { 0x00000002, "systemLevel" },
        { 0x00000001, "initializing" },
    };
    if (errors == 0) return "none";
    for (int i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (errors & names[i].bit) return names[i].name;
    }
    return "unknown";
}
