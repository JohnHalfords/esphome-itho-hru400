// ITHO HRU400 control for Home Assistant ESPHome - see itho_hru400.h for credits.
#include "itho_hru400.h"

#include <SPI.h>

#include <cmath>

namespace esphome {
namespace itho_hru400 {

static const char *const TAG = "itho_hru400";

volatile bool IthoBridge::rx_ready_ = false;
volatile uint32_t IthoBridge::isr_count_ = 0;

// fan_info values (byte 18 of a 31DA status message) that the HRU400 reports after accepting a command.
// Determined by logging a real Spider remote:  speed 1 -> 0x01, speed 2 -> 0x02, speed 3 -> 0x03,
// Auto -> 0x18, timer -> 0x0D (in that case the timer minutes are reported as well).
static constexpr uint8_t FAN_INFO_LOW = 0x01;
static constexpr uint8_t FAN_INFO_MEDIUM = 0x02;
static constexpr uint8_t FAN_INFO_HIGH = 0x03;
static constexpr uint8_t FAN_INFO_AUTO = 0x18;
static constexpr uint8_t FAN_INFO_TIMER = 0x0D;

// ISR: only raises a flag. All SPI traffic happens later in loop(), never inside the interrupt.
void IRAM_ATTR IthoBridge::on_gdo_edge_() {
  isr_count_++;
  rx_ready_ = true;
}

void IthoBridge::attach_rx_irq_() {
  // IOCFG0=0x06: GDO0 goes high when a sync word is detected. The receiver runs in "infinite packet length"
  // mode, so there is no end-of-packet edge to wait for; instead readData() starts draining the 64-byte FIFO
  // right after the rising edge and keeps reading while the message arrives.
  attachInterrupt(digitalPinToInterrupt(gdo0_pin_), on_gdo_edge_, RISING);
}
void IthoBridge::detach_rx_irq_() { detachInterrupt(digitalPinToInterrupt(gdo0_pin_)); }

void IthoBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "ITHO HRU400 control:");
  ESP_LOGCONFIG(TAG, "  Pins: CS=GPIO%u SCK=GPIO%u MISO=GPIO%u MOSI=GPIO%u GDO0=GPIO%u", cs_pin_, sck_pin_, miso_pin_,
                mosi_pin_, gdo0_pin_);
  ESP_LOGCONFIG(TAG, "  HRU400 ID: %06lX, our virtual remote ID: %06lX", static_cast<unsigned long>(hru_id_),
                static_cast<unsigned long>(remote_id_));
  ESP_LOGCONFIG(TAG, "  tx_repeats=%u tx_gap=%u ms command_attempts=%u status_poll=%lu ms", tx_repeats_, tx_gap_ms_,
                max_attempts_, static_cast<unsigned long>(poll_interval_ms_));
  ESP_LOGCONFIG(TAG, "  freq_offset=%d (%.1f kHz) debug_rx=%d discover_devices=%d", freq_offset_, freq_offset_ * 1.587f,
                debug_rx_, discover_devices_);
  for (const auto &d : devices_) {
    const char *t = d.type == DEVICE_CO2 ? "co2" : (d.type == DEVICE_HUMIDITY ? "humidity" : "temperature");
    ESP_LOGCONFIG(TAG, "  Device %06lX: %s", static_cast<unsigned long>(d.id), t);
  }
}

