#include "asyncStepper.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_slot_config.h"
#include "driver/gpio.h"
#include "reporter.h"
#include "stateConfig.h"
#include "driver/pulse_cnt.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "math.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us - выдержка DIR при развороте

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

// static IRAM_ATTR bool pcnt_on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *arg) {
//     asyncStepper_t* stepper = (asyncStepper_t*) arg;
//     stepper->_motorStatus = STEPPER_MOTOR_STOPPED;
//     stepper->_currentPosition = stepper->_targetPosition;
//     // Останавливаем двигатель
//     mcpwm_timer_start_stop(stepper->_timer, MCPWM_TIMER_STOP_EMPTY);
//     return false;
// }

void speedStepper_init(speedStepper_t *stepper, gpio_num_t step_pin, gpio_num_t dir_pin, uint8_t pulseWidth){
	stepper->_stepPin = step_pin;
    stepper->_directionPin = dir_pin;

    ///-----------------------mcpwm init------------------------------
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = stepper->_resolution,  // 1MHz
        .period_ticks = 100,      // 1KHz
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .flags.update_period_on_empty = true,
    }; 
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &stepper->_timer));

    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &stepper->_oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(stepper->_oper, stepper->_timer));

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(stepper->_oper, &comparator_config, &stepper->_comparator));
    
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(stepper->_comparator, pulseWidth));

    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = stepper->_stepPin,
        .flags.io_loop_back = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(stepper->_oper, &generator_config, &stepper->_generator));

    // go high on counter empty
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(stepper->_generator,
                                                              MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    // go low on compare threshold
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(stepper->_generator,
                                                                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, stepper->_comparator, MCPWM_GEN_ACTION_LOW)));


    // Настройка GPIO для DIR
    gpio_config_t dir_gpio = {
        .pin_bit_mask = (1ULL << stepper->_directionPin),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&dir_gpio));


    // Запуск таймера
    ESP_ERROR_CHECK(mcpwm_timer_enable(stepper->_timer));
    ESP_LOGD(TAG, "init speedStepper end");
}


void speedStepper_stop(speedStepper_t *stepper) {
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->_timer, MCPWM_TIMER_STOP_FULL));
    stepper->_state=SPEED_STEPPER_STOP;
}

void speedStepper_start(speedStepper_t *stepper) {
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->_timer, MCPWM_TIMER_START_NO_STOP));
    stepper->_state=SPEED_STEPPER_RUN;
}

void speedStepper_setSpeed(speedStepper_t *stepper, int32_t spd) {
    if(spd==0){
        return;
    }
    uint32_t period = stepper->_resolution/abs(spd);
    if(period < stepper->_minPeriod){
        period = stepper->_minPeriod;
    }
    mcpwm_timer_set_period(stepper->_timer, period);
    //ESP_LOGD(TAG, "setPer %ld", period);
}

void speedStepper_setDirection(speedStepper_t *stepper, int8_t clockwise) {
    //ESP_LOGD(TAG, "setDirection %d", clockwise);
    if(clockwise==1){
        gpio_set_level(stepper->_directionPin, !stepper->_dirInverse);
    }else{
        gpio_set_level(stepper->_directionPin, stepper->_dirInverse);
    }
}




//+++++++++++++++++++++++++++++++++stepper++++++++++++++++++++++++++++++++++++++++

#define STATE_ACCEL 0
#define STATE_RUN 1
#define STATE_DECEL 2
#define STATE_STOP 3

// Минимальный период step-импульса в тиках mcpwm (resolution=1MHz -> 5 тиков = 200kHz).
// Защищает таймер mcpwm от слишком высокой частоты при большом currentSpeed.
#define STEPPER_MIN_PERIOD 5

// Парковка - только по ТОЧНОМУ равенству currentPos==targetPos.
// Раньше стоял допуск в 2 шага: программный тик ловил остаток <=2 и парковал
// мотор, не дожидаясь аппаратной watch-точки PCNT - moveTo:10000 заканчивался
// на 9998. PCNT считает импульсы, реально ушедшие в драйвер (io_loop_back), так
// что недоезд физический. Допуск был костылём от автоколебаний старой логики,
// когда таймер на нулевой скорости продолжал пульсировать на полу minSpeed;
// теперь генерация на нуле гасится (pulsesPaused), а перелёт в пару шагов
// исправляется разворотом на минимальной скорости до аппаратной остановки.
// От разноса защищает STEPPER_CORR_MAX ниже.

