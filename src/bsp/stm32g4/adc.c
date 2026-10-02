/*
 * adc.c -- see adc.h. ADC1 through its registers (as hrtim.c / dac.c); the HAL ADC driver is
 * not part of this project. The DMA channel uses the HAL DMA driver.
 *
 * Clock: the synchronous AHB clock divided by 4 (ADC12_COMMON CCR.CKMODE = 3), 42.5 MHz at the
 * 170 MHz core clock, below the converter's 60 MHz limit. A conversion takes (sampling time +
 * 12.5) clocks.
 *
 * Trigger: TIM6's update event (TRGO) starts one scan of the 7-conversion sequence (the ADC's
 * regular external trigger 13); the 7 results are written by DMA2 channel 1 into a circular
 * buffer holding two blocks; the half and full transfer interrupts (priority 2) average the
 * block just completed and call the application.
 */
#include "adc.h"
#include "adc_hw.h"
#include "ctrlr_config.h"
#include "main.h"

#define ADC_SPIN_LIMIT      (200000U)
#define ADC_CLOCK_HZ        (42500000.0f)
#define ADC_EXTSEL_TIM6_TRGO (13U)               /* RM0440: ADC1/2 regular trigger EXT13 = TIM6_TRGO */
#define VREFINT_CAL_ADDR    ((const volatile uint16_t *)0x1FFF75AAUL)
#define VREFINT_SMP_CODE    (6U)                 /* 247.5 clocks: the bandgap needs >= 4 us */

/* ADC1 channel of each scan position: the six inputs in wire order, then VREFINT. */
static const uint8_t kChannel[ADC_SCAN_LEN] = { 1U, 2U, 6U, 15U, 7U, 12U, 18U };

/* The eight sampling times, in clocks x10, indexed by the SMPx code. */
static const uint16_t kSampleX10[8] = { 25U, 65U, 125U, 245U, 475U, 925U, 2475U, 6405U };

DMA_HandleTypeDef hdmaAdc;

static uint8_t  g_ready = 0U;
static uint8_t  g_running = 0U;
static uint8_t  g_smpCode = 4U;                 /* 47.5 clocks until Adc_Init() applies the config default */
static uint16_t g_block = 1U;                   /* scans per block */
static adc_block_handler_t g_handler = NULL;
static uint16_t g_dma[2U * ADC_MAX_BLOCK_SCANS * ADC_SCAN_LEN];
static float    g_mean[ADC_SCAN_LEN];
static volatile uint8_t  g_meanValid = 0U;
static volatile uint32_t g_blocks = 0U;

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

static uint8_t CodeForX10(uint16_t clocksX10)
{
    for (uint8_t i = 0U; i < 8U; i++)
    {
        if (kSampleX10[i] == clocksX10) { return (uint8_t)(i + 1U); }   /* +1: 0 is "not found" */
    }
    return 0U;
}

/* Averages the block starting at `base` and hands it to the application. Runs in the DMA interrupt. */
static void ProcessBlock(const uint16_t *base)
{
    uint32_t sum[ADC_SCAN_LEN] = { 0U };
    float inv = 1.0f / (float)g_block;

    for (uint16_t s = 0U; s < g_block; s++)
    {
        for (uint8_t k = 0U; k < ADC_SCAN_LEN; k++)
        {
            sum[k] += base[(uint32_t)s * ADC_SCAN_LEN + k];
        }
    }
    for (uint8_t k = 0U; k < ADC_SCAN_LEN; k++)
    {
        g_mean[k] = (float)sum[k] * inv;
    }
    g_meanValid = 1U;
    g_blocks++;
    if (g_handler != NULL)
    {
        g_handler(g_mean);
    }
}

static void HalfCb(DMA_HandleTypeDef *h)
{
    (void)h;
    ProcessBlock(&g_dma[0]);
}

static void FullCb(DMA_HandleTypeDef *h)
{
    (void)h;
    ProcessBlock(&g_dma[(uint32_t)g_block * ADC_SCAN_LEN]);
}

