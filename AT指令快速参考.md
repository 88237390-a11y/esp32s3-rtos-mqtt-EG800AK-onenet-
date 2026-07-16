# EG800AK-CN AT 指令快速参考

> 适用：移远 EG800AK-CN | LTE Standard (A) 系列 | OneNET 物联网平台
> 基于：AT手册 V1.3 + MQTT V1.6 + GNSS V1.1 + 低功耗 V1.2 + LTE Standard(A) MQTT 应用指导
> 日期：2026-07-10

---

## 引脚对应表（EG800AK-CN ↔ ESP32-S3）

| EG800AK | ESP32-S3 | 功能 |
|---------|:---:|------|
| EN | GPIO 10 | 拉高开机，拉低关机 |
| MAIN_DTR | GPIO 9 | 拉低=强制清醒，拉高=允许睡眠 |
| TX | GPIO 6 | ESP32 RX ← 模块 TX |
| RX | GPIO 5 | ESP32 TX → 模块 RX |
| VIN | 独立5V | 模块供电 |
| GND | GND | 公共地 |

- UART 端口：ESP32 UART1, RX=GPIO6, TX=GPIO5, 波特率 115200
- 与旧 EG800N-CN 的区别：UART RX 从 GPIO4 改为 GPIO6

---

## 一、基础指令

| 指令 | 说明 | 响应 |
|------|------|------|
| `AT` | 确认串口通信 | OK |
| `ATI` | 查询模块型号和固件版本 | Quectel / EG800AKCN / Revision:... |
| `AT+GSN` | 查询 IMEI 号 | `<IMEI>` |
| `AT+CFUN=<fun>` | 模块功能模式 | 0=最小, 1=全功能(默认), 4=飞行模式 |
| `AT+QPOWD` | 安全关机 | POWERED DOWN（等3秒后断电） |
| `ATE0` | 关闭回显 | OK |

---

## 二、4G 网络指令

### SIM 卡
| 指令 | 说明 |
|------|------|
| `AT+CPIN?` | SIM 卡状态：`+CPIN: READY` = 就绪 |

### 网络注册（★ 4G 用 AT+CEREG）
| 指令 | 说明 |
|------|------|
| `AT+CEREG?` | **EPS 网络注册（4G 用这个）**，stat=1 或 5 才能上网 |
| `AT+CREG?` | CS 域注册（2G/3G 用，不要用这个查 4G） |
| `AT+CSQ` | 信号强度，rssi 0-31，≥10 可用，99=无信号 |
| `AT+COPS?` | 查询运营商 |
| `AT+QNWINFO` | 详细网络信息（频段等） |

### PDP 激活（★ 移远推荐）
| 指令 | 说明 |
|------|------|
| `AT+QICSGP=1,1,"CMNET","","",0` | 配置 PDP（移动） |
| `AT+QICSGP=1,1,"UNINET","","",0` | 配置 PDP（联通） |
| `AT+QICSGP=1,1,"CTNET","","",0` | 配置 PDP（电信） |
| `AT+QIACT=1` | 激活 PDP |
| `AT+CGPADDR=1` | 查询分配的 IP |

---

## 三、MQTT 指令

### 3.1 QMTCFG 参数配置

全部配置项（用 `AT+QMTCFG=?` 查看完整列表）：

| 配置项 | 说明 | 示例 |
|--------|------|------|
| `"version"` | MQTT 协议版本 (3=v3.1, **4=v3.1.1**) | `AT+QMTCFG="version",0,4` |
| `"pdpcid"` | 绑定的 PDP 上下文 ID | `AT+QMTCFG="pdpcid",0,1` |
| `"keepalive"` | 保活时间 0-3600 秒（默认 120） | `AT+QMTCFG="keepalive",0,120` |
| `"session"` | 会话类型 (0=保留, 1=Clean) | `AT+QMTCFG="session",0,1` |
| `"timeout"` | 超时(秒)+重试次数+超时通知 | `AT+QMTCFG="timeout",0,10,3,1` |
| `"recv/mode"` | 接收模式 (0=URC 上报, 1=缓存) | `AT+QMTCFG="recv/mode",0,0,1` |
| `"send/mode"` | 发送格式 (0=字符串, 1=HEX) | — |
| `"ssl"` | SSL 模式 (0=TCP, 1=SSL) | `AT+QMTCFG="ssl",0,1` |
| `"will"` | 遗嘱消息配置 | — |
| `"onenet"` | **★ OneNET 专用**：产品ID + Access Key | `AT+QMTCFG="onenet",0,"PID","Key"` |

> 注意：配置掉电丢失，每次上电需重新配置。

### 3.2 连接指令（★ client_idx 是 0-5，不是 PDP 编号）

