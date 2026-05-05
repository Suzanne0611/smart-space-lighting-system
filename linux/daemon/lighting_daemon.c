/*
 * lighting_daemon.c
 * 狀態決策 daemon + MQTT 發布
 *
 * 功能：
 *   1. read(/dev/light_sensor) → 取得 lux 值（blocking）
 *   2. read(/dev/presence)     → 取得 PIR 狀態
 *   3. 根據 idle_counter 決定 STATE
 *   4. write(/dev/lighting)    → 送 STATE 給 Pico W
 *   5. MQTT publish            → 發布 LUX / STATE / presence
 *   6. MQTT subscribe          → 接收 server.js 的手動覆蓋通知
 *
 * 編譯：gcc -o lighting_daemon lighting_daemon.c -lmosquitto
 * 執行：sudo ./lighting_daemon/lighting_daemon &
 * 結束：sudo kill $(pgrep lighting_daemon)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <mosquitto.h>

#define DEV_LIGHT_SENSOR "/dev/light_sensor"
#define DEV_LIGHTING "/dev/lighting"
#define DEV_PRESENCE "/dev/presence"

#define IDLE_THRESHOLD 5
#define SLEEP_THRESHOLD 20

// MQTT setting
#define MQTT_HOST "localhost"
#define MQTT_PORT 1883
#define MQTT_TOPIC_LUX "smartspace/lighting/lux"
#define MQTT_TOPIC_STATE "smartspace/lighting/state"
#define MQTT_TOPIC_PIR "smartspace/lighting/presence"
#define MQTT_TOPIC_IDLE_COUNTER "smartspace/lighting/idle_counter"
#define MQTT_TOPIC_OVERRIDE "smartspace/lighting/override" 

static volatile int running = 1;
static volatile int manual_override = 0; 
static struct mosquitto *mosq = NULL;

void handle_signal(int sig)
{
    (void)sig;
    running = 0;
    printf("\n[daemon] Termination signal received. Exiting...\n");
}

// MQTT 收到訊息的 callback
void on_message(struct mosquitto *m, void *obj,
                const struct mosquitto_message *msg)
{
    (void)m;
    (void)obj;
    if (strcmp(msg->topic, MQTT_TOPIC_OVERRIDE) == 0)
    {
        int val = atoi((char *)msg->payload);
        manual_override = (val != 0) ? 1 : 0;
        printf("[daemon] 收到 override 通知：%s → %s\n",
               MQTT_TOPIC_OVERRIDE,
               manual_override ? "手動模式（暫停自動決策）" : "自動模式（恢復自動決策）");
    }
}

int send_state(int lighting_fd, const char *state)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "STATE:%s\n", state);
    int ret = write(lighting_fd, buf, len);
    if (ret < 0)
    {
        perror("[daemon] write /dev/lighting failed");
        return -1;
    }
    printf("[daemon] → Pico W: STATE:%s\n", state);
    return 0;
}

float parse_lux(const char *buf)
{
    if (strncmp(buf, "ERROR:TIMEOUT", 13) == 0)
    {
        printf("[daemon] /dev/light_sensor Timed out. Waiting to recover...\n");
        return -1.0f;
    }
    if (strncmp(buf, "LUX:", 4) == 0)
    {
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

void mqtt_publish(const char *topic, const char *payload)
{
    if (!mosq)
        return;
    int ret = mosquitto_publish(mosq, NULL, topic,
                                strlen(payload), payload, 0, false);
    if (ret != MOSQ_ERR_SUCCESS)
    {
        printf("[daemon] MQTT publish failed:%s\n", mosquitto_strerror(ret));
    }
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[daemon] Launching...\n");

    // init squitto_lib_init();
    mosq = mosquitto_new("lighting_daemon", true, NULL);
    if (!mosq)
    {
        fprintf(stderr, "[daemon] mosquitto_new failed\n");
        return 1;
    }

    // 設定訊息 callback（用於接收 override）
    mosquitto_message_callback_set(mosq, on_message);

    int rc = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "[daemon] MQTT connection failed:%s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    // subscribe OVERRIDE topic
    rc = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_OVERRIDE, 0);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "[daemon] MQTT sub error:%s\n", mosquitto_strerror(rc));
    }
    else
    {
        printf("[daemon] 已訂閱：%s\n", MQTT_TOPIC_OVERRIDE);
    }

    // MQTT 背景執行緒（同時處理發布與訂閱）
    mosquitto_loop_start(mosq);
    printf("[daemon] MQTT connection successfully:%s:%d\n", MQTT_HOST, MQTT_PORT);

    // open /dev/
    int light_fd = open(DEV_LIGHT_SENSOR, O_RDONLY);
    if (light_fd < 0)
    {
        perror("[daemon] Failed to open/dev/light_sensor");
        goto cleanup;
    }

    int lighting_fd = open(DEV_LIGHTING, O_WRONLY);
    if (lighting_fd < 0)
    {
        perror("[daemon] Failed to open /dev/lighting");
        close(light_fd);
        goto cleanup;
    }

    printf("[daemon] Device node opened successfully\n");
    printf("[daemon] Thresholds: IDLE %d s / SLEEP %d s\n",
           IDLE_THRESHOLD, SLEEP_THRESHOLD);

    // (Sending initial ACTIVE state to prevent Pico W timeout)
    send_state(lighting_fd, "ACTIVE");

    int idle_counter = 0;
    const char *current_state = "ACTIVE";
    const char *prev_state = "";
    char lux_buf[64];
    char mqtt_payload[64];

    while (running)
    {
        // 1. blocking read LUX
        memset(lux_buf, 0, sizeof(lux_buf));
        ssize_t n = read(light_fd, lux_buf, sizeof(lux_buf) - 1);

        if (!running)
            break;

        if (n <= 0)
        {
            if (!manual_override)
                send_state(lighting_fd, current_state);
            sleep(1);
            continue;
        }
        lux_buf[n] = '\0';

        float lux = parse_lux(lux_buf);
        if (lux < 0)
        {
            if (!manual_override)
                send_state(lighting_fd, current_state);
            continue;
        }

        printf("[daemon] lux: %.1f\n", lux);

        // 2. read PIR
        int pir = read_pir();
        printf("[daemon] PIR: %d | idle_counter: %d | override: %d\n",
               pir, idle_counter, manual_override);

        // 3. Manual Mode: Publish sensor data only; no decision-making or STATE updates.
        if (manual_override)
        {
            snprintf(mqtt_payload, sizeof(mqtt_payload), "%.1f", lux);
            mqtt_publish(MQTT_TOPIC_LUX, mqtt_payload);
            snprintf(mqtt_payload, sizeof(mqtt_payload), "%d", pir);
            mqtt_publish(MQTT_TOPIC_PIR, mqtt_payload);
            snprintf(mqtt_payload, sizeof(mqtt_payload), "%d", idle_counter);
            mqtt_publish(MQTT_TOPIC_IDLE_COUNTER, mqtt_payload);
            continue;
        }

        // 4.Auto: Decision-making
        if (pir == 1)
        {
            idle_counter = 0;
            current_state = "ACTIVE";
        }
        else
        {
            idle_counter++;
            if (idle_counter > SLEEP_THRESHOLD)
                current_state = "SLEEP";
            else if (idle_counter > IDLE_THRESHOLD)
                current_state = "IDLE";
            else
                current_state = "ACTIVE";
        }

        if (strcmp(current_state, prev_state) != 0)
        {
            printf("[daemon] State changed:%s → %s\n", prev_state, current_state);
            prev_state = current_state;
        }

        // 5. send STATE to Pico W
        send_state(lighting_fd, current_state);

        // 6. MQTT published
        snprintf(mqtt_payload, sizeof(mqtt_payload), "%.1f", lux);
        mqtt_publish(MQTT_TOPIC_LUX, mqtt_payload);

        mqtt_publish(MQTT_TOPIC_STATE, current_state);

        snprintf(mqtt_payload, sizeof(mqtt_payload), "%d", pir);
        mqtt_publish(MQTT_TOPIC_PIR, mqtt_payload);

        snprintf(mqtt_payload, sizeof(mqtt_payload), "%d", idle_counter);
        mqtt_publish(MQTT_TOPIC_IDLE_COUNTER, mqtt_payload);
    }

    printf("[daemon] Over\n");
    send_state(lighting_fd, "ACTIVE");
    close(light_fd);
    close(lighting_fd);

cleanup:
    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
