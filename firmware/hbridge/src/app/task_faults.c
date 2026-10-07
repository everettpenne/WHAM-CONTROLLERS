/*
 * task_faults.c -- fault detection that must run regardless of state.
 */
#include "tasks.h"
#include "state_machine.h"

void TaskFaults_Poll(void)
{
    /* Latches a fault from either hardware source into the state machine,
       and ends a finished shot (state_machine.h). Cheap: flag reads only.
       The outputs themselves were already made safe by the hardware (PC10)
       or the EXTI interrupt (GateDriver_CheckFault()) -- this is the state
       bookkeeping, not the protection. */
    SM_PollFaults();
}
