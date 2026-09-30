/*
 * pfm_input.c
 *
 * See pfm_input.h for full scope/design. HAL_TIM_IC_MspInit()/
 * MspDeInit() live here, not in stm32g4xx_hal_msp.c, matching
 * hrtim.c/qspi_test.c's precedent for hand-added (non-CubeMX)
 * peripherals in this project.
 */

#include "pfm_input_hw.h"
#include "main.h"
#include "mcu.h"

#if (PFM_INPUT_FEATURE_ENABLED != 0)

/* --------------------------------------------------------------------------
 * Channel descriptor table -- one row per PFM_Input_01..06, from
 * docs/pin_mapping_v4.csv + the datasheet-verified AF numbers (see
 * pfm_input.h's own header comment / docs/changelog.txt for how these
 * were confirmed, not guessed). GPIO_NOPULL throughout -- these are
 * actively-driven signal inputs, matching every other actively-driven
 * bus pin already configured in this project (GateDriverStatus,
 * QUADSPI CLK/IO0-3).
 *
 * PA15 (PFM_Input_01) and PB4 (PFM_Input_05) both default to
 * JTAG-debug-port pins at reset (JTDI and JTRST respectively) --
 * reclaiming them as plain AF-muxed GPIO here is standard when only
 * SWD is used (this project's debug link, confirmed by its existing
 * ST-Link/st-flash tooling), not a real risk, just worth this note.
 *
 * dmaInstance/dmaRequest/dmaIrqn added 2026-09-09 for the DMA-based
 * capture redesign -- see this file's own top-of-DMA-section comment
 * below for why. All 6 assigned to DMA1_Channel1..6 (sequentially,
 * one per PFM_Input channel) -- nothing else in this project uses any
 * DMA channel yet, confirmed by grep before choosing these, so there
 * is no conflict to resolve. Request IDs are fixed, chip-defined
 * DMAMUX values (stm32g4xx_hal_dma.h) for each timer/channel's own
 * capture-compare DMA request -- not chosen, just the correct ID for
 * that specific (timer, channel) pair.
 * -------------------------------------------------------------------------- */
typedef struct
{
    GPIO_TypeDef       *port;
    uint16_t            pin;
    uint8_t             af;
    TIM_TypeDef        *timer;
    uint32_t            timChannel;   /* TIM_CHANNEL_1 or TIM_CHANNEL_2 */
    uint8_t             is16Bit;      /* 1 for TIM3/TIM4 (16-bit counters), 0 for
                                          TIM2/TIM5 (32-bit) -- see ProcessDmaChunk()'s
                                          own comment on why this matters for
                                          correct wraparound handling. */
    DMA_Channel_TypeDef *dmaInstance;
    uint32_t             dmaRequest;
    IRQn_Type            dmaIrqn;
} PfmInputDesc_t;

static const PfmInputDesc_t kDesc[PFM_INPUT_NUM_CHANNELS] =
{
    /* PFM_Input_01 */ { GPIOA, GPIO_PIN_15, GPIO_AF1_TIM2, TIM2, TIM_CHANNEL_1, 0U,
                          DMA1_Channel1, DMA_REQUEST_TIM2_CH1, DMA1_Channel1_IRQn },
    /* PFM_Input_02 */ { GPIOD, GPIO_PIN_4,  GPIO_AF2_TIM2, TIM2, TIM_CHANNEL_2, 0U,
                          DMA1_Channel2, DMA_REQUEST_TIM2_CH2, DMA1_Channel2_IRQn },
    /* PFM_Input_03 */ { GPIOB, GPIO_PIN_2,  GPIO_AF2_TIM5, TIM5, TIM_CHANNEL_1, 0U,
                          DMA1_Channel3, DMA_REQUEST_TIM5_CH1, DMA1_Channel3_IRQn },
    /* PFM_Input_04 */ { GPIOC, GPIO_PIN_12, GPIO_AF1_TIM5, TIM5, TIM_CHANNEL_2, 0U,
                          DMA1_Channel4, DMA_REQUEST_TIM5_CH2, DMA1_Channel4_IRQn },
    /* PFM_Input_05 */ { GPIOB, GPIO_PIN_4,  GPIO_AF2_TIM3, TIM3, TIM_CHANNEL_1, 1U,
                          DMA1_Channel5, DMA_REQUEST_TIM3_CH1, DMA1_Channel5_IRQn },
    /* PFM_Input_06 */ { GPIOD, GPIO_PIN_12, GPIO_AF2_TIM4, TIM4, TIM_CHANNEL_1, 1U,
                          DMA1_Channel6, DMA_REQUEST_TIM4_CH1, DMA1_Channel6_IRQn },
};

