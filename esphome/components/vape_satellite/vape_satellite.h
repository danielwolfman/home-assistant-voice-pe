#pragma once

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#include <esp_websocket_client.h>

#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace vape_satellite {

class VapeSatellite : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_url(const std::string &url) { this->url_ = url; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }
  void set_microphone_source(microphone::MicrophoneSource *mic_source) { this->mic_source_ = mic_source; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_input_sample_rate(uint32_t sample_rate) { this->input_sample_rate_ = sample_rate; }
  void set_output_sample_rate(uint32_t sample_rate) { this->output_sample_rate_ = sample_rate; }

  void start(const std::string &wake_word);
  void stop();

 protected:
  enum class State : uint8_t {
    IDLE,
    CONNECTING,
    WAITING_FOR_CAPTURE,
    STREAMING,
  };

  static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  void connect_();
  void disconnect_();
  void send_hello_();
  void send_wake_detected_();
  void send_audio_();
  void handle_text_(const char *data, size_t len);
  void handle_binary_(const uint8_t *data, size_t len);
  void handle_connected_();
  void handle_disconnected_();
  void set_state_(State state);

  std::string url_;
  std::string device_id_;
  std::string pending_wake_word_;
  microphone::MicrophoneSource *mic_source_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  esp_websocket_client_handle_t client_{nullptr};
  std::unique_ptr<RingBuffer> mic_ring_buffer_;
  State state_{State::IDLE};
  bool connected_{false};
  bool hello_acknowledged_{false};
  uint32_t input_sample_rate_{16000};
  uint32_t output_sample_rate_{24000};
  uint32_t last_reconnect_attempt_{0};
  std::vector<uint8_t> send_buffer_;
};

}  // namespace vape_satellite
}  // namespace esphome
