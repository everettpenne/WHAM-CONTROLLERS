#ifndef INC_CTRLR_CONFIG_H_
#define INC_CTRLR_CONFIG_H_

/*
 * ctrlr_config.h
 *
 * The project's one compile-time config file -- board/firmware
 * IDENTITY (HW_BOARD_NAME/REV, FW_VERSION_STRING) and general
 * controller-BEHAVIOR settings (HRTIM_NUM_CHANNELS, GDS_FAULT_POLARITY)
 * together, matching the sibling PFM-STM32G474 project's single
 * supply_config.h. This was briefly split (2026-09-08: identity stayed
 * in version.h, behavior moved here) on the reasoning that identity and
 * behavior are different KINDS of thing -- re-condensed the same day,
 * per project decision, back to one file. version.h no longer exists;
 * don't recreate it.
 *
 * (Module-local, self-contained flags are a further, older exception --
 * e.g. BOOT_JUMP_FEATURE_ENABLED lives in boot_jump.h itself, because
 * that module's whole design point is being independently removable in
 * one place. Don't move flags like that here.)
 */

/* --------------------------------------------------------------------------
 * Board/product identity. HW_BOARD_REV is a placeholder -- set it to
 * match this build's actual PCB silkscreen revision and bump it on
 * every hardware respin so *IDN's report always matches what's
 * physically in hand.
 * -------------------------------------------------------------------------- */
#define HW_BOARD_NAME     "WHAM-HBRIDGE-PFMG474"
#define HW_BOARD_REV      "REVA"

/* Firmware identity. Bump on every release the way the sibling
 * PFM-STM32G474 project does (FW_VERSION_STRING in its
 * supply_config.h). */
#define FW_VERSION_STRING "v0.9"

/* --------------------------------------------------------------------------
 * HRTIM channel count
 *
 * docs/pin_mapping_v4.csv wires out all 6 HRTIM1 channel pairs (A-F /
 * U,V,W,X,Y,Z) on this board. HRTIM_NUM_CHANNELS selects how many of
 * them hrtim.c/pfm.c actually drive, phase-locked and evenly spaced
 * around a shared carrier, as a real compile-time setting
 * (2026-09-08) -- fix it here, rebuild, reflash. It is NOT modifiable
 * at runtime, by design: no variable backs this anywhere, only this
 * #define, so "changing it after the firmware is flashed" isn't a
 * thing that can happen short of building and flashing a different
 * firmware image.
 *
 * Range is 1-5, not 1-6: the HRTIM Master timer has exactly 4 compare
 * registers (MCMP1R-MCMP4R), giving 5 total trigger points (its own
 * PER event + 4 CMPs) -- enough for channels A-E phase-locked to the
 * Master, but not a 6th (F) without a genuinely different (cross-timer)
 * sync scheme. That scheme is NOT implemented here -- the HAL does
 * expose the register support for it (HRTIM_TIMRESETTRIGGER_OTHERx_
 * CMPy, letting one slave timer's reset trigger chain off another
 * slave's compare event instead of the Master's), but it needs new
 * phase math with no existing pattern in this codebase to build from,
 * and was deliberately left as a follow-up rather than bundled into
 * this change -- see AGENTS.md.
 *
 * Channels beyond HRTIM_NUM_CHANNELS (up through Timer F) stay
 * pin/dead-time-reserved but unlocked (ResetTrigger = NONE) and are
 * never started by HRTIM1_PWM_Start() -- exactly how Timers D/E/F
 * behaved before this config existed for the N=3 default. See
 * HRTIM1_FullInit()'s own comments in hrtim.c for the exact mechanism.
 *
 * Raising this value automatically extends the 2026-09-09 HRTIM
 * SET/RESET-collision fix (hrtim.c's HRTIM1_FullInit(), the CMP3-based
 * output SET source) to whichever channels newly become active --
 * verified, not assumed: every place that fix touches (the CMP3
 * compare-register config loop, the per-channel SetSource selector,
 * and HRTIM1_PWM_Start()'s cold-start reforceActive handling) is keyed
 * off this constant, not a hardcoded channel count. No further code
 * changes are needed here when raising N; just re-verify the new
 * channel(s) on real hardware the same way U/V/W were (frequency-ramp
 * table + DSLogic capture -- see docs/changelog.txt), since this fix
 * was derived from real-hardware evidence, not proven in general.
 * -------------------------------------------------------------------------- */
