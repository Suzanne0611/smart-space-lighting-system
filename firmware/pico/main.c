#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "ws2812.pio.h"

// ── Hardware Configuration ──────────────────────────────────────────────────
#define LED_PIN     0
#define LED_COUNT   8

// Zone Definitions (8 LEDs)
// Edge: 0, 1, 6, 7  -> Dimmed (via ZONE_EDGE_RATIO)
// Core: 2 ~ 5       -> Primary Brightness
#define ZONE_EDGE_RATIO  0.35f   // Edge Brightness = Main Brightness * Ratio

// ── UART Configuration ──────────────────────────────────────────────────────
#define UART_ID     uart0
#define UART_TX_PIN 12
#define UART_RX_PIN 13
#define UART_BAUD   115200

// ── System Finite State Machine (FSM) ───────────────────────────────────────
typedef enum {
    STATE_ACTIVE,   // User present, normal operation
    STATE_IDLE,     // Short-term inactivity, power-saving mode
    STATE_SLEEP,    // Long-term inactivity, LEDs OFF
    STATE_ERROR     // Hardware fault, red alert flashing
} SystemState;

SystemState system_state = STATE_ACTIVE;

// Inactivity Counter (Simulated; to be integrated with PIR/Pressure sensors)
static int idle_counter  = 0;
#define IDLE_THRESHOLD   200    // Approx. 200 * 10ms = 2s -> IDLE
#define SLEEP_THRESHOLD  600    // Approx. 600 * 10ms = 6s -> SLEEP

int error_flag = 0; // Triggered via UART or hardware fault

// ── Mode & Color Definitions ────────────────────────────────────────────────
typedef enum { AUTO_MODE, MANUAL_FIXED, MANUAL_GRADIENT } SystemMode;
typedef enum { WHITE, IVORY, WARM } ColorType;

SystemMode current_mode      = AUTO_MODE;
ColorType  current_color     = IVORY;
float      manual_brightness = 0.6f;

uint8_t color_presets[3][3] = {
    {255, 255, 255},  // WHITE
    {255, 230, 150},  // IVORY
    {255, 150,  50},  // WARM
};

// ── Smooth Fade Logic ───────────────────────────────────────────────────────
static float current_brightness = 0.0f; // Current real-time brightness

/**
 * Approaches target brightness smoothly by a fixed step
 */
void smooth_fade(float target, float step) {
    if (current_brightness < target - step)
        current_brightness += step;
    else if (current_brightness > target + step)
        current_brightness -= step;
    else
        current_brightness = target;
}

// ── PIO / WS2812 Output ─────────────────────────────────────────────────────
static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b, float bright) {
    if (bright < 0.0f) bright = 0.0f;
    if (bright > 1.0f) bright = 1.0f;
    return ((uint32_t)((uint8_t)(g * bright)) << 16)
         | ((uint32_t)((uint8_t)(r * bright)) <<  8)
         |  (uint32_t)((uint8_t)(b * bright));
}

// ── Zoned LED Control ───────────────────────────────────────────────────────
void set_zone_leds(uint8_t r, uint8_t g, uint8_t b, float main_bright) {
    float edge_bright = main_bright * ZONE_EDGE_RATIO;
    for (int i = 0; i < LED_COUNT; i++) {
        int is_edge = (i == 0 || i == 1 || i == 6 || i == 7);
        float b_val = is_edge ? edge_bright : main_bright;
        put_pixel(urgb_u32(r, g, b, b_val));
    }
}

void set_all_leds(uint8_t r, uint8_t g, uint8_t b, float bright) {
    uint32_t color = urgb_u32(r, g, b, bright);
    for (int i = 0; i < LED_COUNT; i++) put_pixel(color);
}

// ── Error Alert Logic ───────────────────────────────────────────────────────
static int error_toggle = 0;

void error_flash() {
    error_toggle = !error_toggle;
    if (error_toggle)
        set_all_leds(255, 0, 0, 0.6f);   // Flash Red
    else
        set_all_leds(0, 0, 0, 0.0f);      // Off
}

