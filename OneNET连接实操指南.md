# EG800AK-CN 连接 OneNET 实操指南

> 实战验证通过 | 日期：2026-07-10
> 模块：EG800AK-CN | 服务器：183.230.40.96:1883

---

## 实操流程（已验证，按顺序执行）

### 阶段一：基础初始化

```
AT                                  // 确认串口通信，期待 OK
ATE0                                // 关回显，避免干扰后续解析
AT+CPIN?                            // 查 SIM 卡状态，期待 +CPIN: READY
```

### 阶段二：4G 联网

```
AT+QICSGP=1,1,"CMNET","","",0       // 配置 PDP：cid=1，IP 类型，APN=CMNET（移动）
                                    // 联通改 UNINET，电信改 CTNET，物联网卡问卡商
AT+QIACT=1                          // 激活 PDP，期待 OK
AT+CEREG?                           // 查 4G 注册，期待 +CEREG: 0,1（或 0,5=漫游）
AT+CSQ                              // 查信号强度，rssi ≥10 可用
```

### 阶段三：MQTT 参数配置（掉电会丢，每次上电都要配）

```
AT+QMTCFG="version",0,4             // MQTT 协议 v3.1.1，OneNET 强制要求
AT+QMTCFG="pdpcid",0,1              // MQTT 客户端 0 绑定到激活的 PDP cid=1
AT+QMTCFG="keepalive",0,120         // 心跳保活 120 秒，防止空闲被踢下线
AT+QMTCFG="recv/mode",0,0,1         // 下行数据走 URC 自动上报模式
AT+QMTCFG="onenet",0,"产品ID","access_key"  // OneNET 自动鉴权（替换成你自己的）
```

### 阶段四：建立连接

```
AT+QMTOPEN=0,"183.230.40.96",1883   // TCP 连接 OneNET 服务器，期待 +QMTOPEN: 0,0
AT+QMTCONN=0,"设备名称"             // MQTT 登录，期待 +QMTCONN: 0,0,0
AT+QMTSUB=0,1,"$sys/产品ID/设备名称/cmd/#",1  // 订阅下行指令，期待 +QMTSUB: 0,1,0,1
```

### 阶段五：上报数据

```
AT+QMTPUBEX=0,0,0,0,"$sys/产品ID/设备名称/dp/post/json",<长度>   // 定长发布指令
>{JSON数据}                                                       // 等出现 > 再粘贴数据
```

### 阶段六：断开（需要时）

```
AT+QMTDISC=0                        // 断开 MQTT 连接
AT+QMTCLOSE=0                       // 关闭 TCP 连接
AT+QPOWD                            // 模块安全关机（等 3 秒）
```

---

## 每步含义速查表

| 指令 | 做什么 | 成功标志 | 失败如何处理 |
|------|--------|----------|-------------|
| `AT` | 确认模块活着 | OK | 检查串口接线/波特率 |
| `ATE0` | 关回显 | OK | — |
| `AT+CPIN?` | SIM 卡就绪 | `+CPIN: READY` | 重插 SIM 卡 |
| `AT+QICSGP=...` | 写 APN 配置 | OK | 确认运营商 APN |
| `AT+QIACT=1` | 激活 PDP 拿 IP | OK | 等几秒重试 |
| `AT+CEREG?` | 4G 注册成功 | `+CEREG: 0,1` | 移到窗边/确认 SIM 有流量 |
| `AT+CSQ` | 信号强度 | rssi≥10 | 信号差=换位置 |
| `AT+QMTCFG="version"` | 设 MQTT v3.1.1 | OK | 换固件 |
| `AT+QMTCFG="pdpcid"` | 绑定网络通道 | OK | — |
| `AT+QMTCFG="keepalive"` | 设心跳间隔 | OK | — |
| `AT+QMTCFG="recv/mode"` | 设接收模式 | OK | — |
| `AT+QMTCFG="onenet"` | OneNET 自动鉴权 | OK | 检查产品ID/key是否正确 |
| `AT+QMTOPEN=0,...` | TCP 连服务器 | `+QMTOPEN: 0,0` | 换 IP/端口 |
| `AT+QMTCONN=0,...` | MQTT 登录 | `+QMTCONN: 0,0,0` | 检查设备名/鉴权 |
| `AT+QMTSUB=0,1,...` | 订阅下行 Topic | `+QMTSUB: 0,1,0,1` | 检查 Topic 格式 |
| `AT+QMTPUBEX=0,0,...` | 发布数据 | `+QMTPUBEX: 0,0,0` | 检查 Topic/数据长度 |
| `AT+QMTDISC=0` | 断开 MQTT | OK | — |
| `AT+QMTCLOSE=0` | 关闭 TCP | OK | 没连接时返回 ERROR，正常 |

---

## 已验证 vs 未通过的服务器

| 服务器 | 端口 | 结果 |
|--------|:---:|:--:|
| `183.230.40.96` | `1883` | ✅ QMTOPEN 返回 0,0 |
| `183.230.40.39` | `1883` | ❌ QMTOPEN 返回 0,-1 |
| `183.230.40.39` | `6002` | ❌ QMTOPEN 返回 0,-1 |

**结论：你的环境用 `183.230.40.96:1883`。**

---

## Topic 格式

| 方向 | Topic 模板 |
|------|-----------|
| 上行（上报） | `$sys/{产品ID}/{设备名称}/dp/post/json` |
| 下行（接收） | `$sys/{产品ID}/{设备名称}/cmd/#` |

---

## 常见错误码

| 错误 | 含义 | 排查方向 |
|------|------|----------|
| `+QMTOPEN: 0,-1` | TCP 连接失败 | 换 IP/端口、检查网络是否正常 |
| `+QMTCONN: 0,0,4` | 用户名或密码错误 | 检查 onenet 配置的产品ID/access_key |
| `+QMTCONN: 0,0,5` | 未授权 | 设备未注册或已被禁用 |
| `+QMTSTAT: 0,1` | 被服务器断开 | 检查鉴权、固件版本 |
| `+QMTSTAT: 0,2` | PINGREQ 超时 | 信号差或 keepalive 太长 |
