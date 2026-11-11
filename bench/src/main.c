#include <math.h>

#include <stm32u0xx_hal.h>

#include <mg_pppos.h>
#include <mongoose.h>

#include "bmp280_compensate.h"
#include "bmp280_regs.h"
#include "fifo.h"
#include "ina226_regs.h"
#include "tmr.h"

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

typedef struct {
    uint8_t button;
    float generator;
} context_t;

extern ADC_HandleTypeDef hadc1;
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern I2C_HandleTypeDef hi2c2;
extern I2C_HandleTypeDef hi2c3;
extern UART_HandleTypeDef huart2;
extern const char _binary_index_html_start[];
extern const char _binary_index_html_size[];

void SystemClock_Config();
void MX_GPIO_Init();
void MX_DMA_Init();
void MX_ADC1_Init();
void MX_TIM1_Init();
void MX_TIM2_Init();
void MX_TIM3_Init();
void MX_I2C2_Init();
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

static size_t uart_write(const void *data, size_t data_size, void *arg) {
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

static bool uart_init(void *arg) {
    uart_t *uart = arg;
    uart->huart = &huart2;
    uart->dma_tx_ready = 1;
    uart->dma_rx_pos = 0;
    fifo_init(&uart->fifo_tx);
    fifo_init(&uart->fifo_rx);
    tmr_init(&uart->timeout_tx, 1);
    tmr_init(&uart->timeout_rx, 1);
    HAL_UART_Receive_DMA(uart->huart, uart->dma_rx_buffer, sizeof(uart->dma_rx_buffer));
    return true;
}

static void uart_poll(void *arg, struct mg_pppos *pppos) {
    uart_t *uart = arg;
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

static uart_t uart;

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

#define TACHO_BUFFER 11

typedef struct {
    volatile uint32_t buffer[TACHO_BUFFER];
    volatile uint32_t buffer_counter;
    volatile uint32_t valid;
} tacho_t;

static void tacho_init(tacho_t *tacho) {
    for(uint32_t i = 0; i < TACHO_BUFFER; i++) {
        tacho->buffer[i] = 0;
    }
    tacho->buffer_counter = 0;
    tacho->valid = 0;
    HAL_TIM_Base_Start_IT(&htim2);
}

static void tacho_read(tacho_t *tacho, float *frequency) {
    uint32_t sorted[TACHO_BUFFER];
    uint32_t valid;

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    valid = tacho->valid;
    memcpy(sorted, (uint32_t *)tacho->buffer, TACHO_BUFFER * sizeof(uint32_t));
    __set_PRIMASK(primask);

    if(valid) {
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
    } else {
        *frequency = 0.f;
    }
}

tacho_t tacho;

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin) {
    if(GPIO_Pin == GPIO_PIN_0) {
        const uint32_t period_us = __HAL_TIM_GET_COUNTER(&htim2);
        __HAL_TIM_SET_COUNTER(&htim2, 0);

        tacho.buffer[tacho.buffer_counter] = period_us;
        tacho.buffer_counter++;
        if(tacho.buffer_counter >= TACHO_BUFFER) {
            tacho.buffer_counter = 0;
            tacho.valid = 1;
        }
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if(htim == &htim2) {
        tacho.valid = 0;
        tacho.buffer_counter = 0;
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

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
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
            if((HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_RESET) &&
               (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_RESET) &&
               (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_10) == GPIO_PIN_RESET)) {
                delay_us(1);
                hx711->state = HX711_STATE_READ;
            }
        } break;
        case HX711_STATE_READ: {
            if(hx711->bits < 24) {
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET);
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
                delay_us(5);

                hx711->measurement[0] <<= 1;
                hx711->measurement[1] <<= 1;
                hx711->measurement[2] <<= 1;

                hx711->measurement[0] |= (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET);
                hx711->measurement[1] |= (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_SET);
                hx711->measurement[2] |= (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_10) == GPIO_PIN_SET);

                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
                delay_us(5);

                hx711->bits++;
            } else {
                hx711->state = HX711_STATE_END;
            }
        } break;
        case HX711_STATE_END: {
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
            delay_us(5);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
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

#define DHT22_POWERUP_MS    1000
#define DHT22_MIN_PERIOD_MS 2000
#define DHT22_START_LOW_MS  2
#define DHT22_TIMEOUT_US    100
#define DHT22_SAMPLE_US     48

typedef enum {
    DHT22_IDLE,
    DHT22_START_LOW,
} dht22_state_t;

typedef struct {
    GPIO_TypeDef *port;
    uint32_t pin;

    dht22_state_t state;
    uint32_t ts;
    uint32_t next_allowed;
} dht22_t;

static bool pin_read(const dht22_t *dht22) {
    return (dht22->port->IDR & dht22->pin) != 0u;
}

static void pin_low(const dht22_t *dht22) {
    dht22->port->BSRR = (dht22->pin << 16);
}

static void pin_high(const dht22_t *dht22) {
    dht22->port->BSRR = dht22->pin;
}

static uint8_t wait_level(const dht22_t *dht22, const uint8_t level) {
    for(uint32_t i = 0; i < DHT22_TIMEOUT_US; i++) {
        if(pin_read(dht22) == level) {
            return 1;
        }
        delay_us(1);
    }
    return 0;
}

static uint8_t capture_frame(const dht22_t *dht22, uint8_t data[5]) {
    uint8_t ok = 1;

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    pin_high(dht22);

    if(!wait_level(dht22, 0)) {
        ok = 0;
    }
    if(!wait_level(dht22, 1)) {
        ok = 0;
    }
    if(!wait_level(dht22, 0)) {
        ok = 0;
    }

    // first bit must be skipped, but why?
    wait_level(dht22, 1);
    wait_level(dht22, 0);

    if(ok) {
        data[0] = 0;
        data[1] = 0;
        data[2] = 0;
        data[3] = 0;
        data[4] = 0;

        for(uint8_t i = 0; i < 40; i++) {
            if(!wait_level(dht22, 1)) {
                ok = 0;
                break;
            }

            delay_us(DHT22_SAMPLE_US);

            data[i / 8] <<= 1;
            if(pin_read(dht22)) {
                data[i / 8] |= 0x01;

                if(!wait_level(dht22, 0)) {
                    ok = 0;
                    break;
                }
            }
        }
    }

    __set_PRIMASK(primask);
    return ok;
}

static void dht22_init(dht22_t *dht22, GPIO_TypeDef *port, const uint32_t pin) {
    dht22->port = port;
    dht22->pin = pin;
    dht22->state = DHT22_IDLE;
    dht22->ts = 0;
    dht22->next_allowed = HAL_GetTick() + DHT22_POWERUP_MS;

    pin_high(dht22);
}

static uint8_t dht22_read(dht22_t *dht22, float *humidity, float *temperature) {
    const uint32_t now = HAL_GetTick();

    switch(dht22->state) {
        case DHT22_IDLE: {
            if((int32_t)(now - dht22->next_allowed) < 0) {
                return 0;
            }
            pin_low(dht22);
            dht22->ts = now;
            dht22->state = DHT22_START_LOW;
            return 0;
        } break;
        case DHT22_START_LOW: {
            if((now - dht22->ts) < DHT22_START_LOW_MS) {
                return 0;
            }

            uint8_t data[5];
            const uint8_t ok = capture_frame(dht22, data);

            pin_high(dht22);
            dht22->state = DHT22_IDLE;
            dht22->next_allowed = HAL_GetTick() + DHT22_MIN_PERIOD_MS;

            if(!ok) {
                return 0;
            }

            const uint16_t sum = data[0] + data[1] + data[2] + data[3];
            const uint8_t checksum = sum & 0x00FF;
            if(checksum != data[4]) {
                return 0;
            }

            const uint16_t raw_humidity = (((uint16_t)data[0]) << 8) | data[1];
            const uint16_t raw_temperature = (((uint16_t)(data[2] & 0x7F)) << 8) | data[3];

            if(humidity != NULL) {
                *humidity = 0.1f * raw_humidity;
            }
            if(temperature != NULL) {
                *temperature = 0.1f * raw_temperature;

                if(data[2] & 0x80) {
                    *temperature *= -1.f;
                }
            }
            return 1;
        } break;
    }

    dht22->state = DHT22_IDLE;
    return 0;
}

typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint8_t buffer[6];
    volatile uint8_t ready;
    uint32_t last;
    uint8_t state;
} bmp280_t;

