#ifndef __COMMANDS_H__
#define __COMMANDS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "uart.h"
#include "boot_jump.h"
#include "ctrlr_config.h"
#include "qspi_test.h"
#include "pfm_input.h"
#include "control_runtime.h"

/* --------------------------------------------------------------------------
 * Command handler implementations for WHAM-HBRIDGE-PFMG474.
 *
 * Response conventions (ported from the sibling PFM-STM32G474 project,
 * per project decision):
 *   OK\r\n              - accepted, no data
 *   OK <value>\r\n      - accepted, with return value
 *   ERR <n> <msg>\r\n   - rejected; error codes are stable across versions
 *
 * Error codes: the cmd_err_t enum in cmd_common.h (the single source;
 * docs/command_reference.md documents them for users).
 *
 * Mnemonics are SCPI-style hierarchical patterns matched by
 * src/middleware/scpi/scpi_parser.c -- see that file's header for the
 * short/long-form and multi-layer (':') matching rules. The table of
 * patterns and handlers is command_table.c.
 *
 * Handlers are implemented one file per subsystem (src/app/commands/cmd_*.c):
 *   cmd_system.c   *IDN?, BOOT, DIAGnostic:OPTBytes?/RSTCause, QSPI:ID?
 *   cmd_fwupdate.c FWUPdate:* (in-application firmware update)
 *   cmd_table.c    legacy TABle:*, FIRE, PFM:DIAG?/GAPLOG?
 *   cmd_shot.c     SHOT:* (the time-series shot)
 *   cmd_dac.c      DAC:* (the two analog outputs, PA4/PA5)
 *   cmd_adc.c      ADC:* (the four analog inputs, PA0/PA1/PC0/PB0)
 *   cmd_config.c   CONFig:*
 *   cmd_state.c    FAULT?/FAULT:CLEar, ARM/DISARM/STATE?, GENERAL:TEST:FAULT
 *   cmd_io.c       GDS?
 *   cmd_pfmin.c    PFMIN:*
 * Shared reply/precondition helpers live in cmd_common.c/.h.
 *
 * To add a command: declare its handler here, implement it in the
 * matching cmd_*.c file, and add a row to command_table.c's
 * command_table[]. A new .c file also needs
 * python/sync_build_sources.py run once.
 * -------------------------------------------------------------------------- */

/* Identification / system */
void cmd_idn(uart_instance_t *inst, char *args); /* *IDN? -- board + firmware
                                                      identification, see
                                                      ctrlr_config.h */

/* Serial-bootloader entry. Gated on BOOT_JUMP_FEATURE_ENABLED
 * (boot_jump.h) -- entirely absent, including cmd_parser.c's "BOOT"
 * table row, when that module is disabled. */
#if (BOOT_JUMP_FEATURE_ENABLED != 0)
void cmd_boot(uart_instance_t *inst, char *args); /* BOOT -- reset into the
                                                       ROM serial bootloader,
                                                       see boot_jump.c */
#endif

/* PFM table upload -- a complete shot profile built entirely
 * off-controller (see python/pfm_table_upload.py) and streamed in one
 * step at a time. See pfm.h's "ADDED" header note and
 * docs/command_reference.md for the full protocol and why
 * construction deliberately lives on the host, not here. */
void cmd_table_begin(uart_instance_t *inst, char *args); /* TABle:BEGin -- clears
                                                              the table, opens an
                                                              upload session */
void cmd_table_step(uart_instance_t *inst, char *args);  /* TABle:STEP <per> <cmp0>
                                                              ... <cmp(N-1)> -- appends
                                                              one entry, N = HRTIM_NUM_CHANNELS */
void cmd_table_end(uart_instance_t *inst, char *args);   /* TABle:END -- closes the
                                                              upload session, reports
                                                              the final entry count */
void cmd_table_query(uart_instance_t *inst, char *args); /* TABle? -- reports the
                                                              current entry count */

/* Begins PWM output: (re)starts playback of the currently-uploaded PFM
 * table from step 0 (PFM_Restart(), which also enables the HRTIM
 * channels for output -- see hrtim.c's HRTIM1_PWM_Start()). No ARM/
 * state-machine interlock exists in this minimal firmware -- FIRE
 * always takes effect immediately, whether idle or already mid-shot.
 * Rejects with ERR 5 if the table is empty, ERR 6 if either fault
 * source is latched (see commands.c's AnyFaultLatched()). */