/* See pfm_input.h's own comment on PFM_INPUT_ACTIVE_CHANNEL_MASK --
   single point of control for which channels this build actually
   initializes/arms. */
static uint8_t ChannelIsActive(uint8_t channel)
{
    return (uint8_t)((PFM_INPUT_ACTIVE_CHANNEL_MASK >> channel) & 1U);
}

/* True if `instance` hosts at least one active channel -- PfmInput_Init()
   uses this to decide whether to touch that timer's clock/NVIC/config
   AT ALL, not just whether to arm it. */
static uint8_t TimerHasActiveChannel(const TIM_TypeDef *instance)
{
    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        if ((kDesc[i].timer == instance) && (ChannelIsActive(i) != 0U))
        {
            return 1U;
        }
    }
    return 0U;
}

/* Non-static, declared extern in pfm_input.h -- stm32g4xx_it.c's
   TIM2/TIM3/TIM4/TIM5_IRQHandler()s need to reach these directly for
   HAL_TIM_IRQHandler(), the same pattern hrtim.h's own `extern
   HRTIM_HandleTypeDef hhrtim1` already uses for HRTIM1_Master_IRQHandler(). */
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;
TIM_HandleTypeDef htim5;

/* Non-static, same reasoning -- stm32g4xx_it.c's 6 new
   DMA1_ChannelN_IRQHandler()s need to reach these directly for
   HAL_DMA_IRQHandler(). One per PFM_Input channel, indexed the same
   way as kDesc[]/g_state[] (0 = PFM_Input_01, ..., 5 = PFM_Input_06). */
DMA_HandleTypeDef pfmInputDma[PFM_INPUT_NUM_CHANNELS];

/* --------------------------------------------------------------------------
 * Capture design (rewritten 2026-09-30 -- read this before changing it).
 *
 * A capture is BOUNDED: PfmInput_Arm(M) asks for M periods (M+1 rising edges),
 * or 2M+1 edges in both-edge mode, never more than PFM_INPUT_MAX_EDGES. So each
 * armed channel's DMA (NORMAL, non-circular) writes the timer's capture register
 * straight into that channel's own result array, g_stamp[], and stops by itself
 * when it is full. There is no per-edge CPU work, no interrupt per chunk, no ring
 * buffer, no tail flush and no chunk bookkeeping: the DMA has two interrupts per
 * capture (half and full), and how many edges arrived is (requested - CNDTR).
 * Periods are computed from the stamps when the host asks for them.
 *
 * Why this replaced the earlier circular 32-entry buffer read a half at a time by
 * the DMA interrupt: each such chunk cost about 16 us of CPU (24 us wall time with
 * the per-period HRTIM interrupt preempting), so at 100 kHz with both edges four
 * channels asked for more CPU than existed. The last channel's interrupt was never
 * serviced -- measured on the simulator with an event trail: channel 4 took 53
 * edges and ran zero callbacks while channels 1-3 rotated. The same chunk work had
 * already been found to stall the table-advance interrupt on the first design
 * (docs/changelog.txt, 2026-09-09). Its side effects -- stale buffer contents
 * returned as data, edges lost at the end of a shot -- are gone with it.
 *
 * History of how the signal itself is captured (real hardware, 2026-09-08/09,
 * docs/changelog.txt): per-edge interrupts starved the HRTIM table-advance
 * interrupt, hence DMA. Rising-edge polarity is set once at init and only changed
 * by PfmInput_Arm() while a channel is stopped (raw both-edge mode); never inside
 * an interrupt, never by reading the pin to classify an edge.
 * -------------------------------------------------------------------------- */
