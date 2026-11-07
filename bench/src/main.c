#include <string.h>

#include <stm32u0xx_hal.h>

#include <mg_pppos.h>
#include <mongoose.h>

#include "fifo.h"
#include "ina226_regs.h"
#include "tmr.h"

#define WS_CONN_MAX 16

typedef struct {
    UART_HandleTypeDef *huart;
    fifo_t fifo_tx;
    fifo_t fifo_rx;
    tmr_t timeout_tx;
    tmr_t timeout_rx;
    uint8_t dma_tx_buffer[1024];
    uint8_t dma_rx_buffer[1024];
    volatile uint8_t dma_tx_ready;
    volatile uint32_t dma_rx_pos;
} uart_t;

typedef enum {
    BUTTON_NONE,
    BUTTON_OFFSET,
} button_t;

typedef struct {
    button_t button;
    struct mg_connection *ws_conns[WS_CONN_MAX];
} context_t;

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern I2C_HandleTypeDef hi2c3;
extern UART_HandleTypeDef huart2;
extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];
extern const char _binary_style_css_start[];
extern const char _binary_style_css_end[];
extern const char _binary_script_js_start[];
extern const char _binary_script_js_end[];

static uart_t uart;
static context_t context;

void SystemClock_Config();
void MX_GPIO_Init();
void MX_DMA_Init();
void MX_TIM1_Init();
void MX_TIM2_Init();
void MX_I2C3_Init();
void MX_USART2_UART_Init();

static void uart_send(uart_t *uart) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if(uart->dma_tx_ready) {
        const uint32_t len =
            fifo_read(&uart->fifo_tx, uart->dma_tx_buffer, sizeof(uart->dma_tx_buffer));
        if(len > 0) {
            uart->dma_tx_ready = 0;
            HAL_UART_Transmit_DMA(uart->huart, uart->dma_tx_buffer, len);
        }
    }

    __set_PRIMASK(primask);
}

static void uart_recv(uart_t *uart) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    const uint32_t pos = sizeof(uart->dma_rx_buffer) - __HAL_DMA_GET_COUNTER(uart->huart->hdmarx);

    if(pos != uart->dma_rx_pos) {
        if(pos > uart->dma_rx_pos) {
            fifo_write(&uart->fifo_rx, &uart->dma_rx_buffer[uart->dma_rx_pos],
                       pos - uart->dma_rx_pos);
        } else {
            fifo_write(&uart->fifo_rx, &uart->dma_rx_buffer[uart->dma_rx_pos],
                       sizeof(uart->dma_rx_buffer) - uart->dma_rx_pos);
            if(pos > 0) {
                fifo_write(&uart->fifo_rx, uart->dma_rx_buffer, pos);
            }
        }
        uart->dma_rx_pos = pos;
    }

    __set_PRIMASK(primask);
}

static size_t uart_output(const void *data, size_t data_size, void *arg) {
    uart_t *uart = arg;

    const uint8_t *buffer = data;
    uint32_t n;
    uint32_t total = 0;
    while(total < data_size) {
        n = fifo_write(&uart->fifo_tx, &buffer[total], data_size - total);
        if(n < (data_size - total)) {
            tmr_restart(&uart->timeout_tx);
            uart_send(uart);
        }
        total += n;
    }
    return data_size;
}

static void uart_init(uart_t *uart, UART_HandleTypeDef *huart) {
    uart->huart = huart;
    uart->dma_tx_ready = 1;
    uart->dma_rx_pos = 0;
    fifo_init(&uart->fifo_tx);
    fifo_init(&uart->fifo_rx);
    tmr_init(&uart->timeout_tx, 1);
    tmr_init(&uart->timeout_rx, 1);
    HAL_UART_Receive_DMA(uart->huart, uart->dma_rx_buffer, sizeof(uart->dma_rx_buffer));
}

static void uart_loop(uart_t *uart, struct mg_pppos *pppos) {
    if(tmr_timeout(&uart->timeout_tx)) {
        tmr_reset(&uart->timeout_tx);
        uart_send(uart);
    }

    if(tmr_timeout(&uart->timeout_rx)) {
        tmr_reset(&uart->timeout_rx);
        uart_recv(uart);
    }

    uint8_t buffer_rx[128];
    const uint32_t buffer_len = fifo_read(&uart->fifo_rx, buffer_rx, sizeof(buffer_rx));
    if(buffer_len > 0) {
        mg_pppos_input(pppos, buffer_rx, buffer_len);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == uart.huart) {
        uart.dma_tx_ready = 1;
    }
}

