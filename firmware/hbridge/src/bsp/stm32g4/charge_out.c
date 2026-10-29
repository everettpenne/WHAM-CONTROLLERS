/*
 * charge_out.c -- STM32G4 implementation of drivers/charge_out.h.
 *
 * TIM20, channel 1, its COMPLEMENTARY output CH1N on PG0 (alternate function 2). PG0..PG3 are the only pins of the
 * board's free general-purpose outputs that reach a timer (docs/pin_mapping_v4.csv: PG0 = GPOut_01). The channel's
 * main output is left disabled (CC1E = 0) and only CH1N is enabled (CC1NE = 1, CC1NP = 0, no dead time): per the
 * reference manual's output-control table the pin then carries OC1REF itself, PWM mode 1 -- HIGH while the counter is
 * below CCR1, so CCR1 = (period)/2 is a 50 % duty cycle. Advanced timers only drive their pins while MOE = 1.
 *
 * Reloads (PSC, ARR, CCR1) are buffered; ChargeOut_Start() writes them and forces an update event (UG), which loads
 * them at once and restarts the count, so a new frequency takes effect immediately even from a very slow one.
 *
 * Off: the pin is switched to a plain output at LOW first (GPIO ODR is LOW), only then is the timer stopped, so the
 * pin can never stay HIGH because the counter stopped in the first half of a cycle.
 *
 * TIM20 sits on APB2, the same bus as TIM16/TIM17 (led_indicators.c): the counter clock is twice PCLK2 when the APB2
 * divider is not 1, PCLK2 otherwise (170 MHz here). Resources used: PG0, TIM20 -- nothing else may claim them.
 */
#include "charge_out.h"
#include "main.h"

#define CHARGE_PORT   GPIOG
#define CHARGE_PIN    GPIO_PIN_0

static void PinAsOutputLow(void)
{
    GPIO_InitTypeDef gi = {0};

    CHARGE_PORT->BSRR = (uint32_t)CHARGE_PIN << 16;        /* the output latch LOW before the pin becomes an output */
    gi.Pin   = CHARGE_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(CHARGE_PORT, &gi);
}

static void PinAsTimerOutput(void)
{
    GPIO_InitTypeDef gi = {0};

    gi.Pin       = CHARGE_PIN;
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_HIGH;
    gi.Alternate = GPIO_AF2_TIM20;
    HAL_GPIO_Init(CHARGE_PORT, &gi);
}

void ChargeOut_Init(void)
{
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_TIM20_CLK_ENABLE();
    PinAsOutputLow();

    TIM20->CR1   = TIM_CR1_ARPE;                                       /* buffered ARR, counter stopped */
    TIM20->CCMR1 = (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;      /* PWM mode 1, buffered CCR1 */
    TIM20->CCER  = 0U;
    TIM20->BDTR  = TIM_BDTR_MOE;                                       /* advanced timer: outputs only drive with MOE */
}

uint32_t ChargeOut_ClockHz(void)
{
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    return ((RCC->CFGR & RCC_CFGR_PPRE2_2) != 0U) ? (2U * pclk2) : pclk2;
}

void ChargeOut_Start(uint32_t psc, uint32_t arr, uint32_t ccr)
{
    TIM20->PSC   = psc;
    TIM20->ARR   = arr;
    TIM20->CCR1  = ccr;
    TIM20->CCER  = TIM_CCER_CC1NE;                                     /* CH1N only, active high */
    TIM20->EGR   = TIM_EGR_UG;                                         /* load the buffered values, count from 0 */
    TIM20->CR1  |= TIM_CR1_CEN;
    PinAsTimerOutput();
}

void ChargeOut_Stop(void)
{
    PinAsOutputLow();
    TIM20->CR1  &= ~TIM_CR1_CEN;
    TIM20->CCER  = 0U;
}
