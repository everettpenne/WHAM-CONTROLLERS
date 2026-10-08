# Hardware

KiCad projects for the WHAM controller board and its predecessors, with their fabrication outputs.

| Folder | What |
|---|---|
| `PFM-Controller_V4/` | the current controller board (STM32G474QET6, pin map in `firmware/*/docs/pin_mapping_v4.csv`) |
| `PFM-Controller_V4_Rev2/` | a second revision of the V4 board (KiCad project) |
| `HVPS_Controller_V3/` | the previous board (V3) |

Not tracked on purpose (see `.gitignore`): KiCad backups, autosaves and caches, the fabrication toolkit's `production/backups/`, and
large third-party document archives (`hardware/*.zip`; GitHub refuses files over 100 MB). `PFM-Controller_V4/PFMV4_Prelim_Files-main/`
(a downloaded copy of earlier project files and documents) is left untracked until someone decides it belongs here.

The pin map still lives with each firmware project (the firmware, the SOP and the command reference point at it). Moving it here, with
product columns beside it and a changelog of board revisions, is the natural next step.

**License:** the hardware designs are not licensed yet (all rights reserved); the AGPL in the repository root covers the software only.
See `LICENSING.md`.