static void bmp280_write(bmp280_t *bmp280, uint8_t address, uint8_t value) {
    HAL_I2C_Mem_Write(bmp280->hi2c, BMP280_ADDR_SDO_LOW << 1, address, 1, &value, 1, 100);
}

static void bmp280_init(bmp280_t *bmp280, I2C_HandleTypeDef *hi2c) {
    bmp280->hi2c = hi2c;
    bmp280->state = 0;

    bmp280_write(bmp280, BMP280_REG_RESET, BMP280_RESET_VALUE);

    HAL_Delay(100);

    bmp280_write(bmp280, BMP280_REG_CTRL_MEAS,
                 BMP280_CTRL_TEMP_OVERSAMPLING_2 | BMP280_CTRL_PRESS_OVERSAMPLING_16 |
                     BMP280_CTRL_MODE_NORMAL);

    bmp280_write(bmp280, BMP280_REG_CONFIG,
                 BMP280_CONFIG_STANDBY_0_5MS | BMP280_CONFIG_FILTER_X16 |
                     BMP280_CONFIG_SPI_3WIRE_DISABLE);

    uint8_t buffer[24] = {0};

    HAL_I2C_Mem_Read(bmp280->hi2c, BMP280_ADDR_SDO_LOW << 1, BMP280_REG_CALIB00, 1, buffer,
                     sizeof(buffer), 100);

    dig_T1 = (((uint16_t)buffer[1]) << 8) | buffer[0];
    dig_T2 = (((int16_t)buffer[3]) << 8) | buffer[2];
    dig_T3 = (((int16_t)buffer[5]) << 8) | buffer[4];
    dig_P1 = (((uint16_t)buffer[7]) << 8) | buffer[6];
    dig_P2 = (((int16_t)buffer[9]) << 8) | buffer[8];
    dig_P3 = (((int16_t)buffer[11]) << 8) | buffer[10];
    dig_P4 = (((int16_t)buffer[13]) << 8) | buffer[12];
    dig_P5 = (((int16_t)buffer[15]) << 8) | buffer[14];
    dig_P6 = (((int16_t)buffer[17]) << 8) | buffer[16];
    dig_P7 = (((int16_t)buffer[19]) << 8) | buffer[18];
    dig_P8 = (((int16_t)buffer[21]) << 8) | buffer[20];
    dig_P9 = (((int16_t)buffer[23]) << 8) | buffer[22];

    bmp280->last = HAL_GetTick();
}

