/*
 * tasks.h -- the main loop's tasks. Each TaskX_Poll() does a bounded amount
 * of work and never blocks; App_Poll() calls them in turn.
 */
#ifndef TASKS_H
#define TASKS_H

void TaskFaults_Poll(void);
void TaskScpi_Poll(void);

#endif /* TASKS_H */