| 步骤 | 指令 | 说明 |
|------|------|------|
| 打开连接 | `AT+QMTOPEN=<client_idx>,<host>,<port>` | 第一个参数是 **MQTT 客户端编号 0-5** |
| MQTT 登录 | `AT+QMTCONN=<client_idx>,<clientid>[,<user>,<pwd>]` | 用 `"onenet"` 配置后只需填 clientid |
| 订阅 | `AT+QMTSUB=<client_idx>,<msgid>,<topic>,<qos>` | msgid 1-65535，每次递增 |
| 发布 | `AT+QMTPUBEX=<client_idx>,<msgid>,<qos>,<retain>,<topic>,<length>` → `>{数据}` | **定长发布，等 `>` 再发数据** |
| 退订 | `AT+QMTUNS=<client_idx>,<msgid>,<topic>` | |
| 断开 | `AT+QMTDISC=<client_idx>` | 断开 MQTT 连接 |
| 关闭 | `AT+QMTCLOSE=<client_idx>` | 关闭 TCP 连接 |

### 3.3 MQTT URC（模块主动上报）

| URC | 含义 |
|-----|------|
| `+QMTRECV: <idx>,<msgid>,<topic>,<payload>` | 收到下行数据（recv/mode=0） |
| `+QMTRECV: <idx>,<recv_id>` | 数据缓存通知（recv/mode=1） |
| `+QMTSTAT: <idx>,<err>` | MQTT 链路状态变化 |
| `+QMTPING: <idx>,<result>` | 保活心跳超时 |

**`+QMTSTAT` err_code 含义：**
| 码 | 含义 | 码 | 含义 |
|:--:|------|:--:|------|
| 1 | 被服务器断开 | 5 | 正常断开 |
| 2 | PINGREQ 超时 | 6 | 发送失败断开 |
| 3 | CONNECT 超时 | 7 | 链路不可用 |
| 4 | CONNACK 超时 | 8 | 客户端主动断开 |

---

## 四、OneNET 平台接入（★ EG800AK-CN 当前平台）

### 4.1 服务器地址

| 服务器 | 端口 | 协议 |
|--------|:---:|------|
| `183.230.40.96` | `1883` | 新版 MQTT（推荐） |
| `183.230.40.39` | `6002` | 老版 MQTT（非加密） |
| `183.230.40.39` | `1883` | 老版 MQTT（加密） |

### 4.2 MQTT 认证参数

| 参数 | 值 |
|------|-----|
| MQTT 版本 | v3.1.1（`version=4`，**OneNET 强制要求**） |
| Client ID | **设备名称**（平台创建设备时的名称） |
| Username | **产品 ID** |
| Password | **Token**（动态 HMAC 签名，或由 `QMTCFG="onenet"` 自动生成） |

### 4.3 Token 签名算法

```
原文 = et + '\n' + method + '\n' + res + '\n' + version
     = "2524579200\nsha1\nproducts/<PID>/devices/<DEV>\n2018-10-31"

sign  = Base64(HMAC-SHA1(原文, access_key))

最终 Password = "version=2018-10-31&res=products/<PID>/devices/<DEV>&et=2524579200&method=sha1&sign=<sign>"
```

### 4.4 Topic 格式

| 方向 | Topic | 说明 |
|------|-------|------|
| 上行（发布） | `$sys/<PID>/<DEV>/dp/post/json` | 设备 → OneNET |
| 上行（应答） | `$sys/<PID>/<DEV>/dp/post/json/+` | 发布后平台应答 |
| 下行（订阅） | `$sys/<PID>/<DEV>/cmd/#` | OneNET → 设备 |

### 4.5 完整 AT 连接流程

```
阶段 A — 初始化：
  AT → ATE0 → AT+CFUN=1

阶段 B — 4G 联网：
  AT+CPIN? → AT+QICSGP=1,1,"CMNET","","",0 → AT+QIACT=1
  → AT+CEREG?（等 stat=1 或 5）→ AT+CSQ

阶段 C — MQTT 配置（顺序不能错）：
  AT+QMTCFG="version",0,4              // OneNET 必须 v3.1.1
  AT+QMTCFG="pdpcid",0,1               // 绑定激活的 PDP
  AT+QMTCFG="keepalive",0,120          // 保活
  AT+QMTCFG="recv/mode",0,0,1          // 接收模式
  AT+QMTCFG="onenet",0,"<产品ID>","<access_key>"  // ★ OneNET 自动鉴权

阶段 D — 建立 MQTT 连接：
  AT+QMTOPEN=0,"183.230.40.96",1883     // 或 183.230.40.39:1883
  AT+QMTCONN=0,"<设备名称>"             // 用了 onenet 配置，只需设备名
  AT+QMTSUB=0,1,"$sys/<PID>/<DEV>/cmd/#",1  // 订阅下行

阶段 E — 定时上报：
  AT+QMTPUBEX=0,0,0,0,"$sys/<PID>/<DEV>/dp/post/json",<length>
  >{JSON 数据}

阶段 F — 断开：
  AT+QMTDISC=0 → AT+QMTCLOSE=0 → AT+QPOWD
```

### 4.6 不使用 `QMTCFG="onenet"` 的手动鉴权方式