// Защита от разноса при доводке: сколько разворотов ВБЛИЗИ цели (остаток не
// больше STEPPER_CORR_WINDOW шагов) допускаем на один ход. Если ось столько раз
// перелетела и вернулась, значит цель недостижима (DIR/dirInverse врут, счётчик
// теряет шаги) - паркуемся там, где стоим, вместо бесконечных качаний.
#define STEPPER_CORR_WINDOW 16
#define STEPPER_CORR_MAX    3

// Выдержка DIR перед возобновлением импульсов step, мкс. С запасом перекрывает
// требования типовых драйверов (A4988 ~200нс, DRV8825 ~650нс, TMC - больше).
// Платим ею только в момент разворота, когда скорость и так нулевая.
#define STEPPER_DIR_SETUP_US 10

// Зовётся в том числе из ISR (pcnt_on_target_reached, IRAM_ATTR), поэтому без
// llabs и прочих вызовов, которые могут оказаться во flash: только инлайн-арифметика.
static inline int stepper_atTarget(stepper_t *stepper){
    return (stepper->targetPos == stepper->currentPos);
}

void stepper_getCurrentPos(stepper_t *stepper){
    int pos = 0;
    pcnt_unit_get_count(stepper->pcntUnit, &pos);
    
    int32_t delta= pos - stepper->pcnt_prevPos;
    if (abs(delta) > (INT16_MAX/2)) {
        // Произошло переполнение
        if(delta>0){
            //переполнение в отрицательной зоне
            delta = pos -(stepper->pcnt_prevPos - INT16_MIN);
            //ESP_LOGD(TAG, "overload in negative zone");
        }else{
            delta = pos - (stepper->pcnt_prevPos - INT16_MAX);
            //ESP_LOGD(TAG, "overload in positive zone");
        }
    }
    
    stepper->currentPos = stepper->currentPos + delta;
    stepper->absPos = stepper->absPos + delta; // истинная позиция, переживает хак runSpeed
    //ESP_LOGD(TAG, "currentPos: %ld pos:%ld prevPos:%ld delta:%ld dir:%d", stepper->currentPos, pos, stepper->pcnt_prevPos, delta, stepper->dir);
    stepper->pcnt_prevPos = pos;
}

void stepper_stop(stepper_t *stepper) {
    stepper->targetPos = stepper->currentPos;
    stepper->runSpeedFlag = 0;
    stepper->currentSpeed = 0;
    stepper->targetSpeed = 0;
    // Без ESP_ERROR_CHECK: функция зовётся из ISR (pcnt_on_target_reached), а
    // abort из прерывания - это panic и ребут посреди записи на SD-карту.
    mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_STOP_FULL);
    stepper->state=STOP;
    stepper->pulsesPaused = 0;
    //ESP_LOGD(TAG, "stopped");
}

void stepper_break(stepper_t *stepper) {
    stepper->runSpeedFlag = 0;
    stepper_getCurrentPos(stepper);
    int64_t chisl = (((int64_t)stepper->currentSpeed * (int64_t)stepper->currentSpeed));
    float znam = 2.0f * stepper->accel;
    float accel_distance = chisl / znam;
    int64_t target =stepper->currentPos + accel_distance*stepper->dir; 
    stepper_moveTo(stepper, target);
    ESP_LOGD(TAG, "break_distance: %f stopPoint: %lld", accel_distance, target);
}

// ВНИМАНИЕ: колбэк зовёт stepper_getCurrentPos и stepper_stop -> mcpwm_timer_start_stop,
// которые лежат во flash. Поэтому CONFIG_PCNT_ISR_IRAM_SAFE должен быть ВЫКЛЮЧЕН
// (см. sdkconfig.defaults): иначе прерывание придёт при отключённом кэше flash и
// прошивка упадёт с "Cache disabled but cached memory region accessed".
static bool IRAM_ATTR pcnt_on_target_reached(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_data) {
    stepper_t* stepper = (stepper_t*) user_data;
    stepper_getCurrentPos(stepper);
    //ESP_LOGD(TAG, "currentPos: %ld", stepper->currentPos);
    // pcnt_unit_remove_watch_point(stepper->pcntUnit, stepper->currentPos);
    //stepper->state++;
    // Watch-точка стоит ровно на цели, поэтому здесь останавливаем по точному
    // равенству. Сюда же приходят события границ аккумуляции +-32767 - для них
    // равенства нет, и мотор едет дальше.
    if(stepper_atTarget(stepper)){
        // Останавливаем двигатель
        stepper_stop(stepper);
        pcnt_unit_clear_count(stepper->pcntUnit);
        stepper->pcnt_prevPos = 0;
    }
    int tmpCnt = 0;
    pcnt_unit_get_count(stepper->pcntUnit, &tmpCnt);
    if(tmpCnt!=stepper->pcnt_watchPoint){
        pcnt_unit_clear_count(stepper->pcntUnit);
        stepper->pcnt_prevPos = 0;
    }
    // else if(stepper->currentPos == stepper->breakPoint){
    //     stepper->targetSpeed = 0;
    // }
    //pcnt_unit_remove_watch_point(stepper->pcntUnit, stepper->currentPos);
    return true;
}