// ---------------------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------------------
void IthoBridge::setup() {
  // The CC1101 chip select is driven manually by the driver; SPI.begin() just sets up the bus.
  pinMode(cs_pin_, OUTPUT);
  digitalWrite(cs_pin_, HIGH);
  SPI.begin(sck_pin_, miso_pin_, mosi_pin_, cs_pin_);
  pinMode(gdo0_pin_, INPUT);

  radio_.setPins(cs_pin_, miso_pin_);
  radio_.setDefaultID((remote_id_ >> 16) & 0xFF, (remote_id_ >> 8) & 0xFF, remote_id_ & 0xFF);
  // Frame repetition is handled by this component (one frame per loop pass), so the driver sends one try only.
  radio_.setSendTries(1);
  radio_.setTxGap(tx_gap_ms_);
  radio_.setFreqOffset(freq_offset_);
  radio_.setDebugRx(debug_rx_);
  radio_.init();

  chip_version_ = radio_.getChipVersion();
  if (chip_version_ == 0x00 || chip_version_ == 0xFF) {
    ESP_LOGE(TAG, "CC1101 not found (VERSION=0x%02X): check wiring and power (3.3 V)", chip_version_);
  } else {
    ESP_LOGI(TAG, "CC1101 found, VERSION=0x%02X (0x14 = genuine, 0x04 = legacy, 0x17 = clone)", chip_version_);
  }

  // Register our virtual remote. The RFT-CO2 type is a bidirectional remote that the HRU400 accepts
  // speed/timer commands from. Pairing itself is NOT started here; use join() for that.
  radio_.setBindAllowed(true);
  if (!radio_.addRFDevice((remote_id_ >> 16) & 0xFF, (remote_id_ >> 8) & 0xFF, remote_id_ & 0xFF, RFTCO2, true)) {
    ESP_LOGE(TAG, "Unable to register the virtual remote");
    mark_failed();
    return;
  }
  radio_.setBindAllowed(false);  // do not let unknown remotes alter our device list
  radio_.updateDestinationID((hru_id_ >> 16) & 0xFF, (hru_id_ >> 8) & 0xFF, hru_id_ & 0xFF, 0);

  // The loop must run fast enough to drain the 64-byte RX FIFO (~13 ms at 38.4 kBaud) in time.
  high_freq_.start();
  rx_ready_ = false;
  attach_rx_irq_();
  last_status_ms_ = millis();
  next_poll_ms_ = millis() + 15000;
  ESP_LOGI(TAG, "Started. Virtual remote %06lX -> HRU400 %06lX", static_cast<unsigned long>(remote_id_),
           static_cast<unsigned long>(hru_id_));
}

// ---------------------------------------------------------------------------------------------------------
// Main loop. Order of priority: pairing handshake, received packets, frames to transmit, housekeeping.
// ---------------------------------------------------------------------------------------------------------
void IthoBridge::loop() {
  const uint32_t now = millis();

  // Finish the pairing handshake 300 ms after the HRU400's BindAccept (non-blocking guard interval).
  if (bind_confirm_pending_ && (int32_t)(now - bind_confirm_due_ms_) >= 0) {
    bind_confirm_pending_ = false;
    ESP_LOGI(TAG, "Pairing: sending BindConfirm to %06lX", static_cast<unsigned long>(bind_confirm_target_));
    detach_rx_irq_();
    radio_.setSendTries(tx_repeats_);  // rare event: send the usual number of repeats in one go
    radio_.sendBindConfirm(0);
    radio_.setSendTries(1);
    rx_ready_ = false;
    attach_rx_irq_();
    ESP_LOGI(TAG, "Pairing: BindConfirm sent (delivery cannot be confirmed)");
  }

  if (now - last_status_ms_ >= 30000) {
    last_status_ms_ = now;
    log_status_();
  }
  update_timer_countdown_();

  // A packet that is being received has priority: the FIFO must be drained quickly.
  if (rx_ready_) {
    handle_rx_();
    return;
  }

  // Send the next queued frame (one per loop pass, so no pass blocks for long).
  if (tx_job_.active) {
    if ((int32_t)(now - tx_job_.next_ms) >= 0) send_next_frame_();
    return;
  }

  // Idle: status requests and command verification.
  for (int i = 0; i < 2; i++) {
    if (rq_pending_[i] && (int32_t)(now - rq_due_ms_[i]) >= 0) {
      rq_pending_[i] = false;
      request_status_(i == 0 ? "after command, 1st" : "after command, 2nd");
      return;
    }
  }
  if (pend_.active && (int32_t)(now - pend_.deadline_ms) >= 0) {
    if (pend_.attempts < max_attempts_) {
      ESP_LOGW(TAG, "Command %d not confirmed after attempt %u/%u: sending again", pend_.code, pend_.attempts,
               max_attempts_);
      send_command_();
    } else {
      ESP_LOGW(TAG, "Command %d not confirmed after %u attempts: giving up", pend_.code, pend_.attempts);
      cmd_fail_++;
      pend_.active = false;
    }
    return;
  }
  if (poll_interval_ms_ > 0 && (int32_t)(now - next_poll_ms_) >= 0) {
    next_poll_ms_ = now + poll_interval_ms_;
    request_status_("periodic");
  }
}