如果 `AT+QMTCFG=?` 查出来不支持 `"onenet"`，则手动算好 Token，直接走标准 MQTT 连接：

```
AT+QMTCONN=0,"<设备名>","<产品ID>","version=2018-10-31&res=products/<PID>/devices/<DEV>&et=2524579200&method=sha1&sign=<Base64签名>"
```

### 4.7 EG800AK 已知固件问题

- **现象：** `QMTOPEN` 成功但立即收到 `+QMTSTAT: 0,1`，随后 `QMTCONN` 直接 ERROR
- **原因：** 旧固件 `EG800KLALCR07A02M04_01.001.01.001` 存在 MQTT bug
- **解决：** 升级到最新固件；重连前先执行 `AT+QMTDISC=0` + `AT+QMTCLOSE=0` 清理残留

---

## 五、OneNET 数据格式

### 上行（设备 → OneNET）— 基于 OneJSON 协议

```json
{
  "id": "msg_001",
  "version": "1.0",
  "params": {
    "flame_raw": {"value": 1234},
    "flame_mv": {"value": 1500},
    "fire_detected": {"value": false},
    "encoder_count": {"value": 42},
    "led_state": {"value": true}
  }
}
```

或简化格式：
```json
{
  "id": 123,
  "dp": {
    "flame_raw": [{"v": 1234}],
    "fire_detected": [{"v": 0}]
  }
}
```

### 下行（OneNET → 设备）

```json
{
  "msgid": "xxx",
  "params": {
    "switch1": 1
  }
}
```

---

## 六、GNSS/GPS 定位指令

> EG800AK-CN 支持 GPS + BDS + GLONASS + Galileo

| 步骤 | 指令 | 说明 |
|------|------|------|
| 配置 | `AT+QGPSCFG="nmeasrc",1` | 允许 AT 口获取 NMEA |
| 配置 | `AT+QGPSCFG="gnssconfig",31` | 全星座启用 (GPS+BDS+GLONASS+Galileo) |
| 配置 | `AT+QGPSCFG="autogps",0` | 不自动启动 |
| 启动 | `AT+QGPS=1` | 启用 GNSS |
| **查询位置** | **`AT+QGPSLOC?`** | 返回：UTC,lat,lng,alt,speed,course,mode,sat_num,hdop,vdop,pdop |
| 获取 NMEA | `AT+QGPSGNMEA="GGA"` | 获取指定 NMEA 语句 |
| 关闭 | `AT+QGPS=0` | 省电 |

`AT+QGPSLOC?` 响应格式：
```
+QGPSLOC: 103225.0,39.904200,116.407396,50.5,0.0,0.0,3,15,1.0,1.5,1.8
           UTC      纬度      经度       高度 速度 方向 模式 卫星数 hdop vdop pdop
```
mode: 0=未定位, 1=无效, 2=2D定位, 3=3D定位

---

## 七、低功耗/防掉线控制

| 指令 | 说明 |
|------|------|
| `AT+QSCLK=0` | **彻底禁止睡眠**（调试用，简单稳妥） |
| `AT+QSCLK=1` | 启用 DTR 引脚控制睡眠（省电方案） |

### DTR 引脚控制逻辑
- ESP32 拉低 MAIN_DTR（GPIO 9）→ 模块强制唤醒（需要联网时）
- ESP32 拉高 MAIN_DTR（GPIO 9）→ 模块允许睡眠（闲置时省电）
- 唤醒延迟：轻睡眠 <10ms，深睡眠 50-100ms
- 建议：每次发 AT 指令前，拉低 DTR → 延迟 50ms → 再发指令

### ESP32 代码框架
```c
#define DTR_PIN GPIO_NUM_9

void modem_force_awake(void) {
    gpio_set_level(DTR_PIN, 0);   // 拉低 = 禁止睡眠
}

void modem_allow_sleep(void) {
    gpio_set_level(DTR_PIN, 1);   // 拉高 = 允许睡眠
}

void send_at_command(const char *cmd) {
    modem_force_awake();
    vTaskDelay(pdMS_TO_TICKS(50));  // 等 50ms 稳定
    uart_send(cmd);
    // ... 等待响应 ...
    modem_allow_sleep();
}
```

---

## 八、快速排查

| 现象 | 可能原因 | 检查方法 |
|------|----------|----------|
| QMTOPEN 返回 ERROR | 没激活 PDP / 参数错 | `AT+QIACT?` 确认已激活 |
| QMTOPEN 成功但 +QMTSTAT: 0,1 | 固件 bug / 服务器拒绝 | 先 QMTCLOSE 清理，重试 |
| QMTCONN 返回 ERROR | 认证失败 / MQTT 版本不对 | 确认 version=4，确认 onenet 配置 |
| +QMTSTAT: 0,2 | PINGREQ 超时 | 信号差 or keepalive 太长 |
| 收不到下行 | 没订阅或 Topic 错 | `AT+QMTSUB?` 确认已订阅 |