#define HRTIM_NUM_CHANNELS  (3U)

#if (HRTIM_NUM_CHANNELS < 1U) || (HRTIM_NUM_CHANNELS > 5U)
#error "HRTIM_NUM_CHANNELS must be 1-5 -- see the comment above it in " \
       "ctrlr_config.h for why 6 isn't a simple extension of this scheme."
#endif

/* --------------------------------------------------------------------------
 * GateDriverStatus fault polarity (compile-time)
 *
 * GDS_NORMALLY_HIGH -- pins read HIGH in good operation;
 *                      a LOW reading is a fault.
 * GDS_NORMALLY_LOW  -- pins read LOW in good operation;
 *                      a HIGH reading is a fault.
 *
 * Governs both gate_driver.c's GateDriver_CheckFault() (the EXTI-driven
 * fault interrupt on PE0..PE11, see gate_driver.h) and nothing else --
 * GDS? (commands.c) is a raw, polarity-agnostic HIGH/LOW readback and
 * does not consult this value.
 *
 * Set to NORMALLY_LOW per direct confirmation against this board's
 * actual gate driver ICs, 2026-09-08 -- matching the sibling
 * PFM-STM32G474 project's own configured default (GDS_FAULT_POLARITY =
 * GDS_NORMALLY_LOW in supply_config.h), even though that project's own
 * comment calls NORMALLY_HIGH "the safest" choice in the abstract (a
 * disconnected pin reads LOW and immediately trips) -- the actual wiring
 * on real hardware is the deciding fact here, not the abstract argument.
 *
 * Known consequence, confirmed before choosing this value, not
 * discovered by surprise: a GDS? snapshot taken earlier this session
 * showed GateDriverStatus_03 (PE2) already reading HIGH while the other
 * 11 pins read LOW. Under NORMALLY_LOW, that pin is *already* a fault
 * condition -- expect GateDriver_CheckFault() to latch a fault on it as
 * soon as this interrupt is live (very possibly immediately at boot, if
 * that pin is still HIGH by then). That may well be surfacing a real,
 * previously-invisible fault condition, which is the whole point of
 * this feature -- see docs/changelog.txt.
 *
 * Single source of truth, unlike the sibling project's own documented
 * history (supply_config.h's own comment describes GDS_FAULT_POLARITY
 * once being defined twice -- once there, once in gate_driver.h -- with
 * gate_driver.h's #ifndef silently winning the race every time,
 * making the copy in supply_config.h dead). Not a risk here structurally:
 * gate_driver.h includes THIS file rather than defining anything
 * itself, and this is the only place GDS_NORMALLY_HIGH/LOW/
 * GDS_FAULT_POLARITY are ever defined.
 * -------------------------------------------------------------------------- */
#define GDS_NORMALLY_HIGH    (0U)
#define GDS_NORMALLY_LOW     (1U)

#define GDS_FAULT_POLARITY   GDS_NORMALLY_LOW

/* --------------------------------------------------------------------------
 * Supply modes and their limits (added 2026-09-29)
 *
 * The same firmware drives two different H-bridge supplies; the operator
 * selects which one with CONFig:MODE (supply_mode.h) and TABle:STEP is then
 * checked against that mode's range. Values are the older PFM-STM32G474
 * project's own bounds (its supply_config.h, "Section 1a/1b, 3.1"), still
 * correct per the maintainer.
 *
 *   HVPS     PFM-driven:  10-30 kHz carrier, shared 40-60% duty
 *   LIMITER  PWM-driven:  75-125 kHz carrier, per-phase 5-95% duty
 *
 * Duty is checked as compare/period, the same relation the host table
 * builder uses (python/pfm_table_upload.py cmp_from_duty()). The modes only
 * NARROW the hardware envelope (16-bit period, the compare clamp, the
 * 100 ns dead time); they never widen it.
 *
 * VERIFICATION CAVEAT for LIMITER: this project's HRTIM SET/RESET-collision
 * fix (hrtim.c, 2026-09-09) was confirmed on a logic analyzer only up to
 * 100 kHz -- a frequency-ramp capture had found genuinely missing pulses
 * from about 85 kHz upward before the fix. 100-125 kHz is inside the
 * Limiter range but has NOT been scoped on real hardware; do that before
 * relying on it. (Until 2026-09-29 every TABle:STEP above 100 kHz was
 * rejected outright; PFM_MAX_CARRIER_FREQ_HZ below is now the highest
 * frequency any mode allows.)
 * -------------------------------------------------------------------------- */
