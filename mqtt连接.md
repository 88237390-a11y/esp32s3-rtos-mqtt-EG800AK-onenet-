# ESP32 + EG800 OneNET MQTT 连接 — 正确步骤

**最后更新：** 2026-07-15  
**硬件：** ESP32-S3 + 移远 EG800AK-CN (4G Cat.1 bis)  
**云平台：** 中国移动 OneNET MQTT (183.230.40.96:1883)  
**工程路径：** `E:\desktop\code_0.3`

---

## 一、核心原则

| 原则 | 说明 |
|------|------|
| **协议统一** | Publish Topic 和 Subscribe Topic 必须属于同一个 OneNET 协议（都走物模型，或都走 dp） |
| **AT+QMTPUBEX 两步式** | 模块返回 `>` 不带换行，必须逐字节读取 |
| **URC 任务隔离** | URC 监听任务不能跟 Publish 函数同时读 UART，用 `volatile bool` flag + 短超时协调 |
| **大数组用 static** | 防止 FreeRTOS 任务栈溢出 |
| **`\"` 转义不可靠** | AT+QMTPUB 一步式传 JSON 需要转义双引号，移远模块 AT 解析器不一定支持 |
| **断连自动恢复** | publish 检测 ERROR/RDY → 标记断连 → 主循环自动重启模块重连 |

---

## 二、Topic 与数据格式（物模型协议）

### Publish Topic（上行 — 设备上报属性）
```
$sys/{产品ID}/{设备名称}/thing/property/post
```

### Subscribe Topic（下行 — 接收云端属性设置指令）
```
$sys/{产品ID}/{设备名称}/thing/property/set
```

### 上报 JSON 格式
```json
{
  "id": "25",
  "version": "1.0",
  "params": {
    "heat": {"value": 37},
    "win":  {"value": false}
  }
}
```

### 下行指令 JSON 格式（云端下发属性设置）
```json
{
  "id": "xxx",
  "version": "1.0",
  "params": {
    "win": {"value": true}
  }
}
```

---

## 三、AT 指令完整序列

```
1. AT                          → OK          (确认模块存活)
2. ATE0                        → OK          (关回显)
3. AT+CPIN?                    → +CPIN: READY (SIM 检测)
4. AT+QIDEACT=1                → OK          (反激活残留 PDP)
5. AT+QICSGP=1,1,"CMNET","","",0 → OK       (配置 APN)
6. AT+QIACT=1                  → OK          (激活 PDP)
7. AT+CGPADDR=1                → +CGPADDR:   (确认拿到 IP)
8. AT+CEREG?                   → +CEREG: 0,1 (4G 注册确认)
9. AT+QMTCFG="version",0,4     → OK          (MQTT v3.1.1)
10. AT+QMTCFG="pdpcid",0,1     → OK          (绑定 PDP 上下文)
11. AT+QMTCFG="keepalive",0,120 → OK         (心跳 120s)
12. AT+QMTCFG="recv/mode",0,0,1 → OK         (URC 接收模式)
13. AT+QMTCFG="onenet",0,"产品ID","access_key" → OK (OneNET 自动鉴权)
14. AT+QMTOPEN=0,"183.230.40.96",1883 → +QMTOPEN: 0,0 (TCP 连接)
15. AT+QMTCONN=0,"设备名称"     → +QMTCONN: 0,0,0 (MQTT 登录)
16. AT+QMTSUB=0,1,"$sys/产品ID/设备名称/thing/property/set",0 → +QMTSUB: 0,1,0 (订阅)
17. AT+QMTPUBEX=0,0,0,0,"$sys/产品ID/设备名称/thing/property/post",<长度>
    → 模块返回: >
    → 发送 JSON 数据（长度必须精确匹配）
    → +QMTPUBEX: 0,0,0 (发布成功)
```

---

## 四、关键代码实现

### 4.1 EG800_Publish（核心发布函数）

