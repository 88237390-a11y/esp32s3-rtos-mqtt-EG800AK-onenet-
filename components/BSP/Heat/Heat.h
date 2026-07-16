#ifndef __HEAT_H
#define __HEAT_H

#include <stdint.h>

void     Heat_Init(void);
uint32_t Heat_GetValue(void);       /* 原始 ADC 值 (0~4095) */
float    Heat_GetTemperature(void); /* 温度 (°C) */

#endif
