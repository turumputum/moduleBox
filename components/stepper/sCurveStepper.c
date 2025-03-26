#include <stdio.h>
#include <math.h>
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "driver/pulse_cnt.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sCurveStepper.h"
#include "stepper_motor_encoder.h"
#include "esp_check.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

///---------------------------encoder----------------------------
static float convert_to_smooth_freq(uint32_t freq1, uint32_t freq2, uint32_t freqx){
    float normalize_x = ((float)(freqx - freq1)) / (freq2 - freq1);
    // third-order "smoothstep" function: https://en.wikipedia.org/wiki/Smoothstep
    float smooth_x = normalize_x * normalize_x * (3 - 2 * normalize_x);
    return smooth_x * (freq2 - freq1) + freq1;
}

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t copy_encoder;
    uint32_t sample_points;
    struct {
        uint32_t is_accel_curve: 1;
    } flags;
    rmt_symbol_word_t curve_table[];
} rmt_stepper_curve_encoder_t;

static size_t rmt_encode_stepper_motor_curve(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state){
    rmt_stepper_curve_encoder_t *motor_encoder = __containerof(encoder, rmt_stepper_curve_encoder_t, base);
    rmt_encoder_handle_t copy_encoder = motor_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    uint32_t points_num = *(uint32_t *)primary_data;
    size_t encoded_symbols = 0;
    if (motor_encoder->flags.is_accel_curve) {
        //ESP_LOGD(TAG, "acc encoded_symbols: %d sample_points: %ld points_num: %ld", encoded_symbols, motor_encoder->sample_points, points_num);
        encoded_symbols = copy_encoder->encode(copy_encoder, channel, &motor_encoder->curve_table[0],
                                               points_num * sizeof(rmt_symbol_word_t), &session_state);
    } else {
        //ESP_LOGD(TAG, "dec encoded_symbols: %d sample_points: %ld points_num: %ld", encoded_symbols, motor_encoder->sample_points, points_num);
        encoded_symbols = copy_encoder->encode(copy_encoder, channel, &motor_encoder->curve_table[0] + motor_encoder->sample_points - points_num,
                                               points_num * sizeof(rmt_symbol_word_t), &session_state);
    }
    *ret_state = session_state;
    return encoded_symbols;
}

static esp_err_t rmt_del_stepper_motor_curve_encoder(rmt_encoder_t *encoder){
    rmt_stepper_curve_encoder_t *motor_encoder = __containerof(encoder, rmt_stepper_curve_encoder_t, base);
    rmt_del_encoder(motor_encoder->copy_encoder);
    free(motor_encoder);
    return ESP_OK;
}

static esp_err_t rmt_reset_stepper_motor_curve_encoder(rmt_encoder_t *encoder){
    rmt_stepper_curve_encoder_t *motor_encoder = __containerof(encoder, rmt_stepper_curve_encoder_t, base);
    rmt_encoder_reset(motor_encoder->copy_encoder);
    return ESP_OK;
}

