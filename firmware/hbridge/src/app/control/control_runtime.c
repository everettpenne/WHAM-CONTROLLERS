/*
 * control_runtime.c -- see control_runtime.h.
 */
#include "control_runtime.h"
#include "ctrlr_config.h"
#include "adc.h"
#include "dac.h"
#include "duty_meter.h"
#include "pfm.h"
#include "state_machine.h"
#include "supply_mode.h"
#include "target_profile.h"
#include "shot_profile.h"
#include "hrtim.h"
#include "mcu.h"
#include <stddef.h>

#define REF_TOLERANCE_PCT        (3.0f)     /* VREF+ must be within this of ANALOG_VREF_MV to run closed loop on the ADC */

/* The loops: index 0 = phase U (feedback PA0), 1 = V (PA1), 2 = W (PB0). */
static closed_loop_t g_cl[CTL_LOOPS];
static sim_plant_t   g_sim[CTL_LOOPS];

static uint8_t  g_loopMode[CTL_LOOPS];
static uint8_t  g_fbChannel[CTL_LOOPS] = { 0U, 1U, 3U };   /* ADC index: PA0 = AnalogIn_01, PA1 = _02, PB0 = _04 */
static float    g_holdVolts[CTL_LOOPS];
static uint8_t  g_fbSim = 0U;                     /* 0 ADC, 1 simulated plants, 2 loop 1's plant through the DAC and the real ADC, 3 loop 1 loopback */
static uint8_t  g_meterOn = 0U;                   /* the duty meter is running for a loopback run */
static uint32_t g_noEdgeTicks = 0U;               /* consecutive ticks with no new PFM_Input_01 period */
#define LOOPBACK_NO_SIGNAL_TICKS (200U)           /* 20 ms at 10 kHz: the output the loop is measuring has stopped */
static float    g_dacRatio = 0.5f;                /* ADC volts per DAC volt of the DAC -> amplifier -> ADC loopback */
#define DAC_PLANT_CHANNEL        (1U)       /* PA5 = AnalogOut_02 */
static float    g_fbFilterHz = 0.0f;
static float    g_clipMs = 10.0f;
static uint32_t g_rateHz = CONTROL_RATE_HZ_DEFAULT;
static uint16_t g_scans = CONTROL_SCANS_PER_TICK_DEFAULT;

/* the run in progress */
static volatile uint8_t g_run = 0U;
static volatile uint8_t g_skip = 0U;              /* the first block after the start has samples from before it */
static uint8_t  g_runClosed = 0U;                 /* bit per loop: its duty (or, HVPS, the frequency) comes from its controller */
static uint8_t  g_runOpen = 0U;                   /* bit per phase: its duty follows the shot */
static uint8_t  g_runHvps = 0U;                   /* the HVPS frequency loop (loop 1 only) */
static uint32_t g_runDurationUs = 0U;             /* 0: a hold, until stopped */
static uint32_t g_tick = 0U;
static uint16_t g_lastPer = 0U;
static uint16_t g_lastCmp[CTL_LOOPS];

static uint8_t  g_faultLoop = 0U;                 /* 1-based, 0 none */
static uint8_t  g_faultKind = 0U;

/* timing of the control tick */
static uint32_t g_diagTicks = 0U;
static uint32_t g_diagMaxCycles = 0U;
static uint32_t g_diagSumCycles = 0U;

/* the log: one pool, shared by the loops that were armed */
static int32_t  g_log[CONTROL_LOG_POOL_WORDS];
static volatile uint16_t g_logCount = 0U;
static uint16_t g_logMax = 0U;
static uint16_t g_logDecim = 1U;
static uint16_t g_logPhase = 0U;
static uint8_t  g_logLoops = 1U;                  /* 1: one loop (g_logFirst), CTL_LOOPS: all */
static uint8_t  g_logFirst = 0U;
static volatile uint8_t g_logArmed = 0U;

/* "every loop" or one: the range of loop indices a setter works on; 0 if the argument is neither */
static uint8_t LoopRange(uint8_t loop, uint8_t *lo, uint8_t *hi)
{
    if (loop == CTL_ALL_LOOPS)
    {
        *lo = 0U;
        *hi = CTL_LOOPS;
        return 1U;
    }
    if (loop >= CTL_LOOPS)
    {
        return 0U;
    }
    *lo = loop;
    *hi = (uint8_t)(loop + 1U);
    return 1U;
}

static uint8_t OneLoop(uint8_t loop)
{
    return (loop < CTL_LOOPS) ? loop : 0U;
}