typedef struct
{
    uint16_t want;           /* edges requested for this shot; 0 = not armed */
    uint16_t got;            /* edges stored (final once OnShotEnd ran or the DMA completed) */
    uint8_t  running;        /* capture started and not yet stopped */
    uint8_t  startLevel;     /* pin level before the first captured edge (both-edge mode) */
    uint8_t  periodsValid;   /* period[] matches stamp[] */
    uint16_t overcapture;    /* CCxOF seen (see OverCapture() below) */
    uint32_t period[PFM_INPUT_MAX_PERIODS];
} PfmInputState_t;

static PfmInputState_t g_state[PFM_INPUT_NUM_CHANNELS];

/* Requested and hardware-configured edge mode (pfm_input.h, raw edge mode). */
static uint8_t  g_edgeModeReq[PFM_INPUT_NUM_CHANNELS];
static uint8_t  g_edgeModeHw[PFM_INPUT_NUM_CHANNELS];

/* The captured timer values, DMA's destination. */
static uint32_t g_stamp[PFM_INPUT_NUM_CHANNELS][PFM_INPUT_MAX_EDGES];

/* HAL_TIM_IC_Start_DMA()'s return code per channel (PFMIN:DMASTAT?). */
static HAL_StatusTypeDef g_dmaStartStatus[PFM_INPUT_NUM_CHANNELS];

static uint32_t TimerMask(uint8_t i)
{
    return kDesc[i].is16Bit ? 0xFFFFU : 0xFFFFFFFFU;
}

static TIM_HandleTypeDef *HandleForTimer(const TIM_TypeDef *instance)
{
    if (instance == TIM2) { return &htim2; }
    if (instance == TIM3) { return &htim3; }
    if (instance == TIM4) { return &htim4; }
    if (instance == TIM5) { return &htim5; }
    return NULL;
}

static uint32_t OverflowFlag(uint8_t i)
{
    return (kDesc[i].timChannel == TIM_CHANNEL_1) ? TIM_FLAG_CC1OF : TIM_FLAG_CC2OF;
}

/* CCxOF is sticky: set when a capture arrived while the previous value was still
   unread. Only meaningful while the DMA is still filling the result array (an edge
   then really was missed). Once the array is full nothing reads the register any
   more and the signal carries on, so the flag is set by design -- FinishChannel()
   ignores it for a capture that completed. Returns and clears it. */
static uint16_t TakeOvercapture(uint8_t i)
{
    TIM_HandleTypeDef *htim = HandleForTimer(kDesc[i].timer);
    uint16_t set = 0U;

    if ((htim != NULL) && (__HAL_TIM_GET_FLAG(htim, OverflowFlag(i)) != 0U))
    {
        set = 1U;
        __HAL_TIM_CLEAR_FLAG(htim, OverflowFlag(i));
    }
    return set;
}

