/*
 * cmd_phase.c -- which output phases drive their gate drivers: PHASe:ENAble, PHASe:SELect.
 * Added 2026-10-05.
 *
 * The H-bridge has three phases, U, V and W (HRTIM channels 0, 1, 2; each a main output and its
 * complement with dead time). By default all three run. These commands let the operator output with any
 * combination -- U and W, only W, only V...:
 *
 *   PHASe:ENAble <phases> <0|1>    switch the named phases on (1) or off (0), the others unchanged
 *   PHASe:ENAble? [<phase>]        OK U=1 V=0 W=1, or for one phase OK <0|1>
 *   PHASe:SELect <phases>          exactly these phases on and the rest off: PHASe:SELect UW
 *   PHASe:SELect?                  OK UW (the enabled phases), OK NONE if there are none
 *
 * <phases> is letters in any order and case (U, UW, wu), channel numbers (1, 13), ALL, or (SELect only) NONE.
 *
 * A disabled phase's outputs are never connected to the pins at the start of a shot, so its gate drivers see
 * no switching at all; its timer keeps running in step with the others (hrtim.c), so by design every phase keeps
 * its own FIXED place in the cycle (U at 0, V at 1/3, W at 2/3 of a period) whichever of the others are enabled --
 * disabling V does not move W. (Bench-verified: only the enabled phases toggle; the angles themselves have not
 * been put on a scope for each combination.) The choice is read when a shot starts (FIRE, SOURce:RUN), so it can only be changed in
 * IDLE (ERR 13 otherwise), and ARM refuses while no phase is enabled. It is operator state like XREX's channel
 * enables: durable until reset, deliberately NOT part of a saved profile (a recall must never change which
 * legs of the bridge are driven), and all three phases are enabled at every boot.
 */
#include "commands.h"
#include "cmd_common.h"
#include "ctrlr_config.h"
#include "state_machine.h"
#include "pfm.h"
#include "phase_mask.h"
#include <stdio.h>
#include <string.h>

#define PHASES  ((uint8_t)HRTIM_NUM_CHANNELS)

static const char kListHelp[] = "phases are letters (U, UW), numbers (1, 13) or ALL";

/* Parses a phase list argument; sends the error and returns 0 on a bad or missing one. */
static uint8_t ParseList(uart_instance_t *inst, const char *tok, uint8_t allowNone, uint8_t *mask)
{
    char msg[80];

    if (PhaseMask_Parse(tok, PHASES, allowNone, mask) == PHASE_PARSE_OK)
    {
        return 1U;
    }
    snprintf(msg, sizeof(msg), "%s%s", kListHelp, (allowNone != 0U) ? " or NONE" : "");
    SendErr(inst, ERR_INVALID_ARGS, msg);
    return 0U;
}

static uint8_t RequireIdle(uart_instance_t *inst, const char *what)
{
    char msg[48];

    if (SM_GetState() == SM_STATE_IDLE)
    {
        return 1U;
    }
    snprintf(msg, sizeof(msg), "%s only in IDLE", what);
    SendErr(inst, ERR_INVALID_STATE, msg);
    return 0U;
}

/* PHASe:ENAble <phases> <0|1> */
void cmd_phase_enable(uart_instance_t *inst, char *args)
{
    char *tokList = (args != NULL) ? strtok(args, " \r\n") : NULL;
    char *tokFlag = (tokList != NULL) ? strtok(NULL, " \r\n") : NULL;
    char *extra = (tokFlag != NULL) ? strtok(NULL, " \r\n") : NULL;
    uint8_t mask = 0U;
    uint8_t now;

    if ((tokFlag == NULL) || (extra != NULL) || ((strcmp(tokFlag, "0") != 0) && (strcmp(tokFlag, "1") != 0)))
    {
        SendErr(inst, ERR_INVALID_ARGS, "PHASe:ENAble needs <phases> <0|1>, e.g. PHASe:ENAble UW 1");
        return;
    }
    if (ParseList(inst, tokList, 0U, &mask) == 0U || RequireIdle(inst, "PHASe:ENAble") == 0U)
    {
        return;
    }
    now = PFM_GetPhaseMask();
    PFM_SetPhaseMask((tokFlag[0] == '1') ? (uint8_t)(now | mask) : (uint8_t)(now & (uint8_t)~mask));
    uart_send(inst, "OK\r\n");
}

/* PHASe:ENAble? [<phase>] */
void cmd_phase_enable_query(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t mask = PFM_GetPhaseMask();
    char buf[48];

    if (tok == NULL)
    {
        int n = snprintf(buf, sizeof(buf), "OK");

        for (uint8_t k = 0U; k < PHASES && n > 0 && (size_t)n < sizeof(buf); k++)
        {
            n += snprintf(&buf[n], sizeof(buf) - (size_t)n, " %c=%u", PhaseMask_Letter(k), (unsigned int)((mask >> k) & 1U));
        }
        if (n > 0 && (size_t)n < sizeof(buf) - 3U)
        {
            strcat(buf, "\r\n");
            uart_send(inst, buf);
        }
        return;
    }
    {
        uint8_t one = 0U;

        if (ParseList(inst, tok, 0U, &one) == 0U)
        {
            return;
        }
        if ((one == 0U) || ((one & (uint8_t)(one - 1U)) != 0U))     /* exactly one phase */
        {
            SendErr(inst, ERR_INVALID_ARGS, "PHASe:ENAble? takes one phase (U, V, W) or none");
            return;
        }
        snprintf(buf, sizeof(buf), "OK %u\r\n", (unsigned int)(((mask & one) != 0U) ? 1U : 0U));
        uart_send(inst, buf);
    }
}

/* PHASe:SELect <phases> */
void cmd_phase_select(uart_instance_t *inst, char *args)
{
    char *tok = (args != NULL) ? strtok(args, " \r\n") : NULL;
    uint8_t mask = 0U;

    if ((tok != NULL) && (strtok(NULL, " \r\n") != NULL))
    {
        SendErr(inst, ERR_INVALID_ARGS, "PHASe:SELect takes one list, e.g. PHASe:SELect UW");
        return;
    }
    if (ParseList(inst, tok, 1U, &mask) == 0U || RequireIdle(inst, "PHASe:SELect") == 0U)
    {
        return;
    }
    PFM_SetPhaseMask(mask);
    uart_send(inst, "OK\r\n");
}

/* PHASe:SELect? */
void cmd_phase_select_query(uart_instance_t *inst, char *args)
{
    char list[16];
    char buf[32];
    (void)args;

    PhaseMask_Format(PFM_GetPhaseMask(), PHASES, list, sizeof(list));
    snprintf(buf, sizeof(buf), "OK %s\r\n", list);
    uart_send(inst, buf);
}
