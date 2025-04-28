#include "bemfa_protocol.h"
#include "application.h"
#include "board.h"

// 引入必要的头文件
#include <esp_log.h>          // ESP32日志功能
#include <ml307_mqtt.h>       // MQTT客户端实现
#include <ml307_udp.h>        // UDP通信实现
#include <cJSON.h>            // JSON解析库
#include <cstring>            // C字符串处理
#include <arpa/inet.h>        // 网络地址转换
#include "assets/lang_config.h" // 语言配置

// 定义日志标签，用于ESP32日志输出时的标识
#define TAG "BemfaProtocol"

/**
 * @brief BemfaProtocol类的构造函数
 * 初始化与巴法云服务器通信所需的基本参数
 * 
 * 主要完成以下初始化工作：
 * 1. 设置服务器地址(endpoint_)为巴法云服务器
 * 2. 设置客户端ID，用于在服务器端唯一标识本设备
 * 3. 初始化MQTT通信相关的参数（主题、用户名、密码等）
 * 4. 创建事件组用于同步操作
 */
BemfaProtocol::BemfaProtocol() : Protocol() {
    endpoint_ = "bemfa.com";                                    // 设置巴法云服务器地址
    client_id_ = "6407f6655dd3de7ab3a5476d36c9ab26";          // 设置设备唯一标识符
    private_key_ = "";                                         // 初始化私钥为空
    publish_topic_ = "testtopic";                          // 设置默认的MQTT发布主题 巴法云中主题后增加set可避免受到自己的消息
    username_= "";                                             // 初始化用户名为空
    password_ = "";                                            // 初始化密码为空
    event_group_handle_ = xEventGroupCreate();                 // 创建FreeRTOS事件组，用于任务同步
}

/**
 * @brief 析构函数
 * 负责清理和释放所有资源，确保程序正常退出
 * 
 * 执行的清理工作：
 * 1. 断开MQTT连接并释放MQTT客户端资源
 * 2. 释放UDP连接资源（如果存在）
 * 3. 删除事件组
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

/**
 * @brief 启动协议服务
 * 初始化并启动MQTT客户端，不报告错误
 */
void BemfaProtocol::Start() {
    StartMqttClient(false);
}

/**
 * @brief 启动MQTT协议服务
 * @param report_error 是否报告错误信息
 * @return 返回连接是否成功
 * 
 * 主要功能：
 * 1. 检查并清理已存在的MQTT客户端
 * 2. 验证服务器地址是否有效
 * 3. 创建新的MQTT客户端实例
 * 4. 设置MQTT连接参数（心跳间隔等）
 * 5. 配置断开连接的回调函数
 * 6. 设置消息接收的回调函数
 * 7. 建立MQTT连接
 * 8. 订阅指定主题
 */