static int8_t FindChannelIndex(const TIM_TypeDef *instance, HAL_TIM_ActiveChannel active)
{
    uint32_t timChannel = (active == HAL_TIM_ACTIVE_CHANNEL_1) ? TIM_CHANNEL_1 : TIM_CHANNEL_2;

    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        if ((kDesc[i].timer == instance) && (kDesc[i].timChannel == timChannel))
        {
            return (int8_t)i;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * PfmInput_Init()
 *
 * No prescaler on any of the 4 counters -- full 170 MHz kernel-clock
 * resolution (APB1 undivided, same derivation already documented for
 * HRTIM). Confirmed adequate for this project's ~100 kHz-ish PFM_Input
 * signal range on BOTH the 32-bit (TIM2/TIM5) and 16-bit (TIM3/TIM4)
 * counters involved -- see docs/changelog.txt for the numbers. Each
 * counter's ARR is left at its natural maximum (0xFFFFFFFF for
 * TIM2/TIM5, 0xFFFF for TIM3/TIM4) so it free-runs across its full
 * width rather than wrapping early.
 * -------------------------------------------------------------------------- */
static void InitOneTimer(TIM_HandleTypeDef *htim, TIM_TypeDef *instance, uint32_t period)
{
    TIM_IC_InitTypeDef icCfg = {0};

    htim->Instance               = instance;
    htim->Init.Prescaler         = 0U;
    htim->Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim->Init.Period            = period;
    htim->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim->Init.RepetitionCounter = 0U;
    htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_IC_Init(htim) != HAL_OK)
    {
        Error_Handler();
    }

    /* RISING only, set once here and never changed again -- see this
       file's top comment for why (period-only measurement, no
       in-ISR polarity flip). No ICFilter: no evidence a filter is
       needed for this signal, left at 0. */
    icCfg.ICPolarity  = TIM_ICPOLARITY_RISING;
    icCfg.ICSelection = TIM_ICSELECTION_DIRECTTI;
    icCfg.ICPrescaler = TIM_ICPSC_DIV1;
    icCfg.ICFilter    = 0U;

    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        if ((kDesc[i].timer == instance) && (ChannelIsActive(i) != 0U))
        {
            if (HAL_TIM_IC_ConfigChannel(htim, &icCfg, kDesc[i].timChannel) != HAL_OK)
            {
                Error_Handler();
            }
        }
    }
}

/* One (timer, IRQn, ARR) row per instance -- iterated below so a timer
   with no active channel (PFM_INPUT_ACTIVE_CHANNEL_MASK) is skipped
   ENTIRELY: no HAL_TIM_IC_Init(), no clock enable (InitOneTimer's own
   HAL_TIM_IC_Init() -> HAL_TIM_IC_MspInit() path is what enables the
   peripheral clock), no NVIC priority/enable, no GPIO AF config. Not
   just "initialized but never armed" -- genuinely untouched, per
   pfm_input.h's PFM_INPUT_ACTIVE_CHANNEL_MASK comment (added
   2026-09-09 to cut CPU/interrupt load while a real-hardware timing
   investigation is open -- see docs/changelog.txt). */
typedef struct
{
    TIM_HandleTypeDef *htim;
    TIM_TypeDef        *instance;
    IRQn_Type           irqn;
    uint32_t            arr;
} PfmInputTimerRow_t;

void PfmInput_Init(void)
{
    const PfmInputTimerRow_t rows[] =
    {
        { &htim2, TIM2, TIM2_IRQn, 0xFFFFFFFFU },
        { &htim5, TIM5, TIM5_IRQn, 0xFFFFFFFFU },
        { &htim3, TIM3, TIM3_IRQn, 0xFFFFU },
        { &htim4, TIM4, TIM4_IRQn, 0xFFFFU },
    };

    for (uint8_t r = 0U; r < (sizeof(rows) / sizeof(rows[0])); r++)
    {
        if (TimerHasActiveChannel(rows[r].instance) == 0U)
        {
            continue;   /* no active channel on this timer -- leave it alone */
        }

        InitOneTimer(rows[r].htim, rows[r].instance, rows[r].arr);

        /* Priority (1,0) -- tied with the GateDriverStatus EXTI
           interrupts and (again, as of 2026-09-09 -- see
           HRTIM1_EnableMasterInterrupt()'s own doc comment in hrtim.c
           for why HRTIM1_Master_IRQn moved back to 1) HRTIM1_Master
           itself. Harmless either way: with capture now DMA-driven,
           this TIMx_IRQn is only used for the (now essentially
           unused, since IC DMA mode doesn't route capture events
           through it -- kept for safety/completeness) timer
           update/error paths -- the real per-edge work moved to the
           DMA channel's own IRQn, set up in HAL_TIM_IC_MspInit()
           below, which is DELIBERATELY at a LOWER priority than
           HRTIM1_Master (see that IRQn's own priority comment there)
           -- unlike this essentially-idle IRQn, that one does real,
           non-trivial batch work every 16 captured edges and was
           confirmed on real hardware to starve HRTIM1_Master when
           left this high. */
        HAL_NVIC_SetPriority(rows[r].irqn, 1U, 0U);
        HAL_NVIC_EnableIRQ(rows[r].irqn);
    }
}

