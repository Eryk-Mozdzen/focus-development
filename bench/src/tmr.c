#include <stm32u0xx_hal.h>

#include "tmr.h"

void tmr_init(tmr_t *timer, const uint32_t period) {
    timer->start = HAL_GetTick();
    timer->period = period;
}

void tmr_reset(tmr_t *timer) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    timer->start += timer->period;

    __set_PRIMASK(primask);
}

void tmr_restart(tmr_t *timer) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    timer->start = HAL_GetTick();

    __set_PRIMASK(primask);
}

uint8_t tmr_timeout(tmr_t *timer) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    const uint32_t delta = HAL_GetTick() - timer->start;

    __set_PRIMASK(primask);

    return (delta >= timer->period);
}