#define HVPS_FREQ_MIN_HZ          (10000UL)
#define HVPS_FREQ_MAX_HZ          (30000UL)
#define HVPS_DUTY_MIN_PCT         (40U)
#define HVPS_DUTY_MAX_PCT         (60U)

#define LIMITER_FREQ_MIN_HZ       (75000UL)
#define LIMITER_FREQ_MAX_HZ       (125000UL)
#define LIMITER_DUTY_MIN_PCT      (5U)
#define LIMITER_DUTY_MAX_PCT      (95U)

/* Highest carrier frequency any mode allows. */
#define PFM_MAX_CARRIER_FREQ_HZ  LIMITER_FREQ_MAX_HZ

/* --------------------------------------------------------------------------
 * Shot memory (compile-time; change here and rebuild the .bin)
 *
 * A shot is programmed as a time series of setpoints (SHOT:STEP <t_us>
 * <freq_hz> <duty_pct>, shot_profile.h). The firmware linearly interpolates
 * between them one switching period at a time into a small per-period ring
 * table (pfm.c) that the per-period interrupt plays back and a low-priority
 * interrupt keeps refilled, so the shot's length does not depend on the
 * table's size.
 *
 *   SHOT_SETPOINT_MAX     time-series setpoints, 12 bytes each (500 = 6 kB).
 *                         At least 2: t = 0 first, then the shot's end.
 *   SHOT_MAX_DURATION_US  longest shot, and the largest time a setpoint may
 *                         have. Open-loop shots run from about 10 ms to 1 s.
 *   PFM_TABLE_SIZE        the per-period ring (pfm.h), 8 bytes per entry.
 *                         Even. Also the longest table TABle:* can upload
 *                         directly.
 *
 * Sizing PFM_TABLE_SIZE (measured on the simulator, docs/changelog.txt
 * 2026-10-01). The ring is filled before the first period and refilled by a
 * low-priority interrupt that gets only the CPU the per-period interrupt
 * leaves. That interrupt costs ~590 cycles per period when it has nothing to
 * apply and ~1,270 when every period changes the period or a compare (a ramp);
 * the refill needs ~240 cycles per entry. A period is 1,360 cycles at 125 kHz,
 * so:
 *   - HVPS (10-30 kHz): any size works, 1 s shots included (100 is enough).
 *   - LIMITER, constant or slowly varying: refilled fine up to 125 kHz.
 *   - LIMITER with a step applied EVERY period (a duty or frequency ramp):
 *     sustainable by refill alone only up to ~80 kHz. Above that the refill
 *     falls behind, the ring runs empty and the shot is stopped with a fault
 *     (FAULT:SOURces? TABLE=1). The ring then has to hold most of the shot:
 *     10 ms at 125 kHz is 1,250 periods, so 1,300 entries (10.4 kB), tested on
 *     every worst case (duty 5<->95 % at 125 kHz, 125->75.5 kHz ramps, ...):
 *     the whole shot is generated before the first period.
 * 1,300 is the default for that reason. A build for HVPS only can set 100 and
 * save 9.6 kB; RAM is 42.8 kB of 128 kB used at 1,300. Filling the ring takes
 * ~240 cycles per entry with interrupts masked inside FIRE (1.8 ms at 1,300).
 * -------------------------------------------------------------------------- */
#define SHOT_SETPOINT_MAX        (500U)
#define SHOT_MAX_DURATION_US     (1000000UL)
#define PFM_TABLE_SIZE           (1300U)

#if (SHOT_SETPOINT_MAX < 2U) || (SHOT_SETPOINT_MAX > 5000U)
#error "SHOT_SETPOINT_MAX must be 2..5000"
#endif
#if (PFM_TABLE_SIZE < 8U) || (PFM_TABLE_SIZE > 5000U) || ((PFM_TABLE_SIZE % 2U) != 0U)
#error "PFM_TABLE_SIZE must be even and 8..5000"
#endif
#if (SHOT_MAX_DURATION_US < 1000UL) || (SHOT_MAX_DURATION_US > 60000000UL)
#error "SHOT_MAX_DURATION_US must be 1 ms .. 60 s"
#endif

#endif /* INC_CTRLR_CONFIG_H_ */