void cmd_fire(uart_instance_t *inst, char *args); /* FIRE -- start PWM output */

/* Diagnostic (added 2026-09-09) -- real inter-call timing for the PFM
 * table-advance ISR during the current/most recent shot. See
 * commands.c's own header comment on cmd_pfm_diag() for the full
 * writeup and pfm.h's PFM_GetDiagCounters(). */
void cmd_pfm_diag(uart_instance_t *inst, char *args); /* PFM:DIAG? -- OK
                                                           <callCount> <maxGapCycles>
                                                           <maxGapUs> */

/* TEMPORARY debug command (2026-09-09, second round) -- dumps the raw
 * per-call inter-call gap log behind PFM_GetDiagGapLog() (pfm.h), for
 * the RAMP/HOLD/RAMP real-hardware lag investigation (see
 * docs/changelog.txt). Remove once that investigation is resolved. */
void cmd_pfm_gaplog(uart_instance_t *inst, char *args); /* PFM:GAPLOG? -- OK
                                                             <count> <gap1> <gap2> ... */

/* Status/clear for BOTH of this project's fault sources, combined --
 * PC10/HRTIM1_FLT6 native hardware (hrtim.h, autonomous in silicon)
 * and the GateDriverStatus_01..12 EXTI interrupt (gate_driver.h,
 * PE0..PE11, software/interrupt-driven). See commands.c's
 * AnyFaultLatched() and its own header comment. Latched: FAULT:CLEAR
 * must be sent explicitly before FIRE works again, even after the
 * physical fault condition itself has gone away -- a fault silently
 * clearing on its own, with output resuming unnoticed, is the hazard
 * this avoids. */
void cmd_fault_query(uart_instance_t *inst, char *args); /* FAULT? -- OK 0|1 */
void cmd_fault_clear(uart_instance_t *inst, char *args);  /* FAULT:CLEar -- OK */

/* Reports HRTIM_NUM_CHANNELS (ctrlr_config.h), the compile-time HRTIM
 * channel count this specific firmware build was configured for --
 * lets host tooling (python/pfm_table_upload.py) confirm what a board
 * actually is instead of silently assuming a value that might not
 * match, e.g. after a rebuild with a different channel count. */
void cmd_config_channels(uart_instance_t *inst, char *args); /* CONFig:CHANnels? -- OK <N> */

/* Raw GateDriverStatus_01..12 (PE0..PE11, see gate_driver.h) readback
 * -- a pure diagnostic snapshot, unaffected by and independent of
 * GDS_FAULT_POLARITY/the GateDriverStatus fault interrupt (same 12
 * pins, but a separate mechanism -- see gate_driver.h). One HIGH/LOW
 * state per pin, on a single OK line -- see commands.c for the exact
 * format. */
void cmd_gds_query(uart_instance_t *inst, char *args); /* GDS? -- OK 01=HIGH|LOW ... 12=HIGH|LOW */

/* QUADSPI connectivity test (PE12-PE15/PB10-PB11, W25Q128JVS) -- see
 * qspi_test.h. Issues the flash's standard JEDEC Read ID instruction
 * and reports the 3-byte ID; nothing else (no program/erase, no
 * memory-mapped access). Gated on QSPI_TEST_FEATURE_ENABLED
 * (qspi_test.h), same removability pattern as cmd_boot() above --
 * entirely absent, including cmd_parser.c's "QSPI:ID?" table row, when
 * that module is disabled. */
#if (QSPI_TEST_FEATURE_ENABLED != 0)
void cmd_qspi_id(uart_instance_t *inst, char *args); /* QSPI:ID? -- OK <MFR> <TYPE> <CAP>, hex bytes */
void cmd_sav(uart_instance_t *inst, char *args);                    /* *SAV <slot> [label] -- save settings and tables, IDLE only */
void cmd_rcl(uart_instance_t *inst, char *args);                    /* *RCL <slot> -- apply a saved profile, IDLE only */
void cmd_profile_catalog(uart_instance_t *inst, char *args);        /* CONFig:PROFile:CATalog? */
void cmd_profile_query(uart_instance_t *inst, char *args);          /* CONFig:PROFile? <slot> -- preview, nothing applied */
void cmd_profile_diff(uart_instance_t *inst, char *args);           /* CONFig:PROFile:DIFF? <slot> */
void cmd_profile_clear(uart_instance_t *inst, char *args);          /* CONFig:PROFile:CLEar <slot>, IDLE only */
void cmd_qspi_selftest(uart_instance_t *inst, char *args); /* QSPI:SELFtest -- erase/program/read-back of the scratch sector, IDLE only */
#endif

