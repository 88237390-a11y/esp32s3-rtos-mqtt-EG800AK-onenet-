#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "OLED.h"
#include "Key.h"
#include "Heat.h"
#include "eg800.h"

#define OLED_SCL    4
#define OLED_SDA    5

/* ===== OneNET 三元组（替换为你的真实值） ===== */
#define ONENET_PRODUCT_ID   "yS8CG4x6L5"
#define ONENET_DEVICE_NAME  "text"
#define ONENET_ACCESS_KEY   "bmNzRXdHVU4yVjIySHp0UWpJWVl1dU40S05lUHZnRzE="

static bool g_win_state = true;   /* 按键/云端状态，true=显示温度，false=显示文字 */

/* 下行指令回调（OneNET → 设备），按物模型解析并回复 */
static void on_downlink(const char *topic, const char *payload, int len)
{
    printf("[EG800] Downlink [%d] topic=%s\n", len, topic);
    printf("[EG800] Payload: %s\n", payload);

    /* 提取消息 id（用于 set_reply） */
    char msg_id[32] = {0};
    char *id_start = strstr(payload, "\"id\":\"");
    if (id_start) {
        id_start += 6;
        char *id_end = strchr(id_start, '"');
        if (id_end) {
            int id_len = id_end - id_start;
            if (id_len > (int)sizeof(msg_id) - 1) id_len = sizeof(msg_id) - 1;
            memcpy(msg_id, id_start, id_len);
        }
    }

    /* desired/get_reply 是重连后拉取的缓存值，不应覆盖当前的 win 状态 */
    bool is_desired = strstr(topic, "desired/get_reply") != NULL;

    /* 查找 win 属性值 */
    char *win = strstr(payload, "\"win\"");
    if (win) {
        bool found_true  = strstr(win, "true")  && (strstr(win, "true")  - win < 60);
        bool found_false = strstr(win, "false") && (strstr(win, "false") - win < 60);

        if (is_desired) {
            /* desired/get_reply 只做日志，不更新本地状态，避免被旧缓存值覆盖 */
            printf("[EG800] Desired get reply (ignored for state): %s\n",
                   found_true ? "true" : (found_false ? "false" : "?"));
        } else if (found_true && !found_false) {
            g_win_state = true;
            printf("[EG800] Window -> OPEN\n");
            EG800_SendReply(msg_id, 200, "success");
        } else if (found_false && !found_true) {
            g_win_state = false;
            printf("[EG800] Window -> CLOSE\n");
            EG800_SendReply(msg_id, 200, "success");
        } else {
            printf("[EG800] Win found but value ambiguous (true=%d false=%d)\n",
                   found_true, found_false);
            EG800_SendReply(msg_id, 400, "bad params");
        }
    } else if (!is_desired) {
        EG800_SendReply(msg_id, 404, "unknown property");
    }
}

void app_main(void)
{
    int publish_tick = 0;

    OLED_Init_Pins(OLED_SCL, OLED_SDA);
    Key_Init();
    Heat_Init();

    OLED_ShowString(0, 2, "OLED : ON", OLED_8X16);
    OLED_Update();

    /* EG800 4G 模块初始化 → 联网 → 连接 OneNET */
    esp_err_t ret = EG800_Init(ONENET_PRODUCT_ID, ONENET_DEVICE_NAME,
                               ONENET_ACCESS_KEY, on_downlink);
    if (ret != ESP_OK) {
        printf("[EG800] Init failed: %d\n", ret);
    }

    while (1) {
        /* 断连自动重连 */
        if (!EG800_IsConnected()) {
            printf("[Main] Connection lost, reconnecting...\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            EG800_Deinit();
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_err_t r = EG800_Init(ONENET_PRODUCT_ID, ONENET_DEVICE_NAME,
                                     ONENET_ACCESS_KEY, on_downlink);
            if (r == ESP_OK) {
                printf("[Main] Reconnected!\n");
                publish_tick = 0;
            } else {
                printf("[Main] Reconnect failed: %d, retry in 10s...\n", r);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            continue;
        }

        /* 按键切换 win 状态（同步更新 OLED） */
        if (Key_GetNum()) {
            printf("Key pressed! toggling win state\n");
            g_win_state = !g_win_state;
            if (!g_win_state) {
                OLED_Clear();
                OLED_ShowChinese(1, 1, "我早已麻痹");
                OLED_Update();
            }
        }

        /* OLED 显示：g_win_state 为 true 时显示温度，false 时显示文字 */
        if (g_win_state) {
            OLED_Clear();
            OLED_ShowString(0, 0, "Temp:", OLED_8X16);
            OLED_ShowFloatNum(40, 0, Heat_GetTemperature(), 2, 1, OLED_8X16);
            OLED_ShowString(0, 40, "OLED: ON ", OLED_8X16);
            OLED_Update();
        }

        /* 每 1 秒上报一次（200ms × 5 = 1s），按物模型字段格式 */
        if (EG800_IsConnected() && ++publish_tick >= 5) {
            int heat_val = (int)Heat_GetTemperature();
            char json[256];
            snprintf(json, sizeof(json),
                     "{\"id\":\"%d\",\"version\":\"1.0\","
                     "\"params\":{"
                     "\"heat\":{\"value\":%d},"
                     "\"win\":{\"value\":%s}"
                     "}}",
                     publish_tick, heat_val,
                     g_win_state ? "true" : "false");
            EG800_Publish(json);
            publish_tick = 0;

            /* 上报后拉取期望属性（仅用于日志，不影响本地状态） */
            EG800_QueryDesired();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