static void ApplyClip(void)
{
    for (uint8_t i = 0U; i < CTL_LOOPS; i++)
    {
        g_cl[i].cfg.clipTicks = (uint32_t)(g_clipMs * (float)g_rateHz / 1000.0f + 0.5f);
    }
}

static void ApplyFilter(void)
{
    for (uint8_t i = 0U; i < CTL_LOOPS; i++)
    {
        Feedback_SetFilter(&g_cl[i].fb, g_fbFilterHz, g_cl[i].dt);
    }
}

/* Puts the plant's output voltage on the DAC such that loop 1's feedback input sees what it would from the real
   supply: process volts -> ADC volts (the feedback calibration, inverted) -> DAC volts (the loopback's gain). */
static void SetDacFromPlant(float processVolts)
{
    float dacV = (processVolts / g_cl[0].fb.scale + g_cl[0].fb.offsetV) / g_dacRatio;

    if (dacV < 0.0f) { dacV = 0.0f; }
    if (dacV > (float)ANALOG_VREF_MV / 1000.0f) { dacV = (float)ANALOG_VREF_MV / 1000.0f; }
    (void)Dac_SetMilliVolts(DAC_PLANT_CHANNEL, (uint16_t)(dacV * 1000.0f + 0.5f));
}

static void ReleaseLoopback(void)
{
    if (g_meterOn != 0U)
    {
        DutyMeter_Stop();
        g_meterOn = 0U;
    }
}

static void StopAllLoops(void)
{
    for (uint8_t i = 0U; i < CTL_LOOPS; i++)
    {
        Cl_Stop(&g_cl[i]);
    }
}

/* The step for the time `tUs` into the run: the period and one compare per channel.
   HVPS: loop 1's frequency at the fixed duty, the same compare on every phase.
   Limiter: the carrier is the shot's frequency when any phase follows the shot, else the fixed closed-loop carrier; each
   channel's duty is its controller's (closed), its duty in the shot (open) or a harmless 50 % (a disabled phase: not
   connected to its pins). The open phases' duty also goes into their `out`, so the status and the log show it. */
static uint8_t BuildStep(uint32_t tUs, uint16_t *per, uint16_t cmp[CTL_LOOPS])
{
    uint32_t fMilli;
    uint16_t shotDuty[CTL_LOOPS];

    if (g_runHvps != 0U)
    {
        uint16_t c;

        if (ShotProfile_StepFor((uint32_t)(g_cl[0].out * 1000.0f + 0.5f),
                                (uint32_t)(g_cl[0].cfg.hvpsDutyPct * 100.0f + 0.5f), per, &c) == 0U)
        {
            return 0U;
        }
        for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
        {
            cmp[ch] = c;
        }
        return 1U;
    }
    if (g_runOpen != 0U)
    {
        if (ShotEval_At(tUs, &fMilli, shotDuty) == 0U)
        {
            return 0U;
        }
    }
    else
    {
        fMilli = (uint32_t)(g_cl[0].cfg.limiterFreqHz * 1000.0f + 0.5f);
    }
    if (ShotProfile_PeriodFor(fMilli, per) == 0U)
    {
        return 0U;
    }
    for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
    {
        uint32_t d;

        if ((g_runClosed & (1U << ch)) != 0U)
        {
            d = (uint32_t)(g_cl[ch].out * 100.0f + 0.5f);
        }
        else if ((g_runOpen & (1U << ch)) != 0U)
        {
            d = shotDuty[ch];
            g_cl[ch].out = (float)d / 100.0f;
        }
        else
        {
            d = 5000U;
        }
        cmp[ch] = ShotProfile_CompareFor(*per, d);
    }
    return 1U;
}