esp_err_t stepper_init(stepper_t *stepper, gpio_num_t step_pin, gpio_num_t dir_pin, uint8_t pulseWidth){
	stepper->stepPin = step_pin;
    stepper->dirPin = dir_pin;

    //-----------------------pcnt init------------------------------
    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit = INT16_MIN,
        .flags.accum_count = true, // accumulate the counter value
    };
    esp_err_t pcnt_err = pcnt_new_unit(&unit_config, &stepper->pcntUnit);
    if (pcnt_err != ESP_OK) {
        return pcnt_err;
    }

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
    
    // Register event callbacks
    pcnt_event_callbacks_t cbs = {
        .on_reach = pcnt_on_target_reached,
    };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(stepper->pcntUnit, &cbs, stepper));
    //ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MAX-1));
    //ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MIN+1));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, stepper->pcnt_watchPoint));

    ESP_ERROR_CHECK(pcnt_unit_enable(stepper->pcntUnit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(stepper->pcntUnit));
    ESP_ERROR_CHECK(pcnt_unit_start(stepper->pcntUnit));

    //----------------dir config--------------------------------
    gpio_config_t en_dir_gpio_config = {
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << stepper->stepPin | 1ULL << stepper->dirPin,
    };
    ESP_ERROR_CHECK(gpio_config(&en_dir_gpio_config));

    //-----------------------mcpwm init------------------------------
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = stepper->resolution,  // 1MHz
        .period_ticks = 100,      // 1KHz
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .flags.update_period_on_empty = true,
    };
    // В группе MCPWM три таймера и три оператора - четвёртый stepper их не
    // получит. Раньше здесь стоял ESP_ERROR_CHECK, и лишний мотор в конфиге
    // ронял прошивку в abort и бесконечную перезагрузку. Теперь освобождаем
    // уже занятый PCNT и возвращаем ESP_FAIL - вызывающий сообщит пользователю.
    esp_err_t mcpwm_err = mcpwm_new_timer(&timer_config, &stepper->mcpwmTimer);
    if (mcpwm_err != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm_new_timer failed err:%d - no free MCPWM timer (max 3 steppers)", mcpwm_err);
        pcnt_unit_stop(stepper->pcntUnit);
        pcnt_unit_disable(stepper->pcntUnit);
        pcnt_del_channel(stepper->pcntChan);
        pcnt_del_unit(stepper->pcntUnit);
        stepper->pcntUnit = NULL;
        stepper->pcntChan = NULL;
        return ESP_FAIL;
    }

    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &stepper->mcpwmOper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(stepper->mcpwmOper, stepper->mcpwmTimer));

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(stepper->mcpwmOper, &comparator_config, &stepper->mcpwmComparator));

    // Желаемая длительность HIGH импульса step (мкс при резолюции 1МГц).
    // Реальное значение пересчитывается в stepper_setPeriod: HIGH = min(pulseWidth, period/2).
    stepper->pulseWidth = pulseWidth ? pulseWidth : 1;
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(stepper->mcpwmComparator, stepper->pulseWidth));

    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = stepper->stepPin,
        .flags.io_loop_back = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(stepper->mcpwmOper, &generator_config, &stepper->mcpwmGenerator));

    // go high on counter empty
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(stepper->mcpwmGenerator,MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    // go low on compare threshold
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(stepper->mcpwmGenerator,MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, stepper->mcpwmComparator, MCPWM_GEN_ACTION_LOW)));


    // Запуск таймера
    ESP_ERROR_CHECK(mcpwm_timer_enable(stepper->mcpwmTimer));
    ESP_LOGD(TAG, "init speedStepper end");
    return ESP_OK;
}