esp_err_t rmt_new_stepper_motor_curve_encoder(const stepper_motor_curve_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder) {
    esp_err_t ret = ESP_OK;
    rmt_stepper_curve_encoder_t *step_encoder = NULL;
    uint32_t symbol_duration;
    
    ESP_GOTO_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, err, TAG, "invalid arguments");
    ESP_GOTO_ON_FALSE(config->sample_points, ESP_ERR_INVALID_ARG, err, TAG, "sample points number can't be zero");
    ESP_GOTO_ON_FALSE(config->start_freq_hz != config->end_freq_hz, ESP_ERR_INVALID_ARG, err, TAG, "start freq can't equal to end freq");
    
    step_encoder = rmt_alloc_encoder_mem(sizeof(rmt_stepper_curve_encoder_t) + config->sample_points * sizeof(rmt_symbol_word_t));
    ESP_GOTO_ON_FALSE(step_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for stepper curve encoder");
    
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &step_encoder->copy_encoder), err, TAG, "create copy encoder failed");
    
    bool is_accel_curve = config->start_freq_hz < config->end_freq_hz;
    
    // Рассчитать точное время, необходимое для выполнения всех шагов
    float total_steps = (float)config->sample_points;
    float start_speed = (float)config->start_freq_hz;  // шагов/сек
    float end_speed = (float)config->end_freq_hz;      // шагов/сек
    
    // Для трапецевидного профиля скорости с линейным ускорением и замедлением
    float accel=0;
    float decel=0;
    float time_total;
    
    if (is_accel_curve) {
        // Профиль ускорения
        accel = (end_speed * end_speed - start_speed * start_speed) / (2.0f * total_steps);
        time_total = (end_speed - start_speed) / accel; // Общее время для ускорения
        
        // ESP_LOGD(TAG, "Accel profile: steps=%d, start_speed=%.2f, end_speed=%.2f, accel=%.2f, time=%.4f", 
        //         (int)total_steps, start_speed, end_speed, accel, time_total);
                
        // Создаем таблицу с точным интервалом между шагами
        float current_speed = start_speed;
        float current_time = 0.0f;
        float prev_time = 0.0f;
        
        for (uint32_t i = 0; i < config->sample_points; i++) {
            // Вычисляем время этого конкретного шага
            if (i > 0) {
                // v = v0 + a*t => t = (v - v0) / a
                current_time = (current_speed - start_speed) / accel;
                
                // Точный интервал между шагами (в микросекундах)
                symbol_duration = (uint32_t)((current_time - prev_time) * config->resolution / 2);
            } else {
                // Первый шаг
                symbol_duration = (uint32_t)(config->resolution / start_speed / 2);
            }
            
            // Создаем символ RMT (пара уровней и их длительностей)
            step_encoder->curve_table[i].level0 = 0;
            step_encoder->curve_table[i].duration0 = symbol_duration;
            step_encoder->curve_table[i].level1 = 1;
            step_encoder->curve_table[i].duration1 = symbol_duration;
            
            // Обновляем скорость для следующего шага
            prev_time = current_time;
            current_speed = sqrtf(start_speed * start_speed + 2.0f * accel * (i + 1));
            
            // ESP_LOGD(TAG, "Step %d: speed=%.2f, interval=%.2f μs",i, current_speed, (float)symbol_duration * 2);
        }
        
    } else {
        // Профиль замедления
        decel = (start_speed * start_speed - end_speed * end_speed) / (2.0f * total_steps);
        time_total = (start_speed - end_speed) / decel; // Общее время для замедления
        
        ESP_LOGD(TAG, "Decel profile: steps=%d, start_speed=%.2f, end_speed=%.2f, decel=%.2f, time=%.4f", 
                (int)total_steps, start_speed, end_speed, decel, time_total);
                
        // Создаем таблицу с точным интервалом между шагами
        float current_speed = start_speed;
        float remaining_steps = total_steps;
        
        for (uint32_t i = 0; i < config->sample_points; i++) {
            // Вычисляем интервал для этого шага (в микросекундах)
            symbol_duration = (uint32_t)(config->resolution / current_speed / 2);
            
            // Инвертированный порядок для профиля замедления
            uint32_t idx = config->sample_points - i - 1;
            step_encoder->curve_table[idx].level0 = 0;
            step_encoder->curve_table[idx].duration0 = symbol_duration;
            step_encoder->curve_table[idx].level1 = 1;
            step_encoder->curve_table[idx].duration1 = symbol_duration;
            
            // Обновляем скорость для следующего шага
            remaining_steps -= 1.0f;
            if (remaining_steps > 0) {
                current_speed = sqrtf(start_speed * start_speed - 2.0f * decel * (total_steps - remaining_steps));
                // Предотвращаем слишком низкую скорость
                current_speed = fmaxf(current_speed, end_speed);
            }
            
            // ESP_LOGD(TAG, "Step %d: speed=%.2f, interval=%.2f μs",i, current_speed, (float)symbol_duration * 2);
        }
    }
    
    // Проверка на ошибку в расчётах (шаг ускорения/замедления должен быть положительным)
    ESP_GOTO_ON_FALSE(accel > 0 || decel > 0, ESP_ERR_INVALID_ARG, err, TAG, 
                     "Invalid acceleration/deceleration parameters");

    step_encoder->sample_points = config->sample_points;
    step_encoder->flags.is_accel_curve = is_accel_curve;
    step_encoder->base.del = rmt_del_stepper_motor_curve_encoder;
    step_encoder->base.encode = rmt_encode_stepper_motor_curve;
    step_encoder->base.reset = rmt_reset_stepper_motor_curve_encoder;
    *ret_encoder = &(step_encoder->base);

    // ESP_LOGD(TAG, "Trapezoidal velocity profile encoder created, sample_points: %ld, total_time: %.4f sec", 
    //          step_encoder->sample_points, time_total);
    return ESP_OK;
    
