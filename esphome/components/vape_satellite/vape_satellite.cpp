#include "vape_satellite.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome {
namespace vape_satellite {

static const char *const TAG = "vape_satellite";
static const size_t MIC_RING_BUFFER_SIZE = 64 * 1024;
static const size_t SEND_CHUNK_SIZE = 1280;
static const uint32_t WEBSOCKET_SEND_TIMEOUT_MS = 20;
static const uint32_t SPEAKER_PLAY_TIMEOUT_MS = 100;
static const uint32_t RECONNECT_INTERVAL_MS = 1000;

void VapeSatellite::setup() {
  this->mic_ring_buffer_ = RingBuffer::create(MIC_RING_BUFFER_SIZE);
  this->send_buffer_.resize(SEND_CHUNK_SIZE);

  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if ((this->state_ == State::CONNECTING || this->state_ == State::WAITING_FOR_CAPTURE ||
         this->state_ == State::STREAMING) &&
        this->mic_ring_buffer_ != nullptr) {
      this->mic_ring_buffer_->write(data.data(), data.size());
    }
  });

  if (this->speaker_ != nullptr) {
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, this->output_sample_rate_));
  }
}

void VapeSatellite::loop() {
  if ((this->state_ == State::CONNECTING || this->state_ == State::WAITING_FOR_CAPTURE) && !this->connected_) {
    uint32_t now = millis();
    if (now - this->last_reconnect_attempt_ > RECONNECT_INTERVAL_MS) {
      this->last_reconnect_attempt_ = now;
      this->connect_();
    }
  }

  if (this->state_ == State::CONNECTING || this->state_ == State::WAITING_FOR_CAPTURE ||
      this->state_ == State::STREAMING) {
    this->start_microphone_();
  }

  if (this->state_ == State::STREAMING) {
    this->send_audio_();
  }
}

void VapeSatellite::dump_config() {
  ESP_LOGCONFIG(TAG, "VAPE satellite:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Input: pcm_s16le/%" PRIu32 "/mono", this->input_sample_rate_);
  ESP_LOGCONFIG(TAG, "  Output: pcm_s16le/%" PRIu32 "/mono", this->output_sample_rate_);
}

float VapeSatellite::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void VapeSatellite::start(const std::string &wake_word) {
  this->pending_wake_word_ = wake_word.empty() ? "wake" : wake_word;
  this->reset_microphone_buffer_();
  this->start_microphone_();
  if (!this->connected_) {
    this->set_state_(State::CONNECTING);
    this->connect_();
    return;
  }
  if (!this->hello_acknowledged_) {
    this->send_hello_();
    this->set_state_(State::WAITING_FOR_CAPTURE);
    return;
  }
  this->send_wake_detected_();
  this->set_state_(State::WAITING_FOR_CAPTURE);
}

void VapeSatellite::stop() {
  if (this->client_ != nullptr && this->connected_) {
    const char *message = "{\"type\":\"audio_stop\",\"protocol_version\":1}";
    esp_websocket_client_send_text(this->client_, message, strlen(message), pdMS_TO_TICKS(WEBSOCKET_SEND_TIMEOUT_MS));
  }
  if (this->mic_source_ != nullptr && this->mic_source_->is_running()) {
    this->mic_source_->stop();
  }
  this->reset_microphone_buffer_();
  if (this->speaker_ != nullptr) {
    this->speaker_->stop();
  }
  this->set_state_(State::IDLE);
}

void VapeSatellite::connect_() {
  if (this->client_ != nullptr) {
    esp_websocket_client_stop(this->client_);
    esp_websocket_client_destroy(this->client_);
    this->client_ = nullptr;
  }

  esp_websocket_client_config_t config = {};
  config.uri = this->url_.c_str();
  config.disable_auto_reconnect = true;
  this->client_ = esp_websocket_client_init(&config);
  if (this->client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize WebSocket client");
    return;
  }

  esp_websocket_register_events(this->client_, WEBSOCKET_EVENT_ANY, websocket_event_handler, this);
  esp_err_t err = esp_websocket_client_start(this->client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
    esp_websocket_client_destroy(this->client_);
    this->client_ = nullptr;
    return;
  }
}

void VapeSatellite::disconnect_() {
  this->connected_ = false;
  this->hello_acknowledged_ = false;
  if (this->client_ != nullptr) {
    esp_websocket_client_stop(this->client_);
    esp_websocket_client_destroy(this->client_);
    this->client_ = nullptr;
  }
}

void VapeSatellite::send_hello_() {
  if (this->client_ == nullptr || !this->connected_) {
    return;
  }
  char message[256];
  snprintf(message, sizeof(message),
           "{\"type\":\"hello\",\"protocol_version\":1,\"device_id\":\"%s\",\"formats\":[{\"codec\":\"pcm_s16le\","
           "\"sample_rate\":%" PRIu32 ",\"channels\":1}]}",
           this->device_id_.c_str(), this->input_sample_rate_);
  esp_websocket_client_send_text(this->client_, message, strlen(message), pdMS_TO_TICKS(WEBSOCKET_SEND_TIMEOUT_MS));
}