// Единая точка установки периода step-импульса.
// period - в тиках таймера (мкс при резолюции 1МГц).
// HIGH-уровень импульса не может занимать больше половины периода, иначе high и low
// перестают быть равными - на высокой частоте урезаем pulseWidth до period/2 и пишем warning.
void stepper_setPeriod(stepper_t *stepper, uint32_t period){
    if(period < STEPPER_MIN_PERIOD) period = STEPPER_MIN_PERIOD;
    // Пол по скорости: период не больше resolution/minSpeed (и не больше UINT16_MAX -
    // аппаратный предел таймера). Так мотор никогда не ползёт медленнее minSpeed
    // и короткие доезды не тянутся на ~15 шаг/с.
    uint32_t maxPeriod = UINT16_MAX;
    if(stepper->minSpeed > 0){
        uint32_t minSpeedPeriod = stepper->resolution / stepper->minSpeed;
        if(minSpeedPeriod < maxPeriod) maxPeriod = minSpeedPeriod;
    }
    if(period > maxPeriod) period = maxPeriod;

    uint32_t high = stepper->pulseWidth;
    uint32_t halfPeriod = period / 2;
    uint8_t clamped = 0;
    if(high > halfPeriod){
        high = halfPeriod;
        if(high < 1) high = 1;
        clamped = 1;
    }

    // warn-once: логируем только при входе в режим урезания, чтобы не спамить каждый период
    if(clamped && !stepper->pulseClamped){
        ESP_LOGW(TAG, "step freq too high: pulse %uus needs period>=%uus but period=%luus - HIGH clamped to %luus (50%% duty)",
                 stepper->pulseWidth, (unsigned)(stepper->pulseWidth*2), period, high);
    }
    stepper->pulseClamped = clamped;

    mcpwm_comparator_set_compare_value(stepper->mcpwmComparator, high);
    mcpwm_timer_set_period(stepper->mcpwmTimer, period);
}


void stepper_checkDir(stepper_t *stepper){
    if((stepper->dir==DIR_UP)&&(stepper->currentPos > stepper->targetPos)){
        //надо менять направление вращения
        //ESP_LOGD(TAG, "Dir change needed, up");
        stepper->targetSpeed = 0;
    }else if((stepper->dir==DIR_DOWN)&&(stepper->currentPos < stepper->targetPos)){
        //надо менять направление вращения
        //ESP_LOGD(TAG, "Dir change needed, down");
        stepper->targetSpeed = 0;
    }

    if(stepper->currentSpeed==0){
        int8_t dir = stepper->targetPos > stepper->currentPos ? DIR_UP : DIR_DOWN;
        if(dir!=stepper->dir){
            // Менять DIR, пока идут импульсы step, нельзя: часть драйверов
            // защёлкивает направление по фронту step, и переключение "на ходу"
            // они отрабатывают непредсказуемо - мотор просто встаёт (симптом
            // плавает от драйвера к драйверу). Скорость здесь уже нулевая, но
            // таймер mcpwm продолжает пульсировать на полу minSpeed, поэтому на
            // время смены DIR гасим генерацию, выдерживаем паузу и запускаем снова.
            int wasRunning = (stepper->state == RUN);

            if(wasRunning){
                mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_STOP_FULL);
            }

            /* Разворот вблизи цели - это доводка после перелёта. Считаем их:
               больше STEPPER_CORR_MAX на один ход - паркуемся (см. define). */
            int64_t dist = llabs((int64_t)stepper->targetPos - (int64_t)stepper->currentPos);
            if(dist <= STEPPER_CORR_WINDOW){
                if(++stepper->corrCount > STEPPER_CORR_MAX){
                    ESP_LOGW(TAG, "target unreachable: %d reversals within %d steps, parking at %ld (target %ld)",
                             stepper->corrCount, STEPPER_CORR_WINDOW, stepper->currentPos, stepper->targetPos);
                    stepper_stop(stepper);
                    return;
                }
            }else{
                stepper->corrCount = 0;
            }

            stepper->dir=dir;
            gpio_set_level(stepper->dirPin, dir==DIR_UP ? !stepper->dirInverse : stepper->dirInverse);
            esp_rom_delay_us(STEPPER_DIR_SETUP_US);

            if(wasRunning){
                stepper_setPeriod(stepper, UINT16_MAX);   // возобновляем с самого медленного периода
                mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_START_NO_STOP);
                stepper->pulsesPaused = 0;
            }

            ESP_LOGD(TAG, "Dir changed, newDir:%s dirInverse:%d", stepper->dir==DIR_UP?"up":"down", stepper->dirInverse);

            // Скорость разворота берём по ОСТАТКУ хода: v = sqrt(2*a*s). Раньше
            // здесь безусловно ставился maxSpeed - разгон на полной скорости ради
            // коррекции в пару шагов гарантировал перелёт и раскачивал
            // автоколебания. На длинном ходе формула сама даёт maxSpeed, так что
            // обычные перемещения не замедляются.
            // Порядок клампов важен: сначала пол minSpeed, потом потолок maxSpeed -
            // потолок должен побеждать, иначе runSpeed:0 (maxSpeed==0) уполз бы
            // на minSpeed вместо остановки.
            int64_t v    = (int64_t)sqrt(2.0 * (double)stepper->accel * (double)dist);

            if(v < stepper->minSpeed) v = stepper->minSpeed;
            if(v > stepper->maxSpeed) v = stepper->maxSpeed;

            stepper->targetSpeed = (int32_t)v;
        }

    }
}