/* Sets the capture polarity of channel i to rising-only or both edges.
   Only ever called from PfmInput_Arm(), i.e. while the channel is stopped --
   never during a capture. */
static void ApplyEdgeMode(uint8_t i)
{
    TIM_IC_InitTypeDef icCfg = {0};
    TIM_HandleTypeDef *htim = HandleForTimer(kDesc[i].timer);

    if ((htim == NULL) || (ChannelIsActive(i) == 0U) || (g_edgeModeHw[i] == g_edgeModeReq[i]))
    {
        return;
    }
    icCfg.ICPolarity  = (g_edgeModeReq[i] != 0U) ? TIM_ICPOLARITY_BOTHEDGE : TIM_ICPOLARITY_RISING;
    icCfg.ICSelection = TIM_ICSELECTION_DIRECTTI;
    icCfg.ICPrescaler = TIM_ICPSC_DIV1;
    icCfg.ICFilter    = 0U;
    if (HAL_TIM_IC_ConfigChannel(htim, &icCfg, kDesc[i].timChannel) == HAL_OK)
    {
        g_edgeModeHw[i] = g_edgeModeReq[i];
    }
}

uint8_t PfmInput_SetEdgeMode(uint8_t channel, uint8_t bothEdges)
{
    if (channel >= PFM_INPUT_NUM_CHANNELS)
    {
        return 0U;
    }
    g_edgeModeReq[channel] = (bothEdges != 0U) ? 1U : 0U;
    return 1U;
}

uint8_t PfmInput_GetEdgeMode(uint8_t channel)
{
    return (channel < PFM_INPUT_NUM_CHANNELS) ? g_edgeModeReq[channel] : 0U;
}

const uint32_t *PfmInput_GetRawEdges(uint8_t channel)
{
    return ((channel < PFM_INPUT_NUM_CHANNELS) && (g_edgeModeHw[channel] != 0U)) ? g_stamp[channel] : NULL;
}

uint8_t PfmInput_GetStartLevel(uint8_t channel)
{
    return (channel < PFM_INPUT_NUM_CHANNELS) ? g_state[channel].startLevel : 0U;
}

void PfmInput_Arm(uint16_t m)
{
    uint16_t clamped = (m > (uint16_t)PFM_INPUT_MAX_PERIODS) ? (uint16_t)PFM_INPUT_MAX_PERIODS : m;

    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        PfmInputState_t *st = &g_state[i];

        st->got          = 0U;
        st->running      = 0U;
        st->startLevel   = 0U;
        st->periodsValid = 0U;
        st->overcapture  = 0U;
        st->want         = 0U;
        if ((ChannelIsActive(i) == 0U) || (clamped == 0U))
        {
            continue;
        }
        ApplyEdgeMode(i);
        /* M periods = M+1 rising edges (the first only sets the reference);
           both-edge mode keeps every edge of M periods of a rising-first signal. */
        st->want = (g_edgeModeHw[i] != 0U) ? (uint16_t)(2U * clamped + 1U) : (uint16_t)(clamped + 1U);
    }
}

