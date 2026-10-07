/*
 * board_io.c -- STM32G4 implementation of drivers/board_io.h.
 */
#include "board_io.h"
#include "main.h"

#define GDS_PIN_MASK   (GPIO_PIN_0  | GPIO_PIN_1  | GPIO_PIN_2  | GPIO_PIN_3  | \
                        GPIO_PIN_4  | GPIO_PIN_5  | GPIO_PIN_6  | GPIO_PIN_7  | \
                        GPIO_PIN_8  | GPIO_PIN_9  | GPIO_PIN_10 | GPIO_PIN_11)

void BoardIo_Init(void)
{
  /* GateDriverStatus_01..12 (PE0..PE11, docs/pin_mapping_v4.csv) --
     interrupt-capable digital inputs, no pull. Originally added
     2026-09-08 as plain GPIO_MODE_INPUT alongside the GDS? diagnostic
     command (commands.c); upgraded the same day to
     GPIO_MODE_IT_RISING_FALLING to back a real fault interrupt
     (gate_driver.c's GateDriver_CheckFault()) -- GDS? still works
     identically either way, a plain IDR read. These pins were never
     configured at all before the first of those two changes, so a
     floating/undriven pin would have read an arbitrary level. Pull
     matches the sibling PFM-STM32G474 project's gpio.c config for
     these same 12 pins (GPIO_NOPULL -- gate-driver-IC status outputs,
     actively driven, no internal pull needed).

     Both edges (not just the one GDS_FAULT_POLARITY, ctrlr_config.h,
     currently cares about) so a fault is caught regardless of which
     direction a pin moves -- the actual fault/healthy determination
     happens in GateDriver_CheckFault(), against that compile-time
     setting, not by picking rising-only or falling-only here; that
     keeps this config correct even if GDS_FAULT_POLARITY is ever
     flipped without also revisiting this block.

     __HAL_RCC_SYSCFG_CLK_ENABLE() is required before HAL_GPIO_Init()
     can actually route these pins' EXTI lines (SYSCFG->EXTICR) -- easy
     to omit and get a config that silently never fires. */
  {
      GPIO_InitTypeDef gdsInit = {0};

      __HAL_RCC_GPIOE_CLK_ENABLE();
      __HAL_RCC_SYSCFG_CLK_ENABLE();

      gdsInit.Pin   = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2  | GPIO_PIN_3  |
                       GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6  | GPIO_PIN_7  |
                       GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
      gdsInit.Mode  = GPIO_MODE_IT_RISING_FALLING;
      gdsInit.Pull  = GPIO_NOPULL;
      HAL_GPIO_Init(GPIOE, &gdsInit);

      /* EXTI0..EXTI4 are individual NVIC vectors; EXTI5..9 share
         EXTI9_5_IRQn; EXTI10..15 share EXTI15_10_IRQn -- PE0..PE11
         spans all three groups, 7 vectors total (see stm32g4xx_it.c).
         Priority tied with HRTIM1_Master_IRQn (1,0) -- both are
         output-safety-critical paths, and since neither ISR runs long
         (a register read/compare, occasionally a HRTIM1_PWM_Stop()
         call), a bounded, occasional deferral between the two at equal
         priority is an acceptable tradeoff, not a real latency risk.
         Below USART2 (2,0) -- fault detection preempts serial I/O, not
         the other way around. Safe to configure NVIC priority/enable
         here: this runs after FixSysTickPriority() (main(), USER CODE
         Init) and after HAL_Init()'s own NVIC setup, the same ordering
         constraint HRTIM1_EnableMasterInterrupt() documents for
         itself. */
      HAL_NVIC_SetPriority(EXTI0_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI0_IRQn);
      HAL_NVIC_SetPriority(EXTI1_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI1_IRQn);
      HAL_NVIC_SetPriority(EXTI2_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI2_IRQn);
      HAL_NVIC_SetPriority(EXTI3_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI3_IRQn);
      HAL_NVIC_SetPriority(EXTI4_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI4_IRQn);
      HAL_NVIC_SetPriority(EXTI9_5_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
      HAL_NVIC_SetPriority(EXTI15_10_IRQn, 1U, 0U);
      HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  }
}

uint16_t BoardIo_ReadGateDriverStatus(void)
{
    return (uint16_t)(GPIOE->IDR & GDS_PIN_MASK);
}