/* PFM_Input period/duty capture (PA15/PD4/PB2/PC12/PB4/PD12, TIM2/TIM3/
 * TIM4/TIM5) -- see pfm_input.h. Capture is synchronized to PFM shot
 * lifetime, not these commands directly: PFMIN:CAPTURE only arms a
 * target period count per channel for the NEXT FIRE (PfmInput_Arm());
 * the actual hardware capture starts inside PFM_Restart() (pfm.c) and
 * stops wherever pfm.c stops the shot, bounding a capture's runtime to
 * the shot that started it. Gated on PFM_INPUT_FEATURE_ENABLED
 * (pfm_input.h), same removability pattern as cmd_boot()/cmd_qspi_id()
 * above. */
#if (PFM_INPUT_FEATURE_ENABLED != 0)
void cmd_pfmin_capture(uart_instance_t *inst, char *args); /* PFMIN:CAPTURE <M> -- OK,
                                                                arms all 6 channels for
                                                                the next FIRE */
void cmd_pfmin_status(uart_instance_t *inst, char *args);  /* PFMIN:STATus? -- OK <n1>
                                                                .. <n6>, current
                                                                captured-period counts */
void cmd_pfmin_data(uart_instance_t *inst, char *args);    /* PFMIN:DATA? <ch> -- OK
                                                                <count> <per1> <per2> ...,
                                                                raw ticks (period only,
                                                                no duty), ch = 1-6 */

/* TEMPORARY debug command, 2026-09-09 -- see pfm_input.h's own
 * comment on PfmInput_GetDmaStartStatus(). Remove once DMA capture is
 * confirmed reliable. */
void cmd_pfmin_dmastat(uart_instance_t *inst, char *args); /* PFMIN:DMASTAT? -- OK
                                                                <s1> .. <s6>, last
                                                                HAL_TIM_IC_Start_DMA()
                                                                return code per channel */

/* Raw both-edge mode (duty-cycle re-exploration, 2026-09-30) -- see cmd_pfmin.c */
void cmd_pfmin_edges(uart_instance_t *inst, char *args);         /* PFMIN:EDGes <ch> RISING|BOTH -- OK */
void cmd_pfmin_edges_query(uart_instance_t *inst, char *args);   /* PFMIN:EDGes? -- OK R B R R R R */
void cmd_pfmin_raw(uart_instance_t *inst, char *args);           /* PFMIN:RAW? <ch> [<start> [<count>]] -- OK <n> OVERCAP=<k> LEVEL=<l> SUM=<s> <t0> <d1> ... */
#endif

/* DIAGnostic:OPTBytes?/RSTCause -- boot-source option bits and reset-cause flags, see cmd_system.c */
void cmd_diag_optbytes_query(uart_instance_t *inst, char *args);   /* DIAGnostic:OPTBytes? */
void cmd_diag_rstcause_query(uart_instance_t *inst, char *args);   /* DIAGnostic:RSTCause? */
void cmd_diag_rstcause_clear(uart_instance_t *inst, char *args);   /* DIAGnostic:RSTCause:CLEar */

/* FWUPdate:* -- in-application firmware update, see cmd_fwupdate.c */
void cmd_fwup_begin(uart_instance_t *inst, char *args);     /* FWUPdate:BEGin <size> <crc32hex> */
void cmd_fwup_data(uart_instance_t *inst, char *args);      /* FWUPdate:DATA <offsethex> <hex> */
void cmd_fwup_end(uart_instance_t *inst, char *args);       /* FWUPdate:END */
void cmd_fwup_swap(uart_instance_t *inst, char *args);      /* FWUPdate:SWAP */
void cmd_fwup_rollback(uart_instance_t *inst, char *args);  /* FWUPdate:ROLLback */
void cmd_fwup_abort(uart_instance_t *inst, char *args);     /* FWUPdate:ABORt */
void cmd_fwup_status(uart_instance_t *inst, char *args);    /* FWUPdate:STATus? */