/* The ADC code a closed loop sees this tick. */
static float FeedbackCode(uint8_t ch, const float *means)
{
    if (g_fbSim == 1U)
    {
        /* the plant model answers the step the controller commanded last; its voltage goes through the
           inverse of the feedback calibration so the loop sees what an ADC would read */
        float v = SimPlant_Step(&g_sim[ch], (g_cl[ch].plant == CL_PLANT_LIMITER) ? 1U : 0U, g_lastPer, g_lastCmp[ch], g_cl[ch].dt);
        float adcV = v / g_cl[ch].fb.scale + g_cl[ch].fb.offsetV;
        float code = adcV / g_cl[ch].fb.vrefV * 4095.0f;

        if (code < 0.0f) { code = 0.0f; }
        if (code > 4095.0f) { code = 4095.0f; }
        return code;
    }
    if (ch == 0U)
    {
        if (g_fbSim == 2U)
        {
            /* hardware in the loop: the plant model's output goes out on the DAC and comes back through the
               real feedback path (an amplifier/divider into the ADC input), so the ADC, the board and the loop
               are all real; only the supply is a model. It reaches the ADC one tick later. */
            SetDacFromPlant(SimPlant_Step(&g_sim[0], (g_cl[0].plant == CL_PLANT_LIMITER) ? 1U : 0U, g_lastPer, g_lastCmp[0], g_cl[0].dt));
        }
        else if (g_fbSim == 3U)
        {
            /* loopback: the plant is measured off the controller's own output. PFM_Input_01 (Phase U wired to it)
               gives the duty (Limiter) or the carrier frequency (HVPS) the HRTIM really produced; the plant model turns it into a voltage, the same lag as
               the simulated plant is the digital low-pass, and that voltage goes out on the DAC and back in through
               the real ADC input. */
            float duty, hz;

            if (DutyMeter_Read(&duty, &hz) != 0U)
            {
                g_noEdgeTicks = 0U;
            }
            else if (++g_noEdgeTicks > LOOPBACK_NO_SIGNAL_TICKS)
            {
                return -1.0f;                                          /* PFM_Input_01 stopped toggling: nothing to measure */
            }
            {
                float steady = (g_cl[0].plant == CL_PLANT_LIMITER) ? SimPlant_LimiterSteady(&g_sim[0], duty)
                                                                   : SimPlant_HvpsSteady(&g_sim[0], hz);   /* HVPS: the measured carrier frequency */

                SetDacFromPlant(SimPlant_StepSteady(&g_sim[0], steady, g_cl[0].dt));
            }
        }
    }
    return means[g_fbChannel[ch]];
}

static void EndRun(void)
{
    g_run = 0U;
    StopAllLoops();
    ReleaseLoopback();
}

/* One control tick. Runs in the ADC DMA interrupt (priority 2). */
static void OnBlock(const float *means)
{
    uint16_t per;
    uint16_t cmp[CTL_LOOPS];
    uint32_t t0;
    uint32_t tUs;

    if (g_run == 0U)
    {
        return;
    }
    if (g_skip != 0U)
    {
        g_skip = 0U;
        return;
    }
    t0 = Mcu_CycleCount();
    g_tick++;
    tUs = (uint32_t)(((uint64_t)g_tick * 1000000ULL) / g_rateHz);

    /* the end of a profile run: the shot's length when any phase follows the shot, else the target's */
    if ((g_runDurationUs != 0U) && (tUs >= g_runDurationUs))
    {
        EndRun();
        PFM_LiveRequestStop();
        return;
    }

    for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
    {
        float code;
        cl_status_t st;

        if ((g_runClosed & (1U << ch)) == 0U)
        {
            continue;
        }
        code = FeedbackCode(ch, means);
        if (code < 0.0f)
        {
            EndRun();
            PFM_LiveRequestStop();
            g_faultLoop = (uint8_t)(ch + 1U);
            g_faultKind = 2U;
            SM_ReportControlFault(SM_FAULT_SRC_FEEDBACK);
            return;
        }
        st = Cl_TickCore(&g_cl[ch], code);
        if (st != CL_RUNNING)
        {
            EndRun();
            g_faultLoop = (uint8_t)(ch + 1U);
            switch (st)
            {
                case CL_FAULT_OVERVOLTAGE:
                    g_faultKind = 1U;
                    SM_ReportControlFault(SM_FAULT_SRC_OVERVOLT);
                    break;
                case CL_FAULT_CLIPPED_LOW:
                    g_faultKind = 3U;
                    SM_ReportControlFault(SM_FAULT_SRC_FEEDBACK);
                    break;
                default:
                    g_faultKind = 2U;
                    SM_ReportControlFault(SM_FAULT_SRC_FEEDBACK);
                    break;
            }
            return;
        }
    }

    if (BuildStep(tUs, &per, cmp) == 0U)
    {
        EndRun();
        PFM_LiveRequestStop();
        return;
    }
    g_lastPer = per;
    for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
    {
        g_lastCmp[ch] = cmp[ch];
    }
    PFM_LiveSet(per, cmp);

    if ((g_logArmed != 0U) && (g_logCount < g_logMax))
    {
        g_logPhase++;
        if (g_logPhase >= g_logDecim)
        {
            uint16_t base = (uint16_t)((uint32_t)g_logCount * g_logLoops * 3U);

            g_logPhase = 0U;
            for (uint8_t s = 0U; s < g_logLoops; s++)
            {
                const closed_loop_t *c = &g_cl[(g_logLoops == 1U) ? g_logFirst : s];

                g_log[base + (3U * s) + 0U] = (int32_t)(c->sp * 1000.0f);
                g_log[base + (3U * s) + 1U] = (int32_t)(c->pv * 1000.0f);
                g_log[base + (3U * s) + 2U] = (int32_t)(c->out * 1000.0f);
            }
            g_logCount++;
        }
    }
    {
        uint32_t used = Mcu_CycleCount() - t0;

        g_diagTicks++;
        g_diagSumCycles += used;
        if (used > g_diagMaxCycles)
        {
            g_diagMaxCycles = used;
        }
    }
}

