#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_slot_config.h"
#include "driver/gptimer.h"
#include <esp_timer.h>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include <string.h>
#include "driver/rmt_tx.h"
//#include "accelStepper.h"
#include "executor.h"
#include "stepper.h"

#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

#include "asyncStepper.h"

#include <stdcommand.h>
#include <stdreport.h>

#include <generated_files/gen_stepper.h>

#include <manifest.h>
#include <mbdebug.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";


//---------------------------stepper------------------------------
#define NOT_HOMED 0
#define HOMING 0
#define IDLE 1
#define RUN_POS 2
#define RUN_SPEED 3
#define GOING_HOME 4
// Анабиоз: базирование задано, но дом не найден за homingTimeout. Импульсы step
// молчат, команды движения не принимаются - ждём повторный goHome. Опознаётся
// снаружи по морганию светодиода DIR при тишине на step.
#define HOMING_FAILED 5

// Период смены уровня DIR в анабиозе, мс (два переключения в секунду).
#define ANABIOSIS_BLINK_MS 500

#define UP 1
#define DOWN -1


typedef struct __tag_STEPPERCONFIG{
	uint8_t 				state;
	int 				    dir;
	uint32_t 				accel;
	uint32_t 				maxSpeed;
	int32_t 				minSpeed;
	uint16_t                refreshPeriod;
	int					    homingDir;
    int                     homingSensorState;
    int                     upLimitState;    // аппаратный лимит верх (up/плюс): 1 - движение вверх запрещено
    int                     downLimitState;  // аппаратный лимит низ (down/минус): 1 - движение вниз запрещено
    int32_t                 currentPos;
    int32_t                 targetPos;
	uint32_t				homingSpeed;
	uint32_t				homingTimeout;
	uint8_t					pulseWidth;
	int32_t				    maxVal;
    int32_t                 minVal;
	int 					speedReportFlag;
	int 					posReportFlag;
	int 					stateReportFlag;
    int 					circularCounterFlag;
    int                     goHomeOnStart;
    int                     active_state;

    STDCOMMANDS             cmds;

	int 					posReport;
	int						speedReport;
	int						stateReport;
	int 					homeReport;
} STEPPERCONFIG, * PSTEPPERCONFIG; 

typedef enum
{
	stepCMD_goHome = 0,
	stepCMD_moveToAbs,
    stepCMD_moveToInc,
	stepCMD_runSpeed,
    stepCMD_setMaxSpeed,
    stepCMD_setAccel,
    stepCMD_stop,
	stepCMD_break,
    stepCMD_setHomingSensor,
    stepCMD_setUpLimit,
    stepCMD_setDownLimit
} stepCMD;

/* Положительный остаток (a mod m), m>0 - для режима кругового счетчика */
static int64_t stepper_wrapmod(int64_t a, int64_t m){
    int64_t r = a % m;
    if(r < 0) r += m;
    return r;
}