```c
esp_err_t EG800_Publish(const char *json_str)
{
    if (!g_connected) return ESP_ERR_INVALID_STATE;
    if (!json_str || !json_str[0]) return ESP_ERR_INVALID_ARG;

    // ★ 关键1: 设 flag 阻止 URC 任务读 UART
    g_publish_busy = true;
    xSemaphoreTake(g_uart_mutex, portMAX_DELAY);

    int len = strlen(json_str);
    static char cmd[256];  // ★ 关键2: static 防止栈溢出
    snprintf(cmd, sizeof(cmd),
             "AT+QMTPUBEX=%d,0,0,0,\"%s\",%d",
             MQTT_CLIENT_IDX, g_topic_pub, len);
    eg800_send_cmd(cmd);

    // ★ 关键3: 逐字节读 > (不能用 eg800_read_line，因为 > 不带 \n)
    char ch = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (xTaskGetTickCount() < deadline) {
        if (uart_read_bytes(EG800_UART, (uint8_t *)&ch, 1, pdMS_TO_TICKS(200)) > 0
            && ch == '>') {
            break;
        }
    }
    if (ch != '>') {
        ESP_LOGE(TAG, "Publish: no '>' prompt");
        xSemaphoreGive(g_uart_mutex);
        g_publish_busy = false;
        return ESP_FAIL;
    }

    // ★ 关键4: 发送原始 JSON，不追加 \r\n
    eg800_ensure_awake();
    uart_write_bytes(EG800_UART, json_str, len);

    // 等结果
    esp_err_t ret = eg800_send_cmd_expect(NULL, "+QMTPUBEX:", 5000);

    xSemaphoreGive(g_uart_mutex);
    g_publish_busy = false;
    return ret;
}
```

### 4.2 URC 任务（下行数据监听）

```c
static volatile bool g_publish_busy = false;

static void eg800_urc_task(void *arg)
{
    char line[LINE_BUF_SIZE];
    while (g_connected) {
        // ★ 关键: publish 进行时跳过，不碰 UART
        if (g_publish_busy) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (eg800_read_line(line, sizeof(line), 5000) == ESP_OK) {
            if (line[0] == '\0') continue;

            // 处理 +QMTRECV: 下行数据
            char *urc = strstr(line, "+QMTRECV:");
            if (urc && g_downlink_cb) {
                // 解析 topic 和 payload → 回调
                // ...
            }

            // 处理 +QMTSTAT: 连接状态变化
            urc = strstr(line, "+QMTSTAT:");
            if (urc) {
                if (strstr(line, ",1") || strstr(line, ",2")) {
                    g_connected = false;
                }
            }
        }
    }
    vTaskDelete(NULL);
}
```

### 4.3 main.c 调用示例

```c
#define ONENET_PRODUCT_ID   "yS8CG4x6L5"
#define ONENET_DEVICE_NAME  "text"
#define ONENET_ACCESS_KEY   "bmNzRXdHVU4yVjIySHp0UWpJWVl1dU40S05lUHZnRzE="

void app_main(void)
{
    // ... 传感器初始化 ...

    // 初始化 EG800 → 注网 → MQTT 连接 → 订阅
    EG800_Init(ONENET_PRODUCT_ID, ONENET_DEVICE_NAME,
               ONENET_ACCESS_KEY, on_downlink);

    while (1) {
        if (EG800_IsConnected() && ++publish_tick >= 25) {
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
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

---

## 五、Topic 构造

```c
snprintf(g_topic_pub, sizeof(g_topic_pub),
         "$sys/%s/%s/thing/property/post", g_product_id, g_device_name);
snprintf(g_topic_sub, sizeof(g_topic_sub),
         "$sys/%s/%s/thing/property/set", g_product_id, g_device_name);