void PfmInput_OnShotStart(void)
{
    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        PfmInputState_t *st = &g_state[i];
        TIM_HandleTypeDef *htim = HandleForTimer(kDesc[i].timer);

        if ((st->want == 0U) || (htim == NULL))
        {
            continue;   /* never armed for this shot */
        }

        (void)TakeOvercapture(i);
        g_dmaStartStatus[i] = HAL_TIM_IC_Start_DMA(htim, kDesc[i].timChannel, g_stamp[i], st->want);
        st->running = (g_dmaStartStatus[i] == HAL_OK) ? 1U : 0U;

        /* Pin level before the first captured edge (both-edge mode): the level
           now, undone for every edge already stored (the DMA position counts
           them), repeated until no edge lands between the two reads. Sampled
           after the capture is enabled -- starting the channels takes
           microseconds, long enough for an edge to land between an earlier
           sample and the capture actually running. */
        if ((g_edgeModeHw[i] != 0U) && (st->running != 0U))
        {
            uint32_t n0;
            uint32_t n1;
            uint8_t  level;
            do
            {
                n0    = (uint32_t)st->want - pfmInputDma[i].Instance->CNDTR;
                level = (HAL_GPIO_ReadPin(kDesc[i].port, kDesc[i].pin) != GPIO_PIN_RESET) ? 1U : 0U;
                n1    = (uint32_t)st->want - pfmInputDma[i].Instance->CNDTR;
            } while (n0 != n1);
            st->startLevel = (uint8_t)(level ^ (uint8_t)(n0 & 1U));
        }
    }
}

uint8_t PfmInput_GetDmaStartStatus(uint8_t channel)
{
    if (channel >= PFM_INPUT_NUM_CHANNELS)
    {
        return 0xFFU;
    }
    return (uint8_t)g_dmaStartStatus[channel];
}

/* Ends channel i's capture: how many edges it stored, then stop its DMA. */
static void FinishChannel(uint8_t i)
{
    PfmInputState_t *st = &g_state[i];
    TIM_HandleTypeDef *htim = HandleForTimer(kDesc[i].timer);
    uint32_t remaining = pfmInputDma[i].Instance->CNDTR;

    st->got         = (uint16_t)((remaining >= st->want) ? 0U : (st->want - remaining));
    {
        uint16_t flag = TakeOvercapture(i);
        st->overcapture = (st->got < st->want) ? flag : 0U;   /* see TakeOvercapture() */
    }
    st->running     = 0U;
    st->periodsValid = 0U;
    if (htim != NULL)
    {
        (void)HAL_TIM_IC_Stop_DMA(htim, kDesc[i].timChannel);
    }
}

void PfmInput_OnShotEnd(void)
{
    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        if (g_state[i].running != 0U)
        {
            FinishChannel(i);   /* the shot ended before the capture filled up */
        }
    }
}

/* DMA transfer-complete: this channel's result array is full. */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    int8_t idx = FindChannelIndex(htim->Instance, htim->Channel);

    if ((idx >= 0) && (g_state[idx].running != 0U))
    {
        FinishChannel((uint8_t)idx);
    }
}

uint16_t PfmInput_GetCount(uint8_t channel)
{
    if (channel >= PFM_INPUT_NUM_CHANNELS)
    {
        return 0U;
    }
    if (g_edgeModeHw[channel] != 0U)
    {
        return g_state[channel].got;                                  /* edges */
    }
    return (g_state[channel].got > 0U) ? (uint16_t)(g_state[channel].got - 1U) : 0U;   /* periods */
}

const uint32_t *PfmInput_GetPeriods(uint8_t channel)
{
    PfmInputState_t *st;

    if ((channel >= PFM_INPUT_NUM_CHANNELS) || (g_edgeModeHw[channel] != 0U))
    {
        return NULL;
    }
    st = &g_state[channel];
    if ((st->periodsValid == 0U) && (st->running == 0U))
    {
        uint32_t mask = TimerMask(channel);
        for (uint16_t k = 0U; (k + 1U) < st->got; k++)
        {
            st->period[k] = (g_stamp[channel][k + 1U] - g_stamp[channel][k]) & mask;
        }
        st->periodsValid = 1U;
    }
    return st->period;
}

uint16_t PfmInput_GetOvercaptureCount(uint8_t channel)
{
    return (channel < PFM_INPUT_NUM_CHANNELS) ? g_state[channel].overcapture : 0U;
}

