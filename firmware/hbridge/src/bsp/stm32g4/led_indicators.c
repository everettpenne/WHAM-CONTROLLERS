/*
 * led_indicators.c -- STM32G4 implementation of drivers/led_indicators.h.
 *
 * State LEDs: one BSRR write per GPIO port (set the lit pin, reset the
 * others in the same store), so there is never an instant with two state
 * LEDs lit or a lit LED that disagrees with the state machine.
 *
 * Activity LEDs: each has a one-pulse timer (TIM16 for transmit, TIM17 for
 * receive, counting at 10 kHz) whose update event is a DMA request. The DMA
 * channel (DMA1 channel 7 / 8, memory-to-peripheral, circular, one word) copies a
 * constant -- the LED pin's reset bit -- into the GPIO port's BSRR. A pulse
 * therefore costs the caller three register writes (pin on, counter to 0,
 * timer started) and the pin goes dark LED_ACTIVITY_MS later without the CPU
 * being involved: no interrupt is enabled for either channel, and it works
 * while the main loop is inside a blocking transmit. A second pulse before
 * the timer expires restarts it, so continuous traffic holds the LED lit.
 *
 * Resources used, so nothing else claims them: TIM16, TIM17, DMA1 channels 7
 * and 8 (pfm_input.c has channels 1-6, the ADC sampler DMA2 channel 1). The
 * DMA channels run at low priority; a transfer is one word every few tens of
 * milliseconds at most.
 *
 * Known, accepted limit: if a pulse lands in the few bus cycles between the
 * timer expiring and its DMA write, the LED goes dark for one period before
 * the restarted timer lights nothing; the effect is one missed flash.
 */
#include "led_indicators.h"

#if (LED_INDICATORS_ENABLED != 0)

#include "main.h"

#define LED_TX_PORT        GPIOD
#define LED_TX_PIN         GPIO_PIN_2
#define LED_RX_PORT        GPIOD
#define LED_RX_PIN         GPIO_PIN_3
#define LED_IDLE_PORT      GPIOF
#define LED_IDLE_PIN       GPIO_PIN_14
#define LED_ARMED_PORT     GPIOF
#define LED_ARMED_PIN      GPIO_PIN_2
#define LED_FIRING_PORT    GPIOC
#define LED_FIRING_PIN     GPIO_PIN_3
#define LED_FAULT_PORT     GPIOC
#define LED_FAULT_PIN      GPIO_PIN_2

#define LED_TIMER_TICK_HZ  (10000UL)     /* one-shot timer resolution: 0.1 ms */

#if (LED_ACTIVITY_MS < 1U) || (LED_ACTIVITY_MS > 6000U)
#error "LED_ACTIVITY_MS must be 1..6000 (16-bit timer at 10 kHz)"
#endif

typedef struct
{
    GPIO_TypeDef      *port;
    uint32_t           pin;
    TIM_TypeDef       *tim;
    DMA_Channel_TypeDef *dmaChannel;
    uint32_t           dmaRequest;
    DMA_HandleTypeDef  hdma;
    uint32_t           offWord;          /* the DMA source: the pin's BSRR reset bit */
} activity_led_t;

static activity_led_t s_tx = { LED_TX_PORT, LED_TX_PIN, TIM16, DMA1_Channel7, DMA_REQUEST_TIM16_UP };
static activity_led_t s_rx = { LED_RX_PORT, LED_RX_PIN, TIM17, DMA1_Channel8, DMA_REQUEST_TIM17_UP };

static volatile uint8_t s_activityReady = 0U;

/* Timer tick source: the APB2 timer clock (twice PCLK2 when APB2 is divided). */
static uint32_t Timer16_17Clock(void)
{
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    return ((RCC->CFGR & RCC_CFGR_PPRE2_2) != 0U) ? (2U * pclk2) : pclk2;
}

/* Returns 1 when the one-shot timer and its DMA channel are armed. */
static uint8_t ActivityInit(activity_led_t *a)
{
    a->offWord = a->pin << 16;

    /* One-pulse timer: update event only on overflow (URS), stops itself at
       the overflow (OPM), update raises a DMA request (UDE), no interrupt. */
    a->tim->CR1  = 0U;
    a->tim->PSC  = (Timer16_17Clock() / LED_TIMER_TICK_HZ) - 1U;
    a->tim->ARR  = ((LED_ACTIVITY_MS * LED_TIMER_TICK_HZ) / 1000UL) - 1U;
    a->tim->CNT  = 0U;
    a->tim->DIER = TIM_DIER_UDE;
    a->tim->CR1  = TIM_CR1_OPM | TIM_CR1_URS;
    a->tim->EGR  = TIM_EGR_UG;           /* load PSC; URS keeps this from raising a request */
    a->tim->SR   = 0U;

    /* DMA: one word, constant source, fixed destination (the GPIO port's
       BSRR), circular so it re-arms itself after every transfer. */
    a->hdma.Instance                 = a->dmaChannel;
    a->hdma.Init.Request             = a->dmaRequest;
    a->hdma.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    a->hdma.Init.PeriphInc           = DMA_PINC_DISABLE;
    a->hdma.Init.MemInc              = DMA_MINC_DISABLE;
    a->hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    a->hdma.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    a->hdma.Init.Mode                = DMA_CIRCULAR;
    a->hdma.Init.Priority            = DMA_PRIORITY_LOW;
    if (HAL_DMA_Init(&a->hdma) != HAL_OK)
    {
        return 0U;
    }
    return (HAL_DMA_Start(&a->hdma, (uint32_t)&a->offWord,
                          (uint32_t)&a->port->BSRR, 1U) == HAL_OK) ? 1U : 0U;
}