```

---

## 六、引脚配置（ESP32-S3 → EG800）

| ESP32-S3 GPIO | EG800 引脚 | 功能 |
|:---:|---|------|
| GPIO 9 | EN | 拉高开机，拉低关机 |
| GPIO 10 | DTR | 拉低强制清醒，拉高允许睡眠 |
| GPIO 17 | RX | ESP32 TX → 模块 RX |
| GPIO 18 | TX | ESP32 RX ← 模块 TX |
| UART 1 | — | 115200 baud |

---

## 七、+QMTSTAT 错误码速查

| 错误码 | 含义 |
|:---:|------|
| 1 | 连接被服务器关闭或重置 |
| 2 | PINGREQ 超时 |
| 3 | CONNECT 超时 |
| 4 | CONNACK 超时 |

---

## 八、关键优化点（2026-07-15 新增）

### 8.1 URC 任务超时优化

URC 任务的 `eg800_read_line` 超时从 5000ms 改为 200ms：
```c
if (eg800_read_line(line, sizeof(line), 200) == ESP_OK) {
```
原因：5000ms 阻塞期间看不到 `g_publish_busy` flag，会导致抢 UART。200ms 足够接收完整一行（115200 下 200 字节 ~17ms），同时快速响应 publish 请求。

### 8.2 订阅后重发 ATE0

`AT+QMTCFG` 系列命令可能导致模块回显被重置。MQTT 订阅成功后补一发：
```c
eg800_send_cmd_expect("ATE0", "OK", 1000);
g_connected = true;
```

### 8.3 按键同步 win 属性

物理按键（GPIO6）松手时 toggle `g_win_state`，初值 `true`（OLED ON = 开）：
```c
if (Key_GetNum()) {
    g_win_state = !g_win_state;  // 同步 win
    oled_on = !oled_on;          // 切换 OLED
}
```

### 8.4 断连检测与自动重连

三重检测机制：

```c
// 1) EG800_Publish — 模块返回 ERROR 或 RDY
if (strstr(buf, "ERROR") || strstr(buf, "RDY")) {
    g_connected = false;
}

// 2) URC 任务 — 收到异常 RDY
if (strstr(line, "RDY")) {
    g_connected = false;
    break;
}

// 3) 主循环 — 检测断连后自动重连
if (!EG800_IsConnected()) {
    vTaskDelay(500);          // 等 URC 任务退出
    EG800_Deinit();           // 断电
    vTaskDelay(3000);         // 等模块稳定
    EG800_Init(...);          // 完整重连
}
```

### 8.5 SIM 检测重试

`AT+CPIN?` 返回 `+CME ERROR: 14`（SIM busy）时重试 3 次，间隔 3 秒：
```c
for (int i = 0; i < 3; i++) {
    if (eg800_send_cmd_expect("AT+CPIN?", "READY", 5000) == ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(3000));
}
```

### 8.6 微信小程序

通过 OneNET HTTP API 拉取设备数据：
- URL: `https://api.heclouds.com/things/{pid}/{device}/shadow`
- Header: `api-key: <Master API Key>`
- 每 5 秒轮询，展示温度和按键状态

项目路径：`E:\desktop\wechat-miniapp\`

---

## 九、排查清单

- [ ] Publish 和 Subscribe topic 使用同一协议（都是 thing 或都是 dp）
- [ ] AT+QMTPUBEX 等 `>` 用逐字节读，不用 `eg800_read_line`
- [ ] URC 任务有 `g_publish_busy` 保护 + timeout ≤ 200ms
- [ ] `EG800_Publish` 中 `cmd[]` 声明为 `static`，防止栈溢出
- [ ] `publish_tick = 0` 放在 `snprintf` 之后（否则 id 永远为 0）
- [ ] 订阅 topic 不用 `#` 通配符（模块可能不支持），用精确 topic
- [ ] SIM 卡 APN 正确（移动=CMNET，联通=UNINET，电信=CTNET）
- [ ] 订阅后重发 ATE0 防止回显被 QMTCFG 重置
- [ ] publish 中检测 ERROR 和 RDY 并标记断连
- [ ] 主循环有 `!IsConnected()` 自动重连逻辑
- [ ] 模块供电端并联 ≥1000μF 电容防掉电复位
- [ ] `g_win_state` 初始值与 OLED 状态对齐
- [ ] 按键松手时同步 toggle `g_win_state`
