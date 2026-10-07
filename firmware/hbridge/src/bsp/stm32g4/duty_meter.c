/*
 * duty_meter.c -- see duty_meter.h. TIM2 through its registers (like hrtim.c / dac.c).
 *
 * PWM-input mode (RM0440, TIM "PWM input mode"): TI1 = PA15. IC1 captures TI1 rising into CCR1, IC2 captures
 * the SAME pin falling into CCR2, and TI1FP1 (rising) resets the counter in slave reset mode, so CCR1 is
 * the period and CCR2 the high time, both in timer clocks, TIM2 counting at its input clock with no prescaler.
 */
#include "duty_meter.h"
#include "main.h"

static uint8_t g_on = 0U;
static float g_duty = 0.0f;
static float g_freq = 0.0f;
static struct { uint32_t cr1, smcr, dier, ccmr1, ccer, arr, psc; } g_saved;

uint8_t DutyMeter_Start(void)
{
    GPIO_InitTypeDef gpio = {0};

    if (g_on != 0U)
    {
        return 1U;
    }
    __HAL_RCC_TIM2_CLK_ENABLE();
    if (TIM2->DIER != 0U)
    {
        return 0U;                      /* a PFM_Input capture has TIM2's interrupts or DMA requests on */
    }
    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(GPIOA, &gpio);

    g_saved.cr1 = TIM2->CR1;   g_saved.smcr = TIM2->SMCR;   g_saved.dier = TIM2->DIER;
    g_saved.ccmr1 = TIM2->CCMR1; g_saved.ccer = TIM2->CCER; g_saved.arr = TIM2->ARR;  g_saved.psc = TIM2->PSC;

    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->SMCR = 0U;
    TIM2->CCER = 0U;
    TIM2->PSC = 0U;
    TIM2->ARR = 0xFFFFFFFFU;
    TIM2->EGR = TIM_EGR_UG;             /* load the prescaler and reload value into their shadow registers */
    TIM2->CCMR1 = TIM_CCMR1_CC1S_0 | TIM_CCMR1_CC2S_1;      /* IC1 and IC2 both from TI1, no filter, no prescaler */
    TIM2->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC2P;   /* IC1 rising, IC2 falling */
    TIM2->SMCR = TIM_SMCR_TS_0 | TIM_SMCR_TS_2 | TIM_SMCR_SMS_2;  /* trigger TI1FP1, reset mode */
    TIM2->SR = 0U;
    TIM2->CR1 |= TIM_CR1_CEN;
    g_duty = 0.0f;
    g_freq = 0.0f;
    g_on = 1U;
    return 1U;
}

void DutyMeter_Stop(void)
{
    if (g_on == 0U)
    {
        return;
    }
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->SMCR = 0U;
    TIM2->PSC = g_saved.psc;
    TIM2->ARR = g_saved.arr;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CCMR1 = g_saved.ccmr1;
    TIM2->CCER = g_saved.ccer;
    TIM2->SMCR = g_saved.smcr;
    TIM2->SR = 0U;
    TIM2->DIER = g_saved.dier;
    TIM2->CR1 = g_saved.cr1;
    g_on = 0U;
}

uint8_t DutyMeter_Read(float *duty, float *freqHz)
{
    uint32_t sr;
    uint32_t period;
    uint32_t high;
    uint8_t fresh = 0U;

    if (g_on != 0U)
    {
        sr = TIM2->SR;
        period = TIM2->CCR1;            /* reading a capture register clears its flag */
        high = TIM2->CCR2;
        TIM2->SR = ~(TIM_SR_CC1OF | TIM_SR_CC2OF);
        if (((sr & TIM_SR_CC1IF) != 0U) && (period != 0U))
        {
            g_duty = (float)high / (float)period;
            if (g_duty > 1.0f) { g_duty = 1.0f; }
            g_freq = (float)SystemCoreClock / (float)period;
            fresh = 1U;
        }
    }
    *duty = g_duty;
    *freqHz = g_freq;
    return fresh;
}