void Adc_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t code;

    g_ready = 0U;
    code = CodeForX10(ADC_SAMPLE_X10_DEFAULT);
    g_smpCode = (code != 0U) ? (uint8_t)(code - 1U) : 4U;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;          /* PB0, PB1 */
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;          /* PC0, PC1 */
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
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    if (WaitSet(&ADC1->ISR, ADC_ISR_ADRDY) == 0U)
    {
        return;
    }

    /* The DMA channel: peripheral (ADC1->DR) to memory, halfwords, circular. */
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    hdmaAdc.Instance                 = DMA2_Channel1;
    hdmaAdc.Init.Request             = DMA_REQUEST_ADC1;
    hdmaAdc.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdmaAdc.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdmaAdc.Init.MemInc              = DMA_MINC_ENABLE;
    hdmaAdc.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdmaAdc.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdmaAdc.Init.Mode                = DMA_CIRCULAR;
    hdmaAdc.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdmaAdc) != HAL_OK)
    {
        return;
    }
    hdmaAdc.XferHalfCpltCallback = HalfCb;
    hdmaAdc.XferCpltCallback     = FullCb;
    HAL_NVIC_SetPriority(DMA2_Channel1_IRQn, 2U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Channel1_IRQn);

    g_ready = 1U;
}

uint8_t Adc_IsReady(void)
{
    return g_ready;
}

float Adc_ScanTimeUs(void)
{
    float clocks = 0.0f;

    for (uint8_t k = 0U; k < ADC_SCAN_LEN; k++)
    {
        float smp = (k == ADC_VREFINT_INDEX) ? (float)kSampleX10[VREFINT_SMP_CODE] : (float)kSampleX10[g_smpCode];
        clocks += smp / 10.0f + 12.5f;
    }
    return clocks / ADC_CLOCK_HZ * 1.0e6f;
}

float Adc_ScanTimeUsFor(uint16_t clocksX10)
{
    float clocks = 0.0f;

    if (CodeForX10(clocksX10) == 0U)
    {
        return 0.0f;
    }
    for (uint8_t k = 0U; k < ADC_SCAN_LEN; k++)
    {
        float smp = (k == ADC_VREFINT_INDEX) ? (float)kSampleX10[VREFINT_SMP_CODE] : (float)clocksX10;
        clocks += smp / 10.0f + 12.5f;
    }
    return clocks / ADC_CLOCK_HZ * 1.0e6f;
}

uint8_t Adc_SetSampleTimeX10(uint16_t clocksX10)
{
    uint8_t code = CodeForX10(clocksX10);

    if (code == 0U)
    {
        return 0U;
    }
    g_smpCode = (uint8_t)(code - 1U);
    return 1U;
}

uint16_t Adc_GetSampleTimeX10(void)
{
    return kSampleX10[g_smpCode];
}

uint8_t Adc_SamplerRunning(void)
{
    return g_running;
}

void Adc_StopSampler(void)
{
    TIM6->CR1 &= ~TIM_CR1_CEN;
    if ((ADC1->CR & ADC_CR_ADSTART) != 0U)
    {
        ADC1->CR |= ADC_CR_ADSTP;
        (void)WaitClear(&ADC1->CR, ADC_CR_ADSTP);
    }
    (void)HAL_DMA_Abort(&hdmaAdc);
    ADC1->CFGR &= ~(ADC_CFGR_DMAEN | ADC_CFGR_EXTEN);
    ADC1->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
    g_running = 0U;
}

