#include <string.h>

#include <stm32u0xx_hal.h>

#include <lwip/apps/fs.h>
#include <lwip/apps/httpd.h>
#include <lwip/apps/lwiperf.h>
#include <lwip/init.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <netif/ppp/ppp.h>
#include <netif/ppp/pppos.h>

#include "ina226_regs.h"
#include "websocket.h"

typedef struct {
    uint8_t buffer[1024];
    volatile uint32_t rd;
    volatile uint32_t wr;
} fifo_t;

typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_LOOP,
} state_t;

typedef enum {
    BUTTON_NONE,
    BUTTON_OFFSET,
} button_t;

typedef struct {
    fifo_t fifo_tx;
    fifo_t fifo_rx;
    state_t state;
    button_t button;
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

static volatile uint8_t send_ready = 1;
static volatile uint8_t recv_byte;
static context_t context;

void SystemClock_Config();
void MX_GPIO_Init();
void MX_TIM1_Init();
void MX_TIM2_Init();
void MX_I2C3_Init();
void MX_USART2_UART_Init();

static void fifo_init(fifo_t *fifo) {
    fifo->rd = 0;
    fifo->wr = 0;
}

static uint32_t fifo_write(fifo_t *fifo, const uint8_t *src, uint32_t src_len) {
    uint32_t num = 0;
    while(num < src_len) {
        uint32_t next = fifo->wr + 1;
        if(next >= sizeof(fifo->buffer)) {
            next = 0;
        }
        if(next == fifo->rd) {
            break;
        }
        fifo->buffer[fifo->wr] = src[num];
        fifo->wr = next;
        num++;
    }
    return num;
}

static uint32_t fifo_read(fifo_t *fifo, uint8_t *dst, const uint32_t dst_capacity) {
    uint32_t num = 0;
    while((fifo->rd != fifo->wr) && (num < dst_capacity)) {
        dst[num] = fifo->buffer[fifo->rd];
        fifo->rd++;
        num++;
        if(fifo->rd >= sizeof(fifo->buffer)) {
            fifo->rd = 0;
        }
    }
    return num;
}

static void ppp_link_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
    (void)pcb;

    context_t *context = ctx;

    if(err_code == PPPERR_NONE) {
        if(context->state == STATE_CONNECTING) {
            context->state = STATE_CONNECTED;
        }
    }
}

static uint32_t ppp_output_cb(ppp_pcb *pcb, const void *data, uint32_t data_size, void *ctx) {
    (void)pcb;

    context_t *context = ctx;

    return fifo_write(&context->fifo_tx, data, data_size);
}

static void ws_on_message_cb(void *arg,
                             struct websocket_client *client,
                             const void *message,
                             u32_t message_len) {
    (void)client;
    (void)message_len;

    context_t *context = arg;

    if(strcmp(message, "offset") == 0) {
        context->button = BUTTON_OFFSET;
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

sys_prot_t sys_arch_protect() {
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}

uint32_t sys_now() {
    return HAL_GetTick();
}

uint32_t sys_jiffies() {
    return HAL_GetTick();
}

int fs_open_custom(struct fs_file *file, const char *name) {
    if((strcmp(name, "") == 0) || (strcmp(name, "/") == 0)) {
        name = "index.html";
    }

    if(strcmp(name, "/index.html") == 0) {
        file->data = _binary_index_html_start;
        file->len = _binary_index_html_end - _binary_index_html_start;
        file->index = 0;
        file->flags = 0;
        return 1;
    }

    if(strcmp(name, "/style.css") == 0) {
        file->data = _binary_style_css_start;
        file->len = _binary_style_css_end - _binary_style_css_start;
        file->index = 0;
        file->flags = 0;
        return 1;
    }

    if(strcmp(name, "/script.js") == 0) {
        file->data = _binary_script_js_start;
        file->len = _binary_script_js_end - _binary_script_js_start;
        file->index = 0;
        file->flags = 0;
        return 1;
    }

    return 0;
}

void fs_close_custom(struct fs_file *file) {
    (void)file;
}

int fs_read_custom(struct fs_file *file, char *buffer, int count) {
    int read = file->len - file->index;
    if(read > count) {
        read = count;
    }
    memcpy(buffer, (file->data + file->index), read);
    file->index += read;
    return read;
}

tacho_t tacho;

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin) {
    if(GPIO_Pin == GPIO_PIN_0) {
        tacho_isr(&tacho);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == &huart2) {
        send_ready = 1;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == &huart2) {
        fifo_write(&context.fifo_rx, (uint8_t *)&recv_byte, 1);
        HAL_UART_Receive_IT(&huart2, (uint8_t *)&recv_byte, 1);
    }
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if(hi2c == &hi2c3) {
        i2c_ready = 1;
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

int main() {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_I2C3_Init();
    MX_USART2_UART_Init();

    lwip_init();

    struct netif netif = {0};
    ppp_pcb *ppp = pppos_create(&netif, ppp_output_cb, ppp_link_status_cb, &context);
    ppp_set_default(ppp);

    websocket_server_t *ws_server = NULL;

    context.state = STATE_DISCONNECTED;
    fifo_init(&context.fifo_tx);
    fifo_init(&context.fifo_rx);
    HAL_UART_Receive_IT(&huart2, (uint8_t *)&recv_byte, 1);

    HAL_TIM_Base_Start(&htim1);

    tacho_init(&tacho);

    hx711_t hx711;
    hx711_init(&hx711);

    ina226_t ina226;
    ina226_init(&ina226);

    ds18b20_t ds18b20;
    ds18b20_init(&ds18b20);

    uint32_t prev1 = 0;
    uint32_t prev2 = 0;
    uint8_t send_buffer[1024];
    uint8_t recv_buffer[1024];

    frame_t frame = {0};

    int32_t load_raw[3] = {0};
    int32_t load_offset[3] = {0};

    while(1) {
        const uint32_t timestamp = HAL_GetTick();

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                          ((timestamp % 1000) < 50) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        switch(context.state) {
            case STATE_DISCONNECTED: {
                context.state = STATE_CONNECTING;
                ppp_connect(ppp, 0);
            } break;
            case STATE_CONNECTING: {

            } break;
            case STATE_CONNECTED: {
                lwiperf_start_tcp_server_default(NULL, NULL);

                httpd_init();

                ws_server = websocket_server_new(81);
                websocket_arg(ws_server, &context);
                websocket_on_message(ws_server, ws_on_message_cb);

                context.state = STATE_LOOP;
            } break;
            case STATE_LOOP: {
                if((timestamp - prev1) >= 100) {
                    prev1 = timestamp;

                    for(struct websocket_client *client = ws_server->clients; client != NULL;
                        client = client->next) {
                        websocket_send(client, &frame, sizeof(frame));
                    }
                }
            } break;
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

        const uint32_t recv_len = fifo_read(&context.fifo_rx, recv_buffer, sizeof(recv_buffer));
        if(recv_len > 0) {
            pppos_input(ppp, recv_buffer, recv_len);
        }

        if(send_ready) {
            const uint32_t send_len = fifo_read(&context.fifo_tx, send_buffer, sizeof(send_buffer));
            if(send_len > 0) {
                send_ready = 0;
                HAL_UART_Transmit_IT(&huart2, send_buffer, send_len);
            }
        }

        sys_check_timeouts();
    }

    return 0;
}
