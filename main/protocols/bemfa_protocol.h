#ifndef BEMFA_PROTOCOL_H
#define BEMFA_PROTOCOL_H

#include "protocol.h"
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <udp.h>
#include <mbedtls/aes.h>
#include <functional>
#include <string>
#include <map>
#include <mutex>

#define BEMFA_MQTT_PING_INTERVAL_SECONDS 90
#define BEMFA_MQTT_RECONNECT_INTERVAL_MS 10000
#define BEMFA_MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class Mqtt;  // 前向声明

class BemfaProtocol : public Protocol {
public:
    BemfaProtocol();
    ~BemfaProtocol() override;

    void Start() override;
    void SendAudio(const std::vector<uint8_t>& data) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::string endpoint_;
    std::string client_id_;
    std::string private_key_;
    std::string username_;
    std::string password_;
    std::string publish_topic_;
    Mqtt* mqtt_ = nullptr;  // 添加 mqtt_ 成员变量
    // UDP
    std::mutex channel_mutex_;
    Udp* udp_ = nullptr;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;

    bool StartMqttClient(bool report_error=false);
    std::string DecodeHexString(const std::string& hex_string);
    bool SendText(const std::string& text) override;
};

#endif // BEMFA_PROTOCOL_H
