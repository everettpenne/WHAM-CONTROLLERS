/*
 * qspi_test_hw.h -- BSP-private part of the QUADSPI bring-up (the HAL
 * handle nvstore.c needs to talk to the flash chip). Public interface:
 * src/drivers/qspi_test.h.
 */
#ifndef QSPI_TEST_HW_H
#define QSPI_TEST_HW_H

#include "qspi_test.h"
#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_qspi.h"

/* The initialised QUADSPI handle, or NULL if QspiTest_Init() has not run
 * (or QSPI_TEST_FEATURE_ENABLED is 0). Lets nvstore.c share the one
 * peripheral instead of initialising QUADSPI a second time. */
QSPI_HandleTypeDef *QspiTest_Handle(void);

#endif /* QSPI_TEST_HW_H */
