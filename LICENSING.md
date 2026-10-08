# Licensing

Copyright (C) 2026 Everett Penne.

## The software: GNU Affero General Public License, version 3

The software in this repository is licensed under the **GNU Affero General Public License v3.0 only**
(SPDX: `AGPL-3.0-only`). The full text is in [`LICENSE`](LICENSE).

In short: you may use, study, modify and share it, including commercially, but anything you distribute **or run as a
network service for others** that is based on it must be offered, with its complete source, under the same license (section 13
is what makes this apply to a service such as `host/wham_hub.py`). This file is a summary, not legal advice; the license text governs.

"The software" means what is written for this project:

| Path | |
|---|---|
| `firmware/*/src/` | the application, drivers, middleware and BSP source of both products |
| `firmware/*/tests/` | the off-target unit tests |
| `firmware/*/python/` | the host tools, build scripts and test drivers |
| `host/` | the host service, provisioning tool and their tests |
| `firmware/*/build/` | firmware images built from the above plus the vendor code below; they carry the licenses of the sources they are built from |

The Python files, scripts and makefiles that belong to the project are included; the project's own additions inside the
CubeMX-generated files (the `USER CODE` blocks in `firmware/*/Core/`) are included too.

## Not covered by that license

* **Vendor code keeps its own license.** `firmware/*/Drivers/` is STMicroelectronics' HAL and driver code (BSD-3-Clause, see
  `firmware/*/Drivers/STM32G4xx_HAL_Driver/LICENSE.txt`) and Arm's CMSIS (Apache-2.0, `firmware/*/Drivers/CMSIS/LICENSE.txt`). The
  CubeMX-generated files in `firmware/*/Core/` and the start-up code are ST templates under ST's license (the notice in each file);
  only the project's additions in them are AGPL. These licenses are compatible with the AGPL; they are kept intact and are not
  relicensed.
* **Documentation and hardware designs are not licensed yet.** The documents under `docs/` and `firmware/*/docs/`, and the KiCad
  projects and fabrication files under `hardware/`, are **all rights reserved** until a license is chosen for them. For reference,
  the usual pairings with the AGPL are CC BY-SA 4.0 for documents and CERN-OHL-S-2.0 for hardware; neither has been applied.
* **Third-party material** (for example the Transrex documents in `firmware/xrex/docs/Transrex/`) belongs to its owners and is
  not licensed by this repository.

## Contributions

Contributions are accepted under the same license (AGPL-3.0-only for software). The copyright holder may also offer the software
under other terms by separate agreement.
