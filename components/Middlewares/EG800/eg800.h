#ifndef __EG800_H__
#define __EG800_H__

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 下行数据回调，模块收到 OneNET 下发指令时调用
 * @param topic  消息 Topic
 * @param payload 消息体（JSON 字符串）
 * @param len    消息长度
 */
typedef void (*eg800_downlink_cb_t)(const char *topic, const char *payload, int len);

/**
 * @brief 填写你的 OneNET 三元组，调用此函数完成：
 *        串口初始化 → 模块唤醒 → SIM 检查 → 4G 注册 → PDP 激活
 *        → MQTT 参数配置 → TCP 连接 OneNET → MQTT 登录 → 订阅下行 Topic
 *
 * @param product_id   产品 ID
 * @param device_name  设备名称
 * @param access_key   Access Key（用于 OneNET 自动鉴权）
 * @param cb           下行指令回调（可为 NULL）
 * @return ESP_OK 成功，其他值表示失败阶段
 */
esp_err_t EG800_Init(const char *product_id, const char *device_name,
                     const char *access_key, eg800_downlink_cb_t cb);

/**
 * @brief 向 OneNET 上报 JSON 数据（属性上报 topic）
 * @param json_str  JSON 字符串
 * @return ESP_OK 成功
 */
esp_err_t EG800_Publish(const char *json_str);

/**
 * @brief 向指定 topic 发布消息
 * @param json_str  JSON 字符串
 * @param topic     MQTT topic（不包含外层引号）
 * @return ESP_OK 成功
 */
esp_err_t EG800_PublishTo(const char *json_str, const char *topic);

/**
 * @brief 回复 OneNET 属性设置指令（thing/property/set_reply）
 * @param id     下发指令中的消息 id
 * @param code   结果码，200=成功
 * @param msg    结果描述
 * @return ESP_OK 成功
 */
esp_err_t EG800_SendReply(const char *id, int code, const char *msg);

/**
 * @brief 主动拉取离线期间缓存的期望属性（重连后调用）
 * @return ESP_OK 成功
 */
esp_err_t EG800_QueryDesired(void);

/**
 * @brief 查询 MQTT 是否已连接
 */
bool EG800_IsConnected(void);

/**
 * @brief 获取信号强度 (rssi: 0-31)
 * @param rssi 输出信号值，99=无信号
 */
esp_err_t EG800_GetSignalQuality(uint8_t *rssi);

/**
 * @brief 安全断开 MQTT 并关闭模块
 */
esp_err_t EG800_Deinit(void);

#ifdef __cplusplus
}
#endif

#endif