// ── Simulated Sensor Input ──────────────────────────────────────────────────
float get_simulated_lux() {
    static float lux = 400.0f;
    static int   dir = 1;
    lux += 2.0f * dir;
    if (lux >= 800.0f || lux <= 50.0f) dir *= -1;
    return lux;
}

float brightness_from_lux(float lux) {
    if      (lux > 600.0f) return 0.3f;
    else if (lux > 300.0f) return 0.6f;
    else                   return 0.9f;
}

// ── FSM Update Logic ────────────────────────────────────────────────────────
void update_state(float lux) {
    if (error_flag) {
        system_state = STATE_ERROR;
        idle_counter = 0;
        return;
    }

    // Using low lux as a proxy for 'no user presence'
    if (lux < 80.0f) {
        idle_counter++;
    } else {
        idle_counter = 0;
    }

    if      (idle_counter > SLEEP_THRESHOLD) system_state = STATE_SLEEP;
    else if (idle_counter > IDLE_THRESHOLD)  system_state = STATE_IDLE;
    else                                     system_state = STATE_ACTIVE;
}

// ── Non-blocking UART Reader ────────────────────────────────────────────────
static char uart_buf[64];
static int  uart_pos = 0;

int uart_read_line(void) {
    while (uart_is_readable(UART_ID)) {
        char c = uart_getc(UART_ID);
        if (c == '\r') continue;
        if (c == '\n') {
            uart_buf[uart_pos] = '\0';
            uart_pos = 0;
            return 1;
        }
        if (uart_pos < (int)sizeof(uart_buf) - 1)
            uart_buf[uart_pos++] = c;
    }
    return 0;
}

// ── Command Parser (Interface with RPi4) ────────────────────────────────────
void parse_command(char *cmd) {
    if (strncmp(cmd, "LED:", 4) == 0) {
        int r, g, b, bright_pct;
        if (sscanf(cmd + 4, "R%dG%dB%dB%d", &r, &g, &b, &bright_pct) == 4) {
            color_presets[current_color][0] = (uint8_t)r;
            color_presets[current_color][1] = (uint8_t)g;
            color_presets[current_color][2] = (uint8_t)b;
            manual_brightness = bright_pct / 100.0f;
            current_mode = MANUAL_FIXED;
            uart_puts(UART_ID, "[LC] LED command received. Switching to Manual-Fixed mode.\n");
        }
    }
    else if (strncmp(cmd, "MODE:", 5) == 0) {
        int mode = atoi(cmd + 5);
        if (mode >= 0 && mode <= 2) {
            current_mode = (SystemMode)mode;
            const char *names[] = {"AUTO", "MANUAL_FIXED", "MANUAL_GRADIENT"};
            char msg[64];
            snprintf(msg, sizeof(msg), "[MODE] Current Mode: %s\n", names[mode]);
            uart_puts(UART_ID, msg);
        }
    }
    else if (strncmp(cmd, "COLOR:", 6) == 0) {
        int color = atoi(cmd + 6);
        if (color >= 0 && color <= 2) {
            current_color = (ColorType)color;
            const char *names[] = {"WHITE", "IVORY", "WARM"};
            char msg[64];
            snprintf(msg, sizeof(msg), "[COLOR] Current Color: %s\n", names[color]);
            uart_puts(UART_ID, msg);
        }
    }
    else if (strncmp(cmd, "ERROR:", 6) == 0) {
        error_flag = atoi(cmd + 6);
        char msg[48];
        snprintf(msg, sizeof(msg), "[SYS] error_flag set to %d\n", error_flag);
        uart_puts(UART_ID, msg);
    }
}

static const char *state_name[] = { "ACTIVE", "IDLE", "SLEEP", "ERROR" };