err:
    if (step_encoder) {
        if (step_encoder->copy_encoder) {
            rmt_del_encoder(step_encoder->copy_encoder);
        }
        free(step_encoder);
    }
    return ret;
}

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t copy_encoder;
    uint32_t resolution;
} rmt_stepper_uniform_encoder_t;

static size_t rmt_encode_stepper_motor_uniform(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state){
    rmt_stepper_uniform_encoder_t *motor_encoder = __containerof(encoder, rmt_stepper_uniform_encoder_t, base);
    rmt_encoder_handle_t copy_encoder = motor_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    uint32_t target_freq_hz = *(uint32_t *)primary_data;
    uint32_t symbol_duration = motor_encoder->resolution / target_freq_hz / 2;
    rmt_symbol_word_t freq_sample = {
        .level0 = 0,
        .duration0 = symbol_duration,
        .level1 = 1,
        .duration1 = symbol_duration,
    };
    size_t encoded_symbols = copy_encoder->encode(copy_encoder, channel, &freq_sample, sizeof(freq_sample), &session_state);
    *ret_state = session_state;
    //ESP_LOGD(TAG, "uniform encoded_symbols: %d target_freq_hz: %ld symbol_duration: %ld", encoded_symbols, target_freq_hz, symbol_duration);
    return encoded_symbols;
}

static esp_err_t rmt_del_stepper_motor_uniform_encoder(rmt_encoder_t *encoder){
    rmt_stepper_uniform_encoder_t *motor_encoder = __containerof(encoder, rmt_stepper_uniform_encoder_t, base);
    rmt_del_encoder(motor_encoder->copy_encoder);
    free(motor_encoder);
    return ESP_OK;
}

static esp_err_t rmt_reset_stepper_motor_uniform(rmt_encoder_t *encoder){
    rmt_stepper_uniform_encoder_t *motor_encoder = __containerof(encoder, rmt_stepper_uniform_encoder_t, base);
    rmt_encoder_reset(motor_encoder->copy_encoder);
    return ESP_OK;
}

esp_err_t rmt_new_stepper_motor_uniform_encoder(const stepper_motor_uniform_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder){
    esp_err_t ret = ESP_OK;
    rmt_stepper_uniform_encoder_t *step_encoder = NULL;
    ESP_GOTO_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, err, TAG, "invalid arguments");
    step_encoder = rmt_alloc_encoder_mem(sizeof(rmt_stepper_uniform_encoder_t));
    ESP_GOTO_ON_FALSE(step_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for stepper uniform encoder");
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &step_encoder->copy_encoder), err, TAG, "create copy encoder failed");

    step_encoder->resolution = config->resolution;
    step_encoder->base.del = rmt_del_stepper_motor_uniform_encoder;
    step_encoder->base.encode = rmt_encode_stepper_motor_uniform;
    step_encoder->base.reset = rmt_reset_stepper_motor_uniform;
    *ret_encoder = &(step_encoder->base);
    return ESP_OK;
err:
    if (step_encoder) {
        if (step_encoder->copy_encoder) {
            rmt_del_encoder(step_encoder->copy_encoder);
        }
        free(step_encoder);
    }
    return ret;
}


//---------------------------
esp_err_t sCurveStepper_waitMoveDone(sCurveStepper_t *stepper){
    ESP_RETURN_ON_FALSE(stepper, ESP_ERR_INVALID_ARG, TAG, "Invalid stepper");
    pcnt_unit_get_count(stepper->pcntUnit, &stepper->currentPos);
    while(stepper->currentPos!=stepper->targetPos){
        ESP_LOGD(TAG, "Wait move done currentPos:%ld targetPos:%ld", stepper->currentPos, stepper->targetPos);
        vTaskDelay(10 / portTICK_PERIOD_MS);
        pcnt_unit_get_count(stepper->pcntUnit, &stepper->currentPos);
    }
    return ESP_OK;
}

