/*
 * device_id.h -- the MCU's factory-programmed unique ID (added 2026-10-07).
 * Implemented in src/bsp/<chip>/device_id.c.
 *
 * Every STM32G4 carries a 96-bit number written at the factory, different on every
 * die. With the flash chip's own unique ID (nvstore.h, NvStore_ReadUniqueId) it
 * is what makes one copy of the board distinguishable from another before any
 * serial number has been provisioned -- see docs/device_identity_and_host_service.md
 * at the top of the repository.
 */
#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <stdint.h>

/* The 96-bit unique ID as three 32-bit words, word 0 first (UID[31:0]). */
void DeviceId_ReadMcuUid(uint32_t uid[3]);

#endif /* DEVICE_ID_H */
