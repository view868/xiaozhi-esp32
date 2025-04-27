#include "bemfa_protocol.h"
#include "application.h"
#include "board.h"

#include <esp_log.h>
#include <ml307_mqtt.h>
#include <ml307_udp.h>
#include <cJSON.h>
#include <cstring>  
#include <arpa/inet.h>
#include "assets/lang_config.h"

// 定义日志标签
#define TAG "BemfaProtocol"

/**
 * @brief BemfaProtocol类的构造函数
 * 初始化与巴法云服务器通信所需的基本参数
 */
BemfaProtocol::BemfaProtocol() : Protocol() {
    endpoint_ = "bemfa.com";                                    // 巴法云服务器地址
    client_id_ = "6407f6655dd3de7ab3a5476d36c9ab26";          // 客户端ID，用于身份识别
    private_key_ = "";                                         // 私钥，用于安全认证
    publish_topic_ = "testtopic";                              // MQTT发布主题
    event_group_handle_ = xEventGroupCreate();                 // 创建事件组，用于同步操作
}

/**
 * @brief 析构函数
 * 清理资源，断开连接
 */
BemfaProtocol::~BemfaProtocol() {
    if (mqtt_) {
        mqtt_->Disconnect();    // 断开MQTT连接
        delete mqtt_;           // 释放MQTT客户端
        mqtt_ = nullptr;
    }
    if (udp_ != nullptr) {
        delete udp_;           // 释放UDP连接
    }
    vEventGroupDelete(event_group_handle_);  // 删除事件组
}

void BemfaProtocol::Start() {
    StartMqttClient(false);
}

/**
 * @brief 启动MQTT协议服务
 * 创建MQTT客户端并建立连接
 */
bool BemfaProtocol::StartMqttClient(bool report_error) {
    ESP_LOGI(TAG, "Starting BemfaProtocol...");
    
    // 创建MQTT客户端实例
    mqtt_ = Board::GetInstance().CreateMqtt();
    if (!mqtt_) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return false;
    }

    // 设置MQTT心跳间隔为90秒
    mqtt_->SetKeepAlive(90);

    // 设置断开连接的回调函数
    mqtt_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Disconnected from endpoint");
    });

    // 设置接收消息的回调函数
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        // 解析JSON消息
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        
        // 获取消息类型
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (type == nullptr) {
            ESP_LOGE(TAG, "Message type is not specified");
            cJSON_Delete(root);
            return;
        }

        // 处理不同类型的消息
        if (strcmp(type->valuestring, "goodbye") == 0) {
            // 处理goodbye消息，关闭音频通道
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", 
                    session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            // 处理其他类型的JSON消息
            on_incoming_json_(root);
        }
        
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    // 连接到MQTT服务器
    if (!mqtt_->Connect(endpoint_, 9501, client_id_, username_, password_)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        // SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to MQTT server successfully");

    // 订阅主题
    mqtt_->Subscribe(publish_topic_, 1);
    ESP_LOGI(TAG, "Attempted to subscribe to topic: %s (ignore return value)", publish_topic_.c_str());
    return true;
}


/**
 * @brief 发送文本消息
 * @param text 要发送的文本
 * @return 是否发送成功
 */
bool BemfaProtocol::SendText(const std::string& text) {
    if (!mqtt_) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return false;
    }

    if (publish_topic_.empty()) {
        return false;
    }
    
    // 发布消息到MQTT主题
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        // SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

/**
 * @brief 发送音频数据
 * @param data 要发送的音频数据
 */
void BemfaProtocol::SendAudio(const std::vector<uint8_t>& data) {
    // 待实现音频数据发送功能
}


/**
 * @brief 关闭音频通道
 * 清理UDP连接并发送goodbye消息
 */
void BemfaProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ != nullptr) {
            delete udp_;
            udp_ = nullptr;
        }
    }

    // 发送goodbye消息
    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    // 触发通道关闭回调
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

/**
 * @brief 打开音频通道
 * @return 是否成功打开通道
 */
bool BemfaProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }
    
    busy_sending_audio_ = false;
    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, BEMFA_MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    return true;
}

// 十六进制字符数组
static const char hex_chars[] = "0123456789ABCDEF";

/**
 * @brief 将单个十六进制字符转换为对应的数值
 * @param c 十六进制字符
 * @return 对应的数值
 */
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

/**
 * @brief 解码十六进制字符串
 * @param hex_string 十六进制字符串
 * @return 解码后的字符串
 */
std::string BemfaProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

/**
 * @brief 检查音频通道是否打开
 * @return 通道状态
 */
bool BemfaProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
