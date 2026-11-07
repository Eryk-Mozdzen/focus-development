#include <stm32u0xx_hal.h>

#include "fifo.h"

void fifo_init(fifo_t *fifo) {
    fifo->rd = 0;
    fifo->wr = 0;
}

uint32_t fifo_write(fifo_t *fifo, const uint8_t *src, const uint32_t src_len) {
    uint32_t num = 0;
    while(num < src_len) {
        uint32_t wr = fifo->wr;
        uint32_t next = wr + 1;
        if(next >= sizeof(fifo->buffer)) {
            next = 0;
        }
        if(next == fifo->rd) {
            break;
        }
        fifo->buffer[wr] = src[num];
        __DMB();
        fifo->wr = next;
        num++;
    }
    return num;
}

uint32_t fifo_read(fifo_t *fifo, uint8_t *dst, const uint32_t dst_capacity) {
    uint32_t num = 0;
    while((fifo->rd != fifo->wr) && (num < dst_capacity)) {
        uint32_t rd = fifo->rd;
        uint32_t next = rd + 1;
        dst[num] = fifo->buffer[rd];
        if(next >= sizeof(fifo->buffer)) {
            next = 0;
        }
        __DMB();
        fifo->rd = next;
        num++;
    }
    return num;
}