/* (Re)starts the ADC sampler at the present rate, scans per tick and sampling time. */
static uint8_t RestartSampler(void)
{
    return Adc_StartSampler(g_rateHz * g_scans, g_scans, OnBlock);
}

void Control_Init(void)
{
    for (uint8_t i = 0U; i < CTL_LOOPS; i++)
    {
        Cl_Init(&g_cl[i], g_rateHz, (float)ANALOG_VREF_MV / 1000.0f);
        g_cl[i].ch = i;
        g_cl[i].autoFinish = 0U;                       /* the runtime ends a run itself */
#if (FEEDBACK_VMAX_DEFAULT_V > 0)
        Feedback_SetBipolarRange(&g_cl[i].fb, (float)FEEDBACK_VMAX_DEFAULT_V);
#endif
        SimPlant_Init(&g_sim[i]);
        g_loopMode[i] = 0U;
        g_holdVolts[i] = 0.0f;
    }
    ApplyClip();
    ApplyFilter();
    (void)RestartSampler();
}

static uint8_t PlantIndex(void)
{
    return (SupplyMode_Get() == SUPPLY_MODE_LIMITER) ? CL_PLANT_LIMITER : CL_PLANT_HVPS;
}

uint8_t Control_ClosedRunWanted(void)
{
    if (SupplyMode_Get() == SUPPLY_MODE_HVPS)
    {
        return (g_loopMode[0] != 0U) ? 1U : 0U;
    }
    {
        uint8_t mask = PFM_GetPhaseMask();

        for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
        {
            if (((mask & (1U << ch)) != 0U) && (g_loopMode[ch] != 0U))
            {
                return 1U;
            }
        }
    }
    return 0U;
}