/* Operating state (state_machine.h), see cmd_state.c */
void cmd_fault_sources_query(uart_instance_t *inst, char *args); /* FAULT:SOURces? -- OK PC10=.. GATEDRV=.. TABLE=.. TEST=.. */
void cmd_arm(uart_instance_t *inst, char *args);                 /* ARM -- IDLE -> ARMED */
void cmd_disarm(uart_instance_t *inst, char *args);              /* DISARM -- ARMED -> IDLE */
void cmd_state_query(uart_instance_t *inst, char *args);         /* STATE? -- OK IDLE|ARMED|FIRING|FAULT GENERAL */
void cmd_general_test_fault(uart_instance_t *inst, char *args);  /* GENERAL:TEST:FAULT -- software fault injection */

/* Supply mode (supply_mode.h), see cmd_config.c / cmd_table.c */
void cmd_config_mode(uart_instance_t *inst, char *args);          /* CONFig:MODE LIMiter|HVPS */
void cmd_config_mode_query(uart_instance_t *inst, char *args);    /* CONFig:MODE? -- OK NONE|LIMITER|HVPS */
void cmd_config_limits_query(uart_instance_t *inst, char *args);  /* CONFig:LIMits? -- OK <mode> FREQ <min> <max> DUTY <min> <max> */
void CmdTable_Discard(void);                                      /* drops the table; called on a mode change */