static uint8_t bmp280_read(bmp280_t *bmp280, float *pressure, float *temperature) {
    const uint32_t now = HAL_GetTick();

    switch(bmp280->state) {
        case 0: {
            if((now - bmp280->last) >= 50) {
                bmp280->last = now;
                bmp280->state = 1;
                bmp280->ready = 0;
                memset(bmp280->buffer, 0, sizeof(bmp280->buffer));
                HAL_I2C_Mem_Read_IT(bmp280->hi2c, BMP280_ADDR_SDO_LOW << 1, BMP280_REG_PRESS_MSB, 1,
                                    bmp280->buffer, sizeof(bmp280->buffer));
            }
        } break;
        case 1: {
            if(bmp280->ready) {
                bmp280->ready = 0;
                bmp280->state = 0;

                const int32_t raw_temperature = (((int32_t)bmp280->buffer[3]) << 12) |
                                                (((int32_t)bmp280->buffer[4]) << 4) |
                                                (((int32_t)bmp280->buffer[5]) >> 4);
                const int32_t raw_pressure = (((int32_t)bmp280->buffer[0]) << 12) |
                                             (((int32_t)bmp280->buffer[1]) << 4) |
                                             (((int32_t)bmp280->buffer[2]) >> 4);

                if(temperature != NULL) {
                    *temperature = bmp280_compensate_T_int32(raw_temperature) / 100.f; // *C
                }

                if(pressure != NULL) {
                    *pressure = bmp280_compensate_P_int64(raw_pressure) / 256.f; // Pa
                }

                return 1;
            }
        } break;
    }

    return 0;
}

