#include "Heat.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>


/* ── NTC 参数（按实际模块修改）─── */
#define NTC_R_FIXED   10000.0f   /* 固定电阻 10kΩ */
#define NTC_R0        10000.0f   /* NTC 在 25°C 的阻值 10kΩ */
#define NTC_B         3950.0f    /* B 值 */
#define NTC_T0        298.15f    /* 25°C 对应的开尔文温度 */
#define ADC_VREF      3.3f       /* ADC 参考电压 */
#define ADC_MAX       4095.0f    /* 12-bit ADC 最大值 */


#define HEAT_ADC_UNIT       ADC_UNIT_1          /* ADC1 */
#define HEAT_ADC_CHANNEL    ADC_CHANNEL_1       /* GPIO2 → ADC1_CH1 */
#define HEAT_ADC_ATTEN      ADC_ATTEN_DB_12     /* 0~3.3V 量程 */

static adc_oneshot_unit_handle_t Heat_ADC_Handle;
static uint32_t                  Heat_Value;
static SemaphoreHandle_t         Heat_Mutex;


/**
 * @brief       热敏传感器采样任务
 * @param       pvParameters: 任务参数（未使用）
 * @retval      无
 */
static void Heat_Task(void *pvParameters)
{
    int adc_raw = 0;

    while (1)
    {
        adc_oneshot_read(Heat_ADC_Handle, HEAT_ADC_CHANNEL, &adc_raw);

        xSemaphoreTake(Heat_Mutex, portMAX_DELAY);
        Heat_Value = (uint32_t)adc_raw;
        xSemaphoreGive(Heat_Mutex);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief       初始化热敏传感器 — 配置 ADC + 创建采样任务
 * @param       无
 * @retval      无
 */
void Heat_Init(void)
{
    /* 配置 ADC Oneshot 单元 */
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = HEAT_ADC_UNIT,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&adc_cfg, &Heat_ADC_Handle);

    /* 配置 ADC 通道 */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = HEAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(Heat_ADC_Handle, HEAT_ADC_CHANNEL, &chan_cfg);

    /* 创建互斥锁 */
    Heat_Mutex = xSemaphoreCreateMutex();
    configASSERT(Heat_Mutex != NULL);

    /* 创建采样任务 */
    xTaskCreate(Heat_Task, "HeatTsk", 2048, NULL, 1, NULL);
}

/**
 * @brief       读取最新的 ADC 采样值（非阻塞）
 * @param       无
 * @retval      ADC 原始值 (0~4095)
 */
uint32_t Heat_GetValue(void)
{
    uint32_t Temp = 0;

    xSemaphoreTake(Heat_Mutex, portMAX_DELAY);
    Temp = Heat_Value;
    xSemaphoreGive(Heat_Mutex);

    return Temp;
}

/**
 * @brief       读取温度（摄氏度）
 * @param       无
 * @retval      温度值 (°C)
 * @note        电路: 3.3V → NTC → ADC ← 固定电阻 → GND
 */
float Heat_GetTemperature(void)
{
    uint32_t adc = Heat_GetValue();
    float v = (float)adc / ADC_MAX * ADC_VREF;                 /* ADC → 电压 */
    float r = NTC_R_FIXED * (ADC_VREF / v - 1.0f);            /* 电压 → NTC 阻值 */
    float t_k = 1.0f / (1.0f / NTC_T0 + logf(r / NTC_R0) / NTC_B); /* 阻值 → 开尔文 */
    return t_k - 273.15f;                                      /* 开尔文 → 摄氏度 */
}