void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == uart.huart) {
        tmr_restart(&uart.timeout_rx);
        uart_recv(&uart);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == uart.huart) {
        tmr_restart(&uart.timeout_rx);
        uart_recv(&uart);
    }
}

static void delay_us(const uint32_t time_us) {
    __HAL_TIM_SET_COUNTER(&htim1, 0);
    while(__HAL_TIM_GET_COUNTER(&htim1) < time_us) {
    }
}

#define TACHO_BUFFER 21

typedef struct {
    volatile uint32_t buffer[TACHO_BUFFER];
    volatile uint32_t buffer_counter;
    uint32_t last;
} tacho_t;

static void tacho_init(tacho_t *tacho) {
    for(uint32_t i = 0; i < TACHO_BUFFER; i++) {
        tacho->buffer[i] = 0;
    }
    tacho->buffer_counter = 0;
    tacho->last = 0;
    HAL_TIM_Base_Start(&htim2);
}

static uint32_t tacho_read(tacho_t *tacho, float *frequency) {
    const uint32_t now = HAL_GetTick();

    if((now - tacho->last) >= 100) {
        tacho->last = now;

        uint32_t sorted[TACHO_BUFFER];

        __disable_irq();
        memcpy(sorted, (uint32_t *)tacho->buffer, TACHO_BUFFER * sizeof(uint32_t));
        __enable_irq();

        for(uint32_t i = 1; i < TACHO_BUFFER; i++) {
            const uint32_t key = sorted[i];
            uint32_t j = i;
            while((j > 0) && (sorted[j - 1] > key)) {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j] = key;
        }

        const uint32_t median_period_us = sorted[TACHO_BUFFER / 2];

        *frequency = 1e+6f / median_period_us;

        return 1;
    }

    return 0;
}

static void tacho_isr(tacho_t *tacho) {
    const uint32_t period_us = __HAL_TIM_GET_COUNTER(&htim2);
    __HAL_TIM_SET_COUNTER(&htim2, 0);

    tacho->buffer[tacho->buffer_counter] = period_us;
    tacho->buffer_counter++;
    if(tacho->buffer_counter >= TACHO_BUFFER) {
        tacho->buffer_counter = 0;
    }
}

tacho_t tacho;

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin) {
    if(GPIO_Pin == GPIO_PIN_0) {
        tacho_isr(&tacho);
    }
}

typedef enum {
    HX711_STATE_IDLE,
    HX711_STATE_SYNC,
    HX711_STATE_READ,
    HX711_STATE_END,
} hx711_state_t;

typedef struct {
    hx711_state_t state;
    uint32_t bits;
    uint32_t measurement[3];
} hx711_t;

static void hx711_init(hx711_t *hx711) {
    hx711->state = HX711_STATE_IDLE;

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
}

static uint32_t hx711_read(hx711_t *hx711, int32_t result[3]) {
    switch(hx711->state) {
        case HX711_STATE_IDLE: {
            hx711->measurement[0] = 0;
            hx711->measurement[1] = 0;
            hx711->measurement[2] = 0;
            hx711->bits = 0;
            hx711->state = HX711_STATE_SYNC;
        } break;
        case HX711_STATE_SYNC: {
            if((HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_RESET) &&
               (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_RESET) &&
               (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_RESET)) {
                delay_us(1);
                hx711->state = HX711_STATE_READ;
            }
        } break;
        case HX711_STATE_READ: {
            if(hx711->bits < 24) {
                HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
                delay_us(5);

                hx711->measurement[0] <<= 1;
                hx711->measurement[1] <<= 1;
                hx711->measurement[2] <<= 1;

                hx711->measurement[0] |= (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_SET);
                hx711->measurement[1] |= (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_SET);
                hx711->measurement[2] |= (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET);

                HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
                delay_us(5);

                hx711->bits++;
            } else {
                hx711->state = HX711_STATE_END;
            }
        } break;
        case HX711_STATE_END: {
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
            delay_us(5);
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
            delay_us(5);

            hx711->measurement[0] ^= 0x800000;
            hx711->measurement[1] ^= 0x800000;
            hx711->measurement[2] ^= 0x800000;

            hx711->state = HX711_STATE_IDLE;

            result[0] = hx711->measurement[0];
            result[1] = hx711->measurement[1];
            result[2] = hx711->measurement[2];

            return 1;
        } break;
    }

    return 0;
}