bmp280_t bmp280;

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if(hi2c == bmp280.hi2c) {
        bmp280.ready = 1;
    } else if(hi2c == &hi2c3) {
        i2c_ready = 1;
    }
}

#define NTC_T0      (273.15f + 25.f)
#define NTC_R0      10000.f
#define NTC_B       3950.f
#define NTC_DIV_R   2470.f
#define NTC_DIV_U   3.3f
#define NTC_ADC_RES 4095.f

typedef struct {
    ADC_HandleTypeDef *hadc;
} ntc_t;

static void ntc_init(ntc_t *ntc, ADC_HandleTypeDef *hadc) {
    ntc->hadc = hadc;
}

static void ntc_read(ntc_t *ntc, float *temperature) {
    HAL_ADC_Start(ntc->hadc);

    if(HAL_ADC_PollForConversion(ntc->hadc, 10) == HAL_OK) {
        const uint32_t raw = HAL_ADC_GetValue(ntc->hadc);

        const float voltage = NTC_DIV_U * (raw / NTC_ADC_RES);
        const float resistance = (NTC_DIV_R * NTC_DIV_U / voltage) - NTC_DIV_R;
        const float temp = 1.f / (((1.f / NTC_B) * logf(resistance / NTC_R0)) + (1.f / NTC_T0));

        if(temperature != NULL) {
            *temperature = temp - 273.15f;
        }
    }

    HAL_ADC_Stop(ntc->hadc);
}

uint64_t mg_millis(void) {
    return HAL_GetTick();
}

static const struct mg_mem_file files[2] = {
    {"/web/index.html", (const unsigned char *)_binary_index_html_start,
     (size_t)_binary_index_html_size,                                       0},
    {NULL,              NULL,                                            0, 0},
};

static void http_event(struct mg_connection *c, int ev, void *ev_data) {
    context_t *context = c->fn_data;

    switch(ev) {
        case MG_EV_HTTP_MSG: {
            struct mg_http_message *hm = ev_data;

            if(mg_match(hm->uri, mg_str("/websocket"), NULL)) {
                mg_ws_upgrade(c, hm, NULL);
                return;
            }

            const struct mg_http_serve_opts opts = {
                .root_dir = "/web",
                .fs = &mg_fs_packed,
            };
            mg_mem_files = files;
            mg_http_serve_dir(c, hm, &opts);
        } break;
        case MG_EV_WS_MSG: {
            struct mg_ws_message *wm = ev_data;

            struct mg_str cmd;
            struct mg_str arg;
            mg_span(wm->data, &cmd, &arg, ' ');

            if(mg_strcmp(cmd, mg_str("offset")) == 0) {
                context->button = 1;
            }

            if(mg_strcmp(cmd, mg_str("generator")) == 0) {
                if(arg.len > 0) {
                    uint32_t value = 0;
                    if(mg_str_to_num(arg, 10, &value, sizeof(value))) {
                        if(value > 1000) {
                            value = 1000;
                        }
                        context->generator = 0.001f * value;
                    }
                }
            }
        } break;
    }
}