// ---------------------------------------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------------------------------------
void IthoBridge::handle_rx_() {
  rx_ready_ = false;
  rx_count_++;
  last_rx_ms_ = millis();

  radio_.receivePacket();         // drains the CC1101 FIFO (blocks ~35 ms)
  radio_.decodeBufferedPacket();  // Manchester-like decoding of the raw bytes
  IthoPacket *packet = radio_.checkForNewPacket();
  const bool parsed = radio_.parseMessage(packet);  // header, IDs, opcode, checksum
  const int fe = radio_.getLastFreqEst();

  if (debug_rx_) {
    ESP_LOGD(TAG, "RX #%lu parsed=%d err=%u hdr=0x%02X opcode=0x%04X len=%u id0=%06X id1=%06X id2=%06X freqest=%d "
             "(%.1f kHz) rssi=%d dBm",
             static_cast<unsigned long>(rx_count_), parsed, packet->error, packet->header, packet->opcode,
             packet->len, static_cast<unsigned>(packet->deviceId0), static_cast<unsigned>(packet->deviceId1),
             static_cast<unsigned>(packet->deviceId2), fe, fe * 1.587f, radio_.getLastRssiDbm());
  }
  if (!parsed) {
    rx_parse_fail_++;  // mostly noise or a packet that started while we were busy
    return;
  }
  if (packet->error != 0) {
    rx_crc_err_++;  // checksum error: never trust the content (normal for ~10% of packets in a busy RF area)
    return;
  }
  rx_ok_++;

  // Frequency statistics: used by the STATUS line to suggest a freq_offset (see README, "Frequency tuning").
  fe_sum_all_ += fe;
  fe_n_all_++;
  if (packet->deviceId0 == hru_id_) {
    fe_sum_hru_ += fe;
    fe_n_hru_++;
  }

  const uint8_t *payload = &packet->dataDecoded[packet->payloadPos];
  if (debug_rx_) {
    char hb[128];
    itho_hex(payload, packet->len < 40 ? packet->len : 40, hb, sizeof(hb));
    ESP_LOGD(TAG, "RX OK opcode=0x%04X type=%s payload(%u): %s", packet->opcode,
             MsgType[(packet->header >> 4) & 0x03], packet->len, hb);
  }

  if (discover_devices_) discover_(packet);

  // Status broadcast by our HRU400 (31DA = full status, 31D9 = short status; header type "_I").
  const uint32_t source = packet->deviceId0 ? packet->deviceId0 : packet->deviceId2;
  if (source == hru_id_ && ((packet->header >> 4) & 0x03) == 1 &&
      (packet->opcode == 0x31DA || packet->opcode == 0x31D9)) {
    handle_hru_status_(packet);
  } else {
    handle_device_report_(packet);  // CO2 / humidity / Spider temperature
  }

  // Commands that other remotes (e.g. a Spider) send to our HRU400.
  if (packet->deviceId1 == hru_id_ &&
      (packet->opcode == 0x22F1 || packet->opcode == 0x22F3 || packet->opcode == 0x22F8)) {
    char id_text[12];
    snprintf(id_text, sizeof(id_text), "%02X.%02X.%02X", static_cast<unsigned>((packet->deviceId0 >> 16) & 0xFF),
             static_cast<unsigned>((packet->deviceId0 >> 8) & 0xFF), static_cast<unsigned>(packet->deviceId0 & 0xFF));
    const std::string cmd_text = radio_.LastMessageDecoded(packet).c_str();
    ESP_LOGI(TAG, "Remote command from %s: %s", id_text, cmd_text.c_str());
    if (last_source_ && last_source_str_ != id_text) {
      last_source_str_ = id_text;
      last_source_->publish_state(id_text);
    }
    if (last_command_ && cmd_text != last_cmd_str_) {
      last_cmd_str_ = cmd_text;
      last_command_->publish_state(cmd_text);
    }
  }

  // Pairing: the HRU400 answers our JOIN with a BindAccept; we must confirm it ~300 ms later.
  if (packet->command == IthoBindAccept && radio_.getRFDeviceBidirectional(0)) {
    const uint32_t target = packet->deviceId0 ? packet->deviceId0 : packet->deviceId2;
    if (target == 0) {
      ESP_LOGW(TAG, "BindAccept without sender ID: confirmation not scheduled");
      return;
    }
    ESP_LOGI(TAG, "Pairing: BindAccept received from %06lX", static_cast<unsigned long>(target));
    radio_.updateDestinationID((target >> 16) & 0xFF, (target >> 8) & 0xFF, target & 0xFF, 0);
    bind_confirm_target_ = target;
    bind_confirm_due_ms_ = millis() + 300;
    bind_confirm_pending_ = true;
  }
}