/*
    Управление шаговым двигателем сигналами step-dir через PCNT
    PCNT периферии всего 4 на encoderInc + tachometer + stepper
    slots: 0-5
*/
void configure_stepper(PSTEPPERCONFIG c, int slot_num){
    stdcommand_init(&c->cmds, slot_num);
    /* Если флаг поднят - модуль стартует в выключенном состоянии,
       до прихода action/enable 1 (Конституция §6).
    */
    c->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "[stepper_%d] Initial active_state:%d", slot_num, c->active_state);

    c->dir = UP;
    /* Инверсия направления вращения. up-down. По умолчанию up
	*/
	c->dir = get_option_flag_val(slot_num, "dirInverse") ? DOWN : UP;
	ESP_LOGD(TAG, "[stepper_%d] dir:%s", slot_num, (c->dir == UP)? "up" : "down");

    /* Включить рапорты положения. Флаг.
	*/
	c->posReportFlag = get_option_flag_val(slot_num, "posReport");
	if(c->posReportFlag){
        ESP_LOGD(TAG, "[stepper_%d] posReport enable", slot_num);
    }

    /* Режим кругового счётчика. Флаг. 
	*/
	c->circularCounterFlag = get_option_flag_val(slot_num, "circularCounter");
	if(c->circularCounterFlag){
        ESP_LOGD(TAG, "[stepper_%d] circularCounter enable", slot_num);
    }

    /* Базировать сразу при старте - иначе ждать команду goHome. Флаг.
	*/
	c->goHomeOnStart = get_option_flag_val(slot_num, "goHomeOnStart");
	if(c->goHomeOnStart){
        ESP_LOGD(TAG, "[stepper_%d] goHomeOnStart enable", slot_num);
    }

    /* Включить рапорты скорости. Флаг.
	*/
	c->speedReportFlag = get_option_flag_val(slot_num, "speedReport");
	if(c->speedReportFlag){
        ESP_LOGD(TAG, "[stepper_%d] speedReport enable", slot_num);
    }

    /* Включить рапорты состояния - run или stop. Флаг.
	*/
	c->stateReportFlag = get_option_flag_val(slot_num, "stateReport");
	if(c->stateReportFlag){
        ESP_LOGD(TAG, "[stepper_%d] stateReport enable", slot_num);
    }


    c->state=NOT_HOMED;
    c->homingDir = 0;
    /* Направление базирования up-down - по умолчанию поиск дома выключен.
    */
    if ((c->homingDir = get_option_enum_val(slot_num, "homingDir","", "up", "down", NULL)) < 0){
        
    }
    if(c->homingDir==1){
        c->homingDir = UP;
        ESP_LOGD(TAG, "[stepper_%d] homing dir:up", slot_num);
    }else if(c->homingDir==2){
        c->homingDir = DOWN;
        ESP_LOGD(TAG, "[stepper_%d] homing dir:down", slot_num);
    }else{
        c->homingDir = 0;   // не задан - поиск дома выключен, сразу рабочий режим
        c->state=IDLE;
        ESP_LOGD(TAG, "[stepper_%d] homing procedure disabled", slot_num);
    }



    /* Ускорение и замедление в шаг-сек2. Int 1..2147483647, По умолчанию 100
	*/
	c->accel =  get_option_int_val(slot_num, "accel", "step/sek^2", 100, 1, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] accel:%ld", slot_num, c->accel);

    /* Максимальная скорость в шаг-сек. Int 1..2147483647, По умолчанию 100
	*/
	c->maxSpeed =  get_option_int_val(slot_num, "maxSpeed", "step/sek", 100, 1, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] maxSpeed:%ld", slot_num, c->maxSpeed);

    // Минимальная скорость (старт/стоп трапеции) вычисляется автоматически:
    // тысячная от maxSpeed, но не меньше 2 шаг/с. Опции нет.
    c->minSpeed = c->maxSpeed / 1000;
    if (c->minSpeed < 2) c->minSpeed = 2;
    ESP_LOGD(TAG, "[stepper_%d] minSpeed(auto):%ld step/sek", slot_num, c->minSpeed);
    
    /* Частота обновления в Гц, По умолчанию 20, максимум 100
	*/
	c->refreshPeriod =  1000/get_option_int_val(slot_num, "refreshRate", "fps", 20, 1, 100);
    ESP_LOGD(TAG, "[stepper_%d] refreshPeriod:%d", slot_num, c->refreshPeriod);

    /* Скорость базирования в шаг-сек. Int 1..2147483647, По умолчанию maxSpeed-4. 
	*/
	c->homingSpeed =  get_option_int_val(slot_num, "homingSpeed", "step/sek", c->maxSpeed / 4, 1, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] homingSpeed:%ld", slot_num, c->homingSpeed);

    /* Таймаут базирования в секундах - 0 выключает, По умолчанию 30
	*/
	c->homingTimeout =  get_option_int_val(slot_num, "homingTimeout", "sek", 30, 0, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] homingTimeout:%ld", slot_num, c->homingTimeout);

    // Длительность HIGH импульса step вычисляется автоматически (опции больше нет):
    // базово 10 мкс, но если на maxSpeed минимальный период step мал, ограничиваем
    // HIGH половиной этого периода (50% duty). На лету в stepper_setPeriod HIGH
    // дополнительно зажимается под текущую частоту.
    {
        uint32_t minPeriod = 1000000UL / (c->maxSpeed > 0 ? c->maxSpeed : 1); // мкс, resolution 1МГц
        uint32_t autoPulse = minPeriod / 2;
        if (autoPulse > 10) autoPulse = 10;
        if (autoPulse < 1)  autoPulse = 1;
        c->pulseWidth = autoPulse;
    }
    ESP_LOGD(TAG, "[stepper_%d] pulseWidth(auto):%d us (maxSpeed:%ld)", slot_num, c->pulseWidth, c->maxSpeed);

    /* Максимальное положение в шагах. Int32 -2147483648..2147483647, По умолчанию INT32_MAX
	*/
	c->maxVal =  get_option_int_val(slot_num, "maxVal", "step", INT32_MAX, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] maxVal:%ld", slot_num, c->maxVal);

    /* Минимальное положение в шагах. Int32 -2147483648..2147483647, По умолчанию INT32_MIN
	*/
	c->minVal =  get_option_int_val(slot_num, "minVal", "step", INT32_MIN, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "[stepper_%d] minVal:%ld", slot_num, c->minVal);

    // Standard topic
	{
		char t_str[strlen(me_config.deviceName)+strlen("/stepper_0")+3];
		sprintf(t_str, "%s/stepper_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart stepper_topic:%s", me_state.action_topic_list[slot_num]);
	}

    /* Текущее положение в шагах. Int32 -2147483648..2147483647
	*/
	c->posReport = stdreport_register(RPTT_string, slot_num, "step", "event/pos");

    /* Текущая скорость в шаг-сек. Int32 -2147483648..2147483647
	*/
	c->speedReport = stdreport_register(RPTT_string, slot_num, "step/sek", "event/speed");

    /* Состояние мотора - run, stop, maxVal, minVal, upLimit или downLimit
	*/
	c->stateReport = stdreport_register(RPTT_string, slot_num, "", "event/state");

    /* Состояние базирования - disable, waitingCommand, homing, done или homingTimeout
	*/
	c->homeReport = stdreport_register(RPTT_string, slot_num, "", "event/homingState");

    /* Запустить базирование. Без параметров. 
    */
    stdcommand_register(&c->cmds, stepCMD_goHome, "action/goHome", PARAMT_none);

    /* Перейти в абсолютную позицию. Int32 -2147483648..2147483647.
    */
    stdcommand_register(&c->cmds, stepCMD_moveToAbs, "action/moveToAbs", PARAMT_int);

    /* Сместиться на приращение. Int32 -2147483648..2147483647.
    */
    stdcommand_register(&c->cmds, stepCMD_moveToInc, "action/moveToInc", PARAMT_int);

    /* Вращать с заданной скоростью. Int32 -2147483648..2147483647.
    */
    stdcommand_register(&c->cmds, stepCMD_runSpeed, "action/runSpeed", PARAMT_int);

    /* Установить максимальную скорость. Int32 0..2147483647.
    */
    stdcommand_register(&c->cmds, stepCMD_setMaxSpeed, "action/setMaxSpeed", PARAMT_int);

    /* Установить ускорение. Int32 0..2147483647.
    */
    stdcommand_register(&c->cmds, stepCMD_setAccel, "action/setAccel", PARAMT_int);

    /* Экстренная остановка.
    */
    stdcommand_register(&c->cmds, stepCMD_stop, "action/stop", PARAMT_none);

    /* Остановка с торможением.
    */
    stdcommand_register(&c->cmds, stepCMD_break, "action/break", PARAMT_none);

    c->homingSensorState=-1;
    /* Состояние датчика нуля 0-1
    */
    stdcommand_register(&c->cmds, stepCMD_setHomingSensor, "action/setHomingSensor", PARAMT_int);

    /* Аппаратный лимит хода вверх (up/плюс) 0-1 - 1 запрещает движение в плюс
    */
    stdcommand_register(&c->cmds, stepCMD_setUpLimit, "action/setUpLimit", PARAMT_int);

    /* Аппаратный лимит хода вниз (down/минус) 0-1 - 1 запрещает движение в минус
    */
    stdcommand_register(&c->cmds, stepCMD_setDownLimit, "action/setDownLimit", PARAMT_int);


    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}