ctl_result_t Control_Prepare(uint8_t useProfile)
{
    uint16_t per;
    uint16_t cmp[CTL_LOOPS];
    uint8_t enabled = PFM_GetPhaseMask();
    uint8_t closed = 0U;
    uint8_t open = 0U;
    uint8_t hvps;

    if (g_run != 0U)
    {
        return CTL_BUSY;
    }
    if (SupplyMode_Get() == SUPPLY_MODE_NONE)
    {
        return CTL_NO_MODE;
    }
    hvps = (SupplyMode_Get() == SUPPLY_MODE_HVPS) ? 1U : 0U;
    if (hvps != 0U)
    {
        closed = (g_loopMode[0] != 0U) ? 1U : 0U;        /* the one frequency loop; every phase follows it */
    }
    else
    {
        for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
        {
            if ((enabled & (1U << ch)) != 0U)
            {
                if (g_loopMode[ch] != 0U) { closed |= (uint8_t)(1U << ch); }
                else                      { open |= (uint8_t)(1U << ch); }
            }
        }
    }
    if (closed == 0U)
    {
        return CTL_BAD_CONFIG;                           /* nothing to close: the open-loop shot is FIRE's other path */
    }
    if ((useProfile == 0U) && (open != 0U))
    {
        return CTL_NEED_CLOSED;
    }
    if ((useProfile != 0U) && (Target_IsComplete() == 0U))
    {
        return CTL_NO_TARGET;
    }
    if ((open != 0U) && (ShotProfile_IsComplete() == 0U))
    {
        return CTL_NO_SHOT;
    }
    if (g_fbSim != 1U)
    {
        uint16_t code = 0U;
        uint16_t cal = 0U;

        if ((Adc_SamplerRunning() == 0U) || (Adc_ReadVrefint(&code, &cal) == 0U) || (code == 0U))
        {
            return CTL_NO_FEEDBACK;
        }
        /* the ADC's reference must be what the scaling assumes: the internal buffer is out of regulation
           if VDDA sags below its minimum for the selected voltage */
        {
            float vref = 3000.0f * (float)cal / (float)code;
            float want = (float)ANALOG_VREF_MV;
            float dev = (vref - want) / want * 100.0f;

            if ((dev > REF_TOLERANCE_PCT) || (dev < -REF_TOLERANCE_PCT))
            {
                return CTL_REF_BAD;
            }
        }
    }
    if ((g_fbSim == 3U) && ((closed & 1U) != 0U))
    {
        if ((PFM_GetPhaseEnabled(0U) == 0U) || (DutyMeter_Start() == 0U))     /* the meter reads Phase U's output */
        {
            return CTL_NO_METER;
        }
        g_meterOn = 1U;
        g_noEdgeTicks = 0U;
    }

    Target_Rewind();
    ShotEval_Rewind();
    for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
    {
        if ((hvps != 0U) ? (ch != 0U) : ((closed & (1U << ch)) == 0U))
        {
            continue;
        }
        if (Cl_StartCore(&g_cl[ch], useProfile, g_holdVolts[ch]) == 0U)
        {
            StopAllLoops();
            ReleaseLoopback();
            return CTL_BAD_CONFIG;
        }
    }
    g_runHvps = hvps;
    g_runClosed = (hvps != 0U) ? 1U : closed;
    g_runOpen = open;
    g_runDurationUs = 0U;
    if (useProfile != 0U)
    {
        g_runDurationUs = (open != 0U) ? ShotProfile_DurationUs() : Target_DurationUs();
    }
    g_tick = 0U;
    g_faultLoop = 0U;
    g_faultKind = 0U;
    if (BuildStep(0U, &per, cmp) == 0U)
    {
        StopAllLoops();
        ReleaseLoopback();
        return CTL_BAD_CONFIG;
    }
    g_lastPer = per;
    for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
    {
        g_lastCmp[ch] = cmp[ch];
    }
    {
        supply_limits_t hv;

        if (SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &hv) != 0U)
        {
            for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
            {
                g_sim[ch].fMinHz = (float)hv.freqMinHz;       /* the linear HVPS plant reaches zero at the band's edges */
                g_sim[ch].fMaxHz = (float)hv.freqMaxHz;
            }
        }
    }
    for (uint8_t ch = 0U; ch < CTL_LOOPS; ch++)
    {
        if ((g_runClosed & (1U << ch)) != 0U)
        {
            SimPlant_Reset(&g_sim[ch], g_cl[ch].sp);   /* the supply is already at the run's starting voltage: no start-up transient in the model */
        }
    }
    if (((g_fbSim == 2U) || (g_fbSim == 3U)) && ((g_runClosed & 1U) != 0U))
    {
        SetDacFromPlant(g_cl[0].sp);   /* the plant's starting voltage is on the input before the first tick */
    }
    PFM_LiveSet(per, cmp);
    PFM_SelectSource(PFM_SOURCE_LIVE);
    if (g_logArmed != 0U)
    {
        g_logCount = 0U;
        g_logPhase = 0U;
    }
    return CTL_OK;
}

void Control_Go(void)
{
    g_skip = 1U;
    g_run = 1U;
}

void Control_Stop(void)
{
    if (g_run != 0U)
    {
        g_run = 0U;
        StopAllLoops();
    }
    if (PFM_GetSource() == PFM_SOURCE_LIVE)
    {
        PFM_LiveRequestStop();
    }
    ReleaseLoopback();
}

uint8_t Control_IsRunning(void)
{
    return g_run;
}

uint8_t Control_GetLoopMode(uint8_t loop)
{
    return g_loopMode[OneLoop(loop)];
}

void Control_SetLoopMode(uint8_t loop, uint8_t mode)
{
    uint8_t lo, hi;

    if (LoopRange(loop, &lo, &hi) == 0U)
    {
        return;
    }
    for (uint8_t i = lo; i < hi; i++)
    {
        g_loopMode[i] = (mode != 0U) ? 1U : 0U;
    }
}

void Control_SetHoldVolts(uint8_t loop, float volts)
{
    uint8_t lo, hi;

    if (LoopRange(loop, &lo, &hi) == 0U)
    {
        return;
    }
    for (uint8_t i = lo; i < hi; i++)
    {
        g_holdVolts[i] = volts;
        Cl_SetHoldVolts(&g_cl[i], volts);
    }
}

float Control_GetHoldVolts(uint8_t loop)
{
    return g_holdVolts[OneLoop(loop)];
}

void Control_GetStatus(uint8_t loop, float *sp, float *pv, float *out)
{
    const closed_loop_t *c = &g_cl[OneLoop(loop)];

    *sp = c->sp;
    *pv = c->pv;
    *out = c->out;
}

void Control_GetFault(uint8_t *loop, uint8_t *kind)
{
    *loop = g_faultLoop;
    *kind = g_faultKind;
}

void Control_GetDiag(uint32_t *ticks, uint32_t *maxCycles, uint32_t *sumCycles, uint32_t *budgetCycles)
{
    *ticks = g_diagTicks;
    *maxCycles = g_diagMaxCycles;
    *sumCycles = g_diagSumCycles;
    *budgetCycles = (uint32_t)(HRTIM_TIMER_CLK_HZ / g_rateHz);     /* the CPU runs at the HRTIM clock (170 MHz) */
}

