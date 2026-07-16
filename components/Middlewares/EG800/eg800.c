#include "eg800.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

/* ============================== 硬件引脚 =============================== */
#define EG800_EN_PIN   GPIO_NUM_9    /* EN: 拉高开机，拉低关机              */
#define EG800_DTR_PIN  GPIO_NUM_10   /* DTR: 拉低强制清醒，拉高允许睡眠     */
#define EG800_TX_PIN   GPIO_NUM_17   /* ESP32 TX → 模块 RX                 */
#define EG800_RX_PIN   GPIO_NUM_18   /* ESP32 RX ← 模块 TX                 */
#define EG800_UART     UART_NUM_1
#define EG800_BAUD     115200

/* ============================== MQTT 常量 =============================== */
#define ONENET_SERVER  "218.201.45.2"
#define ONENET_PORT    1883
#define MQTT_CLIENT_IDX 0

/* ============================== 内部缓冲 ================================ */
#define RX_BUF_SIZE    1024
#define LINE_BUF_SIZE  256
#define TOPIC_BUF_SIZE 128
#define JSON_BUF_SIZE  512

/* ============================== 静态变量 ================================ */
static const char *TAG = "EG800";

static char g_product_id[32];
static char g_device_name[32];
static char g_access_key[64];
static char g_topic_pub[128];
static char g_topic_sub[128];
static char g_topic_set_reply[128];
static char g_topic_desired_get[128];
static char g_topic_desired_get_reply[128];

static bool g_connected = false;
static bool g_desired_ok = false;  /* desired_get_reply 订阅是否成功 */
static volatile bool g_publish_busy = false;
static SemaphoreHandle_t g_uart_mutex = NULL;
static eg800_downlink_cb_t g_downlink_cb = NULL;

/* ============================== 内部声明 ================================ */
static esp_err_t eg800_send_cmd(const char *cmd);
static esp_err_t eg800_send_cmd_expect(const char *cmd, const char *expect,
                                        int timeout_ms);
static esp_err_t eg800_read_line(char *buf, int max_len, int timeout_ms);
static void eg800_flush_rx(void);
static void eg800_ensure_awake(void);
static void eg800_urc_task(void *arg);
static esp_err_t eg800_wait_urc(const char *prefix, int timeout_ms,
                                char *out, int out_len);