/* --------------------------------------------------------------------------
 * HAL_TIM_IC_MspInit()/MspDeInit() -- GPIO AF config + peripheral clock
 * enable for whichever of TIM2/TIM3/TIM4/TIM5 is being initialized,
 * dispatched on htim->Instance since all 4 timers share this one
 * callback, PLUS (2026-09-09) the DMA channel setup for every active
 * channel on this timer -- see kDesc[] at the top of this file for
 * the pin/AF/DMA table.
 * -------------------------------------------------------------------------- */
void HAL_TIM_IC_MspInit(TIM_HandleTypeDef *htim)
{
    GPIO_InitTypeDef gpioInit = {0};

    if (htim->Instance == TIM2)      { __HAL_RCC_TIM2_CLK_ENABLE(); }
    else if (htim->Instance == TIM3) { __HAL_RCC_TIM3_CLK_ENABLE(); }
    else if (htim->Instance == TIM4) { __HAL_RCC_TIM4_CLK_ENABLE(); }
    else if (htim->Instance == TIM5) { __HAL_RCC_TIM5_CLK_ENABLE(); }
    else                             { return; }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* All 6 possible DMA channels live on DMA1 -- one clock enable
       covers whichever of them this build actually uses. DMAMUX1 is a
       SEPARATE peripheral on G4 (does the actual request routing,
       e.g. "TIM2_CH1 -> DMA1_Channel1") -- BUGFIX, real hardware,
       2026-09-09: without its own clock enabled, DMA1 itself reports
       success (HAL_TIM_IC_Start_DMA() returns HAL_OK) but no request
       ever actually reaches it, so no transfers ever happen -- capture
       silently reports 0 periods with no error anywhere. Confirmed via
       a temporary PFMIN:DMASTAT? debug command (commands.c) showing
       HAL_OK while PFMIN:STATUS? stayed at 0. */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();

    gpioInit.Mode  = GPIO_MODE_AF_PP;
    gpioInit.Pull  = GPIO_NOPULL;
    gpioInit.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        if ((kDesc[i].timer != htim->Instance) || (ChannelIsActive(i) == 0U))
        {
            continue;
        }

        gpioInit.Pin       = kDesc[i].pin;
        gpioInit.Alternate = kDesc[i].af;
        HAL_GPIO_Init(kDesc[i].port, &gpioInit);

        /* DMA channel setup for this PFM_Input channel -- circular
           mode (continuous capture, matching the old IT design's
           "keep capturing until targetCount, then explicitly stop"
           behavior), peripheral-to-memory, no peripheral increment
           (always reading the same CCRx register), memory increment
           through g_dmaBuf[i][] (word-aligned, matches CCRx's own
           32-bit register width even on TIM3/TIM4's 16-bit counters --
           the upper 16 bits just read as zero). Priority: medium --
           no evidence yet that this needs tuning; revisit if multiple
           simultaneous channels show any sign of contention. */
        pfmInputDma[i].Instance                 = kDesc[i].dmaInstance;
        pfmInputDma[i].Init.Request              = kDesc[i].dmaRequest;
        pfmInputDma[i].Init.Direction            = DMA_PERIPH_TO_MEMORY;
        pfmInputDma[i].Init.PeriphInc            = DMA_PINC_DISABLE;
        pfmInputDma[i].Init.MemInc               = DMA_MINC_ENABLE;
        pfmInputDma[i].Init.PeriphDataAlignment  = DMA_PDATAALIGN_WORD;
        pfmInputDma[i].Init.MemDataAlignment     = DMA_MDATAALIGN_WORD;
        pfmInputDma[i].Init.Mode                 = DMA_NORMAL;   /* one bounded fill, see the capture design note above */
        pfmInputDma[i].Init.Priority             = DMA_PRIORITY_MEDIUM;

        if (HAL_DMA_Init(&pfmInputDma[i]) != HAL_OK)
        {
            Error_Handler();
        }

        if (kDesc[i].timChannel == TIM_CHANNEL_1)
        {
            __HAL_LINKDMA(htim, hdma[TIM_DMA_ID_CC1], pfmInputDma[i]);
        }
        else
        {
            __HAL_LINKDMA(htim, hdma[TIM_DMA_ID_CC2], pfmInputDma[i]);
        }

        /* Priority 3 (was 1), DEMOTED, confirmed on real hardware,
           2026-09-09 -- see HRTIM1_EnableMasterInterrupt()'s own doc
           comment in hrtim.c for the full writeup. Root cause of a
           real-hardware bug this session found (a long PFM table
           hold run leaves real output ~77 periods behind the table):
           this ISR runs HAL_TIM_IC_CaptureHalfCpltCallback()/
           CaptureCallback(), which batch-processes 16 already-DMA-
           captured edges at a time -- genuine, non-trivial work, not
           the near-instant per-edge register read the old IT design
           had. Left at priority 1 (tied with, so able to preempt,
           HRTIM1_Master_IRQn's old priority 2), that batch work
           delayed PFM's table-advance ISR by up to ~18500 cycles
           (~109 us) roughly every 8 periods -- confirmed with the new
           raw per-call gap log (PFM_GetDiagGapLog(), pfm.c,
           PFM:GAPLOG?), where every spike lined up exactly with a
           16-edge DMA half/full-transfer boundary. Demoting to 3
           (below HRTIM1_Master's restored priority 1 AND below the
           GateDriverStatus/TIMx_IRQn tier at 2) costs nothing here:
           the actual edge CAPTURE is DMA hardware, autonomous of any
           IRQ priority -- only this batch-processing ISR's own
           completion needs to happen before the 32-entry circular
           buffer wraps a further 16 entries, a window of ~8 more
           periods (worlds more headroom than HRTIM1_Master's own
           ISR, which is short enough to add negligible delay even if
           it preempts this one repeatedly). */
        HAL_NVIC_SetPriority(kDesc[i].dmaIrqn, 3U, 0U);
        HAL_NVIC_EnableIRQ(kDesc[i].dmaIrqn);
    }
}

