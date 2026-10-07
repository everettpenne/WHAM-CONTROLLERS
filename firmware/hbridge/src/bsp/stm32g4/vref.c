/*
 * vref.c -- see vref.h.
 */
#include "vref.h"
#include "ctrlr_config.h"
#include "main.h"

/* VRS codes of the STM32G4 VREFBUF: 00 = 2.048 V, 01 = 2.5 V, 10 = 2.9 V. (Confirmed
   by measuring VREF+ with the internal bandgap, ADC:VREF?: code 00 gave 2.053 V; the
   first version of this file had 2.5 V and 2.048 V swapped.) */
#if ANALOG_VREF_MV == 2048U
#define VREF_VRS  (0U)
#elif ANALOG_VREF_MV == 2500U
#define VREF_VRS  (VREFBUF_CSR_VRS_0)
#elif ANALOG_VREF_MV == 2900U
#define VREF_VRS  (VREFBUF_CSR_VRS_1)
#else
#error "ANALOG_VREF_MV must be 2048, 2500 or 2900 (the VREFBUF output settings)"
#endif

static uint8_t g_ready = 0U;

uint8_t Vref_Init(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    VREFBUF->CSR = VREF_VRS | VREFBUF_CSR_ENVR;      /* buffer on, HIZ = 0: it drives the VREF+ pin */
    for (uint32_t i = 0U; i < 400000U; i++)          /* settles in well under a millisecond with the board's capacitor */
    {
        if ((VREFBUF->CSR & VREFBUF_CSR_VRR) != 0U)
        {
            g_ready = 1U;
            return 1U;
        }
    }
    g_ready = 0U;
    return 0U;
}

uint8_t Vref_IsReady(void)
{
    return g_ready;
}