// Values reported by the sensors / Spiders that are listed under "devices:" in the YAML.
void IthoBridge::handle_device_report_(IthoPacket *packet) {
  const uint32_t source = packet->deviceId0 ? packet->deviceId0 : packet->deviceId2;
  const uint8_t *p = &packet->dataDecoded[packet->payloadPos];
  for (const auto &d : devices_) {
    if (d.id != source || d.sensor == nullptr) continue;
    if (d.type == DEVICE_CO2 && packet->opcode == 0x1298 && packet->len >= 3) {
      // payload: [0] unknown, [1..2] CO2 in ppm (big endian)
      const uint16_t ppm = (static_cast<uint16_t>(p[1]) << 8) | p[2];
      if (ppm > 0 && ppm <= 10000) {
        d.sensor->publish_state(ppm);
        ESP_LOGD(TAG, "CO2 %06lX: %u ppm", static_cast<unsigned long>(source), ppm);
      } else {
        ESP_LOGW(TAG, "Ignoring implausible CO2 value %u ppm from %06lX", ppm, static_cast<unsigned long>(source));
      }
    } else if (d.type == DEVICE_HUMIDITY && packet->opcode == 0x12A0 && packet->len >= 2) {
      // payload: [0] unknown, [1] humidity %RH, [2..3] temperature (x0.01 C), [4..5] dew point (x0.01 C)
      if (p[1] <= 100) {
        d.sensor->publish_state(p[1]);
        ESP_LOGD(TAG, "Humidity %06lX: %u %%", static_cast<unsigned long>(source), p[1]);
      } else {
        ESP_LOGW(TAG, "Ignoring implausible humidity %u from %06lX", p[1], static_cast<unsigned long>(source));
      }
    } else if (d.type == DEVICE_TEMPERATURE && packet->opcode == 0x30C9 && packet->len >= 3) {
      // payload: [0] zone number, [1..2] temperature in 0.01 C (signed, big endian)
      const float temp = static_cast<int16_t>((static_cast<uint16_t>(p[1]) << 8) | p[2]) / 100.0f;
      if (temp > -40.0f && temp < 80.0f) {
        d.sensor->publish_state(temp);
        ESP_LOGD(TAG, "Temperature %06lX zone %u: %.2f C", static_cast<unsigned long>(source), p[0], temp);
      } else {
        ESP_LOGW(TAG, "Ignoring implausible temperature %.2f from %06lX", temp, static_cast<unsigned long>(source));
      }
    }
  }
}

