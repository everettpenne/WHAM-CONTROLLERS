# WHAM-HBRIDGE-PFMG474

Firmware for the H-bridge power supplies of the WHAM PFM/HRTIM controller
family, on an STM32G474QET6: the **Limiter** (PWM-driven) and the **HVPS**
(PFM-driven). Both are 3-channel outputs (Phase U+UN, V+VN, W+WN, with
complements and dead time) that read back 12 gate-drive fault signals.
Phase-locked, table-driven multi-channel PWM/PFM generation with a
serial (SCPI-style) command interface.

Renamed from `WHAM-SWITCH-PFMG474` (2026-09-29), which was itself a copy
of [WHAM-PFMG474-V4](https://github.com/everettpenne/WHAM-PFMG474-V4)
(2026-09-09). The sibling project
[WHAM-XREX-PFMG474](https://github.com/everettpenne/WHAM-XREX-PFMG474)
(closed-loop PID controller for the Transrex magnet supplies, same PCB)
is where the newer platform features were built first; they are being
ported here in phases. See [`AGENTS.md`](AGENTS.md) for the full
orientation and [`docs/changelog.txt`](docs/changelog.txt) for the dated
history.