void HAL_TIM_IC_MspDeInit(TIM_HandleTypeDef *htim)
{
    for (uint8_t i = 0U; i < PFM_INPUT_NUM_CHANNELS; i++)
    {
        if ((kDesc[i].timer == htim->Instance) && (ChannelIsActive(i) != 0U))
        {
            HAL_GPIO_DeInit(kDesc[i].port, kDesc[i].pin);
            HAL_NVIC_DisableIRQ(kDesc[i].dmaIrqn);
            HAL_DMA_DeInit(&pfmInputDma[i]);
        }
    }
}

#else /* PFM_INPUT_FEATURE_ENABLED == 0 */

void PfmInput_Init(void) { }
void PfmInput_Arm(uint16_t m) { (void)m; }
void PfmInput_OnShotStart(void) { }
void PfmInput_OnShotEnd(void) { }
uint16_t PfmInput_GetCount(uint8_t channel) { (void)channel; return 0U; }
const uint32_t *PfmInput_GetPeriods(uint8_t channel) { (void)channel; return NULL; }
uint16_t PfmInput_GetOvercaptureCount(uint8_t channel) { (void)channel; return 0U; }
uint8_t PfmInput_GetDmaStartStatus(uint8_t channel) { (void)channel; return 0xFFU; }
uint8_t PfmInput_SetEdgeMode(uint8_t channel, uint8_t bothEdges) { (void)channel; (void)bothEdges; return 0U; }
uint8_t PfmInput_GetEdgeMode(uint8_t channel) { (void)channel; return 0U; }
const uint32_t *PfmInput_GetRawEdges(uint8_t channel) { (void)channel; return NULL; }
uint8_t PfmInput_GetStartLevel(uint8_t channel) { (void)channel; return 0U; }

#endif /* PFM_INPUT_FEATURE_ENABLED */