#define PWR_R_SHUNT     0.0039f
#define PWR_MAX_CURRENT 30.f

typedef enum {
    INA226_STATE_IDLE,
    INA226_STATE_WAIT_FOR_VOLTAGE,
    INA226_STATE_WAIT_FOR_CURRENT,
} ina226_state_t;

static volatile uint8_t i2c_buffer[2];
static volatile uint8_t i2c_ready = 0;

typedef struct {
    ina226_state_t state;
    int16_t voltage;
    int16_t current;
} ina226_t;

static void ina226_write(uint8_t address, uint16_t value) {
    uint8_t reverse[2] = {(uint8_t)(value >> 8), (uint8_t)(value)};

    HAL_I2C_Mem_Write(&hi2c3, INA226_ADDR << 1, address, 1, reverse, 2, 100);
}

static void ina226_init(ina226_t *ina226) {
    ina226->state = INA226_STATE_IDLE;

    ina226_write(INA226_REG_CONFIGURATION, INA226_CONFIGURATION_RESET);

    HAL_Delay(100);

    ina226_write(INA226_REG_CONFIGURATION, INA226_CONFIGURATION_AVERAGE_1 |
                                               INA226_CONFIGURATION_BUS_VOLTAGE_CONV_1_1MS |
                                               INA226_CONFIGURATION_SHUNT_VOLTAGE_CONV_1_1MS |
                                               INA226_CONFIGURATION_MODE_CONTINUOUS_SHUNT_BUS);

    ina226_write(INA226_REG_MASK_ENABLE, INA226_MASK_ENABLE_CONVERSION_READY |
                                             INA226_MASK_ENABLE_ALERT_POLARITY_ACTIVE_LOW |
                                             INA226_MASK_ENABLE_ALERT_LATCH_TRANSPARENT);

    const uint16_t calib = INA226_CALIBRATION_VALUE(PWR_MAX_CURRENT, PWR_R_SHUNT);

    ina226_write(INA226_REG_CALIBRATION, calib);
}

static void ina226_read(ina226_t *ina226, float *voltage, float *current) {
    switch(ina226->state) {
        case INA226_STATE_IDLE: {
            ina226->state = INA226_STATE_WAIT_FOR_VOLTAGE;

            i2c_ready = 0;
            HAL_I2C_Mem_Read_IT(&hi2c3, INA226_ADDR << 1, INA226_REG_BUS_VOLTAGE, 1,
                                (uint8_t *)i2c_buffer, 2);
        } break;
        case INA226_STATE_WAIT_FOR_VOLTAGE: {
            if(i2c_ready) {
                ina226->voltage = (((uint16_t)i2c_buffer[0]) << 8) | i2c_buffer[1];
                ina226->state = INA226_STATE_WAIT_FOR_CURRENT;

                i2c_ready = 0;
                HAL_I2C_Mem_Read_IT(&hi2c3, INA226_ADDR << 1, INA226_REG_CURRENT, 1,
                                    (uint8_t *)i2c_buffer, 2);
            }
        } break;
        case INA226_STATE_WAIT_FOR_CURRENT: {
            if(i2c_ready) {
                ina226->current = (((uint16_t)i2c_buffer[0]) << 8) | i2c_buffer[1];

                *voltage = ina226->voltage * INA226_LSB_BUS_VOLTAGE;
                *current = ina226->current * INA226_LSB_CURRENT(PWR_MAX_CURRENT);

                ina226->state = INA226_STATE_IDLE;
            }
        } break;
    }
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if(hi2c == &hi2c3) {
        i2c_ready = 1;
    }
}

#define DS18B20_SKIP_ROM        0xCC
#define DS18B20_CONVERT_T       0x44
#define DS18B20_READ_SCRATCHPAD 0xBE

typedef enum {
    DS18B20_STATE_IDLE,
    DS18B20_STATE_WAIT,
    DS18B20_STATE_END,
} ds18b20_state_t;