/* ========================== 硬件初始化 ================================== */
static void eg800_hw_init(void)
{
    /* EN 引脚：推挽输出，默认低（关机），拉高开机 */
    gpio_config_t io = {
        .pin_bit_mask = BIT64(EG800_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(EG800_EN_PIN, 0);

    /* DTR 引脚：默认拉低 = 禁止模块睡眠 */
    io.pin_bit_mask = BIT64(EG800_DTR_PIN);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    gpio_set_level(EG800_DTR_PIN, 0);

    /* UART */
    uart_config_t uart_cfg = {
        .baud_rate = EG800_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(EG800_UART, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(EG800_UART, &uart_cfg);
    uart_set_pin(EG800_UART, EG800_TX_PIN, EG800_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/* ========================== 模块开关机 ================================== */
static void eg800_power_on(void)
{
    ESP_LOGI(TAG, "Powering on module...");
    eg800_flush_rx();
    gpio_set_level(EG800_DTR_PIN, 0);   /* 禁止睡眠 */
    gpio_set_level(EG800_EN_PIN, 1);    /* 拉高 EN 开机 */

    /* 等模块输出 "RDY"，超时 10 秒 */
    char line[LINE_BUF_SIZE];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (xTaskGetTickCount() < deadline) {
        if (eg800_read_line(line, sizeof(line), 5000) == ESP_OK) {
            ESP_LOGI(TAG, "Module: %s", line);
            if (strstr(line, "RDY")) break;
        }
    }
    /* RDY 之后模块内部还需约 5 秒初始化协议栈 */
    vTaskDelay(pdMS_TO_TICKS(5000));
    eg800_flush_rx();
}

static void eg800_power_off(void)
{
    ESP_LOGI(TAG, "Powering off module...");
    gpio_set_level(EG800_EN_PIN, 0);
}

/* ========================= UART 收发基础 ================================ */
static void eg800_flush_rx(void)
{
    uart_flush_input(EG800_UART);
}

static void eg800_ensure_awake(void)
{
    gpio_set_level(EG800_DTR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* 发 AT 命令，自动补 \r\n */
static esp_err_t eg800_send_cmd(const char *cmd)
{
    eg800_ensure_awake();
    uart_write_bytes(EG800_UART, cmd, strlen(cmd));
    uart_write_bytes(EG800_UART, "\r\n", 2);
    return ESP_OK;
}

/* 读一行，遇到 \n 或超时返回。去掉尾部的 \r */
static esp_err_t eg800_read_line(char *buf, int max_len, int timeout_ms)
{
    int pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (pos < max_len - 1) {
        if (xTaskGetTickCount() > deadline) {
            buf[pos] = '\0';
            return ESP_ERR_TIMEOUT;
        }
        uint8_t ch;
        int n = uart_read_bytes(EG800_UART, &ch, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;

        if (ch == '\n') {
            /* 去掉上一字节的 \r */
            if (pos > 0 && buf[pos - 1] == '\r') pos--;
            buf[pos] = '\0';
            return (pos > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
        }
        buf[pos++] = (char)ch;
    }
    buf[pos] = '\0';
    return ESP_OK;
}

/* 发命令，循环读行直到收到 expect 或超时 */
static esp_err_t eg800_send_cmd_expect(const char *cmd, const char *expect,
                                        int timeout_ms)
{
    char line[LINE_BUF_SIZE];
    char last_line[LINE_BUF_SIZE] = {0};
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    if (cmd) {
        eg800_send_cmd(cmd);
    }

    while (xTaskGetTickCount() < deadline) {
        int remain = (deadline - xTaskGetTickCount()) * portTICK_PERIOD_MS;
        if (remain <= 0) break;

        if (eg800_read_line(line, sizeof(line), remain) != ESP_OK) continue;

        /* 忽略空行 */
        if (line[0] == '\0') continue;
        /* 忽略命令回显 */
        if (cmd && strstr(line, cmd)) continue;

        /* 保存最近一条非回显行，用于超时时打印 */
        strncpy(last_line, line, sizeof(last_line) - 1);

        if (strstr(line, expect)) {
            return ESP_OK;
        }

        /* 检查是否返回 ERROR */
        if (strcmp(line, "ERROR") == 0) {
            ESP_LOGE(TAG, "Got ERROR for cmd: %s", cmd ? cmd : "(null)");
            return ESP_FAIL;
        }
    }
    ESP_LOGE(TAG, "Timeout waiting '%s' for cmd: %s (last: %s)",
             expect, cmd ? cmd : "(null)", last_line[0] ? last_line : "<empty>");
    return ESP_ERR_TIMEOUT;
}

/* 等特定 URC（例如 +QMTOPEN: 0,0），并提取完整行 */
static esp_err_t eg800_wait_urc(const char *prefix, int timeout_ms,
                                char *out, int out_len)
{
    char line[LINE_BUF_SIZE];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        int remain = (deadline - xTaskGetTickCount()) * portTICK_PERIOD_MS;
        if (remain <= 0) break;

        if (eg800_read_line(line, sizeof(line), remain) != ESP_OK) continue;
        if (line[0] == '\0') continue;

        if (strstr(line, prefix)) {
            if (out) {
                strncpy(out, line, out_len);
                out[out_len - 1] = '\0';
            }
            return ESP_OK;
        }
        if (strcmp(line, "ERROR") == 0) return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

/* ========================== 连接流程 ==================================== */

/* 阶段 A：检查模块存活 */
static esp_err_t eg800_check_alive(void)
{
    ESP_LOGI(TAG, "Checking module...");
    eg800_flush_rx();

    /* 多发几次 AT 确保波特率同步（有的模块刚上电需要同步） */
    for (int i = 0; i < 5; i++) {
        if (eg800_send_cmd_expect("AT", "OK", 3000) == ESP_OK) {
            eg800_send_cmd_expect("ATE0", "OK", 2000);
            /* 查询模块是否支持 MQTT 发布命令，打印响应内容 */
            ESP_LOGI(TAG, "Query AT+QMTPUB=? ...");
            esp_err_t r1 = eg800_send_cmd_expect("AT+QMTPUB=?", "OK", 2000);
            ESP_LOGI(TAG, "AT+QMTPUB=? %s", r1 == ESP_OK ? "OK" : "FAIL");
            ESP_LOGI(TAG, "Query AT+QMTPUBEX=? ...");
            esp_err_t r2 = eg800_send_cmd_expect("AT+QMTPUBEX=?", "OK", 2000);
            ESP_LOGI(TAG, "AT+QMTPUBEX=? %s", r2 == ESP_OK ? "OK" : "FAIL");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_FAIL;
}

/* 阶段 B：检查 SIM + 配置 PDP + 激活 */
static esp_err_t eg800_network_attach(void)
{
    ESP_LOGI(TAG, "Checking SIM...");
    esp_err_t sim_ret = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        eg800_flush_rx();
        sim_ret = eg800_send_cmd_expect("AT+CPIN?", "READY", 5000);
        if (sim_ret == ESP_OK) break;
        ESP_LOGW(TAG, "SIM not ready, retry %d/3...", i + 1);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (sim_ret != ESP_OK) {
        ESP_LOGE(TAG, "SIM not ready after 3 retries");
        return ESP_FAIL;
    }

    /* 反激活残留 PDP，忽略结果 */
    eg800_send_cmd_expect("AT+QIDEACT=1", "OK", 3000);
    vTaskDelay(pdMS_TO_TICKS(500));
    eg800_flush_rx();

    ESP_LOGI(TAG, "Configuring PDP...");
    if (eg800_send_cmd_expect("AT+QICSGP=1,1,\"CMNET\",\"\",\"\",0", "OK", 3000) != ESP_OK) {
        ESP_LOGE(TAG, "PDP config failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activating PDP (may take several seconds)...");
    if (eg800_send_cmd_expect("AT+QIACT=1", "OK", 15000) != ESP_OK) {
        ESP_LOGE(TAG, "PDP activate failed — check signal or APN");
        return ESP_FAIL;
    }

    /* 等 PDP 稳定 + 用 CGPADDR 确认拿到 IP */
    vTaskDelay(pdMS_TO_TICKS(3000));
    eg800_flush_rx();
    ESP_LOGI(TAG, "Checking assigned IP...");
    if (eg800_send_cmd_expect("AT+CGPADDR=1", "+CGPADDR:", 3000) != ESP_OK) {
        ESP_LOGE(TAG, "No IP assigned — PDP may not be active");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Checking 4G registration...");
    if (eg800_send_cmd_expect("AT+CEREG?", "+CEREG:", 3000) != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "4G network ready");
    return ESP_OK;
}

/* 阶段 C：MQTT 参数配置 */
static esp_err_t eg800_mqtt_config(void)
{
    ESP_LOGI(TAG, "Configuring MQTT...");

    if (eg800_send_cmd_expect("AT+QMTCFG=\"version\",0,4", "OK", 1000) != ESP_OK)
        return ESP_FAIL;
    if (eg800_send_cmd_expect("AT+QMTCFG=\"pdpcid\",0,1", "OK", 1000) != ESP_OK)
        return ESP_FAIL;
    if (eg800_send_cmd_expect("AT+QMTCFG=\"keepalive\",0,120", "OK", 1000) != ESP_OK)
        return ESP_FAIL;
    if (eg800_send_cmd_expect("AT+QMTCFG=\"recv/mode\",0,0,1", "OK", 1000) != ESP_OK)
        return ESP_FAIL;

    /* OneNET 自动鉴权 */
    char buf[256];
    snprintf(buf, sizeof(buf),
             "AT+QMTCFG=\"onenet\",0,\"%s\",\"%s\"",
             g_product_id, g_access_key);
    if (eg800_send_cmd_expect(buf, "OK", 2000) != ESP_OK) {
        ESP_LOGE(TAG, "OneNET config failed — check product_id / access_key");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT configured");
    return ESP_OK;
}

/* 阶段 D：TCP + MQTT 登录 + 订阅 */
static esp_err_t eg800_mqtt_connect(void)
{
    ESP_LOGI(TAG, "Opening TCP to %s:%d...", ONENET_SERVER, ONENET_PORT);

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "AT+QMTOPEN=%d,\"%s\",%d", MQTT_CLIENT_IDX, ONENET_SERVER, ONENET_PORT);

    /* QMTOPEN 返回 URC +QMTOPEN: 0,0 表示成功，或 0,-1 表示失败 */
    eg800_send_cmd(cmd);
    char line[LINE_BUF_SIZE];
    if (eg800_wait_urc("+QMTOPEN:", 15000, line, sizeof(line)) != ESP_OK) {
        ESP_LOGE(TAG, "TCP connect failed");
        return ESP_FAIL;
    }
    if (strstr(line, "-1")) {
        ESP_LOGE(TAG, "TCP connect rejected: %s", line);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TCP connected");

    /* MQTT 登录 */
    snprintf(cmd, sizeof(cmd), "AT+QMTCONN=%d,\"%s\"",
             MQTT_CLIENT_IDX, g_device_name);
    if (eg800_send_cmd_expect(cmd, "+QMTCONN:", 8000) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT login failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT logged in");

    /* 阶段 E：订阅下行 Topic — thing/property/set（实时推送，部分 broker 支持） */
    snprintf(cmd, sizeof(cmd), "AT+QMTSUB=%d,1,\"%s\",0",
             MQTT_CLIENT_IDX, g_topic_sub);
    if (eg800_send_cmd_expect(cmd, "+QMTSUB:", 5000) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT subscribe set failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT subscribed: %s", g_topic_sub);

    /* 订阅 desired/get_reply（拉取离线/缓存期望值，新平台主要下行通道） */
    snprintf(cmd, sizeof(cmd), "AT+QMTSUB=%d,1,\"%s\",0",
             MQTT_CLIENT_IDX, g_topic_desired_get_reply);
    if (eg800_send_cmd_expect(cmd, "+QMTSUB:", 5000) == ESP_OK) {
        g_desired_ok = true;
        ESP_LOGI(TAG, "MQTT subscribed: %s", g_topic_desired_get_reply);
    } else {
        g_desired_ok = false;
        ESP_LOGW(TAG, "desired_get_reply subscribe failed, will retry later");
    }

    /* 重新关回显（QMTCFG 等操作可能导致回显被重置） */
    eg800_send_cmd_expect("ATE0", "OK", 1000);

    g_connected = true;
    return ESP_OK;
}

/* ========================== URC 接收任务 ================================ */
static void eg800_urc_task(void *arg)
{
    char line[LINE_BUF_SIZE];
    ESP_LOGI(TAG, "URC task started");

    while (g_connected) {
        /* publish 进行中时不读 UART，避免抢数据 */
        if (g_publish_busy) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (eg800_read_line(line, sizeof(line), 200) == ESP_OK) {
            if (line[0] == '\0') continue;

            /* 模块异常重启（RDY 是上电就绪信号，正常运行时不应出现） */
            if (strstr(line, "RDY")) {
                ESP_LOGW(TAG, "Module reset detected (RDY)");
                g_connected = false;
                break;
            }

            /* 收到下行数据：+QMTRECV: <idx>,<msgid>,<topic>,<payload> */
            /* EG800 实际格式可能为 +QMTRECV: 0,msgid,"topic",len,"payload" */
            char *urc = strstr(line, "+QMTRECV:");
            if (urc && g_downlink_cb) {
                char *p1 = strchr(urc, ',');               /* 跳过 idx */
                char *p2 = p1 ? strchr(p1 + 1, ',') : NULL; /* 跳过 msgid */
                char *p3 = p2 ? strchr(p2 + 1, ',') : NULL; /* topic 后的逗号 */
                if (p1 && p2 && p3) {
                    char *topic_start = p2 + 2; /* 跳过 ," */
                    char *topic_end = strchr(topic_start, '"');
                    if (topic_end) {
                        int topic_len = topic_end - topic_start;
                        char topic[TOPIC_BUF_SIZE];
                        int tlen = topic_len < (int)sizeof(topic) - 1
                                   ? topic_len : (int)sizeof(topic) - 1;
                        memcpy(topic, topic_start, tlen);
                        topic[tlen] = '\0';

                        /* 提取 payload：EG800 可能带 payload_len 前缀也可能不带 */
                        /* 带前缀: 49,"{...}" → 跳过 "49,"    无前缀: "{...}" → 直接用 */
                        char *payload_start = p3 + 1;
                        if (*payload_start >= '0' && *payload_start <= '9') {
                            char *p4 = strchr(payload_start, ',');
                            if (p4) payload_start = p4 + 1;
                        }

                        /* 去掉 payload 外层引号 */
                        int plen = strlen(payload_start);
                        if (plen >= 2 && payload_start[0] == '"'
                            && payload_start[plen - 1] == '"') {
                            payload_start++;
                            plen -= 2;
                        }
                        g_downlink_cb(topic, payload_start, plen);
                    }
                }
                continue;
            }

            /* 连接状态变化：+QMTSTAT: <idx>,<err> */
            urc = strstr(line, "+QMTSTAT:");
            if (urc) {
                ESP_LOGW(TAG, "MQTT status: %s", line);
                if (strstr(line, ",1") || strstr(line, ",2")) {
                    g_connected = false;
                    ESP_LOGE(TAG, "MQTT disconnected");
                }
                continue;
            }
        }
    }
    vTaskDelete(NULL);
}

/* ========================== 公开 API ==================================== */
esp_err_t EG800_Init(const char *product_id, const char *device_name,
                     const char *access_key, eg800_downlink_cb_t cb)
{
    if (!product_id || !device_name || !access_key) return ESP_ERR_INVALID_ARG;

    /* 保存三元组 */
    strncpy(g_product_id, product_id, sizeof(g_product_id) - 1);
    strncpy(g_device_name, device_name, sizeof(g_device_name) - 1);
    strncpy(g_access_key, access_key, sizeof(g_access_key) - 1);

    snprintf(g_topic_pub, sizeof(g_topic_pub),
             "$sys/%s/%s/thing/property/post", g_product_id, g_device_name);
    snprintf(g_topic_sub, sizeof(g_topic_sub),
             "$sys/%s/%s/thing/property/set", g_product_id, g_device_name);
    snprintf(g_topic_set_reply, sizeof(g_topic_set_reply),
             "$sys/%s/%s/thing/property/set_reply", g_product_id, g_device_name);
    snprintf(g_topic_desired_get, sizeof(g_topic_desired_get),
             "$sys/%s/%s/thing/property/desired/get", g_product_id, g_device_name);
    snprintf(g_topic_desired_get_reply, sizeof(g_topic_desired_get_reply),
             "$sys/%s/%s/thing/property/desired/get_reply", g_product_id, g_device_name);

    g_downlink_cb = cb;

    /* 互斥锁 */
    if (!g_uart_mutex) {
        g_uart_mutex = xSemaphoreCreateMutex();
    }

    /* 硬件 + 上电 */
    eg800_hw_init();
    eg800_power_on();

    /* 逐阶段执行 */
    if (eg800_check_alive() != ESP_OK) {
        ESP_LOGE(TAG, "Module not responding");
        return ESP_FAIL;
    }
    if (eg800_network_attach() != ESP_OK) {
        ESP_LOGE(TAG, "Network attach failed");
        return ESP_FAIL;
    }
    if (eg800_mqtt_config() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT config failed");
        return ESP_FAIL;
    }
    if (eg800_mqtt_connect() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT connect failed");
        return ESP_FAIL;
    }

    /* 启动 URC 监听任务 */
    xTaskCreate(eg800_urc_task, "eg800_urc", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "EG800 Init complete — connected to OneNET");
    return ESP_OK;
}

esp_err_t EG800_Publish(const char *json_str)
{
    return EG800_PublishTo(json_str, g_topic_pub);
}

esp_err_t EG800_PublishTo(const char *json_str, const char *topic)
{
    if (!g_connected) return ESP_ERR_INVALID_STATE;
    if (!json_str || !json_str[0] || !topic) return ESP_ERR_INVALID_ARG;

    g_publish_busy = true;
    xSemaphoreTake(g_uart_mutex, portMAX_DELAY);

    int len = strlen(json_str);
    static char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "AT+QMTPUBEX=%d,0,0,0,\"%s\",%d",
             MQTT_CLIENT_IDX, topic, len);
    ESP_LOGI(TAG, "TX: %s", cmd);
    eg800_send_cmd(cmd);

    /* 读取所有返回字节 */
    char buf[256] = {0};
    int pos = 0;
    char ch = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(EG800_UART, (uint8_t *)&ch, 1, pdMS_TO_TICKS(200));
        if (n > 0) {
            if (pos < (int)sizeof(buf) - 1) buf[pos++] = ch;
            if (ch == '>') break;
        }
    }
    ESP_LOGI(TAG, "RX[%d]: %.*s", pos, pos, buf);

    if (ch != '>') {
        /* 模块返回 ERROR 或 RDY（重启）说明 MQTT 连接已断开 */
        if (strstr(buf, "ERROR")) {
            ESP_LOGE(TAG, "Publish: MQTT disconnected (ERROR)");
            g_connected = false;
        } else if (strstr(buf, "RDY")) {
            ESP_LOGE(TAG, "Publish: module reset (RDY)");
            g_connected = false;
        } else {
            ESP_LOGE(TAG, "Publish: no '>' prompt");
        }
        xSemaphoreGive(g_uart_mutex);
        g_publish_busy = false;
        return ESP_FAIL;
    }

    /* 发送 JSON 数据（不追加 \r\n） */
    eg800_ensure_awake();
    uart_write_bytes(EG800_UART, json_str, len);

    /* 等结果 */
    esp_err_t ret = eg800_send_cmd_expect(NULL, "+QMTPUBEX:", 5000);

    xSemaphoreGive(g_uart_mutex);
    g_publish_busy = false;
    return ret;
}

bool EG800_IsConnected(void)
{
    return g_connected;
}

esp_err_t EG800_GetSignalQuality(uint8_t *rssi)
{
    if (!rssi) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(g_uart_mutex, portMAX_DELAY);

    char line[LINE_BUF_SIZE];
    eg800_send_cmd("AT+CSQ");
    esp_err_t ret = eg800_wait_urc("+CSQ:", 3000, line, sizeof(line));

    xSemaphoreGive(g_uart_mutex);

    if (ret == ESP_OK) {
        /* +CSQ: <rssi>,<ber>  → 提取第一个数字 */
        char *p = strchr(line, ':');
        if (p) {
            *rssi = (uint8_t)atoi(p + 1);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t EG800_SendReply(const char *id, int code, const char *msg)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}",
             id ? id : "", code, msg ? msg : "");
    ESP_LOGI(TAG, "Sending set_reply: %s", json);
    return EG800_PublishTo(json, g_topic_set_reply);
}

esp_err_t EG800_QueryDesired(void)
{
    if (!g_connected) return ESP_ERR_INVALID_STATE;
    char json[64];
    snprintf(json, sizeof(json), "{\"id\":\"%lu\",\"version\":\"1.0\"}",
             (unsigned long)xTaskGetTickCount());
    return EG800_PublishTo(json, g_topic_desired_get);
}

esp_err_t EG800_Deinit(void)
{
    g_connected = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    xSemaphoreTake(g_uart_mutex, portMAX_DELAY);

    eg800_send_cmd_expect("AT+QMTDISC=0", "OK", 3000);
    eg800_send_cmd_expect("AT+QMTCLOSE=0", "OK", 3000);

    xSemaphoreGive(g_uart_mutex);

    eg800_power_off();
    ESP_LOGI(TAG, "Deinit done");
    return ESP_OK;
}