uint8_t Control_ActuatorIsHz(void)
{
    return (PlantIndex() == CL_PLANT_HVPS) ? 1U : 0U;
}

/* ---- settings ---- */

/* The sampler and the loop timing are one setting group: the rate times the scans per tick is the
   scan rate, and the scan must fit in its period at the sampling time. */
static ctl_set_result_t ApplyTiming(uint32_t rateHz, uint16_t scans, uint16_t smpX10)
{
    float scanUs = Adc_ScanTimeUsFor(smpX10);

    if (g_run != 0U)
    {
        return CTL_SET_BUSY;
    }
    if ((rateHz < 1000U) || (rateHz > 20000U) || (scans < 1U) || (scans > ADC_MAX_BLOCK_SCANS) || (scanUs <= 0.0f) ||
        ((rateHz * scans) < 3000U) || ((rateHz * scans) > 400000U) ||
        ((scanUs * 1.15f) > (1.0e6f / (float)(rateHz * scans))))
    {
        return CTL_SET_RANGE;
    }
    g_rateHz = rateHz;
    g_scans = scans;
    (void)Adc_SetSampleTimeX10(smpX10);
    for (uint8_t i = 0U; i < CTL_LOOPS; i++)
    {
        Cl_SetRate(&g_cl[i], g_rateHz);
    }
    ApplyClip();
    ApplyFilter();
    return (RestartSampler() != 0U) ? CTL_SET_OK : CTL_SET_RANGE;
}

ctl_set_result_t Control_SetRateHz(uint32_t hz)
{
    return ApplyTiming(hz, g_scans, Adc_GetSampleTimeX10());
}

uint32_t Control_GetRateHz(void)
{
    return g_rateHz;
}

ctl_set_result_t Control_SetScansPerTick(uint16_t n)
{
    return ApplyTiming(g_rateHz, n, Adc_GetSampleTimeX10());
}

uint16_t Control_GetScansPerTick(void)
{
    return g_scans;
}

ctl_set_result_t Control_SetSampleTimeX10(uint16_t x10)
{
    return ApplyTiming(g_rateHz, g_scans, x10);
}

uint16_t Control_GetSampleTimeX10(void)
{
    return Adc_GetSampleTimeX10();
}

ctl_set_result_t Control_SetGainsFor(uint8_t loop, uint8_t plant, float kp, float ki, float kd)
{
    uint8_t lo, hi;

    if (g_run != 0U)
    {
        return CTL_SET_BUSY;
    }
    if ((LoopRange(loop, &lo, &hi) == 0U) || (plant >= CL_PLANT_COUNT) || (kp < 0.0f) || (ki < 0.0f) || (kd < 0.0f))
    {
        return CTL_SET_RANGE;
    }
    for (uint8_t i = lo; i < hi; i++)
    {
        g_cl[i].cfg.kp[plant] = kp;
        g_cl[i].cfg.ki[plant] = ki;
        g_cl[i].cfg.kd[plant] = kd;
    }
    return CTL_SET_OK;
}

void Control_GetGainsFor(uint8_t loop, uint8_t plant, float *kp, float *ki, float *kd)
{
    const closed_loop_t *c = &g_cl[OneLoop(loop)];

    if (plant >= CL_PLANT_COUNT)
    {
        plant = 0U;
    }
    *kp = c->cfg.kp[plant];
    *ki = c->cfg.ki[plant];
    *kd = c->cfg.kd[plant];
}

ctl_set_result_t Control_SetGains(uint8_t loop, float kp, float ki, float kd)
{
    return Control_SetGainsFor(loop, PlantIndex(), kp, ki, kd);
}

void Control_GetGains(uint8_t loop, float *kp, float *ki, float *kd)
{
    Control_GetGainsFor(loop, PlantIndex(), kp, ki, kd);
}

/* ---- the closed-loop plant parameters (moved here from cmd_control.c so the saved profiles set them
   through the same validation as the serial commands). They are shared: every loop's copy is set. ---- */

#define SET_ALL_CFG(field, value) \
    do { for (uint8_t i_ = 0U; i_ < CTL_LOOPS; i_++) { g_cl[i_].cfg.field = (value); } } while (0)

ctl_set_result_t Control_SetHvpsDuty(float pct)
{
    supply_limits_t lim;

    (void)SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &lim);
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((pct < (float)lim.dutyMinPct) || (pct > (float)lim.dutyMaxPct)) { return CTL_SET_RANGE; }
    SET_ALL_CFG(hvpsDutyPct, pct);
    return CTL_SET_OK;
}
float Control_GetHvpsDuty(void) { return g_cl[0].cfg.hvpsDutyPct; }

