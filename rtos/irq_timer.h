#ifndef AMP_IRQ_TIMER_H
#define AMP_IRQ_TIMER_H

#include <stdint.h>

void amp_wait_for_start(void);
void amp_setup_tick(void);
void amp_clear_tick(void);
void vApplicationIRQHandler(uint64_t iar);

#endif