uint8_t Adc_StartSampler(uint32_t scanRateHz, uint16_t blockScans, adc_block_handler_t handler)
{
    uint32_t arr;

    if ((g_ready == 0U) || (handler == NULL) || (blockScans < 1U) || (blockScans > ADC_MAX_BLOCK_SCANS) ||
        (scanRateHz < 3000U) || (scanRateHz > 400000U))
    {
        return 0U;
    }
    /* the whole scan must fit in the trigger period, with a margin: a trigger that arrives while the
       scan is still running is lost */
    if ((Adc_ScanTimeUs() * 1.15f) > (1.0e6f / (float)scanRateHz))
    {
        return 0U;
    }

    Adc_StopSampler();
    g_block = blockScans;
    g_handler = handler;
    g_meanValid = 0U;
    g_blocks = 0U;

    /* sampling times: SMPR1 holds channels 0-9, SMPR2 channels 10-18 */
    {
        uint32_t s = g_smpCode;
        ADC1->SMPR1 = (s << ADC_SMPR1_SMP1_Pos) | (s << ADC_SMPR1_SMP2_Pos) |
                      (s << ADC_SMPR1_SMP6_Pos) | (s << ADC_SMPR1_SMP7_Pos);
        ADC1->SMPR2 = (s << ADC_SMPR2_SMP12_Pos) | (s << ADC_SMPR2_SMP15_Pos) |
                      ((uint32_t)VREFINT_SMP_CODE << ADC_SMPR2_SMP18_Pos);
    }
    ADC1->SQR1 = ((ADC_SCAN_LEN - 1U) << ADC_SQR1_L_Pos) | ((uint32_t)kChannel[0] << ADC_SQR1_SQ1_Pos) |
                 ((uint32_t)kChannel[1] << ADC_SQR1_SQ2_Pos) | ((uint32_t)kChannel[2] << ADC_SQR1_SQ3_Pos) |
                 ((uint32_t)kChannel[3] << ADC_SQR1_SQ4_Pos);
    ADC1->SQR2 = ((uint32_t)kChannel[4] << ADC_SQR2_SQ5_Pos) | ((uint32_t)kChannel[5] << ADC_SQR2_SQ6_Pos) |
                 ((uint32_t)kChannel[6] << ADC_SQR2_SQ7_Pos);
    ADC1->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_DMAEN | ADC_CFGR_DMACFG | (ADC_EXTSEL_TIM6_TRGO << ADC_CFGR_EXTSEL_Pos) |
                 ADC_CFGR_EXTEN_0;                           /* rising edge of the trigger */
    ADC1->CFGR2 = 0U;

    /* TIM6: counts at the 170 MHz APB1 timer clock; the update event is the trigger output */
    __HAL_RCC_TIM6_CLK_ENABLE();
    arr = (170000000U + (scanRateHz / 2U)) / scanRateHz - 1U;
    TIM6->CR1 = 0U;
    TIM6->PSC = 0U;
    TIM6->ARR = arr;
    TIM6->CNT = 0U;
    TIM6->CR2 = (2U << TIM_CR2_MMS_Pos);                     /* MMS = 010: update -> TRGO */
    TIM6->EGR = TIM_EGR_UG;

    if (HAL_DMA_Start_IT(&hdmaAdc, (uint32_t)&ADC1->DR, (uint32_t)g_dma,
                         2U * (uint32_t)blockScans * ADC_SCAN_LEN) != HAL_OK)
    {
        return 0U;
    }
    ADC1->CR |= ADC_CR_ADSTART;                              /* armed: the first trigger starts a scan */
    TIM6->CR1 = TIM_CR1_CEN;
    g_running = 1U;
    return 1U;
}

uint8_t Adc_Read(uint8_t channel, uint16_t *code)
{
    if ((channel >= ADC_NUM_CHANNELS) || (g_meanValid == 0U))
    {
        return 0U;
    }
    *code = (uint16_t)(g_mean[channel] + 0.5f);
    return 1U;
}

uint8_t Adc_ReadVrefint(uint16_t *code, uint16_t *calCode)
{
    if (g_meanValid == 0U)
    {
        return 0U;
    }
    *code = (uint16_t)(g_mean[ADC_VREFINT_INDEX] + 0.5f);
    *calCode = *VREFINT_CAL_ADDR;
    return 1U;
}

uint32_t Adc_GetBlockCount(void)
{
    return g_blocks;
}
