# Saved profiles: what WHAM-XREX-PFMG474 and WHAM-HBRIDGE-PFMG474 share

The two controllers run on the **same board** and keep saved profiles (`*SAV` / `*RCL`) in the **same
external flash chip** (Winbond W25Q128JVS, 16 MiB, on QUADSPI). This file is the single description of how
they divide the chip and which code is common. The identical file is in both repos.

## Flash map

| Address range | Owner | Notes |
|---|---|---|
| 0 .. 64 KiB (`0x000000` - `0x00FFFF`) | WHAM-XREX-PFMG474 | 8 slots x 2 copies x 4 KiB (`PROFILE_COPY_SECTORS` 1) |
| 64 KiB .. 320 KiB (`0x010000` - `0x04FFFF`) | WHAM-HBRIDGE-PFMG474 | 8 slots x 2 copies x 16 KiB (`PROFILE_COPY_SECTORS` 4) |
| 320 KiB .. 16 MiB - 4 KiB | unused | room for another product or a larger layout |
| last 4 KiB (`0xFFF000`) | `QSPI:SELFtest` scratch | erased and rewritten freely; never store data here |

| Product | Magic (first 4 bytes of a copy) | Region base |
|---|---|---|
| XREX | `WSPF` | 0 |
| HBRIDGE | `HSPF` | 64 KiB |

A product reads another product's slot as **corrupt** (wrong magic), never as a profile: the two share this
board and chip, field ids overlap, and a profile must never be decoded by the wrong firmware. Regions do not
overlap, so a board that has run both keeps both products' profiles and neither overwrites the other's.
To add a product: pick a magic, take the next free region (the table above), set the four profile constants in
its `ctrlr_config.h` (`PROFILE_STORE_BASE`, `PROFILE_SLOT_COUNT`, `PROFILE_COPY_SECTORS`,
`PROFILE_MAGIC_BYTES`), and add it here. `profile_store.h` refuses to build if the slots would run into the
scratch sector.

## Shared code

These are byte-identical in both repos; `python/check_shared.py` (the same script in both) compares them with
the sibling checkout and reports any drift:

* `src/drivers/nvstore.h`, `src/bsp/stm32g4/nvstore.c` -- the byte store on the chip (read, 4 KiB sector erase,
  page program with read-back, status registers);
* `src/drivers/qspi_test.h`, `src/bsp/stm32g4/qspi_test.c`, `qspi_test_hw.h` -- the QUADSPI bring-up and the
  shared handle;
* `src/app/settings/profile_store.h`, `profile_store.c` -- slots, alternating copies, 48-byte header with CRC-32,
  the buffered API and the streaming writer/reader (the header is written LAST, so a power cut at any point of a
  save leaves the previous data intact);
* `tests/test_profile_store.c`, `fake_nvstore.c/.h` -- the host tests, including a power cut injected at every
  step of small saves and at a spread of steps of a large multi-sector save (the test adapts to the product's
  copy size);
* the float-to-text routine in `settings_codec` (`SettingsCodec_FormatFloat`, a printf-free `%g`) and its
  declaration.

## What stays different (by design)

* **What a payload means.** `settings_codec.h`'s field table and snapshot, and `settings.c` (capture/apply
  through that product's own setters). Field ids are a frozen contract within each product. The record framing
  `[id u8][channel u8][length u8][value]` is the same.
* **The channel byte.** XREX: a Transrex channel. HBRIDGE: a plant (0 HVPS, 1 Limiter); HBRIDGE also sends its
  shot, target and HVPS-curve tables as chunked records, streamed to and from flash because they do not fit in RAM.
* **The profile constants** above, and each product's commands' extras (HBRIDGE's recall selects the shot
  source and drops tables first).

## Compatibility

The format did not change when the store was generalized (2026-10-05): the old XREX store and the shared one
produce byte-identical flash images for the same saves and each reads the other's, so profiles saved by earlier
XREX firmware stay readable. (Checked on the host by writing the same set of saves with the old and the new
code, comparing the two 64 KiB images, and reading each with the other.) The shared store accepts payloads up
to `PROFILE_COPY_BYTES - 48` bytes (4,048 for XREX); XREX's own limit is its 512-byte working buffer.
