/*
 * adc.c -- see adc.h. ADC1 through its registers (as hrtim.c / dac.c); the HAL ADC
 * driver is not part of this project.
 *
 * Clock: the synchronous AHB clock divided by 4 (ADC12_COMMON CCR.CKMODE = 3), 42.5 MHz
 * at the 170 MHz core clock, below the converter's 60 MHz limit; no RCC kernel-clock
 * selection is needed that way. Sampling time 247.5 clocks (5.8 us) on every channel
 * used. Oversampling 16x with a 4-bit right shift keeps the result 12 bits.
 */
#include "adc.h"
#include "main.h"

#define ADC_SPIN_LIMIT  (200000U)     /* bounds every wait below; a conversion takes ~100 us */

/* The factory VREFINT calibration: the 12-bit code of the internal reference with VREF+ = 3.0 V, 30 degC
   (RM0440 / datasheet "Internal voltage reference"). */
#define VREFINT_CAL_ADDR  ((const volatile uint16_t *)0x1FFF75AAUL)
#define VREFINT_CHANNEL   (18U)

static const uint8_t kChannel[ADC_NUM_CHANNELS] = { 1U, 2U, 6U, 15U };   /* ADC1_INx for AnalogIn_01..04 */

static uint8_t g_ready = 0U;

static uint8_t WaitClear(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0U; i < ADC_SPIN_LIMIT; i++)
    {
        if ((*reg & mask) == 0U) { return 1U; }
    }
    return 0U;
}

static uint8_t WaitSet(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0U; i < ADC_SPIN_LIMIT; i++)
    {
        if ((*reg & mask) != 0U) { return 1U; }
    }
    return 0U;
}

void Adc_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    g_ready = 0U;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_Init(GPIOC, &gpio);

    __HAL_RCC_ADC12_CLK_ENABLE();
    ADC12_COMMON->CCR = ADC_CCR_CKMODE | ADC_CCR_VREFEN;   /* HCLK / 4; the VREFINT path on */

    /* Voltage regulator up (leave deep power-down first), then calibrate single-ended. */
    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC1->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1);                                           /* t_ADCVREG_STUP is 20 us */
    ADC1->CR &= ~ADC_CR_ADCALDIF;
    ADC1->CR |= ADC_CR_ADCAL;
    if (WaitClear(&ADC1->CR, ADC_CR_ADCAL) == 0U)
    {
        return;
    }

    ADC1->CFGR2 = ADC_CFGR2_ROVSE | (3U << ADC_CFGR2_OVSR_Pos) | (4U << ADC_CFGR2_OVSS_Pos);   /* 16x, shift 4 */
    ADC1->SMPR1 = (6U << ADC_SMPR1_SMP1_Pos) | (6U << ADC_SMPR1_SMP2_Pos) | (6U << ADC_SMPR1_SMP6_Pos);
    ADC1->SMPR2 = (6U << ADC_SMPR2_SMP15_Pos) | (6U << ADC_SMPR2_SMP18_Pos);

    ADC1->ISR = ADC_ISR_ADRDY;                              /* clear, then enable and wait for ready */
    ADC1->CR |= ADC_CR_ADEN;
    if (WaitSet(&ADC1->ISR, ADC_ISR_ADRDY) == 0U)
    {
        return;
    }
    g_ready = 1U;
}

uint8_t Adc_IsReady(void)
{
    return g_ready;
}

/* One averaged conversion of ADC1 channel `chan`. */
static uint8_t Convert(uint32_t chan, uint16_t *code)
{
    if (g_ready == 0U)
    {
        return 0U;
    }
    ADC1->SQR1 = chan << ADC_SQR1_SQ1_Pos;                  /* a sequence of one conversion */
    ADC1->ISR = ADC_ISR_EOC | ADC_ISR_EOS;
    ADC1->CR |= ADC_CR_ADSTART;
    if (WaitSet(&ADC1->ISR, ADC_ISR_EOC) == 0U)
    {
        return 0U;
    }
    *code = (uint16_t)(ADC1->DR & 0xFFFU);                  /* reading DR clears EOC */
    return 1U;
}

uint8_t Adc_Read(uint8_t channel, uint16_t *code)
{
    if (channel >= ADC_NUM_CHANNELS)
    {
        return 0U;
    }
    return Convert(kChannel[channel], code);
}

uint8_t Adc_ReadVrefint(uint16_t *code, uint16_t *calCode)
{
    if (Convert(VREFINT_CHANNEL, code) == 0U)
    {
        return 0U;
    }
    *calCode = *VREFINT_CAL_ADDR;
    return 1U;
}
