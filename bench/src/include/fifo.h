#ifndef FIFO_H
#define FIFO_H

#include <stdint.h>

typedef struct {
    volatile uint32_t rd;
    volatile uint32_t wr;
    uint8_t buffer[1024];
} fifo_t;

void fifo_init(fifo_t *fifo);
uint32_t fifo_write(fifo_t *fifo, const uint8_t *src, const uint32_t src_len);
uint32_t fifo_read(fifo_t *fifo, uint8_t *dst, const uint32_t dst_capacity);

#endif
