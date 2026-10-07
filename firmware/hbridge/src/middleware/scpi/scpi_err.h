/*
 * scpi_err.h -- the numeric error codes of the serial command protocol: "ERR <code> <message>".
 *
 * SHARED, byte-identical in WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474 (python/check_shared.py), and
 * described once in docs/scpi_conventions.md. A code means the same thing in both products or is used by
 * only one of them: codes are NEVER renumbered or reused, so host tooling can rely on them; a product
 * simply never sends a code it has no use for. Retire a code by leaving it unused.
 */
#ifndef SCPI_ERR_H
#define SCPI_ERR_H

typedef enum
{
    ERR_UNKNOWN_COMMAND     = 1,   /* no command_table[] pattern matched */
    ERR_TABLE_NOT_UPLOADING = 2,   /* a *:STEP or *:END without its *:BEGin (TABle:, SHOT:, TARGet:, CURVe:) */
    ERR_TABLE_FULL          = 3,   /* an upload table or time series is full */
    ERR_TABLE_STEP_INVALID  = 4,   /* an uploaded step is malformed (count, range, ordering) */
    ERR_TABLE_EMPTY         = 5,   /* nothing uploaded to play (FIRE and friends) */
    ERR_FAULT_LATCHED       = 6,   /* a fault is latched -- FAULT:CLEar required */
    ERR_QSPI_FAILED         = 7,   /* QUADSPI / external flash command failed or timed out */
    ERR_PFMIN_BAD_CHANNEL   = 8,   /* PFM_Input channel not 1-6 */
    ERR_PFMIN_BAD_COUNT     = 9,   /* PFMIN:CAPTURE M out of range */
    ERR_OUT_OF_MODE_LIMITS  = 10,  /* a step's carrier frequency or duty is outside the selected supply mode's
                                      limits (HBRIDGE); in XREX the same code means "carrier above the maximum" */
    ERR_CARRIER_TOO_HIGH    = ERR_OUT_OF_MODE_LIMITS,   /* XREX's name for code 10 */
    ERR_INVALID_CHANNEL     = 11,  /* channel (XREX: 1-N) or control loop (HBRIDGE: 1) not valid */
    ERR_INVALID_ARGS        = 12,  /* wrong argument count, not a number, or out of range */
    ERR_INVALID_STATE       = 13,  /* not allowed in the current state */
    ERR_INVALID_NICKNAME    = 14,  /* XREX only: CHANnel:NICKname rules */
    ERR_EXT_ENABLE_LOW      = 15,  /* XREX only: EXTernal:ENAble on and the PF13 interlock reads LOW */
    /* 16: retired (an old XREX external-trigger coupling) -- do not reuse */
    ERR_FWUPDATE            = 17,  /* FWUPdate:* flash/CRC/image/option-byte failure */
    ERR_PROFILE             = 18,  /* saved profiles (*SAV, *RCL, CONFig:PROFile): empty or corrupt slot, or the flash failed */
    ERR_IDENTITY            = 19   /* SYS:SERial / SYS:LABel / SYS:ID:CLEar refused: a serial is already provisioned (clear it first),
                                      none is provisioned yet, or the stored record is unusable */
} cmd_err_t;

#endif /* SCPI_ERR_H */