// Status message of the HRU400. Layout of 31DA (29 payload bytes), verified against a real Spider remote:
//   [5]      indoor humidity in %
//   [18]     fan_info (see FAN_INFO_* above)
//   [19]     exhaust fan speed, [20] supply fan speed, in 0.5 % units (0xC8 = 100 %)
//   [21..22] timer minutes (big endian)
// 31D9 (3 payload bytes): [2] fan speed in 0.5 % units.
void IthoBridge::handle_hru_status_(IthoPacket *packet) {
  const uint8_t *p = &packet->dataDecoded[packet->payloadPos];
  float speed = -1;

  if (packet->opcode == 0x31DA && packet->len >= 29) {
    speed = p[19] / 2.0f;
    const int hum = p[5];
    const int info = p[18];
    const int rem = (p[21] << 8) | p[22];

    // Log only when something relevant changed (the HRU400 sends dozens of identical copies per burst).
    if (debug_rx_ || info != last_info_ || rem != last_rem_ || fabsf(speed - last_logged_speed_) >= 3.0f) {
      last_info_ = info;
      last_rem_ = rem;
      last_logged_speed_ = speed;
      ESP_LOGI(TAG, "HRU400 status: speed=%.1f%% fan_info=0x%02X humidity=%d%% timer=%d min", speed, info, hum, rem);
    }
    if (humidity_ && hum <= 100 && hum != last_hum_) {
      last_hum_ = hum;
      humidity_->publish_state(hum);
    }

    // Timer: the HRU400 reports the programmed minutes but does not count down, so we do that ourselves.
    const bool timer_active = (info == FAN_INFO_TIMER && rem > 0);
    if (timer_active) {
      if (rem != hru_timer_raw_ || timer_end_ms_ == 0) start_timer_countdown_(rem);
      hru_timer_raw_ = rem;
    } else {
      hru_timer_raw_ = -1;
      stop_timer_countdown_();
    }
    if (timer_) {
      char t[24];
      if (timer_active) snprintf(t, sizeof(t), "%d min", rem);
      else snprintf(t, sizeof(t), "none");
      if (last_timer_text_ != t) {
        last_timer_text_ = t;
        timer_->publish_state(t);
      }
    }

    // The status is the HRU400's answer to a command: has our command been accepted?
    if (pend_.active && info == pend_.expect_info && (pend_.expect_rem < 0 || rem == pend_.expect_rem)) {
      pend_.active = false;
      rq_pending_[0] = rq_pending_[1] = false;
      cmd_ok_++;
      if (pend_.attempts == 1) cmd_ok_first_++;
      if (pend_.expect_rem > 0) start_timer_countdown_(pend_.expect_rem);  // restart, even if the value is unchanged
      ESP_LOGI(TAG, "Command %d confirmed after %u attempt(s), %lu ms after the last frame", pend_.code,
               pend_.attempts, static_cast<unsigned long>(millis() - pend_.sent_ms));
    }
  } else if (packet->opcode == 0x31D9 && packet->len >= 3) {
    speed = p[2] / 2.0f;
  }

  if (fan_speed_ && speed >= 0 && speed <= 100 && speed != last_speed_) {
    last_speed_ = speed;
    fan_speed_->publish_state(speed);
  }
}

// Discovery: log every RF sender once, so users can find the IDs of their own sensors and Spiders.
void IthoBridge::discover_(IthoPacket *packet) {
  const uint32_t source = packet->deviceId0 ? packet->deviceId0 : packet->deviceId2;
  if (source == 0) return;

  if (!has_seen_(source)) {
    const char *what = "unknown";
    switch (packet->opcode) {
      case 0x1298: what = "CO2 sensor (RFT-CO2)"; break;
      case 0x12A0: what = "humidity sensor (RFT-RV)"; break;
      case 0x30C9: what = "Spider / zone temperature"; break;
      case 0x22C9: what = "Spider / zone set point"; break;
      case 0x22F1: case 0x22F3: case 0x22F8: what = "remote control (speed/timer command)"; break;
      case 0x31DA: case 0x31D9: what = "ventilation unit (status)"; break;
      case 0x2E10: what = "presence (PIR) sensor"; break;
      case 0x1060: what = "battery status"; break;
      case 0x1FC9: what = "pairing message"; break;
      default: break;
    }
    bool configured = (source == hru_id_);
    for (const auto &d : devices_) configured = configured || (d.id == source);
    ESP_LOGI(TAG, "Discovery: RF device %06lX sends opcode 0x%04X = %s%s", static_cast<unsigned long>(source),
             packet->opcode, what, configured ? " [configured]" : "");
  }

  // The HRU400 sends its status to every device that is paired with it: that list shows YOUR sensors and Spiders.
  if (source == hru_id_ && (packet->opcode == 0x31DA || packet->opcode == 0x31D9) && packet->deviceId1 != 0 &&
      !has_seen_(0x80000000u | packet->deviceId1)) {
    ESP_LOGI(TAG, "Discovery: your HRU400 %06lX reports status to paired device %06lX%s",
             static_cast<unsigned long>(hru_id_), static_cast<unsigned long>(packet->deviceId1),
             packet->deviceId1 == remote_id_ ? " (this ESP)" : "");
  }
}