/* Closed loop (control_runtime.h), see cmd_control.c */
void cmd_config_pid_rate(uart_instance_t *inst, char *args);            /* CONFig:PIDRate */
void cmd_config_pid_rate_query(uart_instance_t *inst, char *args);      /* CONFig:PIDRate? */
void cmd_config_adc_average(uart_instance_t *inst, char *args);            /* CONFig:ADC:AVERage */
void cmd_config_adc_average_query(uart_instance_t *inst, char *args);      /* CONFig:ADC:AVERage? */
void cmd_config_adc_sample(uart_instance_t *inst, char *args);            /* CONFig:ADC:SAMPle */
void cmd_config_adc_sample_query(uart_instance_t *inst, char *args);      /* CONFig:ADC:SAMPle? */
void cmd_config_slew_rate(uart_instance_t *inst, char *args);            /* CONFig:SLEWRate */
void cmd_config_slew_rate_query(uart_instance_t *inst, char *args);      /* CONFig:SLEWRate? */
void cmd_config_max_volts(uart_instance_t *inst, char *args);            /* CONFig:MAXVolts */
void cmd_config_max_volts_query(uart_instance_t *inst, char *args);      /* CONFig:MAXVolts? */
void cmd_config_fb_scale(uart_instance_t *inst, char *args);            /* CONFig:FEEDback:SCALE */
void cmd_config_fb_scale_query(uart_instance_t *inst, char *args);      /* CONFig:FEEDback:SCALE? */
void cmd_config_fb_offset(uart_instance_t *inst, char *args);            /* CONFig:FEEDback:OFFSet */
void cmd_config_fb_offset_query(uart_instance_t *inst, char *args);      /* CONFig:FEEDback:OFFSet? */
void cmd_config_fb_range(uart_instance_t *inst, char *args);             /* CONFig:FEEDback:RANGE */
void cmd_config_fb_range_query(uart_instance_t *inst, char *args);       /* CONFig:FEEDback:RANGE? */
void cmd_config_fb_dacratio(uart_instance_t *inst, char *args);          /* CONFig:FEEDback:DACRatio */
void cmd_config_fb_dacratio_query(uart_instance_t *inst, char *args);    /* CONFig:FEEDback:DACRatio? */
void cmd_config_fb_filter(uart_instance_t *inst, char *args);            /* CONFig:FEEDback:FILTer */
void cmd_config_fb_filter_query(uart_instance_t *inst, char *args);      /* CONFig:FEEDback:FILTer? */
void cmd_config_fb_clip(uart_instance_t *inst, char *args);            /* CONFig:FEEDback:CLIPTime */
void cmd_config_fb_clip_query(uart_instance_t *inst, char *args);      /* CONFig:FEEDback:CLIPTime? */
void cmd_config_fb_channel(uart_instance_t *inst, char *args);            /* CONFig:FEEDback:CHANnel */
void cmd_config_fb_channel_query(uart_instance_t *inst, char *args);      /* CONFig:FEEDback:CHANnel? */
void cmd_config_fb_source(uart_instance_t *inst, char *args);            /* CONFig:FEEDback:SOURce */
void cmd_config_fb_source_query(uart_instance_t *inst, char *args);      /* CONFig:FEEDback:SOURce? */
void cmd_config_cl_duty(uart_instance_t *inst, char *args);            /* CONFig:CLOSEDloop:DUTY */
void cmd_config_cl_duty_query(uart_instance_t *inst, char *args);      /* CONFig:CLOSEDloop:DUTY? */
void cmd_config_cl_freq(uart_instance_t *inst, char *args);            /* CONFig:CLOSEDloop:FREQuency */
void cmd_config_cl_freq_query(uart_instance_t *inst, char *args);      /* CONFig:CLOSEDloop:FREQuency? */
void cmd_config_cl_res(uart_instance_t *inst, char *args);            /* CONFig:CLOSEDloop:RESonance */
void cmd_config_cl_res_query(uart_instance_t *inst, char *args);      /* CONFig:CLOSEDloop:RESonance? */
void cmd_config_cl_vpeak(uart_instance_t *inst, char *args);            /* CONFig:CLOSEDloop:VPEAK */
void cmd_config_cl_vpeak_query(uart_instance_t *inst, char *args);      /* CONFig:CLOSEDloop:VPEAK? */
void cmd_config_cl_vscale(uart_instance_t *inst, char *args);            /* CONFig:CLOSEDloop:VSCALE */
void cmd_config_cl_vscale_query(uart_instance_t *inst, char *args);      /* CONFig:CLOSEDloop:VSCALE? */
void cmd_config_cl_branch(uart_instance_t *inst, char *args);            /* CONFig:CLOSEDloop:BRANCH */
void cmd_config_cl_branch_query(uart_instance_t *inst, char *args);      /* CONFig:CLOSEDloop:BRANCH? */
void cmd_curve_begin(uart_instance_t *inst, char *args);                 /* CONFig:CLOSEDloop:CURVe:BEGin */
void cmd_curve_step(uart_instance_t *inst, char *args);                  /* CONFig:CLOSEDloop:CURVe:STEP <Hz> <volts> */
void cmd_curve_end(uart_instance_t *inst, char *args);                   /* CONFig:CLOSEDloop:CURVe:END */
void cmd_curve_clear(uart_instance_t *inst, char *args);                 /* CONFig:CLOSEDloop:CURVe:CLEar (back to the linear default) */
void cmd_curve_query(uart_instance_t *inst, char *args);                 /* CONFig:CLOSEDloop:CURVe? */
void cmd_curve_point_query(uart_instance_t *inst, char *args);           /* CONFig:CLOSEDloop:CURVe:POINt? <i> */
void cmd_sim_plant_shape(uart_instance_t *inst, char *args);             /* SIM:PLANT:SHAPE LINEAR|LORENTZ */
void cmd_sim_plant_shape_query(uart_instance_t *inst, char *args);       /* SIM:PLANT:SHAPE? */
void cmd_sim_plant_tau(uart_instance_t *inst, char *args);            /* SIM:PLANT:TAU */
void cmd_sim_plant_tau_query(uart_instance_t *inst, char *args);      /* SIM:PLANT:TAU? */
void cmd_sim_plant_gain(uart_instance_t *inst, char *args);            /* SIM:PLANT:GAIN */
void cmd_sim_plant_gain_query(uart_instance_t *inst, char *args);      /* SIM:PLANT:GAIN? */
void cmd_sim_plant_res(uart_instance_t *inst, char *args);            /* SIM:PLANT:RESonance */
void cmd_sim_plant_res_query(uart_instance_t *inst, char *args);      /* SIM:PLANT:RESonance? */
void cmd_sim_plant_width(uart_instance_t *inst, char *args);            /* SIM:PLANT:WIDTH */
void cmd_sim_plant_width_query(uart_instance_t *inst, char *args);      /* SIM:PLANT:WIDTH? */
void cmd_sim_plant_vpeak(uart_instance_t *inst, char *args);            /* SIM:PLANT:VPEAK */
void cmd_sim_plant_vpeak_query(uart_instance_t *inst, char *args);      /* SIM:PLANT:VPEAK? */
void cmd_sim_plant_vscale(uart_instance_t *inst, char *args);            /* SIM:PLANT:VSCALE */
void cmd_sim_plant_vscale_query(uart_instance_t *inst, char *args);      /* SIM:PLANT:VSCALE? */
void cmd_pid_loopmode(uart_instance_t *inst, char *args);            /* PID:LOOPMODE */
void cmd_pid_loopmode_query(uart_instance_t *inst, char *args);      /* PID:LOOPMODE? */
void cmd_pid_gains(uart_instance_t *inst, char *args);            /* PID:GAINS */
void cmd_pid_gains_query(uart_instance_t *inst, char *args);      /* PID:GAINS? */
void cmd_source_setpoint(uart_instance_t *inst, char *args);            /* SOURce:SETpoint */
void cmd_source_setpoint_query(uart_instance_t *inst, char *args);      /* SOURce:SETpoint? */
void cmd_source_run(uart_instance_t *inst, char *args);            /* SOURce:RUN */
void cmd_source_stop(uart_instance_t *inst, char *args);            /* SOURce:STOP */
void cmd_source_status(uart_instance_t *inst, char *args);            /* SOURce:STATus? */
void cmd_target_begin(uart_instance_t *inst, char *args);            /* TARGet:BEGin */
void cmd_target_step(uart_instance_t *inst, char *args);            /* TARGet:STEP */
void cmd_target_end(uart_instance_t *inst, char *args);            /* TARGet:END */
void cmd_target_clear(uart_instance_t *inst, char *args);            /* TARGet:CLEar */
void cmd_target_query(uart_instance_t *inst, char *args);            /* TARGet? */
void cmd_target_point_query(uart_instance_t *inst, char *args);            /* TARGet:POINt? */
void cmd_log_arm(uart_instance_t *inst, char *args);            /* LOG:ARM */
void cmd_log_data(uart_instance_t *inst, char *args);            /* LOG:DATA? */
uint8_t Cmd_ReportCtlResult(uart_instance_t *inst, ctl_result_t r);   /* ERR for a failed closed-loop start; 1 if r was an error */
void CmdTarget_Discard(void);                                      /* drops the target profile */

