#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

typedef struct {
    uint32_t start;
    uint32_t period;
} tmr_t;

void tmr_init(tmr_t *timer, const uint32_t period);
void tmr_reset(tmr_t *timer);
void tmr_restart(tmr_t *timer);
uint8_t tmr_timeout(tmr_t *timer);

#endif
