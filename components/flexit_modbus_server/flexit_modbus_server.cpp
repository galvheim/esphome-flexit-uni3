// Flexit Modbus-server for ESPHome.
//
// Basert på MSkjel/esphome-flexit-modbus-server (MIT-lisens, commit 82065d3):
// https://github.com/MSkjel/esphome-flexit-modbus-server
//
// Endringer i denne kopien:
//  - Svarer på CS60-ens oppstartsskanning (fn 04, reg 0–3) slik det originale
//    CI600-panelet gjør. Da godtar CS60 ESP-en som panel også når CI600 er fjernet.
//  - Strengere lengdesjekk av 0x10-rammer, så en falsk rammestart ikke låser
//    rammedeleren mens den venter på bytes som aldri kommer.
//  - Diagnose: et sammendrag hvert 60. s (DEBUG) og hex-logg av rammer (VERBOSE).

#include "flexit_modbus_server.h"
#ifdef USE_FLEXIT_TCP_BRIDGE
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace flexit_modbus_server {

// Expected total length of the frame starting at `frame` (function code at
// frame[1]), given `available` bytes are present. The CS60 blasts frames with no
// interframe gap, so we size each one by its function code and let the CRC confirm
// the boundary. Every frame on this bus is 8 bytes except the 0x10 register
// broadcasts (sized by their byte-count field) and exceptions (5). Returns 0 for
// a function code we don't handle, so the caller resyncs.
static size_t expected_frame_length(const uint8_t *frame, size_t available) {
  const uint8_t function = frame[1];

  switch (function) {
    case 0x01:    // read coils (the CS60's coil poll)
    case 0x03:    // read holding registers
    case 0x04:    // read input registers (startup server discovery)
    case 0x06:    // write single register
    case 0x65:    // CS60 custom register/coil write
      return 8;
    case 0x10: {  // write multiple registers
      if (available < 7)
        return available + 1;                            // need the byte count (frame[6]) first
      // Modbus requires byte_count == 2 * quantity, with quantity in 1..123. Reject anything
      // else at once, so a phantom 0x10 header can't stall the splitter waiting for bytes.
      size_t quantity = (frame[4] << 8) | frame[5];
      size_t byte_count = frame[6];
      if (quantity == 0 || quantity > 123 || byte_count != quantity * 2)
        return 0;
      return byte_count + 9;                             // 7 header + byte_count data + 2 CRC
    }
    default:
      return (function & 0x80) ? 5 : 0;                  // exception response, else unknown
  }
}

std::string mode_to_string(uint16_t mode) {
  if (mode < NUM_MODES) {
    return MODE_STRINGS[mode];
  }
  return "Invalid mode";
}

uint16_t string_to_mode(StringRef mode_str) {
  for (uint16_t i = 0; i < NUM_MODES; ++i) {
    if (mode_str == MODE_STRINGS[i]) {
      return i;
    }
  }
  // Default to "Normal" mode if not found.
  return 2;
}

FlexitModbusServer::FlexitModbusServer() {}

void FlexitModbusServer::dump_config() {
  ESP_LOGCONFIG(TAG, "Flexit Modbus Server:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", server_address_);
  ESP_LOGCONFIG(TAG, "  Baud Rate: %u", (unsigned) baudRate());

  if (tx_enable_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  TX Enable Pin: GPIO%d", tx_enable_pin_);
    ESP_LOGCONFIG(TAG, "  TX Enable Direct: %s", tx_enable_direct_ ? "YES" : "NO");
  }

#ifdef USE_FLEXIT_TCP_BRIDGE
  ESP_LOGCONFIG(TAG, "  TCP Bridge Enabled: %s", tcp_bridge_enabled_ ? "YES" : "NO");

  if (tcp_bridge_enabled_) {
    ESP_LOGCONFIG(TAG, "  TCP Bridge Port: %u", tcp_bridge_port_);
    ESP_LOGCONFIG(TAG, "  TCP Bridge Max Clients: %u", tcp_bridge_max_clients_);

    if (tcp_server_ != nullptr &&
        wifi::global_wifi_component != nullptr &&
        wifi::global_wifi_component->is_connected()) {

      auto ips = wifi::global_wifi_component->get_ip_addresses();

      if (!ips.empty()) {
        char ip_buf[network::IP_ADDRESS_BUFFER_SIZE];
        ips[0].str_to(ip_buf);
        ESP_LOGCONFIG(TAG, "  TCP Bridge Status: Running on tcp://%s:%u",
                      ip_buf, tcp_bridge_port_);
      } else {
        ESP_LOGCONFIG(TAG, "  TCP Bridge Status: Waiting for IP...");
      }
    } else {
      ESP_LOGCONFIG(TAG, "  TCP Bridge Status: Waiting for WiFi...");
    }
  }
#endif  // USE_FLEXIT_TCP_BRIDGE
}

void FlexitModbusServer::setup() {
  // Initialize the new ModbusRTUServer instance using our Stream interface (this),
  // the baud rate from our UART parent, the server address, and the maximum number
  // of coils and holding registers.
  mb_.begin(this, baudRate(), server_address_, tx_enable_pin_, tx_enable_direct_, MAX_NUM_COILS, MAX_NUM_HOLDING_REGISTERS, 0, 4);

  // CS60 leter etter paneler ved oppstart med "fn 04 reg 0 x4". Det ekte
  // CI600-panelet svarer 0, 0, 1, 0x0300 (avlest på bussen). Svar det samme,
  // så CS60 godtar oss som et panel og begynner å polle coils.
  mb_.setInputRegister(0, 0x0000);
  mb_.setInputRegister(1, 0x0000);
  mb_.setInputRegister(2, 0x0001);
  mb_.setInputRegister(3, 0x0300);

  // The CX60 doesnt follow the Modbus RTU spec: it ignores interframe gaps
  // and blasts frames back-to-back, so we can't delimit by timing. Take over
  // framing instead: walk the raw buffer, accept the first length whose CRC
  // validates, dispatch it, and keep any trailing partial frame for next time.
  mb_.onRawBuffer = [this](uint8_t* data, size_t length) -> size_t {
    size_t offset = 0;

    while (length - offset >= MIN_FRAME_LENGTH) {
      size_t avail = length - offset;
      size_t len = expected_frame_length(data + offset, avail);

      if (len == 0) {                            // not a frame start we know -> resync
        ++offset;
        continue;
      }
      if (len > avail)                           // frame not fully here yet -> wait
        break;
      if (!mb_.checkCrc(data + offset, len)) {   // right size, bad CRC -> resync
        ++offset;
        continue;
      }

      uint8_t fn = data[offset + 1];
      if (data[offset] == server_address_) {
        diag_own_[fn == 0x04 ? 0 : fn == 0x01 ? 1 : fn == 0x03 ? 2 : 3]++;
      } else if (data[offset] == 0) {
        diag_bcast_++;
      }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
      // Logg alt til oss unntatt coil-poll, og alle 0x65 (kommando-kvittering)
      if ((data[offset] == server_address_ && fn != 0x01) || fn == 0x65) {
        char hex[3 * 16 + 1];
        size_t n = len < 16 ? len : 16;
        for (size_t k = 0; k < n; k++)
          snprintf(hex + 3 * k, 4, "%02X ", data[offset + k]);
        ESP_LOGV(TAG, "RX %s", hex);
      }
#endif
      mb_.processFrame(data + offset, len);
      offset += len;
    }

    return offset;
  };

  // This is used as a cmd coil/register reset. Should we check the CRC?
  mb_.onInvalidFunction = [this](uint8_t* data, size_t length, bool broadcast) {
    uint8_t function_code = data[1];
    
    if (function_code == 0x65) {
        uint16_t address = (data[2] << 8) | data[3];
        uint16_t value = (data[4] << 8) | data[5];
        
        mb_.setHoldingRegister(address, value);
        mb_.setCoil(address, 0);
      
        return;
    }

    mb_.sendException(data[1], 0x01, broadcast);
  };

#ifdef USE_FLEXIT_TCP_BRIDGE
  if (tcp_bridge_enabled_) {
    setup_tcp_bridge_();
  }
#endif  // USE_FLEXIT_TCP_BRIDGE
}

void FlexitModbusServer::loop() {
  mb_.update();

  uint32_t now = millis();
  // Sammendrag for feilsøking. broadcast=0: ingen data fra CS60 (sjekk A/B og
  // baudrate). coil-poll=0: CS60 har ikke funnet oss (ESP-en må starte før CS60).
  if (now - diag_ms_ >= 60000) {
    diag_ms_ = now;
    ESP_LOGD(TAG, "Til adresse %u: oppstartsskann(04)=%u coil-poll(01)=%u les(03)=%u annet=%u | broadcast fra CS60=%u",
             server_address_, (unsigned) diag_own_[0], (unsigned) diag_own_[1],
             (unsigned) diag_own_[2], (unsigned) diag_own_[3], (unsigned) diag_bcast_);
  }

#ifdef USE_FLEXIT_TCP_BRIDGE
  if (tcp_bridge_enabled_) {
    handle_tcp_bridge_();
  }
#endif  // USE_FLEXIT_TCP_BRIDGE
}

void FlexitModbusServer::write_holding_register(HoldingRegisterIndex reg, uint16_t value) {
  mb_.setHoldingRegister(reg, value);
}

uint16_t FlexitModbusServer::read_holding_register(HoldingRegisterIndex reg) {
  return mb_.getHoldingRegister(reg);
}

float FlexitModbusServer::read_holding_register_temperature(HoldingRegisterIndex reg) {
  // Convert the raw register value to a temperature (divide by 10).
  return static_cast<int16_t>(mb_.getHoldingRegister(reg)) / 10.0f;
}

float FlexitModbusServer::read_holding_register_hours(HoldingRegisterIndex high_reg) {
  // Combine two registers: the high word and the subsequent low word.
  uint32_t rawSeconds = (static_cast<uint32_t>(mb_.getHoldingRegister(high_reg)) << 16)
                          + static_cast<uint32_t>(mb_.getHoldingRegister(high_reg + 1));
  return rawSeconds / 3600.0f;
}

void FlexitModbusServer::send_cmd(HoldingRegisterIndex cmd_register, uint16_t value) {
  // Write the command value to the register and set the corresponding coil.
  ESP_LOGD(TAG, "send_cmd reg 0x%03X = %u", (unsigned) cmd_register, (unsigned) value);
  mb_.setHoldingRegister(cmd_register, value);
  mb_.setCoil(cmd_register, 1);
}

// ---------------------------------------------------------
// ESPHome UART Device Requirements
// ---------------------------------------------------------
uint32_t FlexitModbusServer::baudRate() {
  // Return the baud rate from the parent UART device.
  return this->parent_->get_baud_rate();
}

// ---------------------------------------------------------
// Setters (for configuration via __init__.py)
// ---------------------------------------------------------
void FlexitModbusServer::set_server_address(uint8_t address) {
  server_address_ = address;
}

void FlexitModbusServer::set_tx_enable_pin(int16_t pin) {
  tx_enable_pin_ = pin;
}

void FlexitModbusServer::set_tx_enable_direct(bool val) {
  tx_enable_direct_ = val;
}

#ifdef USE_FLEXIT_TCP_BRIDGE
void FlexitModbusServer::set_tcp_bridge_enabled(bool enabled) {
  tcp_bridge_enabled_ = enabled;
}

void FlexitModbusServer::set_tcp_bridge_port(uint16_t port) {
  tcp_bridge_port_ = port;
}

void FlexitModbusServer::set_tcp_bridge_max_clients(uint8_t max_clients) {
  tcp_bridge_max_clients_ = max_clients;
}
#endif  // USE_FLEXIT_TCP_BRIDGE

// ---------------------------------------------------------
// Stream interface implementation (required by ModbusRTUServer)
// ---------------------------------------------------------
size_t FlexitModbusServer::write(uint8_t data) {
  size_t result = uart::UARTDevice::write(data);
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  if (tx_log_.size() < 16)
    tx_log_.push_back(data);
#endif

#ifdef USE_FLEXIT_TCP_BRIDGE
  if (tcp_bridge_enabled_ && tcp_server_ != nullptr && !tcp_clients_.empty()) {
    uart_to_tcp_buffer_.push_back(data);
  }
#endif

  return result;
}

int FlexitModbusServer::available() {
  return uart::UARTDevice::available();
}

int FlexitModbusServer::read() {
  int v = uart::UARTDevice::read();
  
  if (v < 0) {
    return v;
  }

#ifdef USE_FLEXIT_TCP_BRIDGE
  if (tcp_bridge_enabled_ && tcp_server_ != nullptr && !tcp_clients_.empty()) {
    uart_rx_mirror_.push_back(static_cast<uint8_t>(v));
  }
#endif

  return v;
}

int FlexitModbusServer::peek() {
  return uart::UARTDevice::peek();
}

void FlexitModbusServer::flush() {
  uart::UARTDevice::flush();

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  // Logg våre svar, unntatt svar på coil-poll (~15/s)
  if (tx_log_.size() >= 2 && tx_log_[1] != 0x01) {
    char hex[3 * 16 + 1];
    for (size_t k = 0; k < tx_log_.size(); k++)
      snprintf(hex + 3 * k, 4, "%02X ", tx_log_[k]);
    ESP_LOGV(TAG, "TX %s", hex);
  }
  tx_log_.clear();
#endif

#ifdef USE_FLEXIT_TCP_BRIDGE
  if (!(tcp_bridge_enabled_ && tcp_server_ != nullptr))
    return;

  auto send_framed_block = [this](const std::vector<uint8_t> &buf, uint8_t dir) {
    if (buf.empty())
      return;

    uint16_t len = static_cast<uint16_t>(buf.size());
    uint8_t header[3] = {
      dir,
      static_cast<uint8_t>((len >> 8) & 0xFF),
      static_cast<uint8_t>(len & 0xFF),
    };

    for (auto &client : tcp_clients_) {
      if (!client.connected())
        continue;

      client.write(header, sizeof(header));
      client.write(buf.data(), buf.size());
    }
  };

  if (!uart_to_tcp_buffer_.empty()) {
    send_framed_block(uart_to_tcp_buffer_, FRAME_DIR_TX);
    uart_to_tcp_buffer_.clear();
  }

  if (!uart_rx_mirror_.empty()) {
    send_framed_block(uart_rx_mirror_, FRAME_DIR_RX);
    uart_rx_mirror_.clear();
  }
#endif  // USE_FLEXIT_TCP_BRIDGE
}

// ---------------------------------------------------------
// TCP Bridge Implementation
// ---------------------------------------------------------
#ifdef USE_FLEXIT_TCP_BRIDGE
void FlexitModbusServer::setup_tcp_bridge_() {
  if (wifi::global_wifi_component == nullptr ||
      !wifi::global_wifi_component->is_connected()) {
    return;
  }

  auto ips = wifi::global_wifi_component->get_ip_addresses();
  if (ips.empty()) {
    return;
  }

  tcp_server_ = new WiFiServer(tcp_bridge_port_);
  tcp_server_->begin();
  tcp_server_->setNoDelay(true);

  char ip_buf[network::IP_ADDRESS_BUFFER_SIZE];
  ips[0].str_to(ip_buf);
  ESP_LOGI(TAG, "TCP Bridge: Server started on tcp://%s:%u (max clients: %u)",
          ip_buf, tcp_bridge_port_, tcp_bridge_max_clients_);
}

void FlexitModbusServer::handle_tcp_bridge_() {
  if (tcp_server_ == nullptr) {
    if (wifi::global_wifi_component != nullptr &&
        wifi::global_wifi_component->is_connected()) {
      setup_tcp_bridge_();
    }
    return;
  }

  accept_tcp_clients_();
  cleanup_tcp_clients_();
}

void FlexitModbusServer::accept_tcp_clients_() {
  if (tcp_server_->hasClient()) {
    WiFiClient new_client = tcp_server_->accept();

    if (tcp_clients_.size() >= tcp_bridge_max_clients_) {
      ESP_LOGW(TAG, "TCP Bridge: Max clients (%u) reached, rejecting connection from %s",
              tcp_bridge_max_clients_, new_client.remoteIP().toString().c_str());
      new_client.stop();
    } else {
      tcp_clients_.push_back(new_client);
      ESP_LOGI(TAG, "TCP Bridge: Client connected from %s (total: %u)",
              new_client.remoteIP().toString().c_str(), tcp_clients_.size());
    }
  }
}

void FlexitModbusServer::cleanup_tcp_clients_() {
  for (auto it = tcp_clients_.begin(); it != tcp_clients_.end();) {
    if (!it->connected()) {
      ESP_LOGI(TAG, "TCP Bridge: Client disconnected from %s (total: %u)",
              it->remoteIP().toString().c_str(), tcp_clients_.size() - 1);

      it->stop();
      it = tcp_clients_.erase(it);
    } else {
      ++it;
    }
  }
}
#endif  // USE_FLEXIT_TCP_BRIDGE

}  // namespace flexit_modbus_server
}  // namespace esphome
