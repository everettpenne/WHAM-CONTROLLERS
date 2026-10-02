/*
 * adc_hw.h -- BSP-private part of the ADC driver: the DMA handle stm32g4xx_it.c's
 * DMA2_Channel1_IRQHandler() needs. Public interface: src/drivers/adc.h.
 */
#ifndef ADC_HW_H
#define ADC_HW_H

#include "adc.h"
#include "stm32g4xx_hal.h"

extern DMA_HandleTypeDef hdmaAdc;

#endif /* ADC_HW_H */
