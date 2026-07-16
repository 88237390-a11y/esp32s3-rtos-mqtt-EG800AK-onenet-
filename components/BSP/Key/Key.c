#include "Key.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"


#define KEY_GPIO_PIN    GPIO_NUM_6      /* 按键引脚 */

static uint8_t     Key_Num;
static SemaphoreHandle_t Key_Mutex;


/**
 * @brief       按键扫描消抖（每 1ms 调用一次）
 * @param       无
 * @retval      无
 */
static void Key_Tick(void)
{
    static uint8_t Count;
    static uint8_t CurrState, PrevState;

    Count++;
    if (Count >= 20)
    {
        Count = 0;

        PrevState = CurrState;

        if (gpio_get_level(KEY_GPIO_PIN) == 0)
        {
            CurrState = 1;
        }
        else
        {
            CurrState = 0;
        }

        /* 松手检测：上次按下，当前松开 → 记录一次有效按键 */
        if (CurrState == 0 && PrevState != 0)
        {
            xSemaphoreTake(Key_Mutex, portMAX_DELAY);
            Key_Num = 1;
            xSemaphoreGive(Key_Mutex);
        }
    }
}

/**
 * @brief       按键扫描任务
 * @param       pvParameters: 任务参数（未使用）
 * @retval      无
 */
static void Key_Task(void *pvParameters)
{
    while (1)
    {
        Key_Tick();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * @brief       初始化按键 — 配置 GPIO + 创建扫描任务
 * @param       无
 * @retval      无
 */
void Key_Init(void)
{
    gpio_config_t gpio_init_struct = {0};
 
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_INPUT;                /* 输入模式 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 使能上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 失能下拉 */
    gpio_init_struct.pin_bit_mask = 1ULL << KEY_GPIO_PIN;   /* 按键引脚位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置 GPIO */

    Key_Mutex = xSemaphoreCreateMutex();                    /* 创建互斥锁 */
    configASSERT(Key_Mutex != NULL);

    xTaskCreate(Key_Task, "KeyTsk", 1024, NULL, 1, NULL);
}

/**
 * @brief       读取并清除按键标志（非阻塞）
 * @param       无
 * @retval      有按键返回 1，无按键返回 0
 */
uint8_t Key_GetNum(void)
{
    uint8_t Temp = 0;

    xSemaphoreTake(Key_Mutex, portMAX_DELAY);
    if (Key_Num)
    {
        Temp = Key_Num;
        Key_Num = 0;
    }
    xSemaphoreGive(Key_Mutex);

    return Temp;
}