// ── Main Loop ───────────────────────────────────────────────────────────────
int main() {
    stdio_init_all();
    sleep_ms(2000);

    if (cyw43_arch_init()) {
        uart_puts(UART_ID, "cyw43 init failed\n");
        return -1;
    }

    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    uint offset = pio_add_program(pio0, &ws2812_program);
    ws2812_program_init(pio0, 0, offset, LED_PIN, 800000, false);

    uart_puts(UART_ID, "=== Initialization Complete. System Booting... ===\n");
    uart_puts(UART_ID, "LEDs: 8 | Zones: Core(2-5), Edge(0,1,6,7)\n");
    uart_puts(UART_ID, "Features: FSM + Zoning + Fade + Error Alerts\n\n");

    float    gradient_step    = 0.0f;
    uint32_t last_send        = 0;
    uint32_t last_error_blink = 0;

    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // ── Sensor Reading ──
        float lux = get_simulated_lux();

        // ── FSM Update ──
        update_state(lux);

        // ── RPi4 Command Intake ──
        if (uart_read_line()) {
            parse_command(uart_buf);
        }

        // ── Telemetry Output (Every 1s) ──
        if (now - last_send >= 1000) {
            char msg[160];

            if (system_state == STATE_ERROR) {
                snprintf(msg, sizeof(msg), "[ERROR] System Fault | flag=%d | Red alert active\n", error_flag);
            } else if (system_state == STATE_SLEEP) {
                snprintf(msg, sizeof(msg), "[SLEEP] Inactive | lux: %.1f | LEDs OFF\n", lux);
            } else if (system_state == STATE_IDLE) {
                snprintf(msg, sizeof(msg), "[IDLE] Short Inactivity | lux: %.1f | Power Saving (20%%)\n", lux);
            } else {
                if (current_mode == AUTO_MODE) {
                    int pct = (lux > 600.0f) ? 30 : (lux > 300.0f ? 60 : 90);
                    snprintf(msg, sizeof(msg), "[ACTIVE/AUTO] Lux: %.1f | Core: %d%% | Edge: %.0f%%\n",
                             lux, pct, pct * ZONE_EDGE_RATIO);
                } else if (current_mode == MANUAL_FIXED) {
                    snprintf(msg, sizeof(msg), "[ACTIVE/MANUAL] Color: %d | Core: %.0f%% | Edge: %.0f%%\n",
                             current_color, manual_brightness * 100.0f, manual_brightness * ZONE_EDGE_RATIO * 100.0f);
                } else {
                    float b = 0.35f + 0.25f * sinf(gradient_step);
                    snprintf(msg, sizeof(msg), "[ACTIVE/GRADIENT] Core: %.0f%% | Edge: %.0f%%\n",
                             b * 100.0f, b * ZONE_EDGE_RATIO * 100.0f);
                }
            }
            uart_puts(UART_ID, msg);

            // Reporting LUX to Kernel Module & STATE to RPi4
            char telemetry[64];
            snprintf(telemetry, sizeof(telemetry), "LUX:%.1f\nSTATE:%s\n", lux, state_name[system_state]);
            uart_puts(UART_ID, telemetry);

            last_send = now;
        }

        // ── Lighting Control Logic ──
        if (system_state == STATE_ERROR) {
            if (now - last_error_blink >= 200) {
                error_flash();
                last_error_blink = now;
            }
        } else if (system_state == STATE_SLEEP) {
            smooth_fade(0.0f, 0.005f);
            uint8_t *c = color_presets[current_color];
            set_zone_leds(c[0], c[1], c[2], current_brightness);
        } else if (system_state == STATE_IDLE) {
            smooth_fade(0.2f, 0.005f);
            uint8_t *c = color_presets[current_color];
            set_zone_leds(c[0], c[1], c[2], current_brightness);
        } else {
            float target;
            uint8_t r, g, b;
            if (current_mode == AUTO_MODE) {
                target = brightness_from_lux(lux);
                r = color_presets[IVORY][0]; g = color_presets[IVORY][1]; b = color_presets[IVORY][2];
            } else if (current_mode == MANUAL_FIXED) {
                target = manual_brightness;
                r = color_presets[current_color][0]; g = color_presets[current_color][1]; b = color_presets[current_color][2];
            } else {
                gradient_step += 0.05f;
                target = 0.35f + 0.25f * sinf(gradient_step);
                r = color_presets[current_color][0]; g = color_presets[current_color][1]; b = color_presets[current_color][2];
            }
            smooth_fade(target, 0.005f);
            set_zone_leds(r, g, b, current_brightness);
        }

        sleep_ms(10); // Faster polling for smoother fading
    }
    return 0;
}