#define HOMING_WAITING 1
#define HOMING_TO_SENSOR 2
#define HOMING_OUT_SENSOR 3

// Отложенная очередь: команды движения, пришедшие во время базирования, копим
// здесь и проигрываем по завершению процедуры (все они несут один int-аргумент).
#define STEPPER_DEFERRED_MAX 8
typedef struct { int cmd; int32_t arg; } stepper_deferred_t;

// Команды движения-параметров, которые можно отложить и проиграть помощником.
static inline int stepper_is_motion_cmd(int cmd){
    return cmd==stepCMD_moveToAbs || cmd==stepCMD_moveToInc ||
           cmd==stepCMD_runSpeed  || cmd==stepCMD_setMaxSpeed ||
           cmd==stepCMD_setAccel;
}

/* Вернуть пину DIR уровень, соответствующий логическому направлению мотора.
   Обязателен при выходе из анабиоза: stepper_checkDir трогает пин только в
   момент СМЕНЫ stepper->dir, а моргание уже увело физический уровень - без
   восстановления первый же ход после повторного goHome мог пойти не туда. */
static void stepper_restoreDirPin(stepper_t *stepper){
    gpio_set_level(stepper->dirPin, stepper->dir==DIR_UP ? !stepper->dirInverse : stepper->dirInverse);
}

/* Исполнение команд движения-параметров. Вынесено отдельно, чтобы одинаково
   выполнять их и в реальном времени, и при проигрывании отложенной очереди
   после базирования. arg - уже разобранное целое (atoi). */
