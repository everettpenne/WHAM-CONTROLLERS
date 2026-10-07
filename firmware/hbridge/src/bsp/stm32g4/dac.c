/*
 * dac.c -- see dac.h. DAC1 is driven through its registers (like hrtim.c); the
 * HAL DAC driver is not part of this project.
 *
 * DAC1 channel 1 -> PA4, channel 2 -> PA5, both "normal mode, connected to the
 * external pin, output buffer enabled" (MCR.MODEx = 0), software-written, no
 * trigger: a write to DHR12Rx reaches the output register one APB clock later.
 */
#include "dac.h"
#include "ctrlr_config.h"
#include "main.h"

static uint16_t g_code[DAC_NUM_CHANNELS];

void Dac_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin  = GPIO_PIN_4 | GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    __HAL_RCC_DAC1_CLK_ENABLE();
    DAC1->MCR = 0U;                       /* both channels: external pin, buffer on, normal mode */
    DAC1->DHR12R1 = 0U;
    DAC1->DHR12R2 = 0U;
    DAC1->CR = DAC_CR_EN1 | DAC_CR_EN2;   /* no triggers, no DMA */
    HAL_Delay(1);                         /* the converters need a few microseconds to wake up */

    g_code[0] = 0U;
    g_code[1] = 0U;
}

uint8_t Dac_SetMilliVolts(uint8_t channel, uint16_t milliVolts)
{
    uint32_t code;

    if ((channel >= DAC_NUM_CHANNELS) || (milliVolts > ANALOG_VREF_MV))
    {
        return 0U;
    }
    code = ((uint32_t)milliVolts * 4095U + (ANALOG_VREF_MV / 2U)) / ANALOG_VREF_MV;
    g_code[channel] = (uint16_t)code;
    if (channel == 0U)
    {
        DAC1->DHR12R1 = code;
    }
    else
    {
        DAC1->DHR12R2 = code;
    }
    return 1U;
}

uint16_t Dac_GetCode(uint8_t channel)
{
    return (channel < DAC_NUM_CHANNELS) ? g_code[channel] : 0U;
}

uint16_t Dac_GetOutputCode(uint8_t channel)
{
    if (channel >= DAC_NUM_CHANNELS)
    {
        return 0U;
    }
    return (uint16_t)((channel == 0U) ? DAC1->DOR1 : DAC1->DOR2);
}
