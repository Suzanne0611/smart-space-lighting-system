/**
 * Functionality:
 * 1. read(/dev/light_sensor) -> Get lux value (Blocking; waits for new data)
 * 2. read(/dev/presence)     -> Get PIR status
 * 3. Determine STATE based on idle_counter
 * 4. write(/dev/lighting)    -> Send STATE to Pico W
 *
 * Compilation:
 * gcc -o lighting_daemon lighting_daemon.c
 *
 * Execution:
 * sudo /root/lighting_daemon/lighting_daemon
 *
 * Termination:
 * sudo kill $(pgrep lighting_daemon)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#define DEV_LIGHT_SENSOR  "/dev/light_sensor"
#define DEV_LIGHTING      "/dev/lighting"
#define DEV_PRESENCE "/dev/presence"

#define IDLE_THRESHOLD    120
#define SLEEP_THRESHOLD   300

static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
    printf("\n[daemon] Termination signal received. Exiting...\n");
}

int send_state(int lighting_fd, const char *state) {
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "STATE:%s\n", state);
    int  ret = write(lighting_fd, buf, len);
    if (ret < 0) {
        perror("[daemon] write /dev/lighting failed");
        return -1;
    }
    printf("[daemon] → Pico W: STATE:%s\n", state);
    return 0;
}

float parse_lux(const char *buf) {
    if (strncmp(buf, "ERROR:TIMEOUT", 13) == 0) {
        printf("[daemon] /dev/light_sensor Timed out. Waiting to recover...\n");
        return -1.0f;
    }
    if (strncmp(buf, "LUX:", 4) == 0) {
        return atof(buf + 4);
    }
    return -1.0f;
}

int read_pir(void)
{
    int pir = 0;
    char buf[4] = {0};
    int fd = open(DEV_PRESENCE, O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, buf, sizeof(buf) - 1) > 0)
        pir = atoi(buf);
    close(fd);
    return pir;
}

int main(void) {
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[daemon] Launching...\n");

    int light_fd = open(DEV_LIGHT_SENSOR, O_RDONLY);
    if (light_fd < 0) {
        perror("[daemon] Failed to open /dev/light_sensor");
        return 1;
    }

    int lighting_fd = open(DEV_LIGHTING, O_WRONLY);
    if (lighting_fd < 0) {
        perror("[daemon] Failed to open /dev/lighting");
        close(light_fd);
        return 1;
    }

    printf("[daemon] Device node opened successfully\n");
    printf("[daemon] Thresholds: IDLE %d s / SLEEP %d s\n",
           IDLE_THRESHOLD, SLEEP_THRESHOLD);

    // (Sending initial ACTIVE state to prevent Pico W timeout)
    send_state(lighting_fd, "ACTIVE");

    int         idle_counter  = 0;
    const char *current_state = "ACTIVE";
    const char *prev_state    = "";

    char lux_buf[64];

    while (running) {
        // 1. blocking read
        memset(lux_buf, 0, sizeof(lux_buf));
        ssize_t n = read(light_fd, lux_buf, sizeof(lux_buf) - 1);

        if (!running) break;

        if (n <= 0) {
            send_state(lighting_fd, current_state);
            sleep(1);
            continue;
        }
        lux_buf[n] = '\0';

        float lux = parse_lux(lux_buf);
        if (lux < 0) {
            send_state(lighting_fd, current_state);
            continue;
        }

        // 2. read PIR
        int pir = read_pir();
        printf("[daemon] lux: %.1f | PIR: %d | idle_counter: %d\n",
               lux, pir, idle_counter);

        // 3. Decision-making
        if (pir == 1) {
            idle_counter  = 0;
            current_state = "ACTIVE";
        } else {
            idle_counter++;
            if      (idle_counter > SLEEP_THRESHOLD) current_state = "SLEEP";
            else if (idle_counter > IDLE_THRESHOLD)  current_state = "IDLE";
            else                                      current_state = "ACTIVE";
        }

        if (strcmp(current_state, prev_state) != 0) {
            printf("[daemon] State changed:%s → %s\n", prev_state, current_state);
            prev_state = current_state;
        }

        send_state(lighting_fd, current_state);
    }

    printf("[daemon] Exiting: Sent ACTIVE, closing device node...\n");
    send_state(lighting_fd, "ACTIVE");
    close(light_fd);
    close(lighting_fd);
    return 0;
}