static void stepper_exec_motion(PSTEPPERCONFIG c, stepper_t *stepper, int cmd, int32_t arg, int slot_num){
    int32_t target;
    switch(cmd){
        case stepCMD_moveToInc:
            c->state = RUN_POS;
            stepper->runSpeedFlag = 0;
            stepper->maxSpeed = c->maxSpeed;             // восстанавливаем cap позиционирования (runSpeed мог его испортить)
            target = stepper->currentPos + arg;
            if(!c->circularCounterFlag){                 // в круговом режиме ход не ограничиваем
                if(target > c->maxVal) target = c->maxVal;
                if(target < c->minVal) target = c->minVal;
            }
            // аппаратные лимиты: не назначаем цель в запрещённом направлении
            if(c->upLimitState   && target > stepper->currentPos) target = stepper->currentPos;
            if(c->downLimitState && target < stepper->currentPos) target = stepper->currentPos;
            stepper_moveTo(stepper, target);
            ESP_LOGD(TAG, "[stepper_%d] moveTo:%ld", slot_num, target);
            break;

        case stepCMD_moveToAbs:
            c->state = RUN_POS;
            stepper->runSpeedFlag = 0;
            stepper->maxSpeed = c->maxSpeed;             // восстанавливаем cap позиционирования
            target = arg;
            if(c->circularCounterFlag && (c->maxVal > c->minVal)){
                // круговой режим: кратчайший путь к цели с учетом заворота диапазона
                int64_t range = (int64_t)c->maxVal - (int64_t)c->minVal;
                int64_t cur   = stepper->currentPos;
                int64_t fwd   = stepper_wrapmod((int64_t)target - cur, range); // путь вперед [0,range)
                int64_t delta = (fwd <= range - fwd) ? fwd : (fwd - range);    // короче вперед или назад
                target = (int32_t)(cur + delta);
            }else{
                if(target > c->maxVal) target = c->maxVal;   // ограничение хода
                if(target < c->minVal) target = c->minVal;
            }
            // аппаратные лимиты: не назначаем цель в запрещённом направлении
            if(c->upLimitState   && target > stepper->currentPos) target = stepper->currentPos;
            if(c->downLimitState && target < stepper->currentPos) target = stepper->currentPos;
            stepper_moveTo(stepper, target);
            ESP_LOGD(TAG, "[stepper_%d] moveTo:%ld", slot_num, target);
            break;

        case stepCMD_runSpeed:
            // аппаратный лимит блокирует старт в запрещённую сторону
            if((c->upLimitState && arg > 0) || (c->downLimitState && arg < 0)){
                ESP_LOGD(TAG, "[stepper_%d] runSpeed %ld blocked by hw limit", slot_num, (long)arg);
                break;
            }
            c->state = RUN_SPEED;
            stepper->runSpeedFlag = 1;
            /* maxSpeed - это МОДУЛЬ скорости (setMaxSpeed рядом не зря делает abs).
               Раньше сюда клали знаковый arg, и отрицательная maxSpeed уводила
               targetSpeed-currentSpeed в минус. При реверсе currentSpeed тогда мог
               ПЕРЕСКОЧИТЬ ноль (шаг рампы не делит скорость нацело), а checkDir
               меняет DIR строго по currentSpeed==0 - смена направления не
               срабатывала вовсе, мотор продолжал получать step в старую сторону.
               Знак теперь задаёт только цель хода. */
            stepper->maxSpeed = abs(arg);
            stepper_moveTo(stepper, (arg > 0) ? (INT32_MAX-1) : (INT32_MIN+1));
            ESP_LOGD(TAG, "[stepper_%d] runSpeed:%ld dir:%s", slot_num, (long)arg, (arg > 0) ? "up" : "down");
            break;

        case stepCMD_setMaxSpeed:
            // Меняем постоянный cap скорости позиционирования (magnitude).
            c->maxSpeed = abs(arg);
            stepper->maxSpeed = c->maxSpeed;
            ESP_LOGD(TAG, "[stepper_%d] set maxSpeed:%ld", slot_num, stepper->maxSpeed);
            break;

        case stepCMD_setAccel:
            stepper->accel = arg;
            ESP_LOGD(TAG, "[stepper_%d] set accel:%ld", slot_num, stepper->accel);
            break;
    }
}