typedef struct {
    ds18b20_state_t state;
    uint32_t begin;
} ds18b20_t;

static void ds18b20_reset() {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_RESET);
    delay_us(480);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
    delay_us(480);
}

static void ds18b20_write_byte(uint8_t byte) {
    for(uint8_t i = 0; i < 8; i++) {
        if((byte >> i) & 0x01) {
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_RESET);
            delay_us(6);
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
            delay_us(64);
        } else {
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_RESET);
            delay_us(60);
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
            delay_us(10);
        }
    }
}

static uint8_t ds18b20_read_byte() {
    uint8_t byte = 0;
    for(uint8_t i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_RESET);
        delay_us(3);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
        delay_us(10);
        if(HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_10) == GPIO_PIN_SET) {
            byte |= (1U << i);
        }
        delay_us(53);
    }
    return byte;
}

static void ds18b20_init(ds18b20_t *ds18b20) {
    ds18b20->state = DS18B20_STATE_IDLE;

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_10, GPIO_PIN_SET);
}

static void ds18b20_read(ds18b20_t *ds18b20, float *temperature) {
    switch(ds18b20->state) {
        case DS18B20_STATE_IDLE: {
            ds18b20_reset();
            ds18b20_write_byte(DS18B20_SKIP_ROM);
            ds18b20_write_byte(DS18B20_CONVERT_T);

            ds18b20->state = DS18B20_STATE_WAIT;
            ds18b20->begin = HAL_GetTick();
        } break;
        case DS18B20_STATE_WAIT: {
            if((HAL_GetTick() - ds18b20->begin) >= 750) {
                ds18b20->state = DS18B20_STATE_END;
            }
        } break;
        case DS18B20_STATE_END: {
            ds18b20_reset();
            ds18b20_write_byte(DS18B20_SKIP_ROM);
            ds18b20_write_byte(DS18B20_READ_SCRATCHPAD);

            uint8_t buffer[9];
            for(uint8_t i = 0; i < 9; i++) {
                buffer[i] = ds18b20_read_byte();
            }

            const int16_t raw = (((uint16_t)buffer[1]) << 8) | buffer[0];

            *temperature = raw * 0.0625f;

            ds18b20->state = DS18B20_STATE_IDLE;
        } break;
    }
}

typedef struct {
    float thrust;
    float torque;
    float velocity;
    float temperature;
    float voltage;
    float current;
    int32_t load[3];
    float frequency;
} frame_t;

uint64_t mg_millis(void) {
    return HAL_GetTick();
}

#define CHUNK 512

struct asset {
    const char *uri;
    const char *mime;
    const char *start;
    const char *end;
};

struct tx {
    const char *p;
    size_t left;
};

static const struct asset assets[] = {
    {"/index.html", "text/html",              _binary_index_html_start, _binary_index_html_end},
    {"/style.css",  "text/css",               _binary_style_css_start,  _binary_style_css_end },
    {"/script.js",  "application/javascript", _binary_script_js_start,  _binary_script_js_end },
};

static inline void tx_load(struct mg_connection *c, struct tx *t) {
    memcpy(t, c->data, sizeof(*t));
}

static inline void tx_store(struct mg_connection *c, const struct tx *t) {
    memcpy(c->data, t, sizeof(*t));
}