void stepper_moveTo(stepper_t *stepper, int32_t pos){
    stepper->targetPos = pos;
    stepper->targetSpeed = stepper->maxSpeed;
    stepper->corrCount = 0;

    pcnt_unit_remove_watch_point(stepper->pcntUnit, stepper->pcnt_watchPoint);
    pcnt_unit_remove_watch_point(stepper->pcntUnit, INT16_MAX);
    pcnt_unit_remove_watch_point(stepper->pcntUnit, INT16_MIN);

    /* Позицию читаем и счётчик обнуляем вплотную друг к другу: импульсы,
       проскочившие между чтением и обнулением, теряются для currentPos. Раньше
       между ними стояли снятие watch-точек и checkDir - при команде на ходу
       это стоило шаг-другой на каждый moveTo. */
    stepper_getCurrentPos(stepper);
    pcnt_unit_clear_count(stepper->pcntUnit);
    stepper->pcnt_prevPos = 0;

    int64_t distance = (int64_t)stepper->targetPos - (int64_t)stepper->currentPos;
    if(distance==0){
        // Стоим на цели: watch-точки уже сняты, взводим только границы
        // аккумуляции, чтобы счётчик не потерялся до следующего хода.
        pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MAX);
        pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MIN);
        if(stepper->state != STOP) stepper_stop(stepper);
        return;
    }