ctl_set_result_t Control_SetLimiterFreq(float hz)
{
    supply_limits_t lim;

    (void)SupplyMode_GetLimits(SUPPLY_MODE_LIMITER, &lim);
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((hz < (float)lim.freqMinHz) || (hz > (float)lim.freqMaxHz)) { return CTL_SET_RANGE; }
    SET_ALL_CFG(limiterFreqHz, hz);
    return CTL_SET_OK;
}
float Control_GetLimiterFreq(void) { return g_cl[0].cfg.limiterFreqHz; }

ctl_set_result_t Control_SetHvpsResonance(float hz)
{
    supply_limits_t lim;

    (void)SupplyMode_GetLimits(SUPPLY_MODE_HVPS, &lim);
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((hz < (float)lim.freqMinHz) || (hz > (float)lim.freqMaxHz)) { return CTL_SET_RANGE; }
    SET_ALL_CFG(hvpsResonanceHz, hz);
    return CTL_SET_OK;
}
float Control_GetHvpsResonance(void) { return g_cl[0].cfg.hvpsResonanceHz; }

ctl_set_result_t Control_SetHvpsVpeak(float volts)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if (volts <= 0.0f) { return CTL_SET_RANGE; }
    SET_ALL_CFG(hvpsVpeak, volts);
    return CTL_SET_OK;
}
float Control_GetHvpsVpeak(void) { return g_cl[0].cfg.hvpsVpeak; }

ctl_set_result_t Control_SetLimiterVscale(float volts)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if (volts <= 0.0f) { return CTL_SET_RANGE; }
    SET_ALL_CFG(limiterVscale, volts);
    return CTL_SET_OK;
}
float Control_GetLimiterVscale(void) { return g_cl[0].cfg.limiterVscale; }

ctl_set_result_t Control_SetHvpsAbove(uint8_t above)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    SET_ALL_CFG(hvpsAbove, (above != 0U) ? 1U : 0U);
    return CTL_SET_OK;
}
uint8_t Control_GetHvpsAbove(void) { return g_cl[0].cfg.hvpsAbove; }

ctl_set_result_t Control_SetFeedbackChannel(uint8_t loop, uint8_t channelIndex)
{
    uint8_t lo, hi;

    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((LoopRange(loop, &lo, &hi) == 0U) || (channelIndex >= ADC_NUM_CHANNELS)) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++)
    {
        g_fbChannel[i] = channelIndex;
    }
    return CTL_SET_OK;
}

uint8_t Control_GetFeedbackChannel(uint8_t loop)
{
    return g_fbChannel[OneLoop(loop)];
}

ctl_set_result_t Control_SetFeedbackScale(uint8_t loop, float scale)
{
    uint8_t lo, hi;

    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((LoopRange(loop, &lo, &hi) == 0U) || (scale <= 0.0f) || (scale > 1.0e6f)) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++)
    {
        Feedback_SetCalibration(&g_cl[i].fb, scale, g_cl[i].fb.offsetV);
    }
    return CTL_SET_OK;
}

float Control_GetFeedbackScale(uint8_t loop)
{
    return g_cl[OneLoop(loop)].fb.scale;
}

ctl_set_result_t Control_SetFeedbackOffset(uint8_t loop, float adcVolts)
{
    uint8_t lo, hi;

    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((LoopRange(loop, &lo, &hi) == 0U) || (adcVolts < 0.0f) || (adcVolts > g_cl[0].fb.vrefV)) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++)
    {
        Feedback_SetCalibration(&g_cl[i].fb, g_cl[i].fb.scale, adcVolts);
    }
    return CTL_SET_OK;
}

float Control_GetFeedbackOffset(uint8_t loop)
{
    return g_cl[OneLoop(loop)].fb.offsetV;
}

ctl_set_result_t Control_SetFeedbackRange(uint8_t loop, float vmax)
{
    uint8_t lo, hi;

    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((LoopRange(loop, &lo, &hi) == 0U) || (vmax <= 0.0f) || (vmax > 1.0e5f)) { return CTL_SET_RANGE; }
    for (uint8_t i = lo; i < hi; i++)
    {
        Feedback_SetBipolarRange(&g_cl[i].fb, vmax);
    }
    return CTL_SET_OK;
}

float Control_GetFeedbackRange(uint8_t loop)
{
    const closed_loop_t *c = &g_cl[OneLoop(loop)];

    /* the process voltage at the top of the ADC range -- +Vmax for a bipolar calibration */
    return (c->fb.vrefV - c->fb.offsetV) * c->fb.scale;
}