//------------------sCurveStepper------------------------------
esp_err_t sCurveStepper_init(sCurveStepper_t *stepper) {
    
    
    
    // PCNT unit configuration
    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit = INT16_MIN,
        .flags.accum_count = true, // accumulate the counter value
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &stepper->pcntUnit));

    // PCNT channel configuration
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = stepper->stepPin,
        .level_gpio_num = stepper->dirPin, // No level GPIO
    };
    ESP_ERROR_CHECK(pcnt_new_channel(stepper->pcntUnit, &chan_config, &stepper->pcntChan));
    
        //Настраиваем счет в зависимости от DIR
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(stepper->pcntChan,
                                                  PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                  PCNT_CHANNEL_EDGE_ACTION_HOLD));
    
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(stepper->pcntChan,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    // Set initial counter value
    //ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(pcnt, &(pcnt_glitch_filter_config_t){.max_glitch_ns = 1000}), TAG, "Set glitch filter failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(stepper->pcntUnit), TAG, "Enable PCNT unit failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(stepper->pcntUnit), TAG, "Clear PCNT count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(stepper->pcntUnit), TAG, "Start PCNT unit failed");
    
    gpio_config_t en_dir_gpio_config = {
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << stepper->stepPin | 1ULL << stepper->dirPin,
    };
    ESP_ERROR_CHECK(gpio_config(&en_dir_gpio_config));

    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = stepper->stepPin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 10,
        .flags.io_loop_back = true,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &stepper->rmtHandle), TAG, "Create RMT TX channel failed");
    //ESP_RETURN_ON_ERROR(rmt_enable(stepper->rmtHandle), TAG, "Enable RMT channel failed");
    
    
    ESP_LOGD(TAG, "rmtChannel created freeHeap:%d", xPortGetFreeHeapSize());

    return ESP_OK;
}