// ВАЖНО: считаем в int64. distance может превышать INT32_MAX (диапазон позиции
    // INT32_MIN..INT32_MAX), и усечение в int32 ДО свертки ломало бы модуль 32767.
    // Асимметрия 32767/32768 - не ошибка: это периоды аккумуляции HW PCNT для
    // high_limit (INT16_MAX) и low_limit (INT16_MIN) соответственно.
    int64_t watchPoint = distance;

    stepper_checkDir(stepper);

    /* Свёртку делаем по ЗНАКУ ОСТАТКА, а не по stepper->dir. При команде на ходу
       в обратную сторону checkDir выше НЕ меняет dir - он ждёт, пока скорость
       дойдёт до нуля, - то есть здесь dir ещё старый. Раньше из-за этого ни одна
       ветка свёртки не выбиралась, watchPoint зажимался в рельс ±32767 (видно в
       логе: 'watchPoint:-32767' при distance -76176), и аппаратное прерывание
       прибытия для реверсивных ходов не срабатывало вовсе. Обе петли выполнять
       безопасно: значение не может быть одновременно больше MAX и меньше MIN. */
    while(watchPoint>INT16_MAX){
        watchPoint-=INT16_MAX;
    }
    while(watchPoint<INT16_MIN){
        watchPoint-=INT16_MIN;
    }
    // Точка останова цели не должна совпадать с граничной watch-точкой аккумуляции
    // (INT16_MAX / INT16_MIN). Иначе второй add падает ('add watchpoint failed'),
    // PCNT остается с единственной watch-точкой на high_limit и уходит в вырожденное
    // состояние (видно при runSpeed: distance кратен 32767 -> watchPoint == 32767).
    // Сдвиг на 1 шаг на точность останова не влияет.
    if(watchPoint >= INT16_MAX) watchPoint = INT16_MAX - 1;
    if(watchPoint <= INT16_MIN) watchPoint = INT16_MIN + 1;
    stepper->pcnt_watchPoint = (int16_t)watchPoint;

    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, stepper->pcnt_watchPoint));
    // Взводим ОБЕ границы аккумуляции, а не только "по текущему dir". Направление
    // может смениться ПОЗЖЕ: при реверсе на ходу checkDir разворачивает мотор лишь
    // когда скорость дойдёт до нуля, уже после moveTo. Тогда нужная граница
    // осталась бы невзведённой, PCNT перестал бы аккумулировать на ±32767, и
    // currentPos уехал бы. Watch-точек хватает: thresh + high_limit + low_limit.
    pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MAX);
    pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MIN);
    //ESP_LOGD(TAG, "add watch point %d", stepper->pcnt_watchPoint);


    /* Профиль скорости больше НЕ планируется здесь заранее: stepper_speedUpdate
       каждый тик пересчитывает потолок скорости от остатка пути. Прежний расчёт
       reachableSpeed отсюда убран - он мог перебить обнулённый checkDir'ом
       targetSpeed и на один тик продолжить разгон в старую сторону. */

    /* breakWay нужен только хаку runSpeed. Считаем его от maxSpeed, а НЕ от
       targetSpeed: checkDir выше обнуляет targetSpeed при развороте на ходу, и
       тогда отсюда выходил нулевой тормозной путь - в логе 'chisl:0
       break_distance:0 breakPoint:0' при команде против движения. */
    int64_t chisl = ((int64_t)stepper->maxSpeed * (int64_t)stepper->maxSpeed);
    float znam = 2.0f * stepper->accel;
    float break_distance = chisl / znam;
    ESP_LOGD(TAG, "chisl:%lld znam:%f break_distance: %f maxSpeed: %ld currentSpeed: %ld", chisl, znam, break_distance, stepper->maxSpeed, stepper->currentSpeed);
    if(break_distance>llabs(distance)){
        break_distance = llabs(distance);
    }

    stepper->breakPoint = stepper->targetPos-(break_distance*stepper->dir);
    stepper->breakWay = break_distance;
    if(stepper->state==STOP){
        stepper_setPeriod(stepper, UINT16_MAX);
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_START_NO_STOP));
        stepper->state=RUN;
        stepper->pulsesPaused = 0;
    }

    ESP_LOGD(TAG, "currentPos:%ld targetPos:%ld watchPoint:%d accel_distance: %f breakPoint: %ld  state:%s", stepper->currentPos, stepper->targetPos, stepper->pcnt_watchPoint, break_distance, stepper->breakPoint, stepper->state==STOP?"STOP":"RUN");
    //ESP_LOGD(TAG, "currentSpeed: %ld targetSpeed: %ld dir:%s", stepper->currentSpeed, stepper->targetSpeed, stepper->dir==DIR_UP?"up":"down");
}

