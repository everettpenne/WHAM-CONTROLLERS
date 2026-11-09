/*
 * xrex_io.c
 *
 * See xrex_io.h for full scope/design.
 */

#include "xrex_io.h"
#include "ctrlr_config.h"
#include "gate_driver.h"
#include "state_machine.h"
#include "pid.h"
#include "board_io.h"
#include "fault_mask.h"
#include <stddef.h>

/* XRn_OCP pins (PF4/PF8/PF12/PF5 for XR1..XR4 -- an irregular order) are
   BOARD_SIG_XR_OCP(ch); the pin table lives in the BSP (board_io.c). */

/* XR_WATER_FLT_POLARITY/XR_TMP_FLT_POLARITY/XR_ENERPRO_FLT_POLARITY/
   XR_OCP_FLT_POLARITY (ctrlr_config.h) -- RUNTIME-CONFIGURABLE as of
   2026-09-22, direct request. Back CONFig:FaultPolarity:WATER/:TEMP/
   :ENERPRO/:OCP (cmd_config.c) via XrexIo_SetFaultPolarity*() (below).
   Declared OUTSIDE the BUILD_TARGET_SIMULATOR guard below (unlike
   BitIsFault() itself) so the setters/getters compile and work on
   BOTH targets, same as DEBUG:FAULT:BYPASS's own "not build-target-
   guarded" precedent -- harmless on the simulator build, which never
   actually reads these (its own SIM:FAULT:* fault injection is a
   completely separate mechanism, sim_transrex.c). */
static uint32_t g_xrWaterFltPolarity   = XR_WATER_FLT_POLARITY;
static uint32_t g_xrTmpFltPolarity     = XR_TMP_FLT_POLARITY;
static uint32_t g_xrEnerproFltPolarity = XR_ENERPRO_FLT_POLARITY;
static uint32_t g_xrOcpFltPolarity     = XR_OCP_FLT_POLARITY;

/* Shared setter body -- `polarity` must be exactly FAULT_POLARITY_
   NORMALLY_HIGH (0) or FAULT_POLARITY_NORMALLY_LOW (1), matching
   these two named constants' own encoding (ctrlr_config.h); anything
   else is rejected rather than silently stored as some third,
   undefined polarity. */
static uint8_t SetFaultPolarity(uint32_t *dest, uint32_t polarity)
{
    if ((polarity != FAULT_POLARITY_NORMALLY_HIGH) && (polarity != FAULT_POLARITY_NORMALLY_LOW))
    {
        return 0U;
    }
    *dest = polarity;
    return 1U;
}

uint8_t XrexIo_SetFaultPolarityWater(uint32_t polarity)
{
    return SetFaultPolarity(&g_xrWaterFltPolarity, polarity);
}
uint32_t XrexIo_GetFaultPolarityWater(void)
{
    return g_xrWaterFltPolarity;
}

uint8_t XrexIo_SetFaultPolarityTemp(uint32_t polarity)
{
    return SetFaultPolarity(&g_xrTmpFltPolarity, polarity);
}
uint32_t XrexIo_GetFaultPolarityTemp(void)
{
    return g_xrTmpFltPolarity;
}

uint8_t XrexIo_SetFaultPolarityEnerpro(uint32_t polarity)
{
    return SetFaultPolarity(&g_xrEnerproFltPolarity, polarity);
}
uint32_t XrexIo_GetFaultPolarityEnerpro(void)
{
    return g_xrEnerproFltPolarity;
}

uint8_t XrexIo_SetFaultPolarityOcp(uint32_t polarity)
{
    return SetFaultPolarity(&g_xrOcpFltPolarity, polarity);
}
uint32_t XrexIo_GetFaultPolarityOcp(void)
{
    return g_xrOcpFltPolarity;
}

#if !defined(BUILD_TARGET_SIMULATOR)
/* Shared polarity-interpretation helper -- `bit` is a raw HIGH(1)/LOW(0)
   level, `polarity` is one of ctrlr_config.h's FAULT_POLARITY_NORMALLY_*
   constants. Returns 1 if this level represents a fault under that
   polarity. Only used by the controller-target bodies of the four fault
   pollers below (2026-09-21 -- see their own comments on why they're
   no-ops on BUILD_TARGET_SIMULATOR) -- guarded out here too, otherwise
   unused on that build target. */
static uint8_t BitIsFault(uint8_t bit, uint32_t polarity)
{
    return (polarity == FAULT_POLARITY_NORMALLY_HIGH) ? (bit == 0U) : (bit != 0U);
}
#endif /* !BUILD_TARGET_SIMULATOR */

#if !defined(BUILD_TARGET_SIMULATOR)
/* Bit ch set for every channel that is currently enabled (SOURce:ENAble). */
static uint8_t EnabledChannelMask(void)
{
    uint8_t m = 0U;
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        if (PID_GetChannelEnable(ch) != 0U)
        {
            m |= (uint8_t)(1U << ch);
        }
    }
    return m;
}
#endif /* !BUILD_TARGET_SIMULATOR */