int main() {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_I2C2_Init();
    MX_I2C3_Init();
    MX_USART2_UART_Init();

    hx711_t hx711;
    ina226_t ina226;
    dht22_t dht22;
    ntc_t ntc;
    context_t context = {0};

    HAL_TIM_Base_Start(&htim1);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
    tacho_init(&tacho);
    hx711_init(&hx711);
    ina226_init(&ina226);
    dht22_init(&dht22, GPIOA, GPIO_PIN_11);
    bmp280_init(&bmp280, &hi2c2);
    ntc_init(&ntc, &hadc1);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_pppos pppos;
    mg_pppos_init_state(&pppos, uart_write, uart_init, uart_poll, NULL, &uart);

    struct mg_tcpip_if netif = {
        .driver = mg_pppos_driver(&pppos),
        .driver_data = &pppos,
        .l2type = MG_TCPIP_L2_PPP,
    };
    mg_tcpip_init(&mgr, &netif);

    mg_http_listen(&mgr, "http://0.0.0.0:80", http_event, &context);

    uint32_t prev1 = 0;
    uint32_t prev2 = 0;

    float thrust = 0;
    float torque = 0;
    float velocity = 0;
    float voltage = 0;
    float current = 0;
    int32_t load[3] = {0};
    float frequency = 0;
    float temperature = 0;
    float env1_humidity = 0;
    float env1_temperature = 0;
    float env2_pressure = 0;
    float env2_temperature = 0;

    int32_t load_raw[3] = {0};
    int32_t load_offset[3] = {0};

    while(1) {
        const uint32_t timestamp = HAL_GetTick();

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                          ((timestamp % 1000) < 50) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        if(hx711_read(&hx711, load_raw)) {
            if(context.button) {
                context.button = 0;
                load_offset[0] = load_raw[0];
                load_offset[1] = load_raw[1];
                load_offset[2] = load_raw[2];
            }

            load[0] = load_raw[0] - load_offset[0];
            load[1] = load_raw[1] - load_offset[1];
            load[2] = load_raw[2] - load_offset[2];

            const float g = 9.8067f;
            const float arm = 0.270f;
            const float k = 0.265f / -105000.f;

            const float mass = 0.226f;
            const float t1 = -mass * arm * g;
            const float l11 = 1981956.f;
            const float l21 = 1409267.f;
            const float t2 = +mass * arm * g;
            const float l12 = -1503401.f;
            const float l22 = -1901835.f;

            const float k1 = ((t1 * l22) - (t2 * l21)) / ((l11 * l22) - (l12 * l21));
            const float k2 = (-(t1 * l12) + (t2 * l11)) / ((l11 * l22) - (l12 * l21));

            thrust = load[2] * k * g;
            torque = (k1 * load[0]) + (k2 * load[1]);
        }

        dht22_read(&dht22, &env1_humidity, &env1_temperature);
        bmp280_read(&bmp280, &env2_pressure, &env2_temperature);

        if((timestamp - prev1) >= 50) {
            prev1 = timestamp;

            ina226_read(&ina226, &voltage, &current);
            tacho_read(&tacho, &frequency);
            ntc_read(&ntc, &temperature);

            velocity = 6.283185307f * frequency;

            __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 1000 + (1000 * context.generator));
        }

        if((timestamp - prev2) >= 100) {
            prev2 = timestamp;

            char json[512];
            const uint32_t json_len =
                mg_snprintf(json, sizeof(json),
                            "{"
                            "\"thrust\":%f,"
                            "\"thrust_raw\":%ld,"
                            "\"torque\":%f,"
                            "\"torque_raw1\":%ld,"
                            "\"torque_raw2\":%ld,"
                            "\"vel\":%f,"
                            "\"vel_raw\":%f,"
                            "\"curr\":%f,"
                            "\"volt\":%f,"
                            "\"temp\":%f,"
                            "\"env1_hum\":%f,"
                            "\"env1_temp\":%f,"
                            "\"env2_press\":%f,"
                            "\"env2_temp\":%f,"
                            "\"gen\":%f"
                            "}",
                            thrust, load[2], torque, load[0], load[1], velocity, frequency, current,
                            voltage, temperature, env1_humidity, env1_temperature, env2_pressure,
                            env2_temperature, context.generator);

            for(struct mg_connection *c = mgr.conns; c != NULL; c = c->next) {
                if(c->is_websocket) {
                    mg_ws_send(c, json, json_len, WEBSOCKET_OP_TEXT);
                }
            }
        }

        mg_mgr_poll(&mgr, 1);
    }

    return 0;
}