bool BemfaProtocol::StartMqttClient(bool report_error) {
    ESP_LOGI(TAG, "Starting BemfaProtocol...");               // 输出启动日志

    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");         // 检查MQTT客户端是否已存在
        delete mqtt_;                                         // 释放已存在的MQTT客户端
    }

    if (endpoint_.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");      // 检查服务器地址是否为空
        if (report_error) {
            // SetError(Lang::Strings::SERVER_NOT_FOUND);     // 如果需要报告错误则设置错误信息
        }
        return false;
    }
    
    mqtt_ = Board::GetInstance().CreateMqtt();                // 创建新的MQTT客户端实例
    if (!mqtt_) {
        ESP_LOGE(TAG, "Failed to create MQTT client");        // 创建失败时记录错误
        return false;
    }

    mqtt_->SetKeepAlive(90);                                  // 设置MQTT心跳间隔为90秒

    mqtt_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Disconnected from endpoint");          // 设置断开连接时的回调函数
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());           // 解析收到的JSON消息
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());  // JSON解析失败
            return;
        }
        
        cJSON* type = cJSON_GetObjectItem(root, "type");      // 获取消息类型字段
        if (type == nullptr) {
            ESP_LOGE(TAG, "Message type is not specified");   // 消息类型不存在
            cJSON_Delete(root);                               // 释放JSON对象
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        }else if (strcmp(type->valuestring, "goodbye") == 0) {      // 处理goodbye类型的消息
            auto session_id = cJSON_GetObjectItem(root, "session_id");  // 获取会话ID
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", 
                    session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {  // 调度关闭音频通道的任务
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {            // 处理其他类型的JSON消息
            on_incoming_json_(root);                          // 调用用户设置的JSON处理回调
        }
        
        cJSON_Delete(root);                                   // 释放JSON对象
        last_incoming_time_ = std::chrono::steady_clock::now();  // 更新最后接收消息的时间
    });

    // 连接MQTT服务器
    if (!mqtt_->Connect(endpoint_, 9501, client_id_, username_, password_)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");       // 连接失败时记录错误
        return false;
    }

    ESP_LOGI(TAG, "Connected to MQTT server successfully");   // 连接成功

    mqtt_->Subscribe(publish_topic_, 1);                      // 订阅指定主题，QoS级别为1
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
    if (!mqtt_->Publish(publish_topic_ + "/set", text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        // SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

/**
 * @brief 发送音频数据
 * @param data 要发送的音频数据
 * 
 * 注意：此功能尚未实现，预留接口用于后续开发
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
    ESP_LOGE(TAG, "Close audio channel");

}

/**
 * @brief 打开音频通道
 * @return 是否成功打开通道
 * 
 * 执行步骤：
 * 1. 检查MQTT连接状态，必要时重新连接
 * 2. 重置音频传输相关的状态标志
 * 3. 清除事件组中的服务器hello事件标志
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

    // 发送 hello 消息申请 UDP 通道
    std::string message = "{";
    message += "\"type\":\"hello\",";
    message += "\"version\": 3,";
    message += "\"transport\":\"udp\",";
    message += "\"audio_params\":{";
    message += "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, \"frame_duration\":" + std::to_string(OPUS_FRAME_DURATION_MS);
    message += "}}";
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, BEMFA_MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & BEMFA_MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    return true;
}

/**
 * @brief 解析服务器发送的hello消息
 * @param root cJSON对象，包含服务器配置信息
 * 
 * 解析内容：
 * 1. 传输方式（必须是UDP）
 * 2. 会话ID
 * 3. 音频参数（采样率、帧持续时间）
 * 4. UDP服务器配置（服务器地址、端口、密钥等）
 * 5. 初始化AES加密上下文
 */
void BemfaProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");  // 获取传输方式
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);  // 不支持的传输方式
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");  // 获取会话ID
    if (session_id != nullptr) {
        session_id_ = session_id->valuestring;                // 保存会话ID
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");  // 获取音频参数
    if (audio_params != NULL) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");  // 获取采样率
        if (sample_rate != NULL) {
            server_sample_rate_ = sample_rate->valueint;      // 保存服务器采样率
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");  // 获取帧持续时间
        if (frame_duration != NULL) {
            server_frame_duration_ = frame_duration->valueint;  // 保存帧持续时间
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");             // 获取UDP配置
    if (udp == nullptr) {
        ESP_LOGE(TAG, "UDP is not specified");               // UDP配置不存在
        return;
    }
    
    // 解析UDP服务器配置
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;  // 服务器地址
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;        // 端口号
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;       // 加密密钥
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;   // 加密随机数

    aes_nonce_ = DecodeHexString(nonce);                     // 解码nonce
    mbedtls_aes_init(&aes_ctx_);                            // 初始化AES加密上下文
    mbedtls_aes_setkey_enc(&aes_ctx_, 
        (const unsigned char*)DecodeHexString(key).c_str(),  // 设置AES加密密钥
        128);                                                // 使用128位密钥长度
    local_sequence_ = 0;                                     // 初始化本地序列号
    remote_sequence_ = 0;                                    // 初始化远程序列号
    xEventGroupSetBits(event_group_handle_, BEMFA_MQTT_PROTOCOL_SERVER_HELLO_EVENT);  // 设置hello事件标志
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
 * 
 * 工作原理：
 * 1. 预分配结果字符串空间
 * 2. 每两个十六进制字符转换为一个字节
 * 3. 使用位运算组合高位和低位
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
 * 
 * 检查条件：
 * 1. UDP连接是否存在
 * 2. 是否发生错误
 * 3. 是否超时
 */
bool BemfaProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