uint32_t XrexIo_ScanGateDriverFaults(uint16_t raw12)
{
#if defined(BUILD_TARGET_SIMULATOR)
    /* REAL BUG, found and fixed 2026-09-21: on the simulator, PE0..PE11
       (GateDriver_Read(), what `raw12` is) are REPURPOSED as the
       ENA_OUT/CONTACT_OUT gating INPUT (see sim_transrex.c's own pin-
       role table) -- legitimate signals from the controller's normal
       gating activity, not real Water/Temp fault levels. This
       unmodified controller code was reading those same bits as if
       they meant Water/Temp fault status the WHOLE session, and
       reporting a spurious fault (hard-disabling that channel's own
       HRTIM output, killing its FEEDBACK transmission) essentially
       every time the controller gated a channel on/off during normal
       testing -- root cause of a day-long "controller receives
       nothing, seemingly randomly" investigation (see docs/
       changelog.txt's own extensive entry). No real meaning on this
       build target -- always report healthy. */
    (void)raw12;
    return 0UL;
#else
    return FaultMask_FromGateDriver(raw12, EnabledChannelMask(), g_xrWaterFltPolarity,
                                    g_xrTmpFltPolarity, g_xrEnerproFltPolarity);
#endif /* BUILD_TARGET_SIMULATOR */
}

uint8_t XrexIo_EvaluateGateDriverFault(uint16_t raw12)
{
    /* Enerpro deliberately excluded here as of 2026-09-18 -- see this
       file's own header comment and XrexIo_PollEnerproFaults() below,
       which owns Enerpro's own (differently-routed) fault check. */
    return ((XrexIo_ScanGateDriverFaults(raw12) & FM_WATER_TMP_MASK) != 0UL) ? 1U : 0U;
}

void XrexIo_PollEnerproFaults(uint16_t raw12)
{
    const uint32_t m = XrexIo_ScanGateDriverFaults(raw12) & FM_ENERPRO_MASK;   /* 0 on the simulator build */

    for (uint8_t ch = 0U; (m != 0UL) && (ch < HRTIM_NUM_CHANNELS); ch++)
    {
        if ((m & FM_ENERPRO(ch)) != 0UL)
        {
            SM_ReportEnerproFault(ch);
        }
    }
}

void XrexIo_PollGateDriverFaults(void)
{
#if defined(BUILD_TARGET_SIMULATOR)
    return;   /* PE0..PE11 mean something else on the simulator, see XrexIo_ScanGateDriverFaults() */
#else
    GateDriver_PollFault();
#endif
}

void XrexIo_PollOcpFaults(void)
{
#if defined(BUILD_TARGET_SIMULATOR)
    /* REAL BUG, found and fixed 2026-09-21: the OCP inputs (PF4/PF8/PF12/PF5)
       has no real meaning on this build target -- genuinely floating/
       unconnected, no documented repurposing role (unlike PE0..PE11).
       Reading them as real OCP fault levels tripped a spurious,
       essentially random per-channel fault (hard-disabling that
       channel's own HRTIM output -- killing FEEDBACK transmission)
       purely from electrical noise on an unconnected pin, active from
       the moment the simulator booted (PID_GetChannelEnable() defaults
       to enabled for every channel at boot -- see PID_Init()). Root
       cause of a day-long "controller receives nothing, seemingly
       randomly" investigation -- see docs/changelog.txt's own
       extensive entry. No-op here. */
    return;
#else
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        if (PID_GetChannelEnable(ch) == 0U)
        {
            continue;   /* gated -- see this file's own header comment */
        }

        uint8_t bit = BoardIo_Read(BOARD_SIG_XR_OCP(ch));
        if (BitIsFault(bit, g_xrOcpFltPolarity) != 0U)
        {
            SM_ReportOcpFault(ch);
        }
    }
#endif /* BUILD_TARGET_SIMULATOR */
}

uint8_t XrexIo_GetChannelStatus(uint8_t channel, uint8_t *water, uint8_t *tmp,
                                 uint8_t *enerpro, uint8_t *ocp)
{
    if (channel >= HRTIM_NUM_CHANNELS)
    {
        return 0U;
    }

    uint16_t raw = GateDriver_Read();

    if (water != NULL)   { *water   = (uint8_t)((raw >> (XR_WATER_FLT_BIT_BASE + channel)) & 1U); }
    if (tmp != NULL)     { *tmp     = (uint8_t)((raw >> (XR_TMP_FLT_BIT_BASE + channel)) & 1U); }
    if (enerpro != NULL) { *enerpro = (uint8_t)((raw >> (XR_ENERPRO_FLT_BIT_BASE + channel)) & 1U); }
    if (ocp != NULL)     { *ocp = BoardIo_Read(BOARD_SIG_XR_OCP(channel)); }

    return 1U;
}

