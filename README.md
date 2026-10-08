# WHAM controllers

Firmware, hardware files and host software for the WHAM controller board (STM32G474QET6) and the two
products that run on it.

```
firmware/
  xrex/      WHAM-XREX-PFMG474    Transrex magnet-supply PFM PID controller
  hbridge/   WHAM-HBRIDGE-PFMG474 3-phase H-bridge (Limiter PWM / HVPS PFM)
hardware/    board files, pin map, hardware notes (see hardware/README.md)
host/        host-side software that talks to the boards: the per-controller service (see docs/)
docs/        design documents that span products (identity, host service, repo layout)
```

Each firmware directory is a complete, self-contained project (its own AGENTS.md, build, tests, docs) and keeps its own
history, imported from the former standalone repositories. Code that both products share is still copied byte-for-byte
and guarded by `firmware/<product>/python/check_shared.py`; see each project's `docs/saved_profiles_shared.md`.

## History

This repository replaces the standalone `WHAM-XREX-PFMG474` and `WHAM-HBRIDGE-PFMG474` repositories (imported on 2026-10-07 with their
full histories; the old repositories stop at that point and are no longer updated). All the projects are maintained here from now on:
firmware for both products, the hardware files, the host software and the design documents. `docs/device_identity_and_host_service.md`
is the plan for board identity and the host service.

## License

The software is licensed under the GNU Affero General Public License v3.0 only (`AGPL-3.0-only`): see [`LICENSE`](LICENSE).
What that covers, what keeps its own license (the ST and Arm vendor code), and what is not licensed yet (documents and
hardware designs) is spelled out in [`LICENSING.md`](LICENSING.md).