void LedIndicators_Init(void)
{
    GPIO_InitTypeDef init = {0};
    uint8_t txOk;
    uint8_t rxOk;

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_TIM16_CLK_ENABLE();
    __HAL_RCC_TIM17_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* Output level low before the pins become outputs: no flash at start-up. */
    LED_TX_PORT->BSRR    = ((uint32_t)(LED_TX_PIN | LED_RX_PIN)) << 16;
    LED_IDLE_PORT->BSRR  = ((uint32_t)(LED_IDLE_PIN | LED_ARMED_PIN)) << 16;
    LED_FIRING_PORT->BSRR = ((uint32_t)(LED_FIRING_PIN | LED_FAULT_PIN)) << 16;

    init.Mode  = GPIO_MODE_OUTPUT_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Pin = LED_TX_PIN | LED_RX_PIN;       HAL_GPIO_Init(GPIOD, &init);
    init.Pin = LED_IDLE_PIN | LED_ARMED_PIN;  HAL_GPIO_Init(GPIOF, &init);
    init.Pin = LED_FIRING_PIN | LED_FAULT_PIN; HAL_GPIO_Init(GPIOC, &init);

    txOk = ActivityInit(&s_tx);
    rxOk = ActivityInit(&s_rx);
    /* If either DMA channel could not be armed the pulse would never end:
       leave the activity LEDs dark rather than stuck on. */
    s_activityReady = ((txOk != 0U) && (rxOk != 0U)) ? 1U : 0U;
}

void LedIndicators_ShowState(led_state_t state)
{
    uint32_t fOn = 0U;
    uint32_t cOn = 0U;

    switch (state)
    {
        case LED_STATE_IDLE:   fOn = LED_IDLE_PIN;   break;
        case LED_STATE_ARMED:  fOn = LED_ARMED_PIN;  break;
        case LED_STATE_FIRING: cOn = LED_FIRING_PIN; break;
        case LED_STATE_FAULT:  cOn = LED_FAULT_PIN;  break;
        default:               break;
    }

    /* Low half of BSRR sets, high half resets: one store per port. */
    LED_IDLE_PORT->BSRR   = fOn | ((((uint32_t)(LED_IDLE_PIN | LED_ARMED_PIN)) & ~fOn) << 16);
    LED_FIRING_PORT->BSRR = cOn | ((((uint32_t)(LED_FIRING_PIN | LED_FAULT_PIN)) & ~cOn) << 16);
}

static inline void Pulse(const activity_led_t *a)
{
    if (s_activityReady != 0U)
    {
        a->port->BSRR = a->pin;
        a->tim->CNT   = 0U;
        a->tim->CR1   = TIM_CR1_OPM | TIM_CR1_URS | TIM_CR1_CEN;
    }
}

void LedIndicators_PulseTx(void) { Pulse(&s_tx); }
void LedIndicators_PulseRx(void) { Pulse(&s_rx); }

uint8_t LedIndicators_GetMask(void)
{
    uint8_t m = 0U;

    if ((LED_TX_PORT->ODR     & LED_TX_PIN)     != 0U) { m |= LED_MASK_TX; }
    if ((LED_RX_PORT->ODR     & LED_RX_PIN)     != 0U) { m |= LED_MASK_RX; }
    if ((LED_IDLE_PORT->ODR   & LED_IDLE_PIN)   != 0U) { m |= LED_MASK_IDLE; }
    if ((LED_ARMED_PORT->ODR  & LED_ARMED_PIN)  != 0U) { m |= LED_MASK_ARMED; }
    if ((LED_FIRING_PORT->ODR & LED_FIRING_PIN) != 0U) { m |= LED_MASK_FIRING; }
    if ((LED_FAULT_PORT->ODR  & LED_FAULT_PIN)  != 0U) { m |= LED_MASK_FAULT; }
    return m;
}

#endif /* LED_INDICATORS_ENABLED */