void VapeSatellite::send_wake_detected_() {
  if (this->client_ == nullptr || !this->connected_) {
    return;
  }
  char message[256];
  snprintf(message, sizeof(message),
           "{\"type\":\"wake_detected\",\"protocol_version\":1,\"wake_word\":\"%s\",\"device_id\":\"%s\"}",
           this->pending_wake_word_.c_str(), this->device_id_.c_str());
  esp_websocket_client_send_text(this->client_, message, strlen(message), pdMS_TO_TICKS(WEBSOCKET_SEND_TIMEOUT_MS));
}

void VapeSatellite::send_audio_() {
  if (this->client_ == nullptr || !this->connected_ || this->mic_ring_buffer_ == nullptr) {
    return;
  }

  size_t available = this->mic_ring_buffer_->available();
  while (available >= SEND_CHUNK_SIZE) {
    size_t read_bytes = this->mic_ring_buffer_->read(this->send_buffer_.data(), SEND_CHUNK_SIZE, 0);
    if (read_bytes == 0) {
      return;
    }
    int sent = esp_websocket_client_send_bin(this->client_, reinterpret_cast<const char *>(this->send_buffer_.data()),
                                             read_bytes, pdMS_TO_TICKS(WEBSOCKET_SEND_TIMEOUT_MS));
    if (sent < 0) {
      ESP_LOGW(TAG, "Failed to send microphone audio");
      return;
    }
    available = this->mic_ring_buffer_->available();
  }
}

void VapeSatellite::start_microphone_() {
  if (this->mic_source_ != nullptr && !this->mic_source_->is_running()) {
    this->mic_source_->start();
  }
}

void VapeSatellite::reset_microphone_buffer_() {
  if (this->mic_ring_buffer_ != nullptr) {
    this->mic_ring_buffer_->reset();
  }
}

void VapeSatellite::handle_text_(const char *data, size_t len) {
  std::string message(data, len);
  ESP_LOGV(TAG, "Received control: %s", message.c_str());

  if (message.find("\"type\":\"hello_ack\"") != std::string::npos) {
    this->hello_acknowledged_ = true;
    if (!this->pending_wake_word_.empty()) {
      this->send_wake_detected_();
      this->set_state_(State::WAITING_FOR_CAPTURE);
    }
    return;
  }

  if (message.find("\"type\":\"start_capture\"") != std::string::npos) {
    this->listening_trigger_.trigger();
    this->set_state_(State::STREAMING);
    return;
  }

  if (message.find("\"type\":\"start_playback\"") != std::string::npos) {
    this->speaking_trigger_.trigger();
    if (this->speaker_ != nullptr) {
      this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, this->output_sample_rate_));
      if (!this->speaker_->is_running()) {
        this->speaker_->start();
      }
    }
    return;
  }

  if (message.find("\"type\":\"set_state\"") != std::string::npos) {
    this->handle_remote_state_(message);
    return;
  }

  if (message.find("\"type\":\"stop_playback\"") != std::string::npos) {
    if (this->speaker_ != nullptr) {
      this->speaker_->stop();
    }
    return;
  }
}

void VapeSatellite::handle_remote_state_(const std::string &message) {
  if (message.find("\"state\":\"idle\"") != std::string::npos) {
    this->idle_trigger_.trigger();
  } else if (message.find("\"state\":\"listening\"") != std::string::npos) {
    this->listening_trigger_.trigger();
  } else if (message.find("\"state\":\"thinking\"") != std::string::npos) {
    this->thinking_trigger_.trigger();
  } else if (message.find("\"state\":\"speaking\"") != std::string::npos) {
    this->speaking_trigger_.trigger();
  }
}

void VapeSatellite::handle_binary_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr || data == nullptr || len == 0) {
    return;
  }
  if (!this->speaker_->is_running()) {
    this->speaker_->start();
  }
  size_t written = 0;
  while (written < len) {
    size_t bytes = this->speaker_->play(data + written, len - written, pdMS_TO_TICKS(SPEAKER_PLAY_TIMEOUT_MS));
    if (bytes == 0) {
      break;
    }
    written += bytes;
  }
  if (written < len) {
    ESP_LOGW(TAG, "Dropped %zu of %zu playback bytes", len - written, len);
  }
}

void VapeSatellite::handle_connected_() {
  this->connected_ = true;
  this->send_hello_();
  this->set_state_(State::WAITING_FOR_CAPTURE);
}

void VapeSatellite::handle_disconnected_() {
  this->connected_ = false;
  this->hello_acknowledged_ = false;
  if (this->mic_source_ != nullptr && this->mic_source_->is_running()) {
    this->mic_source_->stop();
  }
  if (this->state_ != State::IDLE) {
    this->set_state_(State::CONNECTING);
  }
}

void VapeSatellite::set_state_(State state) {
  if (this->state_ == state) {
    return;
  }
  this->state_ = state;
}

void VapeSatellite::websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                            void *event_data) {
  auto *satellite = static_cast<VapeSatellite *>(handler_args);
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      satellite->handle_connected_();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      satellite->handle_disconnected_();
      break;
    case WEBSOCKET_EVENT_DATA:
      if (data->op_code == 0x01) {
        satellite->handle_text_(data->data_ptr, data->data_len);
      } else if (data->op_code == 0x02) {
        satellite->handle_binary_(reinterpret_cast<const uint8_t *>(data->data_ptr), data->data_len);
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "WebSocket error");
      break;
    default:
      break;
  }
}

}  // namespace vape_satellite
}  // namespace esphome
