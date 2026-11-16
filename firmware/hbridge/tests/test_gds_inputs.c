/*
 * test_gds_inputs.c -- the GateDriverStatus fault inputs: polarity evaluation, the per-input enable switch and the
 * nicknames (src/app/protection/gds_inputs.c), and their serial commands (src/app/commands/cmd_gds.c) against a fake
 * pin read, a fake fault check and a fake operating state.
 */
#include "commands.h"
#include "gds_inputs.h"
#include "state_machine.h"
#include "gate_driver.h"
#include "fake_uart.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

extern uart_instance_t uart2;                                  /* fake_uart.c's */
static uart_instance_t *const inst = &uart2;

/* ---- fakes for what cmd_gds.c calls ---- */
static uint16_t f_pins;            /* the pins GateDriver_Read() returns */
static int      f_checks;          /* GateDriver_CheckFault() calls */
static SM_State_t f_state;
static uint16_t f_lastBadSeen;     /* what the fault check would have latched on the last call */
uint16_t GateDriver_Read(void) { return f_pins; }
void GateDriver_CheckFault(void) { f_checks++; f_lastBadSeen = GdsInputs_FaultBits(f_pins); }
uint16_t GateDriver_DebugBadRaw(void) { return 0x123U; }
uint32_t GateDriver_DebugEdges(void) { return 7U; }
SM_State_t SM_GetState(void) { return f_state; }

static void Reset(void)
{
    GdsInputs_Reset();
    f_pins = 0xFFFU; f_checks = 0; f_state = SM_STATE_IDLE; f_lastBadSeen = 0U;
}

static void Run(void (*h)(uart_instance_t *, char *), const char *args)
{
    char a[96];
    fake_uart_clear();
    if (args == NULL) { h(inst, NULL); return; }
    strncpy(a, args, sizeof(a) - 1U); a[sizeof(a) - 1U] = '\0';
    h(inst, a);
}

static void TestPolarity(void)
{
    /* the pure evaluation, both polarities: bit set = pin HIGH */
    CHECK(GdsInputs_RawFaultBits(0x0000U, 1U) == 0x000U);        /* normally low, all low: healthy */
    CHECK(GdsInputs_RawFaultBits(0x0004U, 1U) == 0x004U);        /* a HIGH pin is the fault */
    CHECK(GdsInputs_RawFaultBits(0xFFFFU, 1U) == 0xFFFU);        /* bits above 12 never count */
    CHECK(GdsInputs_RawFaultBits(0x0FFFU, 0U) == 0x000U);        /* normally high, all high: healthy */
    CHECK(GdsInputs_RawFaultBits(0x0FFBU, 0U) == 0x004U);        /* a LOW pin is the fault */
    CHECK(GdsInputs_RawFaultBits(0x0000U, 0U) == 0xFFFU);        /* nothing driving them: all faults */
    CHECK(GdsInputs_RawFaultBits(0xF000U, 0U) == 0xFFFU);
    /* the build's own polarity is the HBRIDGE one, per the supply config */
    CHECK(GdsInputs_PolarityIsNormallyLow() == 0U);
    Reset();
    CHECK(GdsInputs_FaultBits(0x0FFFU) == 0U);
    CHECK(GdsInputs_FaultBits(0x0FFBU) == 0x004U);
    Run(cmd_gds_polarity_query, NULL); CHECK_STR(fake_uart_out, "OK NORMALLY_HIGH\r\n");
}

static void TestEnableMask(void)
{
    Reset();
    CHECK(GdsInputs_EnabledMask() == 0x0FFFU);                    /* GDS_DEFAULT_ENABLE_MASK: all on */
    for (uint8_t i = 1U; i <= 12U; i++) { CHECK(GdsInputs_IsEnabled(i) == 1U); }
    CHECK(GdsInputs_IsEnabled(0U) == 0U && GdsInputs_IsEnabled(13U) == 0U);

    CHECK(GdsInputs_SetEnabled(3U, 0U) == 1U);
    CHECK(GdsInputs_EnabledMask() == 0x0FFBU && GdsInputs_IsEnabled(3U) == 0U && GdsInputs_IsEnabled(4U) == 1U);
    /* a switched-off input that reads as a fault is ignored; the others still count */
    CHECK(GdsInputs_FaultBits(0x0FFBU) == 0U);
    CHECK(GdsInputs_FaultBits(0x0FF3U) == 0x008U);
    CHECK(GdsInputs_SetEnabled(3U, 1U) == 1U);
    CHECK(GdsInputs_FaultBits(0x0FFBU) == 0x004U);
    CHECK(GdsInputs_SetEnabled(0U, 0U) == 1U && GdsInputs_EnabledMask() == 0U);          /* 0 = all */
    CHECK(GdsInputs_FaultBits(0x0000U) == 0U);
    CHECK(GdsInputs_SetEnabled(0U, 1U) == 1U && GdsInputs_EnabledMask() == 0x0FFFU);
    CHECK(GdsInputs_SetEnabled(13U, 0U) == 0U && GdsInputs_EnabledMask() == 0x0FFFU);    /* bad input: unchanged */
    CHECK(GdsInputs_SetEnabled(12U, 0U) == 1U && GdsInputs_EnabledMask() == 0x07FFU);
}

