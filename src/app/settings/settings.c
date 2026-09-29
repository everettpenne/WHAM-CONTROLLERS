/*
 * settings.c -- see settings.h.
 */
#include "settings.h"
#include "pid.h"
#include "xrex_io.h"
#include "board_io.h"
#include <stdio.h>
#include <string.h>

static uint8_t Present(const settings_snapshot_t *s, uint8_t id, uint8_t ch)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        if (kSettingsFields[fi].id == id)
        {
            return SettingsCodec_IsPresent(s, fi, ch);
        }
    }
    return 0U;
}

static void MarkPresent(settings_snapshot_t *s, uint8_t id, uint8_t ch)
{
    for (uint8_t fi = 0U; fi < kSettingsFieldCount; fi++)
    {
        if (kSettingsFields[fi].id == id)
        {
            SettingsCodec_SetPresent(s, fi, ch);
            return;
        }
    }
}

void Settings_Capture(settings_snapshot_t *s)
{
    uint8_t haveTiming;

    SettingsCodec_Clear(s);

    s->pidRateHz     = PID_GetLoopRateHz();
    s->turnonHz      = PID_GetTurnonFreqHz();
    s->maxFreqHz     = PID_GetMaxFreqHz();
    s->slewHzPerTick = PID_GetSlewRateHzPerTick();
    s->faultRampS    = PID_GetFaultRampDownTimeS();
    s->polWater      = (uint8_t)XrexIo_GetFaultPolarityWater();
    s->polTemp       = (uint8_t)XrexIo_GetFaultPolarityTemp();
    s->polEnerpro    = (uint8_t)XrexIo_GetFaultPolarityEnerpro();
    s->polOcp        = (uint8_t)XrexIo_GetFaultPolarityOcp();
    s->gpoutEnable   = BoardIo_Read(BOARD_SIG_GPOUT_ENABLE);
    s->pwmAltEnable  = BoardIo_Read(BOARD_SIG_PWMALT_ENABLE);
    MarkPresent(s, SETF_PID_RATE_HZ, 0U);
    MarkPresent(s, SETF_TURNON_HZ, 0U);
    MarkPresent(s, SETF_MAX_FREQ_HZ, 0U);
    MarkPresent(s, SETF_SLEW_HZ_PER_TICK, 0U);
    MarkPresent(s, SETF_FAULT_RAMP_S, 0U);
    MarkPresent(s, SETF_POL_WATER, 0U);
    MarkPresent(s, SETF_POL_TEMP, 0U);
    MarkPresent(s, SETF_POL_ENERPRO, 0U);
    MarkPresent(s, SETF_POL_OCP, 0U);
    MarkPresent(s, SETF_GPOUT_ENABLE, 0U);
    MarkPresent(s, SETF_PWMALT_ENABLE, 0U);

    haveTiming = PID_GetProfileTiming(&s->rampUpMs, &s->flatTopMs, &s->rampDownMs);
    if (haveTiming != 0U)
    {
        MarkPresent(s, SETF_RAMP_UP_MS, 0U);
        MarkPresent(s, SETF_FLAT_TOP_MS, 0U);
        MarkPresent(s, SETF_RAMP_DOWN_MS, 0U);
    }

    for (uint8_t c = 0U; c < HRTIM_NUM_CHANNELS; c++)
    {
        settings_channel_t *ch = &s->ch[c];

        (void)PID_GetGains(c, &ch->kp, &ch->ki, &ch->kd);
        ch->closedLoop = PID_GetLoopMode(c);
        (void)PID_GetMaxCurrentA(c, &ch->maxCurrentA);
        (void)PID_GetProfileCurrent(c, &ch->demandCurrentA);
        strncpy(ch->nickname, PID_GetChannelNickname(c), SET_NICKNAME_MAX);
        ch->nickname[SET_NICKNAME_MAX] = '\0';

        MarkPresent(s, SETF_KP, c);
        MarkPresent(s, SETF_KI, c);
        MarkPresent(s, SETF_KD, c);
        MarkPresent(s, SETF_CLOSED_LOOP, c);
        MarkPresent(s, SETF_MAX_CURRENT_A, c);
        MarkPresent(s, SETF_DEMAND_CURRENT_A, c);
        MarkPresent(s, SETF_NICKNAME, c);
    }
}

static void Record(settings_apply_result_t *r, uint8_t ok, const char *name, uint8_t ch)
{
    if (ok != 0U)
    {
        r->applied++;
        return;
    }
    r->failed++;
    {
        size_t used = strlen(r->failedNames);
        char item[24];
        size_t n;

        if (ch == SET_CHANNEL_SYSTEM)
        {
            n = (size_t)snprintf(item, sizeof(item), "%s", name);
        }
        else
        {
            n = (size_t)snprintf(item, sizeof(item), "ch%u.%s", (unsigned int)(ch + 1U), name);
        }
        if (used + n + 2U < sizeof(r->failedNames))
        {
            if (used != 0U)
            {
                r->failedNames[used++] = ',';
            }
            memcpy(&r->failedNames[used], item, n + 1U);
        }
    }
}