/* XRn_ENA_OUT/XRn_CONTACT_OUT (PG0-PG3/PG4-PG7, verified against
   docs/pin_mapping_v4.csv's "XREX Pin Name" column, 2026-09-17) are
   BOARD_SIG_XR_ENA_OUT(ch)/BOARD_SIG_XR_CONTACT_OUT(ch); the pin table
   lives in the BSP (board_io.c). */

void XrexIo_SetEnableOutput(uint8_t channel, uint8_t on)
{
    if (channel >= HRTIM_NUM_CHANNELS)
    {
        return;
    }
    BoardIo_Write(BOARD_SIG_XR_ENA_OUT(channel), on);
}

uint8_t XrexIo_GetEnableOutput(uint8_t channel)
{
    if (channel >= HRTIM_NUM_CHANNELS)
    {
        return 0U;
    }
    return BoardIo_Read(BOARD_SIG_XR_ENA_OUT(channel));
}

void XrexIo_SetContactorOutput(uint8_t channel, uint8_t on)
{
    if (channel >= HRTIM_NUM_CHANNELS)
    {
        return;
    }
    BoardIo_Write(BOARD_SIG_XR_CONTACT_OUT(channel), on);
}

uint8_t XrexIo_GetContactorOutput(uint8_t channel)
{
    if (channel >= HRTIM_NUM_CHANNELS)
    {
        return 0U;
    }
    return BoardIo_Read(BOARD_SIG_XR_CONTACT_OUT(channel));
}

/* Shared by XrexIo_EnableOutputsReadyToArm()/XrexIo_PollEnableOutputFaults()
   below -- 1 if `channel`'s own ENA_OUT AND CONTACT_OUT are BOTH
   currently HIGH, 0 otherwise. Treated as one combined per-channel
   condition throughout this feature, not two independently-faultable
   ones -- see state_machine.h's own enable-output section for why. */
static uint8_t EnableOutputsOk(uint8_t channel)
{
    return (uint8_t)((XrexIo_GetEnableOutput(channel) != 0U) &&
                      (XrexIo_GetContactorOutput(channel) != 0U));
}

uint8_t XrexIo_EnableOutputsReadyToArm(void)
{
    return (XrexIo_FindNotReadyChannel() == 0xFFU) ? 1U : 0U;
}

/* Added 2026-09-22, direct request: ARM used to report only a generic
   "conditions not met" on failure -- an operator with one misconfigured
   channel out of four had no way to tell which one from the wire
   protocol alone. Same scan as XrexIo_EnableOutputsReadyToArm() above
   (now implemented in terms of this function instead of duplicating
   the loop), but returns the first offending 0-based channel instead
   of collapsing to a bool -- 0xFF is the "all channels ready, nothing
   to report" sentinel, matching SM_GetFaultChannel()'s own established
   not-applicable convention. Backs cmd_arm()'s (cmd_state.c) specific
   "Channel N's ENA_OUT/CONTACT_OUT..." error text. */
uint8_t XrexIo_FindNotReadyChannel(void)
{
    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        if (PID_GetChannelEnable(ch) == 0U)
        {
            continue;   /* gated -- see this file's own header comment */
        }
        if (EnableOutputsOk(ch) == 0U)
        {
            return ch;
        }
    }
    return 0xFFU;
}

void XrexIo_PollEnableOutputFaults(void)
{
#if defined(BUILD_TARGET_SIMULATOR)
    /* Same class of bug as the other three fault pollers in this file
       (found 2026-09-21, see their own comments) -- EnableOutputsOk()
       reads PG0..PG7, which on this build target are the simulator's
       own Water+Temp/Enerpro fault-INJECTION transmitters (sim_transrex.c),
       not a real "am I connected" signal from a Transrex. Deliberately
       injecting a Water+Temp/Enerpro fault via SIM:FAULT:* would drive
       these same pins LOW, which this unmodified check would misread as
       ENA_OUT/CONTACT_OUT missing and spuriously hard-disable the
       channel. No-op here. */
    return;
#else
    /* Per direct instruction, this precondition is only meaningful once
       actually armed -- unlike Water/Temp/Enerpro/OCP's always-on
       checking, nothing here runs at all while IDLE (or FAULT, though
       SM_ReportEnableOutputFault() itself also short-circuits that
       case defensively). */
    SM_State_t state = SM_GetState();
    if ((state != SM_STATE_ARMED) && (state != SM_STATE_FIRING))
    {
        return;
    }

    for (uint8_t ch = 0U; ch < HRTIM_NUM_CHANNELS; ch++)
    {
        if (PID_GetChannelEnable(ch) == 0U)
        {
            continue;   /* gated -- see this file's own header comment */
        }
        if (EnableOutputsOk(ch) == 0U)
        {
            SM_ReportEnableOutputFault(ch);
        }
    }
#endif /* BUILD_TARGET_SIMULATOR */
}