ctl_set_result_t Control_SetFeedbackFilterHz(float hz)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((hz < 0.0f) || (hz >= (float)g_rateHz / 2.0f)) { return CTL_SET_RANGE; }
    g_fbFilterHz = hz;
    ApplyFilter();
    return CTL_SET_OK;
}

float Control_GetFeedbackFilterHz(void)
{
    return g_fbFilterHz;
}

ctl_set_result_t Control_SetFeedbackSim(uint8_t sim)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if (sim > 3U) { return CTL_SET_RANGE; }
    g_fbSim = sim;
    return CTL_SET_OK;
}

ctl_set_result_t Control_SetDacRatio(float ratio)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((ratio < 0.05f) || (ratio > 2.0f)) { return CTL_SET_RANGE; }
    g_dacRatio = ratio;
    return CTL_SET_OK;
}

float Control_GetDacRatio(void)
{
    return g_dacRatio;
}

uint8_t Control_GetFeedbackSim(void)
{
    return g_fbSim;
}

ctl_set_result_t Control_SetMaxVolts(float volts)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((volts <= 0.0f) || (volts > 10000.0f)) { return CTL_SET_RANGE; }
    SET_ALL_CFG(maxVolts, volts);
    return CTL_SET_OK;
}

float Control_GetMaxVolts(void)
{
    return g_cl[0].cfg.maxVolts;
}

ctl_set_result_t Control_SetClipMs(float ms)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if ((ms < 0.0f) || (ms > 1000.0f)) { return CTL_SET_RANGE; }
    g_clipMs = ms;
    ApplyClip();
    return CTL_SET_OK;
}

float Control_GetClipMs(void)
{
    return g_clipMs;
}

ctl_set_result_t Control_SetSlewPerTick(float perTick)
{
    if (g_run != 0U) { return CTL_SET_BUSY; }
    if (perTick < 0.0f) { return CTL_SET_RANGE; }
    SET_ALL_CFG(slewPerTick, perTick);
    return CTL_SET_OK;
}

float Control_GetSlewPerTick(void)
{
    return g_cl[0].cfg.slewPerTick;
}

closed_loop_t *Control_Loop(void)
{
    return &g_cl[0];
}

closed_loop_t *Control_LoopN(uint8_t loop)
{
    return &g_cl[OneLoop(loop)];
}

sim_plant_t *Control_SimPlant(void)
{
    return &g_sim[0];
}

sim_plant_t *Control_SimPlantN(uint8_t loop)
{
    return &g_sim[OneLoop(loop)];
}

/* ---- logging ---- */

uint16_t Control_LogMaxSamples(uint8_t loopOrAll)
{
    uint8_t loops = (loopOrAll == CTL_ALL_LOOPS) ? CTL_LOOPS : 1U;

    return (uint16_t)(CONTROL_LOG_POOL_WORDS / (3U * loops));
}

void Control_LogArm(uint8_t loopOrAll, uint16_t maxSamples, uint16_t decim)
{
    uint16_t cap = Control_LogMaxSamples(loopOrAll);

    g_logArmed = 0U;
    g_logCount = 0U;
    g_logPhase = 0U;
    g_logLoops = (loopOrAll == CTL_ALL_LOOPS) ? CTL_LOOPS : 1U;
    g_logFirst = (loopOrAll < CTL_LOOPS) ? loopOrAll : 0U;
    g_logMax = (maxSamples > cap) ? cap : maxSamples;
    g_logDecim = (decim < 1U) ? 1U : decim;
    g_logArmed = (g_logMax > 0U) ? 1U : 0U;
}

uint16_t Control_LogCount(void)
{
    return g_logCount;
}

uint32_t Control_LogRateHz(void)
{
    return g_rateHz / g_logDecim;
}

uint8_t Control_LogGet(uint8_t loop, uint16_t i, int32_t *spMilli, int32_t *pvMilli, int32_t *outMilli)
{
    uint8_t slot;
    uint32_t base;

    if (i >= g_logCount)
    {
        return 0U;
    }
    if (g_logLoops == 1U)
    {
        if (loop != g_logFirst)
        {
            return 0U;
        }
        slot = 0U;
    }
    else
    {
        if (loop >= CTL_LOOPS)
        {
            return 0U;
        }
        slot = loop;
    }
    base = ((uint32_t)i * g_logLoops + slot) * 3U;
    *spMilli = g_log[base];
    *pvMilli = g_log[base + 1U];
    *outMilli = g_log[base + 2U];
    return 1U;
}