static void http_event(struct mg_connection *c, int ev, void *ev_data) {
    context_t *context = c->fn_data;

    if(ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        if(mg_match(hm->uri, mg_str("/websocket"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
            for(uint32_t i = 0; i < WS_CONN_MAX; i++) {
                if(context->ws_conns[i] == NULL) {
                    context->ws_conns[i] = c;
                    break;
                }
            }
            return;
        }

        if(mg_strcmp(hm->uri, mg_str("/")) == 0) {
            hm->uri = mg_str("/index.html");
        }

        for(size_t i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
            if(mg_strcmp(hm->uri, mg_str(assets[i].uri)) != 0) {
                continue;
            }

            size_t len = (size_t)(assets[i].end - assets[i].start);

            mg_printf(c,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %lu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      assets[i].mime, (unsigned long)len);

            struct tx t = {assets[i].start, len};
            tx_store(c, &t);
            return;
        }
        mg_http_reply(c, 404, "Connection: close\r\n", "Not found\n");

    } else if(ev == MG_EV_POLL || ev == MG_EV_WRITE) {
        if(c->is_listening || c->is_draining || c->is_closing) {
            return;
        }

        struct tx t;
        tx_load(c, &t);
        if(t.left == 0) {
            return;
        }

        if(c->send.len < CHUNK) {
            size_t n = t.left < CHUNK ? t.left : CHUNK;
            mg_send(c, t.p, n);
            t.p += n;
            t.left -= n;
            tx_store(c, &t);
            if(t.left == 0) {
                c->is_draining = 1;
            }
        }
    } else if(ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

        if(mg_strcmp(wm->data, mg_str("offset")) == 0) {
            context->button = BUTTON_OFFSET;
        }
    } else if(ev == MG_EV_CLOSE) {
        for(uint32_t i = 0; i < WS_CONN_MAX; i++) {
            if(context->ws_conns[i] == c) {
                context->ws_conns[i] = NULL;
            }
        }
    }
}

int main() {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_I2C3_Init();
    MX_USART2_UART_Init();

    hx711_t hx711;
    ina226_t ina226;
    ds18b20_t ds18b20;

    HAL_TIM_Base_Start(&htim1);

    uart_init(&uart, &huart2);
    tacho_init(&tacho);
    hx711_init(&hx711);
    ina226_init(&ina226);
    ds18b20_init(&ds18b20);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_pppos pppos;
    mg_pppos_init_state(&pppos, uart_output, NULL, NULL, &uart);

    struct mg_tcpip_if netif = {0};
    netif.driver = mg_pppos_driver(&pppos);
    netif.driver_data = &pppos;
    netif.l2type = MG_TCPIP_L2_PPP;
    netif.enable_dhcp_client = false;
    mg_tcpip_init(&mgr, &netif);

    mg_http_listen(&mgr, "http://0.0.0.0:80", http_event, &context);

    uint32_t prev1 = 0;
    uint32_t prev2 = 0;

    frame_t frame = {0};

    int32_t load_raw[3] = {0};
    int32_t load_offset[3] = {0};

    while(1) {
        const uint32_t timestamp = HAL_GetTick();

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                          ((timestamp % 1000) < 50) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        if((timestamp - prev1) >= 100) {
            prev1 = timestamp;

            for(uint32_t i = 0; i < WS_CONN_MAX; i++) {
                if(context.ws_conns[i] != NULL) {
                    mg_ws_send(context.ws_conns[i], &frame, sizeof(frame), WEBSOCKET_OP_BINARY);
                }
            }
        }

        switch(context.button) {
            case BUTTON_NONE: {

            } break;
            case BUTTON_OFFSET: {
                load_offset[0] = load_raw[0];
                load_offset[1] = load_raw[1];
                load_offset[2] = load_raw[2];
                context.button = BUTTON_NONE;
            } break;
        }

        if(tacho_read(&tacho, &frame.frequency)) {
            frame.velocity = 6.283185307f * frame.frequency;
        }

        if(hx711_read(&hx711, load_raw)) {
            frame.load[0] = load_raw[0] - load_offset[0];
            frame.load[1] = load_raw[1] - load_offset[1];
            frame.load[2] = load_raw[2] - load_offset[2];

            const float g = 9.8067f;
            const float arm = 0.270f;
            const float k = 0.265f / -105000.f;

            const float mass = 0.212f;
            const float t1 = -mass * arm * g;
            const float l11 = 1368339.f;
            const float l21 = 1900411.f;
            const float t2 = +mass * arm * g;
            const float l12 = -1795504.f;
            const float l22 = -1414293.f;

            const float k1 = ((t1 * l22) - (t2 * l21)) / ((l11 * l22) - (l12 * l21));
            const float k2 = (-(t1 * l12) + (t2 * l11)) / ((l11 * l22) - (l12 * l21));

            frame.thrust = frame.load[0] * k * g;
            frame.torque = (k1 * frame.load[1]) + (k2 * frame.load[2]);
        }

        if((timestamp - prev2) >= 50) {
            prev2 = timestamp;
            ina226_read(&ina226, &frame.voltage, &frame.current);
            ds18b20_read(&ds18b20, &frame.temperature);
        }

        uart_loop(&uart, &pppos);

        mg_mgr_poll(&mgr, 1);
    }

    return 0;
}