static void TestNicknames(void)
{
    Reset();
    CHECK_STR(GdsInputs_Nickname(1U), "");
    CHECK(GdsInputs_SetNickname(3U, "PhaseU_Hi") == 1U);
    CHECK_STR(GdsInputs_Nickname(3U), "PhaseU_Hi");
    CHECK_STR(GdsInputs_Nickname(4U), "");
    CHECK(GdsInputs_SetNickname(3U, "-") == 1U);                  /* clears */
    CHECK_STR(GdsInputs_Nickname(3U), "");
    CHECK(GdsInputs_SetNickname(1U, "123456789012345") == 1U);    /* exactly 15 */
    CHECK(GdsInputs_SetNickname(2U, "1234567890123456") == 0U);   /* 16 */
    CHECK_STR(GdsInputs_Nickname(2U), "");
    CHECK(GdsInputs_SetNickname(0U, "x") == 0U && GdsInputs_SetNickname(13U, "x") == 0U);
    CHECK(GdsInputs_SetNickname(1U, "a b") == 0U);                 /* no spaces */
    CHECK_STR(GdsInputs_Nickname(1U), "123456789012345");          /* a refused name leaves the old one */
    CHECK(GdsInputs_SetNickname(1U, "a=b") == 0U && GdsInputs_SetNickname(1U, "a:b") == 0U && GdsInputs_SetNickname(1U, "a?") == 0U);
    CHECK(GdsInputs_SetNickname(1U, "") == 0U && GdsInputs_SetNickname(1U, NULL) == 0U);
    CHECK(GdsInputs_SetNickname(1U, "W-1.a+b_c") == 1U);
    CHECK_STR(GdsInputs_Nickname(0U), "");
    GdsInputs_Reset();
    CHECK_STR(GdsInputs_Nickname(1U), "");                         /* Reset clears the names */
}

static void TestEnableCommands(void)
{
    Reset();
    Run(cmd_gds_enable_query, NULL);   CHECK_STR(fake_uart_out, "OK 01=1 02=1 03=1 04=1 05=1 06=1 07=1 08=1 09=1 10=1 11=1 12=1\r\n");
    Run(cmd_gds_enable_query, "5");    CHECK_STR(fake_uart_out, "OK 1\r\n");

    Run(cmd_gds_enable, "5 0");        CHECK_STR(fake_uart_out, "OK\r\n");
    CHECK(GdsInputs_IsEnabled(5U) == 0U && GdsInputs_EnabledMask() == 0x0FEFU);
    CHECK(f_checks == 1);                                          /* the pins are re-checked after every change */
    Run(cmd_gds_enable_query, "5");    CHECK_STR(fake_uart_out, "OK 0\r\n");
    Run(cmd_gds_enable_query, NULL);   CHECK_STR(fake_uart_out, "OK 01=1 02=1 03=1 04=1 05=0 06=1 07=1 08=1 09=1 10=1 11=1 12=1\r\n");
    Run(cmd_gds_enable, "5 1");        CHECK_STR(fake_uart_out, "OK\r\n");
    CHECK(GdsInputs_IsEnabled(5U) == 1U && f_checks == 2);

    /* switching an input ON re-checks it: a pin already in fault faults at once */
    Run(cmd_gds_enable, "6 0");
    f_pins = 0x0FDFU;                                              /* pin 6 LOW = fault (NORMALLY_HIGH), but switched off */
    f_checks = 0; f_lastBadSeen = 0U;
    Run(cmd_gds_enable, "7 1");        CHECK(f_lastBadSeen == 0U);   /* pin 6 still ignored */
    Run(cmd_gds_enable, "6 1");        CHECK(f_lastBadSeen == 0x020U && f_checks == 2);

    /* 0 = all */
    Run(cmd_gds_enable, "0 0");        CHECK_STR(fake_uart_out, "OK\r\n"); CHECK(GdsInputs_EnabledMask() == 0U);
    Run(cmd_gds_enable_query, NULL);   CHECK_STR(fake_uart_out, "OK 01=0 02=0 03=0 04=0 05=0 06=0 07=0 08=0 09=0 10=0 11=0 12=0\r\n");
    Run(cmd_gds_enable, "0 1");        CHECK(GdsInputs_EnabledMask() == 0x0FFFU);

    /* argument errors change nothing */
    f_checks = 0;
    Run(cmd_gds_enable, NULL);         CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "3");          CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "13 0");       CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "-1 0");       CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "3x 0");       CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "abc 0");      CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "3 2");        CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable, "3 on");       CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_enable_query, "0");    CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);   /* a query names one input, 1-12 */
    Run(cmd_gds_enable_query, "13");   CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    CHECK(GdsInputs_EnabledMask() == 0x0FFFU && f_checks == 0);
}