void stepper_speedUpdate(stepper_t *stepper, int32_t period){  
    // Прирост скорости за период: Δv = accel * (period_ms / 1000).
    // Умножаем ДО деления (иначе accel<1000 даёт 0), int64 - защита от переполнения.
    int32_t speedIncrement = ((int64_t)stepper->accel * period) / 1000;
    
    if(stepper->runSpeedFlag==1){
        if(llabs(stepper->targetPos-stepper->currentPos)<stepper->breakWay*2){
            stepper->currentPos=0;
        }
    }

    // Цель достигнута ТОЧНО - паркуемся и выходим (обычно это уже сделала
    // watch-точка PCNT в ISR, здесь - страховка). Перелёт на шаг-два сюда не
    // попадает: ниже checkDir развернёт ось и доведёт на минимальной скорости,
    // а остановит её аппаратная watch-точка ровно на цели.
    // В режиме runSpeed сюда не попадаем - хак выше держит остаток хода большим.
    if(stepper_atTarget(stepper)){
        if(stepper->state != STOP){
            stepper_stop(stepper);
        }
        return;
    }

    if (speedIncrement<1) speedIncrement=1;

    /* Разворот, если цель осталась позади: checkDir гасит targetSpeed, а по
       достижении нулевой скорости меняет DIR и назначает скорость возврата. */
    stepper_checkDir(stepper);
    // checkDir мог припарковать ось (лимит разворотов при доводке) - дальше не считаем
    if(stepper->state == STOP) return;

    int8_t needDir = (stepper->targetPos > stepper->currentPos) ? DIR_UP : DIR_DOWN;

    if(stepper->dir == needDir){
        /* Потолок скорости считаем ОТ ОСТАТКА ПУТИ, а не по заранее посчитанной
           точке торможения. Точка торможения планировалась один раз в moveTo и
           после разворота оказывалась по ДРУГУЮ сторону от цели - обратный ход
           шёл вообще без торможения и пролетал цель насквозь. Здесь же профиль
           самокорректирующийся: работает в обе стороны, переживает разворот,
           смену accel-maxSpeed на ходу и джиттер планировщика.

           Формула - не непрерывная v=sqrt(2*a*s), а её ДИСКРЕТНО безопасный
           вариант. За тик скорость держится постоянной, поэтому условие
           'на следующем тике торможения ещё хватит' даёт
                v <= sqrt((a*dt)^2 + 2*a*s) - a*dt,
           где a*dt - это ровно speedIncrement. Непрерывная формула стабильно
           перелетала на v*dt/2 (100 шагов при 10000 шаг-с и 20 мс). */
        int64_t s   = llabs((int64_t)stepper->targetPos - (int64_t)stepper->currentPos);
        int64_t adt = speedIncrement;
        int64_t v   = (int64_t)(sqrt((double)adt*(double)adt + 2.0*(double)stepper->accel*(double)s) - (double)adt);

        if(v < 0) v = 0;
        /* Порядок клампов важен: сначала пол minSpeed, потом потолок maxSpeed -
           потолок должен побеждать, иначе runSpeed:0 (maxSpeed==0) уполз бы на
           minSpeed вместо остановки. */
        if(v < stepper->minSpeed) v = stepper->minSpeed;
        if(v > stepper->maxSpeed) v = stepper->maxSpeed;

        stepper->targetSpeed = (int32_t)v;
    }

    if(stepper->targetSpeed!=stepper->currentSpeed){
        if(stepper->currentSpeed<stepper->targetSpeed){
            stepper->currentSpeed += speedIncrement;
            if(stepper->currentSpeed>stepper->targetSpeed){
                stepper->currentSpeed = stepper->targetSpeed;
            }
        }else{
            stepper->currentSpeed -= speedIncrement;
            if(stepper->currentSpeed<stepper->targetSpeed){
                stepper->currentSpeed = stepper->targetSpeed;
            }
        }
    }

    /* Инвариант: скорость больше нуля - генерация идёт, скорость нулевая -
       генерация выключена. Владелец таймера на ходу - только этот код. */
    if(stepper->currentSpeed > 0){
        // период клампится в stepper_setPeriod (MIN_PERIOD..UINT16_MAX),
        // там же пересчитывается HIGH импульса под текущую частоту
        stepper_setPeriod(stepper, stepper->resolution/stepper->currentSpeed);
        if(stepper->pulsesPaused && stepper->state == RUN){
            mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_START_NO_STOP);
            stepper->pulsesPaused = 0;
        }
    }else{
        /* Скорость дошла до нуля - импульсы обязаны прекратиться. Раньше здесь
           требовалось ТОЧНОЕ currentPos==targetPos, и при остатке хотя бы в шаг
           таймер mcpwm продолжал пульсировать на последнем периоде (медленнее
           ~15 шаг-с он не умеет - период зажат UINT16_MAX). Ось ползла мимо
           цели и сама себя загоняла в автоколебания. */
        if(stepper->dir == needDir){
            // разворот не нужен, ехать некуда: ход окончен (в т-ч runSpeed:0)
            if(stepper->state != STOP){
                stepper_stop(stepper);
            }
        }else if(stepper->state == RUN && !stepper->pulsesPaused){
            /* Нулевая скорость перед разворотом: гасим генерацию, но state=RUN
               оставляем - ход не окончен. Флаг нужен, чтобы вернуть генерацию,
               даже если разворота в итоге не будет: придёт новая команда в
               ТЕКУЩУЮ сторону - checkDir промолчит, а moveTo перезапускает
               таймер только из state==STOP, и ось зависла бы без импульсов. */
            mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_STOP_FULL);
            stepper->pulsesPaused = 1;
        }
    }
}



void stepper_setZero(stepper_t *stepper) {
    ESP_ERROR_CHECK(pcnt_unit_clear_count(stepper->pcntUnit));
    stepper->pcnt_prevPos = 0;
    stepper->currentPos = 0;
    stepper->absPos = 0;
}
