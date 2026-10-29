# PFM V4 Preliminary Design Review

This directory contains supporting files for the PFM V4 controller board
preliminary design review (PDR). These materials are a snapshot provided
for reviewer evaluation of the V4 revision prior to fabrication and test.

## Primary document

**`PFMV4_Documentation.pdf`** is the main review document. It covers:

- Architecture and design rationale
- Pin assignment (STM32G474QET6, LQFP128)
- Hardware block diagrams (MCU, PFM output, fault handling)
- Board physical layout, box interior layout, and 6-layer PCB stackup
- Complete pin listing in package order (appendix)

Refer reviewers to this document first.

## Supporting files

| File | Description |
|---|---|
| `STM32G4 Pin Mapping.csv` | Single-source CSV netlist used to generate pin tables in the main document |
| `command_reference.pdf` | Serial command interface reference for the PFM firmware |
| `KiCAD/` | KiCAD schematic and PCB layout project for the V4 controller |
| `rm0440-stm32g4-series-...pdf` | ST RM0440 — STM32G4 reference manual |
| `stm32g474cb-10.pdf` | DS12288 Rev 6 — STM32G474QET6 datasheet |
| `V3_PDR/` | V3 preliminary design review for comparison with prior revision |
| `production` | BOM and placement files from KiCAD |
| `jlc_fabrication` | Files related to PCB fabrication and assembly from JLCPCB including final BOM |

## Scope

This PDR package is a working snapshot; the latest source for the main
document lives in `V4-documentation/pin_mapping_v4.tex` at the repository
root. The KiCAD project and CSV here are the PDR versions — check the
repository root for the current working copies.