bool IthoBridge::has_seen_(uint32_t key) {
  for (uint8_t i = 0; i < seen_count_; i++)
    if (seen_[i] == key) return true;
  if (seen_count_ < MAX_SEEN) {
    seen_[seen_count_++] = key;
    return false;
  }
  if (!seen_table_full_logged_) {
    seen_table_full_logged_ = true;
    ESP_LOGW(TAG, "Discovery table full (%u entries): no further new devices will be logged", MAX_SEEN);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Timer countdown
// ---------------------------------------------------------------------------------------------------------
static void publish_remaining(sensor::Sensor *s, int *last, int minutes) {
  if (s != nullptr && minutes != *last) {
    *last = minutes;
    s->publish_state(minutes);
  }
}

void IthoBridge::start_timer_countdown_(int minutes) {
  timer_end_ms_ = millis() + static_cast<uint32_t>(minutes) * 60000UL;
  last_timer_tick_ms_ = millis();
  publish_remaining(timer_remaining_, &last_timer_published_, minutes);
}

void IthoBridge::stop_timer_countdown_() {
  timer_end_ms_ = 0;
  publish_remaining(timer_remaining_, &last_timer_published_, 0);
}

void IthoBridge::update_timer_countdown_() {
  if (timer_end_ms_ == 0 || millis() - last_timer_tick_ms_ < 1000) return;
  last_timer_tick_ms_ = millis();
  const int32_t left_ms = static_cast<int32_t>(timer_end_ms_ - millis());
  if (left_ms <= 0) {
    stop_timer_countdown_();
    return;
  }
  publish_remaining(timer_remaining_, &last_timer_published_, (left_ms + 59999) / 60000);  // round up
}

// ---------------------------------------------------------------------------------------------------------
// Transmit path
// ---------------------------------------------------------------------------------------------------------
void IthoBridge::command(int command_code) {
  if (command_code < IthoAway || command_code > IthoAutoNight) {
    ESP_LOGW(TAG, "Command %d rejected: not a speed/timer command", command_code);
    return;
  }
  // Not every command exists for the RFT-CO2 remote type (e.g. "Away" has no message): never fail silently.
  if (radio_.getRemoteCmd(RFTCO2, static_cast<IthoCommand>(command_code)) == nullptr) {
    ESP_LOGW(TAG, "Command %d NOT sent: no message defined for this remote type", command_code);
    return;
  }
  if (tx_job_.active) {
    ESP_LOGW(TAG, "Command %d ignored: still transmitting the previous command", command_code);
    return;
  }

  // What the HRU400 reports once it accepted the command (determined with a real Spider).
  int info = -1, rem = -1;
  switch (command_code) {
    case IthoLow: info = FAN_INFO_LOW; break;
    case IthoMedium: info = FAN_INFO_MEDIUM; break;
    case IthoHigh: info = FAN_INFO_HIGH; break;
    case IthoAuto: info = FAN_INFO_AUTO; break;
    case IthoTimer1: info = FAN_INFO_TIMER; rem = 10; break;
    case IthoTimer2: info = FAN_INFO_TIMER; rem = 20; break;
    case IthoTimer3: info = FAN_INFO_TIMER; rem = 30; break;
    default: break;
  }
  pend_.active = (info >= 0 && max_attempts_ > 1);
  pend_.code = command_code;
  pend_.expect_info = info;
  pend_.expect_rem = rem;
  pend_.attempts = 0;
  if (!pend_.active) ESP_LOGI(TAG, "Command %d: no confirmation rule, sending once", command_code);
  send_command_();
}

void IthoBridge::send_command_() {
  pend_.attempts++;
  cmd_attempts_total_++;
  ESP_LOGI(TAG, "TX command %d (attempt %u/%u)", pend_.code, pend_.attempts, pend_.active ? max_attempts_ : 1);
  start_tx_job_(static_cast<IthoCommand>(pend_.code), true);
}

void IthoBridge::join() {
  // Only use while the HRU400 is in pairing mode.
  ESP_LOGI(TAG, "Pairing: sending JOIN as %06lX", static_cast<unsigned long>(remote_id_));
  bind_confirm_pending_ = false;
  radio_.setBindInitiatorActive(true, 0);
  start_tx_job_(IthoJoin, false);
}

void IthoBridge::leave() {
  ESP_LOGI(TAG, "Pairing: sending LEAVE as %06lX (this removes the virtual remote from the HRU400)",
           static_cast<unsigned long>(remote_id_));
  bind_confirm_pending_ = false;
  start_tx_job_(IthoLeave, false);
}

void IthoBridge::start_tx_job_(IthoCommand cmd, bool is_remote_command) {
  tx_job_.active = true;
  tx_job_.cmd = cmd;
  tx_job_.frames_sent = 0;
  tx_job_.next_ms = millis();
  tx_job_.is_remote_command = is_remote_command;
}

// Sends ONE frame (~25 ms). The remote repeats each command a few times to survive collisions.
void IthoBridge::send_next_frame_() {
  detach_rx_irq_();  // GDO0 toggles while transmitting; that must not look like a received packet
  radio_.sendRFCommand(0, tx_job_.cmd);
  rx_ready_ = false;
  attach_rx_irq_();
  tx_count_++;
  tx_job_.frames_sent++;

  const uint8_t end_state = radio_.getLastTxEndState();
  ESP_LOGD(TAG, "TX frame %u/%u: end_state=0x%02X streamed=%d underflows=%lu wait_timeouts=%lu miso_timeouts=%lu",
           tx_job_.frames_sent, tx_repeats_, end_state, radio_.getLastTxStreamed(),
           static_cast<unsigned long>(radio_.getTxUnderflows()), static_cast<unsigned long>(radio_.getTxWaitTimeouts()),
           static_cast<unsigned long>(radio_.getMisoTimeouts()));
  if (end_state != 0x01) ESP_LOGW(TAG, "Transmission did not end cleanly (end state 0x%02X, expected 0x01)", end_state);

  if (tx_job_.frames_sent >= tx_repeats_) {
    tx_job_.active = false;
    on_tx_job_done_();
  } else {
    tx_job_.next_ms = millis() + tx_gap_ms_;
  }
}

void IthoBridge::on_tx_job_done_() {
  if (!tx_job_.is_remote_command || !pend_.active) return;
  // Ask the HRU400 for its status to see whether the command was accepted, then judge the attempt.
  pend_.sent_ms = millis();
  rq_due_ms_[0] = pend_.sent_ms + 450;
  rq_due_ms_[1] = pend_.sent_ms + 1000;
  rq_pending_[0] = rq_pending_[1] = true;
  pend_.deadline_ms = pend_.sent_ms + 1700;
}

// Status request (RQ 31DA), the same thing a Spider sends; the HRU400 answers with a 31DA message.
void IthoBridge::request_status_(const char *why) {
  ESP_LOGD(TAG, "TX status request (%s)", why);
  detach_rx_irq_();
  radio_.sendRQ31DA(0);
  rx_ready_ = false;
  attach_rx_irq_();
  tx_count_++;
}

// One-line health summary every 30 s: radio state, packet counters, command statistics, frequency offset.
void IthoBridge::log_status_() {
  char age[24];
  if (last_rx_ms_) snprintf(age, sizeof(age), "%lus ago", static_cast<unsigned long>((millis() - last_rx_ms_) / 1000));
  else snprintf(age, sizeof(age), "never");
  const float avg_hru = fe_n_hru_ ? static_cast<float>(fe_sum_hru_) / fe_n_hru_ : 0.0f;
  const float avg_all = fe_n_all_ ? static_cast<float>(fe_sum_all_) / fe_n_all_ : 0.0f;
  ESP_LOGI(TAG,
           "STATUS chip=0x%02X marc=0x%02X gdo0=%d isr=%lu rx=%lu ok=%lu crc_err=%lu parse_fail=%lu tx=%lu "
           "miso_to=%lu rxstate_to=%lu last_rx=%s | commands ok=%lu (1st attempt=%lu) failed=%lu attempts=%lu | "
           "freqest avg all=%.1f (n=%lu) HRU=%.1f (n=%lu) => suggested freq_offset=%+d",
           chip_version_, radio_.getMarcState(), digitalRead(gdo0_pin_), static_cast<unsigned long>(isr_count_),
           static_cast<unsigned long>(rx_count_), static_cast<unsigned long>(rx_ok_),
           static_cast<unsigned long>(rx_crc_err_), static_cast<unsigned long>(rx_parse_fail_),
           static_cast<unsigned long>(tx_count_), static_cast<unsigned long>(radio_.getMisoTimeouts()),
           static_cast<unsigned long>(radio_.getRxStateTimeouts()), age, static_cast<unsigned long>(cmd_ok_),
           static_cast<unsigned long>(cmd_ok_first_), static_cast<unsigned long>(cmd_fail_),
           static_cast<unsigned long>(cmd_attempts_total_), avg_all, static_cast<unsigned long>(fe_n_all_), avg_hru,
           static_cast<unsigned long>(fe_n_hru_),
           fe_n_hru_ ? static_cast<int>(lroundf(avg_hru)) + freq_offset_ : freq_offset_);
}

}  // namespace itho_hru400
}  // namespace esphome