static void TestEnableStates(void)
{
    /* a protection is switched OFF only in IDLE or FAULT; switching ON is always allowed */
    SM_State_t refused[] = { SM_STATE_ARMED, SM_STATE_FIRING };
    Reset();
    for (unsigned k = 0U; k < 2U; k++)
    {
        f_state = refused[k]; f_checks = 0;
        Run(cmd_gds_enable, "4 0");    CHECK(strncmp(fake_uart_out, "ERR 13 ", 7) == 0);
        Run(cmd_gds_enable, "0 0");    CHECK(strncmp(fake_uart_out, "ERR 13 ", 7) == 0);
        CHECK(GdsInputs_EnabledMask() == 0x0FFFU && f_checks == 0);
        Run(cmd_gds_enable, "4 1");    CHECK_STR(fake_uart_out, "OK\r\n");
    }
    f_state = SM_STATE_FAULT;
    Run(cmd_gds_enable, "4 0");        CHECK_STR(fake_uart_out, "OK\r\n"); CHECK(GdsInputs_IsEnabled(4U) == 0U);
    f_state = SM_STATE_IDLE;
    Run(cmd_gds_enable, "4 1");        CHECK_STR(fake_uart_out, "OK\r\n"); CHECK(GdsInputs_IsEnabled(4U) == 1U);
}

static void TestNicknameCommands(void)
{
    Reset();
    Run(cmd_gds_nickname_query, NULL); CHECK_STR(fake_uart_out, "OK 01=- 02=- 03=- 04=- 05=- 06=- 07=- 08=- 09=- 10=- 11=- 12=-\r\n");
    Run(cmd_gds_nickname_query, "3");  CHECK_STR(fake_uart_out, "OK -\r\n");
    Run(cmd_gds_nickname, "3 PhaseU_Hi");  CHECK_STR(fake_uart_out, "OK\r\n");
    Run(cmd_gds_nickname, "12 Water.1");   CHECK_STR(fake_uart_out, "OK\r\n");
    Run(cmd_gds_nickname_query, "3");  CHECK_STR(fake_uart_out, "OK PhaseU_Hi\r\n");
    Run(cmd_gds_nickname_query, NULL); CHECK_STR(fake_uart_out, "OK 01=- 02=- 03=PhaseU_Hi 04=- 05=- 06=- 07=- 08=- 09=- 10=- 11=- 12=Water.1\r\n");
    Run(cmd_gds_nickname, "3 -");      CHECK_STR(fake_uart_out, "OK\r\n");
    Run(cmd_gds_nickname_query, "3");  CHECK_STR(fake_uart_out, "OK -\r\n");

    /* the longest possible reply still fits the buffer, whole and terminated */
    for (uint8_t i = 1U; i <= 12U; i++) { GdsInputs_SetNickname(i, "123456789012345"); }
    Run(cmd_gds_nickname_query, NULL);
    CHECK(strlen(fake_uart_out) == 2U + 12U * (4U + 15U) + 2U);
    CHECK(strcmp(&fake_uart_out[strlen(fake_uart_out) - 2U], "\r\n") == 0);
    CHECK(strstr(fake_uart_out, " 12=123456789012345\r\n") != NULL);

    Reset();
    Run(cmd_gds_nickname, NULL);             CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_nickname, "3");              CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_nickname, "0 x");            CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_nickname, "13 x");           CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_nickname, "3x name");        CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_nickname, "3 bad=name");     CHECK(strncmp(fake_uart_out, "ERR 14 ", 7) == 0);
    Run(cmd_gds_nickname, "3 1234567890123456"); CHECK(strncmp(fake_uart_out, "ERR 14 ", 7) == 0);
    Run(cmd_gds_nickname_query, "0");        CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    Run(cmd_gds_nickname_query, "13");       CHECK(strncmp(fake_uart_out, "ERR 12 ", 7) == 0);
    CHECK_STR(GdsInputs_Nickname(3U), "");
}

static void TestOtherGdsCommands(void)
{
    Reset();
    f_pins = 0x0005U;
    Run(cmd_gds_query, NULL);          CHECK_STR(fake_uart_out, "OK 01=HIGH 02=LOW 03=HIGH 04=LOW 05=LOW 06=LOW 07=LOW 08=LOW 09=LOW 10=LOW 11=LOW 12=LOW\r\n");
    Run(cmd_gds_last_query, NULL);     CHECK_STR(fake_uart_out, "OK 123 7\r\n");
}

int main(void)
{
    TestPolarity();
    TestEnableMask();
    TestNicknames();
    TestEnableCommands();
    TestEnableStates();
    TestNicknameCommands();
    TestOtherGdsCommands();
    return test_report("test_gds_inputs");
}