void stepper_task(void *arg){
    PSTEPPERCONFIG c = calloc(1, sizeof(STEPPERCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };

    int slot_num = (int)(intptr_t)arg;
	
    configure_stepper(c, slot_num);
    c->homingSensorState = -1;

    stepper_t stepper = STEPPER_DEFAULT();
    stepper.dirPin = SLOTS_PIN_MAP[slot_num][0];
    stepper.stepPin = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    
    stepper.accel = c->accel;
    stepper.maxSpeed = c->maxSpeed;
    stepper.minSpeed = c->minSpeed;

	esp_err_t step_err = stepper_init(&stepper, stepper.stepPin, stepper.dirPin, c->pulseWidth);
	if (step_err != ESP_OK) {
		ESP_LOGW(TAG, "PCNT unit limit reached (slot:%d), task terminated. err:%d",
		         slot_num, step_err);
		vTaskDelete(NULL);
	}

    esp_rom_gpio_pad_select_gpio(SLOTS_PIN_MAP[slot_num][2]);
    gpio_set_direction(SLOTS_PIN_MAP[slot_num][2], GPIO_MODE_OUTPUT);
    gpio_set_level(SLOTS_PIN_MAP[slot_num][2], 1);// для модулей out_2ch пин nsleep
    
    int32_t prevPos=0;
    int32_t prevSpeed=0;
    int prevState=-1;

    int homingProcedureState = HOMING_WAITING;
    TickType_t homingStartTick = 0;

    // Очередь команд, отложенных на время базирования (FIFO).
    stepper_deferred_t deferred[STEPPER_DEFERRED_MAX];
    int deferredCount = 0;

    // Моргание DIR в анабиозе. anabiosisBlink=1 означает, что пин DIR сейчас под
    // управлением индикации, а не мотора, и его надо восстановить при выходе.
    int anabiosisBlink = 0;
    int anabiosisDirLevel = 0;
    TickType_t anabiosisBlinkTick = 0;

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, c->active_state);

    if(c->homingDir==0){
        stdreport_s(c->homeReport, "disable");
    }else{
        if(c->goHomeOnStart){
            c->state=GOING_HOME;
        }else{
            stdreport_s(c->homeReport, "waitingCommand");
        }
    }

    // При старте публикуем текущее состояние включённых рапортов (retain не
    // используется): позиция, скорость, run-stop. На старте pos=0, speed=0, stop.
    if(c->posReportFlag){
        char str[15];
        sprintf(str, "%ld", stepper.absPos);
        stdreport_s(c->posReport, str);
        prevPos = stepper.absPos;
    }
    if(c->speedReportFlag){
        char str[15];
        sprintf(str, "%ld", stepper.currentSpeed);
        stdreport_s(c->speedReport, str);
        prevSpeed = stepper.currentSpeed;
    }
    if(c->stateReportFlag){
        stdreport_s(c->stateReport, stepper.state==RUN ? "run" : "stop");
        prevState = (stepper.state==RUN) ? 1 : 0;
    }


    TickType_t lastWakeTime = xTaskGetTickCount();

    while(1){

        int cmd = stdcommand_receive(&c->cmds, &params, 5);
		char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";

        /* Если модуль выключен - принимаем только action/enable, все команды
           движения игнорируем (Конституция §6). */
        if (!c->active_state && cmd != -1 && cmd != STDCMD_ENABLE) {
            ESP_LOGD(TAG, "[stepper_%d] disabled, ignoring cmd:%d", slot_num, cmd);
            cmd = -1;
        }

        /* Анабиоз: дом не найден за отведённое время. Не реагируем ни на что,
           кроме повторной команды базирования. Исключения - enable (выключение
           модуля по Конституции §6 должно проходить всегда) и обновления
           состояний датчика нуля и концевиков: это не команды, а телеметрия, они
           не двигают мотор, но без них повторный goHome стартовал бы по устаревшей
           картине датчиков. */
        if (c->state==HOMING_FAILED && cmd != -1 &&
            cmd != stepCMD_goHome && cmd != STDCMD_ENABLE &&
            cmd != stepCMD_setHomingSensor &&
            cmd != stepCMD_setUpLimit && cmd != stepCMD_setDownLimit) {
            ESP_LOGD(TAG, "[stepper_%d] anabiosis, ignoring cmd:%d", slot_num, cmd);
            cmd = -1;
        }

        /* Классификация: команды движения-параметров исполняет помощник, и их
           можно откладывать. Управляющие (enable, goHome, stop, break,
           setHomingSensor) исполняются немедленно. */
        int isMotion = stepper_is_motion_cmd(cmd);

        /* Во время базирования команды движения сразу не исполняем, а копим в
           очередь и проигрываем по завершению процедуры. setHomingSensor, break,
           stop, goHome и enable проходят немедленно - через switch ниже. */
        if (c->state==GOING_HOME && isMotion) {
            if (deferredCount < STEPPER_DEFERRED_MAX) {
                deferred[deferredCount].cmd = cmd;
                deferred[deferredCount].arg = atoi(cmd_arg);
                deferredCount++;
                ESP_LOGD(TAG, "[stepper_%d] homing, deferring cmd:%d (queued:%d)", slot_num, cmd, deferredCount);
            } else {
                ESP_LOGW(TAG, "[stepper_%d] deferred queue full, dropping cmd:%d", slot_num, cmd);
            }
            cmd = -1;
            isMotion = 0;
        }

        /* Ось не базирована (старт без goHomeOnStart или отмена базирования
           командой stop-break-enable): команды движения игнорируем - ехать по координатам
           на ненайденном нуле нельзя. НЕ откладываем: процедуры нет, копить не для
           чего. Ждём новую goHome; управляющие команды (goHome, stop, break,
           setHomingSensor, лимиты, enable) проходят через switch. */
        if (c->state==NOT_HOMED && isMotion) {
            ESP_LOGD(TAG, "[stepper_%d] not homed, ignoring cmd:%d", slot_num, cmd);
            cmd = -1;
            isMotion = 0;
        }

        if (isMotion) {
            stepper_exec_motion(c, &stepper, cmd, atoi(cmd_arg), slot_num);
        } else {
            switch (cmd){
                case -1: // none
                    break;

                case STDCMD_ENABLE:
                    if (params.count > 0) {
                        int new_state = params.p[0].i ? 1 : 0;
                        if (new_state != c->active_state) {
                            c->active_state = new_state;
                            ESP_LOGD(TAG, "[stepper_%d] enable:%d", slot_num, c->active_state);
                            stdreport_enable(slot_num, c->active_state);
                            if (!c->active_state) {
                                /* Экстренная остановка при выключении (Конституция §6) */
                                stepper_stop(&stepper);
                                /* Если шло базирование - корректно отменяем, иначе автомат
                                   замрёт в GOING_HOME и после enable 1 не доедет. */
                                if (c->state==GOING_HOME) {
                                    c->state = NOT_HOMED;
                                    homingProcedureState = HOMING_WAITING;
                                    deferredCount = 0;
                                    stdreport_s(c->homeReport, "waitingCommand");
                                }
                            }
                        }
                    }
                    break;

                case stepCMD_goHome:
                    homingProcedureState=HOMING_WAITING;
                    c->state = GOING_HOME;
                    ESP_LOGD(TAG, "[stepper_%d] lets go home", slot_num);
                    break;

                case stepCMD_stop:
                    stepper_stop(&stepper);
                    ESP_LOGD(TAG, "[stepper_%d] STOP", slot_num);
                    if(c->state==GOING_HOME){
                        // базирование прервано вручную - отложенная очередь неактуальна
                        c->state=NOT_HOMED;
                        homingProcedureState=HOMING_WAITING;
                        deferredCount=0;
                        stdreport_s(c->homeReport, "waitingCommand");
                    }
                    break;

                case stepCMD_break:
                    stepper_break(&stepper);
                    ESP_LOGD(TAG, "[stepper_%d] break", slot_num);
                    if(c->state==GOING_HOME){
                        c->state=NOT_HOMED;
                        homingProcedureState=HOMING_WAITING;
                        deferredCount=0;
                        stdreport_s(c->homeReport, "waitingCommand");
                    }
                    break;

                case stepCMD_setHomingSensor:
                    c->homingSensorState = atoi(cmd_arg);
                    ESP_LOGD(TAG, "[stepper_%d] setHomingSensor:%d", slot_num, c->homingSensorState);
                    break;

                case stepCMD_setUpLimit:
                    // Обновляем состояние концевика всегда; enforcement - в рабочем цикле.
                    c->upLimitState = atoi(cmd_arg) ? 1 : 0;
                    ESP_LOGD(TAG, "[stepper_%d] setUpLimit:%d", slot_num, c->upLimitState);
                    break;

                case stepCMD_setDownLimit:
                    c->downLimitState = atoi(cmd_arg) ? 1 : 0;
                    ESP_LOGD(TAG, "[stepper_%d] setDownLimit:%d", slot_num, c->downLimitState);
                    break;
            }
        }

        /* --- Индикация анабиоза ---
           Мотор стоит, на step тишина, поэтому единственный способ показать
           наружу режим ожидания - моргать светодиодом DIR. Блок стоит ДО обработки
           GOING_HOME: в тот же тик, когда пришёл повторный goHome, состояние уже
           не HOMING_FAILED, и восстановление уровня DIR отработает раньше, чем
           базирование дёрнет мотор. Пин DIR - вход level для PCNT, но импульсов
           step нет, так что счётчик от моргания не едет. */
        if(c->active_state && c->state==HOMING_FAILED){
            if(!anabiosisBlink){
                anabiosisBlink = 1;
                anabiosisDirLevel = 0;
                anabiosisBlinkTick = xTaskGetTickCount();
                gpio_set_level(stepper.dirPin, anabiosisDirLevel);
            }else if((xTaskGetTickCount()-anabiosisBlinkTick) >= pdMS_TO_TICKS(ANABIOSIS_BLINK_MS)){
                anabiosisBlinkTick = xTaskGetTickCount();
                anabiosisDirLevel = !anabiosisDirLevel;
                gpio_set_level(stepper.dirPin, anabiosisDirLevel);
            }
        }else if(anabiosisBlink){
            anabiosisBlink = 0;
            stepper_restoreDirPin(&stepper);
        }

        if(c->active_state && c->state==GOING_HOME){
            if(homingProcedureState==HOMING_WAITING){
                stdreport_s(c->homeReport, "homing");
                homingStartTick = xTaskGetTickCount();
                stepper.maxSpeed = c->homingSpeed;
                stepper.accel = stepper.maxSpeed*2;

                if(c->homingSensorState==1){
                    homingProcedureState = HOMING_OUT_SENSOR;
                    stepper_moveTo(&stepper,(c->homingDir == UP) ? INT32_MIN : INT32_MAX);
                    ESP_LOGD(TAG, "[stepper_%d] homing out of sensor", slot_num);
                }else{
                    homingProcedureState = HOMING_TO_SENSOR;
                    stepper_moveTo(&stepper, (c->homingDir == UP) ? INT32_MAX : INT32_MIN);
                    ESP_LOGD(TAG, "[stepper_%d] homing to sensor", slot_num);
                }
            }else if(c->homingTimeout>0 &&
                     (xTaskGetTickCount()-homingStartTick) > pdMS_TO_TICKS(c->homingTimeout*1000)){
                // Датчик не найден за отведённое время - уходим в анабиоз: step
                // молчит (stepper_stop гасит mcpwm), любые команды кроме goHome
                // игнорируются, DIR моргает как индикация. Отложенные команды
                // сбрасываем: ехать по накопленным координатам на небазированной оси нельзя.
                stepper_stop(&stepper);
                stepper.maxSpeed = c->maxSpeed;
                stepper.accel = c->accel;
                c->state = HOMING_FAILED;
                homingProcedureState = HOMING_WAITING;
                deferredCount = 0;
                stdreport_s(c->homeReport, "homingTimeout");
                ESP_LOGW(TAG, "[stepper_%d] homing timeout, anabiosis - waiting for goHome", slot_num);
            }else if(homingProcedureState == HOMING_OUT_SENSOR){
                if(c->homingSensorState==0){
                    ESP_LOGD(TAG, "[stepper_%d] sensor reseted, homing again", slot_num);
                    vTaskDelay(500);
                    homingProcedureState = HOMING_TO_SENSOR; 
                    stepper_moveTo(&stepper, (c->homingDir == UP) ? INT32_MAX : INT32_MIN);
                }
            }else if(homingProcedureState == HOMING_TO_SENSOR){
                if(c->homingSensorState==1){
                    stepper_setZero(&stepper);
                    /* Рабочие maxSpeed-accel восстанавливаем ДО торможения:
                       stepper_break строит трапецию по текущему accel, а подмена
                       ускорения сразу ПОСЛЕ него делала торможение резче плана -
                       мотор промахивался мимо цели и уходил в автоколебания. */
                    stepper.maxSpeed = c->maxSpeed;
                    stepper.accel = c->accel;
                    stepper_break(&stepper);
                    //stepper_stop(&stepper);
                    c->state=IDLE;
				    stdreport_s(c->homeReport, "done");
                    // проигрываем команды, накопленные во время базирования (FIFO)
                    for(int k=0; k<deferredCount; k++){
                        stepper_exec_motion(c, &stepper, deferred[k].cmd, deferred[k].arg, slot_num);
                    }
                    if(deferredCount){
                        ESP_LOGD(TAG, "[stepper_%d] replayed %d deferred cmd(s)", slot_num, deferredCount);
                        deferredCount = 0;
                    }
                }
            }
        }

        /* Позицию читаем ДО расчёта профиля. Раньше getCurrentPos стоял ПОСЛЕ
           speedUpdate, и все решения (приехали-ли, пора-ли тормозить, не нужен-ли
           разворот) принимались по координате, устаревшей на целый тик - при
           10000 шаг-с и 20 мс это 200 шагов слепоты при допуске парковки в 2 шага. */
        stepper_getCurrentPos(&stepper);
        //ESP_LOGD(TAG, "currentPos: %ld prevPos:%ld dir:%d", stepper.currentPos,  stepper.pcnt_prevPos,  stepper.dir);

        /* В анабиозе профиль скорости не считаем вовсе: единственный владелец
           таймера mcpwm на ходу - speedUpdate, и пока его нет, генерация step
           гарантированно не возобновится. */
        if (c->active_state && c->state!=HOMING_FAILED) {
            stepper_speedUpdate(&stepper, c->refreshPeriod);
        }

        // --- Ограничение хода по minVal-maxVal и индикация границ ---
        // Работает и в позиционном режиме, и в режиме скорости (runSpeed).
        // absPos - истинная позиция, переживающая хак runSpeed. В круговом режиме
        // границы отключены. По умолчанию maxVal-INT32_MAX, minVal-INT32_MIN,
        // поэтому без настройки границы никогда не срабатывают.
        // В базировании границы НЕ действуют: координаты ещё не валидны (ноль не
        // найден), а ход к датчику идёт до INT32_MAX-MIN - иначе бэкстоп остановил
        // бы мотор посреди процедуры. Индикацию границ там тоже не даём: в state
        // во время поиска нуля должен идти обычный run-stop.
        int atMaxVal = 0, atMinVal = 0;
        if(!c->circularCounterFlag && c->state!=GOING_HOME){
            // Тормозной путь на текущей скорости: v^2/(2a). Считаем в int64 -
            // при большом maxSpeed квадрат скорости и 2*accel переполняют int32.
            int64_t brakeWay = ((int64_t)stepper.currentSpeed * (int64_t)stepper.currentSpeed)
                               / (2 * (int64_t)(stepper.accel > 0 ? stepper.accel : 1));

            // Предиктивное торможение в режиме скорости: начинаем тормозить заранее,
            // чтобы плавно (по трапеции) встать точно на границе. Перед moveTo снимаем
            // runSpeedFlag, иначе хак сброса currentPos не даст торможению завершиться.
            // Цель задаём в координатах currentPos: (maxVal - absPos) - остаток хода.
            if(c->active_state && c->state==RUN_SPEED && stepper.state==RUN){
                if(stepper.dir==DIR_UP && stepper.absPos < c->maxVal &&
                   (int64_t)stepper.absPos + brakeWay >= (int64_t)c->maxVal){
                    stepper.runSpeedFlag = 0;
                    stepper_moveTo(&stepper, stepper.currentPos + (c->maxVal - stepper.absPos));
                    c->state = RUN_POS;
                    ESP_LOGD(TAG, "[stepper_%d] braking to maxVal:%ld", slot_num, c->maxVal);
                }else if(stepper.dir==DIR_DOWN && stepper.absPos > c->minVal &&
                         (int64_t)stepper.absPos - brakeWay <= (int64_t)c->minVal){
                    stepper.runSpeedFlag = 0;
                    stepper_moveTo(&stepper, stepper.currentPos + (c->minVal - stepper.absPos));
                    c->state = RUN_POS;
                    ESP_LOGD(TAG, "[stepper_%d] braking to minVal:%ld", slot_num, c->minVal);
                }
            }

            // Индикация границ + жёсткий backstop: если позицию всё же вынесло за
            // границу (например runSpeed без запаса на торможение) - стоп немедленно.
            if(stepper.absPos >= c->maxVal){
                // не индицируем границу, если уже уезжаем от неё
                if(!(stepper.state==RUN && stepper.dir==DIR_DOWN)) atMaxVal = 1;
                if(c->active_state && stepper.state==RUN && stepper.dir==DIR_UP){
                    stepper_stop(&stepper);
                    if(c->state==RUN_SPEED) c->state=IDLE;
                    ESP_LOGD(TAG, "[stepper_%d] maxVal:%ld backstop", slot_num, c->maxVal);
                }
            }else if(stepper.absPos <= c->minVal){
                if(!(stepper.state==RUN && stepper.dir==DIR_UP)) atMinVal = 1;
                if(c->active_state && stepper.state==RUN && stepper.dir==DIR_DOWN){
                    stepper_stop(&stepper);
                    if(c->state==RUN_SPEED) c->state=IDLE;
                    ESP_LOGD(TAG, "[stepper_%d] minVal:%ld backstop", slot_num, c->minVal);
                }
            }
        }

        // --- Аппаратные лимиты (концевики) + индикация ---
        // upLimit запрещает движение up/плюс, downLimit - down/минус. Из-под лимита
        // всегда можно уехать в обратную сторону. В базировании лимиты игнорируем.
        int atUpLimit = 0, atDownLimit = 0;
        if(c->state!=GOING_HOME){
            atUpLimit   = c->upLimitState   ? 1 : 0;
            atDownLimit = c->downLimitState ? 1 : 0;
            if(c->active_state && stepper.state==RUN){
                if(c->upLimitState && stepper.dir==DIR_UP){
                    stepper_stop(&stepper);
                    if(c->state==RUN_SPEED) c->state=IDLE;
                    ESP_LOGD(TAG, "[stepper_%d] upLimit hit, stop", slot_num);
                }else if(c->downLimitState && stepper.dir==DIR_DOWN){
                    stepper_stop(&stepper);
                    if(c->state==RUN_SPEED) c->state=IDLE;
                    ESP_LOGD(TAG, "[stepper_%d] downLimit hit, stop", slot_num);
                }
            }
        }

        if(c->posReportFlag){
            // absPos - истинная позиция (переживает хак runSpeed); в круговом режиме заворачиваем
            int32_t reportPos = stepper.absPos;
            if(c->circularCounterFlag && (c->maxVal > c->minVal)){
                int64_t range = (int64_t)c->maxVal - (int64_t)c->minVal;
                reportPos = (int32_t)((int64_t)c->minVal + stepper_wrapmod((int64_t)stepper.absPos - c->minVal, range));
            }
            if(reportPos!=prevPos){
                char str[15];
                sprintf(str, "%ld", reportPos);
				stdreport_s(c->posReport, str);
                //ESP_LOGD(TAG, "Stepper_%d curentPos:%s", slot_num, str);
                prevPos=reportPos;
            }
        }

        if(c->speedReportFlag){
            if(prevSpeed!=stepper.currentSpeed){
                char str[15];
                sprintf(str, "%ld", stepper.currentSpeed);
                stdreport_s(c->speedReport, str);
                //report(str,slot_num);
                //ESP_LOGD(TAG, "Stepper_%d curentSpeed:%s", slot_num, str);
                prevSpeed=stepper.currentSpeed;
            }
        }

        if(c->stateReportFlag){
            // 0-stop 1-run 2-maxVal 3-minVal 4-upLimit 5-downLimit.
            // Приоритет: аппаратный лимит > программная граница > run-stop.
            int reportState = (stepper.state==RUN) ? 1 : 0;
            if(atMaxVal) reportState = 2;
            else if(atMinVal) reportState = 3;
            if(atUpLimit) reportState = 4;
            else if(atDownLimit) reportState = 5;
            if(prevState!=reportState){
                const char * stateStr = "stop";
                if(reportState==1) stateStr = "run";
                else if(reportState==2) stateStr = "maxVal";
                else if(reportState==3) stateStr = "minVal";
                else if(reportState==4) stateStr = "upLimit";
                else if(reportState==5) stateStr = "downLimit";
                stdreport_s(c->stateReport, stateStr);
                prevState=reportState;
            }
        }

        vTaskDelayUntil(&lastWakeTime, c->refreshPeriod);
    }

}



void start_stepper_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	char tmpString[60];
	sprintf(tmpString, "task_stepper_%d", slot_num);
	//xTaskCreate(stepper_task, tmpString, 1024*12, (void*)(intptr_t)slot_num,configMAX_PRIORITIES, NULL);
    xTaskCreatePinnedToCore(stepper_task, tmpString, 1024*12, (void*)(intptr_t)slot_num,configMAX_PRIORITIES-6, NULL,1);

	ESP_LOGD(TAG,"stepper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_stepper()
{
	return manifesto;
}