void Settings_Apply(const settings_snapshot_t *s, settings_apply_result_t *r)
{
    memset(r, 0, sizeof(*r));

    /* CONFig:TURNONHz must stay below CONFig:MAXFREQHz (each setter checks
       against the other's CURRENT value), so if the first order is refused
       try the other one before giving up. */
    {
        uint8_t hasT = Present(s, SETF_TURNON_HZ, 0U);
        uint8_t hasM = Present(s, SETF_MAX_FREQ_HZ, 0U);
        uint8_t okT = 1U;
        uint8_t okM = 1U;

        if (hasM != 0U)
        {
            okM = PID_SetMaxFreqHz(s->maxFreqHz);
        }
        if (hasT != 0U)
        {
            okT = PID_SetTurnonFreqHz(s->turnonHz);
        }
        if (hasT != 0U && hasM != 0U && (okT == 0U || okM == 0U))
        {
            /* the other order (setting the same values again is harmless) */
            okT = PID_SetTurnonFreqHz(s->turnonHz);
            okM = PID_SetMaxFreqHz(s->maxFreqHz);
        }
        if (hasT != 0U)
        {
            Record(r, okT, "TURNONHz", SET_CHANNEL_SYSTEM);
        }
        if (hasM != 0U)
        {
            Record(r, okM, "MAXFREQHz", SET_CHANNEL_SYSTEM);
        }
    }

    if (Present(s, SETF_PID_RATE_HZ, 0U) != 0U)
    {
        Record(r, PID_SetLoopRateHz(s->pidRateHz), "PIDRate", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_SLEW_HZ_PER_TICK, 0U) != 0U)
    {
        Record(r, PID_SetSlewRateHzPerTick(s->slewHzPerTick), "SLEWRate", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_FAULT_RAMP_S, 0U) != 0U)
    {
        Record(r, PID_SetFaultRampDownTimeS(s->faultRampS), "FaultRampTime", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_POL_WATER, 0U) != 0U)
    {
        Record(r, XrexIo_SetFaultPolarityWater(s->polWater), "Polarity.WATER", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_POL_TEMP, 0U) != 0U)
    {
        Record(r, XrexIo_SetFaultPolarityTemp(s->polTemp), "Polarity.TEMP", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_POL_ENERPRO, 0U) != 0U)
    {
        Record(r, XrexIo_SetFaultPolarityEnerpro(s->polEnerpro), "Polarity.ENERPRO", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_POL_OCP, 0U) != 0U)
    {
        Record(r, XrexIo_SetFaultPolarityOcp(s->polOcp), "Polarity.OCP", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_GPOUT_ENABLE, 0U) != 0U)
    {
        BoardIo_Write(BOARD_SIG_GPOUT_ENABLE, (s->gpoutEnable != 0U) ? 1U : 0U);
        Record(r, 1U, "GPOut", SET_CHANNEL_SYSTEM);
    }
    if (Present(s, SETF_PWMALT_ENABLE, 0U) != 0U)
    {
        BoardIo_Write(BOARD_SIG_PWMALT_ENABLE, (s->pwmAltEnable != 0U) ? 1U : 0U);
        Record(r, 1U, "PWMAlt", SET_CHANNEL_SYSTEM);
    }

    /* The three durations are one setting (PID_SetProfileTiming). */
    if (Present(s, SETF_RAMP_UP_MS, 0U) != 0U && Present(s, SETF_FLAT_TOP_MS, 0U) != 0U &&
        Present(s, SETF_RAMP_DOWN_MS, 0U) != 0U)
    {
        Record(r, PID_SetProfileTiming(s->rampUpMs, s->flatTopMs, s->rampDownMs), "Timing", SET_CHANNEL_SYSTEM);
    }

    for (uint8_t c = 0U; c < HRTIM_NUM_CHANNELS; c++)
    {
        const settings_channel_t *ch = &s->ch[c];

        /* Max current first: the demand current is clamped to it. */
        if (Present(s, SETF_MAX_CURRENT_A, c) != 0U)
        {
            Record(r, PID_SetMaxCurrentA(c, ch->maxCurrentA), "MaxCurrent", c);
        }
        if (Present(s, SETF_DEMAND_CURRENT_A, c) != 0U)
        {
            Record(r, PID_SetProfileCurrent(c, ch->demandCurrentA), "DemandCurrent", c);
        }
        if (Present(s, SETF_KP, c) != 0U && Present(s, SETF_KI, c) != 0U && Present(s, SETF_KD, c) != 0U)
        {
            Record(r, PID_SetGains(c, ch->kp, ch->ki, ch->kd), "Gains", c);
        }
        if (Present(s, SETF_CLOSED_LOOP, c) != 0U)
        {
            Record(r, PID_SetLoopMode(c, (ch->closedLoop != 0U) ? 1U : 0U), "ClosedLoop", c);
        }
        if (Present(s, SETF_NICKNAME, c) != 0U)
        {
            uint8_t ok = (ch->nickname[0] == '\0') ? PID_ClearChannelNickname(c)
                                                    : PID_SetChannelNickname(c, ch->nickname);
            Record(r, ok, "Nickname", c);
        }
    }
}
