#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "ws2812.pio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// ── LED 矩陣定義 ──────────────────────────────────────────────────────────────
#define LED_PIN 0
#define LED_COUNT 64
#define MATRIX_W 8
#define ZONE_EDGE_RATIO 0.35f

#define LED_ID_UART_ERROR 56
#define LED_ID_UART_ERROR2 57

static inline int is_center(int idx)
{
    int row = idx / MATRIX_W;
    int col = idx % MATRIX_W;
    return (row >= 2 && row <= 5 && col >= 2 && col <= 5);
}

static inline int is_diag(int idx)
{
    return (idx == LED_ID_UART_ERROR || idx == LED_ID_UART_ERROR2);
}

// ── BH1750 ───────────────────────────────────────────────────────────────────
#define I2C_PORT i2c0
#define I2C_SDA 4
#define I2C_SCL 5
#define BH1750_ADDR 0x23
#define BH1750_POWER_ON 0x01
#define BH1750_CONT_H_MODE 0x10

void bh1750_init(void)
{
    i2c_init(I2C_PORT, 400000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    uint8_t cmd = BH1750_POWER_ON;
    i2c_write_blocking(I2C_PORT, BH1750_ADDR, &cmd, 1, false);
    sleep_ms(10);
    cmd = BH1750_CONT_H_MODE;
    i2c_write_blocking(I2C_PORT, BH1750_ADDR, &cmd, 1, false);
    sleep_ms(180);
}

float bh1750_read_lux(void)
{
    uint8_t buf[2];
    int ret = i2c_read_timeout_us(I2C_PORT, BH1750_ADDR, buf, 2, false, 25000);
    if (ret != 2)
        return -1.0f;
    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    return raw / 1.2f;
}

// ── 系統狀態 ──────────────────────────────────────────────────────────────────
#define UART_TIMEOUT_MS 3000

typedef enum
{
    STATE_ACTIVE,
    STATE_IDLE,
    STATE_SLEEP,
    STATE_ERROR
} SystemState;
typedef enum
{
    AUTO_MODE,
    MANUAL_FIXED,
} SystemMode;
typedef enum
{
    WHITE,
    IVORY,
    WARM
} ColorType;

static volatile SystemState current_state = STATE_ACTIVE;
static volatile SystemMode current_mode = AUTO_MODE;
static volatile ColorType current_color = IVORY;
static volatile float manual_brightness = 0.6f;
static volatile uint8_t manual_rgb[3] = {255, 255, 255};
static volatile int error_flag = 0;
static volatile bool uart_disconnected = false;

static uint8_t color_presets[3][3] = {
    {255, 255, 255},
    {255, 160, 80},
    {255, 150, 50},
};

static const char *state_name[] = {"ACTIVE", "IDLE", "SLEEP", "ERROR"};

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    while (1)
    {
        tight_loop_contents();
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    while (1)
    {
        tight_loop_contents();
    }
}

// ── PIO / LED ─────────────────────────────────────────────────────────────────
static float current_brightness = 0.0f;

static inline void put_pixel(uint32_t pixel_grb)
{
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b, float bright)
{
    if (bright < 0.0f)
        bright = 0.0f;
    if (bright > 1.0f)
        bright = 1.0f;
    return ((uint32_t)((uint8_t)(g * bright)) << 16) | ((uint32_t)((uint8_t)(r * bright)) << 8) | (uint32_t)((uint8_t)(b * bright));
}

void smooth_fade(float target, float step)
{
    if (current_brightness < target - step)
        current_brightness += step;
    else if (current_brightness > target + step)
        current_brightness -= step;
    else
        current_brightness = target;
}

void set_zone_leds_with_diag(uint8_t r, uint8_t g, uint8_t b,
                             float main_bright, bool diag_on)
{
    float edge_bright = main_bright * ZONE_EDGE_RATIO;
    for (int i = 0; i < LED_COUNT; i++)
    {
        if (uart_disconnected && diag_on && is_diag(i))
            put_pixel(urgb_u32(255, 0, 0, 0.7f));
        else
        {
            float bright = is_center(i) ? main_bright : edge_bright;
            put_pixel(urgb_u32(r, g, b, bright));
        }
    }
}

void set_all_leds_with_diag(uint8_t r, uint8_t g, uint8_t b,
                            float bright, bool diag_on)
{
    for (int i = 0; i < LED_COUNT; i++)
    {
        if (uart_disconnected && diag_on && is_diag(i))
            put_pixel(urgb_u32(255, 0, 0, 0.7f));
        else
            put_pixel(urgb_u32(r, g, b, bright));
    }
}

static int error_toggle = 0;
void error_flash(bool diag_on)
{
    error_toggle = !error_toggle;
    set_all_leds_with_diag(
        error_toggle ? 255 : 0, 0, 0,
        error_toggle ? 0.6f : 0.0f,
        diag_on);
}

float brightness_from_lux(float lux)
{
    if (lux > 600.0f)
        return 0.1f;
    else if (lux > 200.0f)
        return 0.35f;
    else
        return 0.95f;
}

// ── USB CDC 讀一行（非阻塞）──────────────────────────────────────────────────
static char uart_buf[64];
static int uart_pos = 0;

int uart_read_line(void)
{
    int c;
    while (uart_is_readable(uart1))
    {
        c = uart_getc(uart1);
        if (c == '\r')
            continue;
        if (c == '\n')
        {
            uart_buf[uart_pos] = '\0';
            uart_pos = 0;
            return 1;
        }
        if (uart_pos < (int)sizeof(uart_buf) - 1)
            uart_buf[uart_pos++] = (char)c;
    }
    return 0;
}

void parse_command(char *cmd)
{
    if (strncmp(cmd, "STATE:", 6) == 0)
    {
        char *s = cmd + 6;
        if (strcmp(s, "ACTIVE") == 0)
        {
            current_state = STATE_ACTIVE;
            error_flag = 0;
        }
        else if (strcmp(s, "IDLE") == 0)
        {
            current_state = STATE_IDLE;
            error_flag = 0;
        }
        else if (strcmp(s, "SLEEP") == 0)
        {
            current_state = STATE_SLEEP;
            error_flag = 0;
        }
        else if (strcmp(s, "ERROR") == 0)
        {
            current_state = STATE_ERROR;
            error_flag = 1;
        }
    }
    else if (strncmp(cmd, "MODE:", 5) == 0)
    {
        int m = atoi(cmd + 5);
        if (m >= 0 && m <= 1)
            current_mode = (SystemMode)m;
    }
    else if (strncmp(cmd, "COLOR:", 6) == 0)
    {
        int c = atoi(cmd + 6);
        if (c >= 0 && c <= 2)
            current_color = (ColorType)c;
    }
    else if (strncmp(cmd, "LED:", 4) == 0)
    {
        if (current_mode != MANUAL_FIXED)
            return;
        int r, g, b, pct;
        if (sscanf(cmd + 4, "R%dG%dB%dL%d", &r, &g, &b, &pct) == 4)
        {
            manual_rgb[0] = (uint8_t)r;
            manual_rgb[1] = (uint8_t)g;
            manual_rgb[2] = (uint8_t)b;
            manual_brightness = pct / 100.0f;
        }
    }
    else if (strncmp(cmd, "ERROR:", 6) == 0)
    {
        error_flag = atoi(cmd + 6);
        if (error_flag)
            current_state = STATE_ERROR;
    }
}

// ── Queue handle ──────────────────────────────────────────────────────────────
static QueueHandle_t lux_queue;
static QueueHandle_t cmd_queue;

// ── Task 1：每 500ms 讀 BH1750 ────────────────────────────────────────────────
void lux_task(void *pvParameters)
{
    float lux = 0.0f;
    while (1)
    {
        float new_lux = bh1750_read_lux();
        if (new_lux >= 0.0f)
            lux = new_lux;
        xQueueOverwrite(lux_queue, &lux);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


void uart_rx_task(void *pvParameters)
{
    char cmd[64];
    while (1)
    {
        if (uart_read_line())
        {
            strncpy(cmd, uart_buf, sizeof(cmd));
            xQueueSend(cmd_queue, cmd, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Task 3：燈光控制主邏輯 ───────────────────────────────────────────────────
void lighting_task(void *pvParameters)
{
    float lux = 0.0f;
    char cmd[64];
    TickType_t last_uart_rx = xTaskGetTickCount();
    TickType_t last_error_blink = 0;
    TickType_t last_diag_blink = 0;
    bool diag_on = false;

    while (1)
    {
        TickType_t now = xTaskGetTickCount();

        // 有新指令就處理，並更新斷訊計時
        if (xQueueReceive(cmd_queue, cmd, 0) == pdTRUE)
        {
            parse_command(cmd);
            last_uart_rx = now;
        }

        // 取最新 lux（不等待）
        xQueuePeek(lux_queue, &lux, 0);

        // 斷訊偵測
        // 手動模式不觸發斷訊診斷燈
        if (current_mode == MANUAL_FIXED)
        {
            uart_disconnected = false;
        }
        else
        {
            uart_disconnected = ((now - last_uart_rx) > pdMS_TO_TICKS(UART_TIMEOUT_MS));
        }

        // 診斷燈 toggle
        if (uart_disconnected)
        {
            if ((now - last_diag_blink) >= pdMS_TO_TICKS(500))
            {
                diag_on = !diag_on;
                last_diag_blink = now;
            }
        }
        else
        {
            diag_on = false;
            last_diag_blink = now;
        }

        // 燈光控制
        if (current_state == STATE_ERROR)
        {
            if ((now - last_error_blink) >= pdMS_TO_TICKS(200))
            {
                error_flash(diag_on);
                last_error_blink = now;
            }
        }
        else
        {
            float target = 0.0f;

            if (current_state == STATE_SLEEP)
            {
                target = 0.0f;
            }
            else if (current_state == STATE_IDLE)
            {
                if (current_mode == MANUAL_FIXED)
                    target = manual_brightness;
                else
                    target = brightness_from_lux(lux) * 0.3f;
            }
            else
            { // STATE_ACTIVE
                if (current_mode == AUTO_MODE)
                    target = brightness_from_lux(lux);
                else if (current_mode == MANUAL_FIXED)
                    target = manual_brightness;
            }

            smooth_fade(target, 0.005f);
            uint8_t *c = (current_mode == MANUAL_FIXED)
                             ? (uint8_t *)manual_rgb
                             : color_presets[current_color];
            set_zone_leds_with_diag(c[0], c[1], c[2], current_brightness, diag_on);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Task 4：每秒回報 LUX 和 STATE ─────────────────────────────────────────────
void heartbeat_task(void *pvParameters)
{
    float lux = 0.0f;
    char buf[64];
    while (1)
    {
        xQueuePeek(lux_queue, &lux, 0);
        snprintf(buf, sizeof(buf), "LUX:%.1f\n", lux);
        uart_puts(uart1, buf);
        snprintf(buf, sizeof(buf), "STATE:%s\n", state_name[current_state]);
        uart_puts(uart1, buf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(void)
{

    uart_init(uart1, 115200);
    gpio_set_function(8, GPIO_FUNC_UART); // TX
    gpio_set_function(9, GPIO_FUNC_UART); // RX

    if (cyw43_arch_init())
        return -1;

    bh1750_init();

    uint offset = pio_add_program(pio0, &ws2812_program);
    ws2812_program_init(pio0, 0, offset, LED_PIN, 800000, false);

    lux_queue = xQueueCreate(1, sizeof(float));
    cmd_queue = xQueueCreate(8, sizeof(char[64]));

    xTaskCreate(lux_task, "lux", 512, NULL, 1, NULL);
    xTaskCreate(uart_rx_task, "uart_rx", 512, NULL, 3, NULL);
    xTaskCreate(lighting_task, "lighting", 1024, NULL, 2, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 512, NULL, 1, NULL);

    vTaskStartScheduler();

    // 不會執行到這裡
    return 0;
}