esp_err_t sCurveStepper_moveTo(sCurveStepper_t *stepper, int32_t position) {
    int32_t current_position;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(stepper->pcntUnit, &current_position), TAG, "Get current position failed");
    stepper->targetPos = position;
    int32_t distance = position - current_position;
    if (distance == 0) {
        return ESP_OK; // Already at target position
    }
    
    // Set direction
    bool direction = distance > 0;
    gpio_set_level(stepper->dirPin, direction ? 1 : 0);
    //ESP_LOGD(TAG, "---Direction: %s distance:%ld", direction ? "Forward" : "Backward", distance);
    // Calculate movement parameters
    uint32_t abs_distance = abs(distance);
    
    // Calculate acceleration/deceleration distance based on steps/sec²
    // s = (v²-u²)/(2a) where s=distance, v=final speed, u=initial speed, a=acceleration
    float accel_distance = (stepper->maxSpeed * stepper->maxSpeed - stepper->minSpeed * stepper->minSpeed) / (2.0 * stepper->accel);
    stepper->accel_steps = (uint32_t)accel_distance;
    stepper->decel_steps = stepper->accel_steps;
    
    // Limit acceleration steps to half the total distance if needed
    if (stepper->accel_steps + stepper->decel_steps > abs_distance) {
        stepper->accel_steps = abs_distance / 2;
        stepper->decel_steps = abs_distance - stepper->accel_steps;
    }
    
    stepper->uniform_steps = abs_distance - stepper->accel_steps - stepper->decel_steps;
    
    // Delete existing encoders if they exist
    if (stepper->accel_encoder != NULL) {
        rmt_del_encoder(stepper->accel_encoder);
        stepper->accel_encoder = NULL;
    }
    
    if (stepper->uniform_encoder != NULL) {
        rmt_del_encoder(stepper->uniform_encoder);
        stepper->uniform_encoder = NULL;
    }
    
    if (stepper->decel_encoder != NULL) {
        rmt_del_encoder(stepper->decel_encoder);
        stepper->decel_encoder = NULL;
    }
    
    // Configure acceleration encoder
    stepper_motor_curve_encoder_config_t accel_encoder_config = {
        .sample_points = stepper->accel_steps,
        .start_freq_hz = stepper->minSpeed,
        .end_freq_hz = stepper->maxSpeed,
        .resolution = 1000000, // 1MHz resolution
    };
    ESP_RETURN_ON_ERROR(rmt_new_stepper_motor_curve_encoder(&accel_encoder_config, &stepper->accel_encoder), 
                        TAG, "Create acceleration encoder failed");
    
    // Configure uniform speed encoder
    stepper_motor_uniform_encoder_config_t uniform_encoder_config = {
        .resolution = 1000000, // 1MHz resolution
    };
    ESP_RETURN_ON_ERROR(rmt_new_stepper_motor_uniform_encoder(&uniform_encoder_config, &stepper->uniform_encoder), 
                        TAG, "Create uniform encoder failed");
    
    // Configure deceleration encoder
    stepper_motor_curve_encoder_config_t decel_encoder_config = {
        .sample_points = stepper->decel_steps,
        .start_freq_hz = stepper->maxSpeed,
        .end_freq_hz = stepper->minSpeed,
        .resolution = 1000000, // 1MHz resolution
    };
    ESP_RETURN_ON_ERROR(rmt_new_stepper_motor_curve_encoder(&decel_encoder_config, &stepper->decel_encoder), 
                        TAG, "Create deceleration encoder failed");

    rmt_enable(stepper->rmtHandle);
    //Prepare and start acceleration phase
    if (stepper->accel_steps > 0) {
        stepper->accel_config.loop_count = 0; // No loop
        //stepper->accel_config.flags.queue_nonblocking = false;
        ESP_RETURN_ON_ERROR(rmt_transmit(stepper->rmtHandle, stepper->accel_encoder, &stepper->accel_steps, sizeof(stepper->accel_steps), &stepper->accel_config),
                          TAG, "Send acceleration curve failed");
    }
    
    // Prepare and start constant speed phase
    if (stepper->uniform_steps > 0) {
        //uint32_t constant_freq = stepper->maxSpeed;
        // rmt_transmit_config_t tx_config = {
        //     .loop_count = constant_steps, // Loop for constant_steps times
        // };
        stepper->uniform_config.loop_count = stepper->uniform_steps; // No loop
        //stepper->uniform_config.flags.queue_nonblocking = false;
        ESP_RETURN_ON_ERROR(rmt_transmit(stepper->rmtHandle, stepper->uniform_encoder, &stepper->maxSpeed, sizeof(stepper->maxSpeed), &stepper->uniform_config),TAG, "Send constant speed pulses failed");
        //ESP_LOGD(TAG, "constant_steps:%ld  loop_count:%d", stepper->uniform_steps, stepper->uniform_config.loop_count);
    }
    
    // // Prepare and start deceleration phase
    if (stepper->decel_steps > 0) {
        // rmt_transmit_config_t tx_config = {
        //     .loop_count = 0, // No loop
        // };
        stepper->decel_config.loop_count = 0; // No loop
        //stepper->decel_config.flags.queue_nonblocking = false;
        ESP_RETURN_ON_ERROR(rmt_transmit(stepper->rmtHandle, stepper->decel_encoder, &stepper->decel_steps, sizeof(stepper->decel_steps), &stepper->decel_config),TAG, "Send deceleration curve failed");
    }

    // vTaskDelay(1000 / portTICK_PERIOD_MS);
    //rmt_tx_wait_all_done(stepper->rmtHandle, -1);
    float accel_time = 2.0f * stepper->accel_steps / (stepper->minSpeed + stepper->maxSpeed);
    float uniform_time = (float)stepper->uniform_steps / stepper->maxSpeed;
    float decel_time = 2.0f * stepper->decel_steps / (stepper->minSpeed + stepper->maxSpeed);
    float moveTime = accel_time + uniform_time + decel_time;
    //ESP_LOGD(TAG, "---moveTime:%f accel_time:%f uniform_time:%f decel_time:%f", moveTime, accel_time, uniform_time, decel_time); 
    ESP_LOGD(TAG, "MoveTo:%ld accel_steps:%ld continius_steps:%ld decel_steps:%ld maxSpeed:%ld minSpeed:%ld", position, stepper->accel_steps, stepper->uniform_steps, stepper->decel_steps, stepper->maxSpeed, stepper->minSpeed); 
    return ESP_OK;
}

esp_err_t sCurveStepper_setZero(sCurveStepper_t *stepper) {
    ESP_RETURN_ON_FALSE(stepper, ESP_ERR_INVALID_ARG, TAG, "Invalid stepper");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(stepper->pcntUnit), TAG, "Clear PCNT count failed");
    stepper->currentPos=0;
    return ESP_OK;
}

esp_err_t sCurveStepper_stop(sCurveStepper_t *stepper) {
    ESP_LOGW(TAG, "Emergency stop requested");

    // Stop any ongoing RMT transmission
    rmt_disable(stepper->rmtHandle);
    rmt_del_channel(stepper->rmtHandle);
    
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = stepper->stepPin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1000,
        .flags.io_loop_back = true,
        .flags.with_dma = true,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &stepper->rmtHandle), TAG, "Create RMT TX channel failed");

    return ESP_OK;
}
