/*
 * command_table.c -- the serial command table and the dispatch entry point.
 *
 * Matching (SCPI-style hierarchical mnemonics, short/long form, '?' query
 * suffix) is src/middleware/scpi/scpi_parser.c -- see its header for the
 * pattern syntax. This file is the product's own part: which pattern runs
 * which handler.
 *
 * To add a command
 * -----------------
 *  1. Implement the handler in the matching cmd_<subsystem>.c
 *  2. Declare it in commands.h
 *  3. Add a { "PATTern:MNEMonic?", handler } row to command_table[]
 *     below. Nothing else changes -- the table is flat, so adding a new
 *     leaf or a whole new subsystem is always just one more row.
 */

#include "commands.h"
#include "cmd_common.h"
#include "scpi_parser.h"
#include "boot_jump.h"
#include "qspi_test.h"
#include "pfm_input.h"

static const scpi_command_t command_table[] = {
    /* Common commands (IEEE 488.2) */
    { "*IDN?",              cmd_idn          },

    /* Serial-bootloader entry -- excluded entirely (not just an
       unreachable row) when BOOT_JUMP_FEATURE_ENABLED is 0, so BOOT
       falls through to "ERR 1 Unknown command" like any other
       unrecognized mnemonic. See boot_jump.h. */
#if (BOOT_JUMP_FEATURE_ENABLED != 0)
    { "BOOT",                cmd_boot         },
#endif

    /* PFM table upload -- see commands.c's own header comment on
       these four handlers. */
    { "TABle:BEGin",         cmd_table_begin  },
    { "TABle:STEP",          cmd_table_step   },
    { "TABle:END",           cmd_table_end    },
    { "TABle?",              cmd_table_query  },

    /* The closed loop -- see cmd_control.c; mirrors XREX's PID:/SOURce:/LOG:/CONFig: pattern. */
    { "CONFig:PIDRate",                     cmd_config_pid_rate },
    { "CONFig:PIDRate?",                    cmd_config_pid_rate_query },
    { "CONFig:ADC:AVERage",                 cmd_config_adc_average },
    { "CONFig:ADC:AVERage?",                cmd_config_adc_average_query },
    { "CONFig:ADC:SAMPle",                  cmd_config_adc_sample },
    { "CONFig:ADC:SAMPle?",                 cmd_config_adc_sample_query },
    { "CONFig:SLEWRate",                    cmd_config_slew_rate },
    { "CONFig:SLEWRate?",                   cmd_config_slew_rate_query },
    { "CONFig:MAXVolts",                    cmd_config_max_volts },
    { "CONFig:MAXVolts?",                   cmd_config_max_volts_query },
    { "CONFig:FEEDback:SCALE",              cmd_config_fb_scale },
    { "CONFig:FEEDback:SCALE?",             cmd_config_fb_scale_query },
    { "CONFig:FEEDback:OFFSet",             cmd_config_fb_offset },
    { "CONFig:FEEDback:OFFSet?",            cmd_config_fb_offset_query },
    { "CONFig:FEEDback:RANGE",              cmd_config_fb_range },
    { "CONFig:FEEDback:RANGE?",             cmd_config_fb_range_query },
    { "CONFig:FEEDback:DACRatio",           cmd_config_fb_dacratio },
    { "CONFig:FEEDback:DACRatio?",          cmd_config_fb_dacratio_query },
    { "CONFig:FEEDback:FILTer",             cmd_config_fb_filter },
    { "CONFig:FEEDback:FILTer?",            cmd_config_fb_filter_query },
    { "CONFig:FEEDback:CLIPTime",           cmd_config_fb_clip },
    { "CONFig:FEEDback:CLIPTime?",          cmd_config_fb_clip_query },
    { "CONFig:FEEDback:CHANnel",            cmd_config_fb_channel },
    { "CONFig:FEEDback:CHANnel?",           cmd_config_fb_channel_query },
    { "CONFig:FEEDback:SOURce",             cmd_config_fb_source },
    { "CONFig:FEEDback:SOURce?",            cmd_config_fb_source_query },
    { "CONFig:CLOSEDloop:DUTY",             cmd_config_cl_duty },
    { "CONFig:CLOSEDloop:DUTY?",            cmd_config_cl_duty_query },
    { "CONFig:CLOSEDloop:FREQuency",        cmd_config_cl_freq },
    { "CONFig:CLOSEDloop:FREQuency?",       cmd_config_cl_freq_query },
    { "CONFig:CLOSEDloop:RESonance",        cmd_config_cl_res },
    { "CONFig:CLOSEDloop:RESonance?",       cmd_config_cl_res_query },
    { "CONFig:CLOSEDloop:VPEAK",            cmd_config_cl_vpeak },
    { "CONFig:CLOSEDloop:VPEAK?",           cmd_config_cl_vpeak_query },
    { "CONFig:CLOSEDloop:VSCALE",           cmd_config_cl_vscale },
    { "CONFig:CLOSEDloop:VSCALE?",          cmd_config_cl_vscale_query },
    { "CONFig:CLOSEDloop:BRANCH",           cmd_config_cl_branch },
    { "CONFig:CLOSEDloop:BRANCH?",          cmd_config_cl_branch_query },
    { "CONFig:CLOSEDloop:CURVe:BEGin",      cmd_curve_begin },
    { "CONFig:CLOSEDloop:CURVe:STEP",       cmd_curve_step },
    { "CONFig:CLOSEDloop:CURVe:END",        cmd_curve_end },
    { "CONFig:CLOSEDloop:CURVe:CLEar",      cmd_curve_clear },
    { "CONFig:CLOSEDloop:CURVe?",           cmd_curve_query },
    { "CONFig:CLOSEDloop:CURVe:POINt?",     cmd_curve_point_query },
    { "SIM:PLANT:SHAPE",                    cmd_sim_plant_shape },
    { "SIM:PLANT:SHAPE?",                   cmd_sim_plant_shape_query },
    { "SIM:PLANT:TAU",                      cmd_sim_plant_tau },
    { "SIM:PLANT:TAU?",                     cmd_sim_plant_tau_query },
    { "SIM:PLANT:GAIN",                     cmd_sim_plant_gain },
    { "SIM:PLANT:GAIN?",                    cmd_sim_plant_gain_query },
    { "SIM:PLANT:RESonance",                cmd_sim_plant_res },
    { "SIM:PLANT:RESonance?",               cmd_sim_plant_res_query },
    { "SIM:PLANT:WIDTH",                    cmd_sim_plant_width },
    { "SIM:PLANT:WIDTH?",                   cmd_sim_plant_width_query },
    { "SIM:PLANT:VPEAK",                    cmd_sim_plant_vpeak },
    { "SIM:PLANT:VPEAK?",                   cmd_sim_plant_vpeak_query },
    { "SIM:PLANT:VSCALE",                   cmd_sim_plant_vscale },
    { "SIM:PLANT:VSCALE?",                  cmd_sim_plant_vscale_query },
    { "PID:LOOPMODE",                       cmd_pid_loopmode },
    { "PID:LOOPMODE?",                      cmd_pid_loopmode_query },
    { "PID:GAINS",                          cmd_pid_gains },
    { "PID:GAINS?",                         cmd_pid_gains_query },
    { "PID:FAULT?",          cmd_pid_fault_query   },
    { "PID:DIAG?",           cmd_pid_diag_query    },
    { "SOURce:SETpoint",                    cmd_source_setpoint },
    { "SOURce:SETpoint?",                   cmd_source_setpoint_query },
    { "SOURce:RUN",                         cmd_source_run },
    { "SOURce:STOP",                        cmd_source_stop },
    { "SOURce:STATus?",                     cmd_source_status },
    { "TARGet:BEGin",                       cmd_target_begin },
    { "TARGet:STEP",                        cmd_target_step },
    { "TARGet:END",                         cmd_target_end },
    { "TARGet:CLEar",                       cmd_target_clear },
    { "TARGet?",                            cmd_target_query },
    { "TARGet:POINt?",                      cmd_target_point_query },
    { "LOG:ARM",                            cmd_log_arm },
    { "LOG:DATA?",                          cmd_log_data },

    { "GDS:LAST?",           cmd_gds_last_query   },

    /* The analog inputs -- see cmd_adc.c. */
    { "ADC:INPut?",          cmd_adc_input_query  },
    { "ADC:STATus?",         cmd_adc_status_query },
    { "ADC:VREF?",           cmd_adc_vref_query   },

    /* The two analog outputs, PA4/PA5 -- see cmd_dac.c. */
    { "DAC:OUTPut",          cmd_dac_output       },
    { "DAC:OUTPut?",         cmd_dac_output_query },

    /* The time-series shot -- see cmd_shot.c. */
    { "SHOT:BEGin",          cmd_shot_begin       },
    { "SHOT:STEP",           cmd_shot_step        },
    { "SHOT:END",            cmd_shot_end         },
    { "SHOT:CLEar",          cmd_shot_clear       },
    { "SHOT?",               cmd_shot_query       },
    { "SHOT:POINt?",         cmd_shot_point_query },

    /* Begins PWM output -- see commands.c's own header comment on
       cmd_fire(). */
    { "FIRE",                cmd_fire         },

    /* PFM table-advance ISR timing diagnostic -- see commands.c's own
       header comment on cmd_pfm_diag(). */
    { "PFM:DIAG?",           cmd_pfm_diag     },

    /* TEMPORARY, see commands.h -- raw per-call gap log behind
       PFM:DIAG?'s aggregate max-gap number. */
    { "PFM:GAPLOG?",         cmd_pfm_gaplog   },

    /* Reports the compile-time HRTIM channel count -- see commands.c's
       own header comment on cmd_config_channels(). */
    { "PHASe:ENAble",        cmd_phase_enable },
    { "PHASe:ENAble?",       cmd_phase_enable_query },
    { "PHASe:SELect",        cmd_phase_select },
    { "PHASe:SELect?",       cmd_phase_select_query },
    { "*SAV",                cmd_sav },
    { "*RCL",                cmd_rcl },
    { "CONFig:PROFile:CATalog?", cmd_profile_catalog },
    { "CONFig:PROFile?",     cmd_profile_query },
    { "CONFig:PROFile:DIFF?", cmd_profile_diff },
    { "CONFig:PROFile:CLEar", cmd_profile_clear },
    { "CONFig:CHANnels?",    cmd_config_channels },
    { "CONFig:MODE",         cmd_config_mode           },
    { "CONFig:MODE?",        cmd_config_mode_query     },
    { "CONFig:LIMits?",      cmd_config_limits_query   },

    /* PC10/HRTIM1_FLT6 hardware fault status/clear -- see commands.c's
       own header comment on cmd_fault_query()/cmd_fault_clear(). */
    /* In-application firmware update (dual-bank) and boot diagnostics --
       see cmd_fwupdate.c / flash_bank.h and cmd_system.c. */
    { "FWUPdate:BEGin",      cmd_fwup_begin       },
    { "FWUPdate:DATA",       cmd_fwup_data        },
    { "FWUPdate:END",        cmd_fwup_end         },
    { "FWUPdate:SWAP",       cmd_fwup_swap        },
    { "FWUPdate:ROLLback",   cmd_fwup_rollback    },
    { "FWUPdate:ABORt",      cmd_fwup_abort       },
    { "FWUPdate:STATus?",    cmd_fwup_status      },
    { "DIAGnostic:OPTBytes?",      cmd_diag_optbytes_query },
    { "DIAGnostic:RSTCause?",      cmd_diag_rstcause_query },
    { "DIAGnostic:RSTCause:CLEar", cmd_diag_rstcause_clear },

    { "FAULT?",              cmd_fault_query  },
    { "ARM",                 cmd_arm          },
    { "DISARM",              cmd_disarm       },
    { "STATE?",              cmd_state_query  },
    { "GENERAL:TEST:FAULT",  cmd_general_test_fault },
    { "FAULT:SOURces?",      cmd_fault_sources_query },
    { "FAULT:CLEar",         cmd_fault_clear  },

    /* Raw GateDriverStatus_01..12 (PE0..PE11) diagnostic readback --
       see commands.c's own header comment on cmd_gds_query(). */
    { "GDS?",                cmd_gds_query    },
    { "SYS:ID?",             cmd_sys_id_query },
    { "SYS:SERial",          cmd_sys_serial         },
    { "SYS:SERial?",         cmd_sys_serial_query   },
    { "SYS:LABel",           cmd_sys_label          },
    { "SYS:LABel?",          cmd_sys_label_query    },
    { "SYS:ID:CLEar",        cmd_sys_id_clear       },
    { "DIAGnostic:LEDs?",    cmd_diag_leds_query },

    /* QUADSPI connectivity test (W25Q128JVS) -- excluded entirely when
       QSPI_TEST_FEATURE_ENABLED is 0, same removability pattern as
       BOOT above. See commands.c's own header comment on
       cmd_qspi_id(). */
#if (QSPI_TEST_FEATURE_ENABLED != 0)
    { "QSPI:ID?",            cmd_qspi_id      },
    { "QSPI:SELFtest",       cmd_qspi_selftest },
#endif

    /* PFM_Input period/duty capture -- excluded entirely when
       PFM_INPUT_FEATURE_ENABLED is 0, same removability pattern as
       BOOT/QSPI:ID? above. See commands.c's own header comment on
       cmd_pfmin_capture()/cmd_pfmin_status()/cmd_pfmin_data(). */
#if (PFM_INPUT_FEATURE_ENABLED != 0)
    { "PFMIN:CAPTURE",       cmd_pfmin_capture },
    { "PFMIN:STATus?",       cmd_pfmin_status  },
    { "PFMIN:DMASTAT?",      cmd_pfmin_dmastat },  /* TEMPORARY, see commands.h */
    { "PFMIN:DATA?",         cmd_pfmin_data    },
    { "PFMIN:EDGes",         cmd_pfmin_edges       },
    { "PFMIN:EDGes?",        cmd_pfmin_edges_query },
    { "PFMIN:RAW?",          cmd_pfmin_raw         },
#endif
};

#define NUM_COMMANDS  (sizeof(command_table) / sizeof(command_table[0]))

void Commands_Dispatch(uart_instance_t *inst, char *line)
{
    if (scpi_dispatch(command_table, NUM_COMMANDS, inst, line) == SCPI_UNKNOWN)
    {
        Scpi_SendErr(inst, ERR_UNKNOWN_COMMAND, "Unknown command");
    }
}
