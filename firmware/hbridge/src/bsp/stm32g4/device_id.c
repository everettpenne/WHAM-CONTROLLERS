/*
 * device_id.c -- STM32G4 implementation of drivers/device_id.h.
 */
#include "device_id.h"
#include "main.h"

void DeviceId_ReadMcuUid(uint32_t uid[3])
{
    uid[0] = HAL_GetUIDw0();
    uid[1] = HAL_GetUIDw1();
    uid[2] = HAL_GetUIDw2();
}