void cmd_gds_last_query(uart_instance_t *inst, char *args);       /* GDS:LAST? -- pins at the last latched fault */

/* Analog inputs (adc.h), see cmd_adc.c */
void cmd_adc_input_query(uart_instance_t *inst, char *args);      /* ADC:INPut? <1-4> -- OK <volts> <code> */
void cmd_adc_status_query(uart_instance_t *inst, char *args);     /* ADC:STATus? -- OK <running> <tickHz> <scans> <sampleX10> <scanUs> <blocks> */
void cmd_adc_vref_query(uart_instance_t *inst, char *args);       /* ADC:VREF? -- OK <volts> <vrefint code> <factory code> */

/* Analog outputs (dac.h), see cmd_dac.c */
void cmd_dac_output(uart_instance_t *inst, char *args);           /* DAC:OUTPut <1|2> <volts> */
void cmd_dac_output_query(uart_instance_t *inst, char *args);     /* DAC:OUTPut? <1|2> -- OK <volts> <code> <register code> */

/* Time-series shot (shot_profile.h), see cmd_shot.c */
void cmd_shot_begin(uart_instance_t *inst, char *args);           /* SHOT:BEGin */
void cmd_shot_step(uart_instance_t *inst, char *args);            /* SHOT:STEP <t_us> <freq_hz> <duty_pct> */
void cmd_shot_end(uart_instance_t *inst, char *args);             /* SHOT:END -- OK <setpoints> <duration_us> */
void cmd_shot_clear(uart_instance_t *inst, char *args);           /* SHOT:CLEar */
void cmd_shot_query(uart_instance_t *inst, char *args);           /* SHOT? -- OK <setpoints> <duration_us> <PROFILE|TABLE> */
void cmd_shot_point_query(uart_instance_t *inst, char *args);     /* SHOT:POINt? <i> -- OK <t_us> <freq_hz> <duty_pct> */
void CmdShot_Discard(void);                                       /* drops the shot; called on a mode change */

/* Matches a received line against the command table and runs the handler
   (or replies ERR 1). Called from the SCPI task (task_scpi.c). */
void Commands_Dispatch(uart_instance_t *inst, char *line);

#ifdef __cplusplus
}
#endif

#endif /* __COMMANDS_H__ */
