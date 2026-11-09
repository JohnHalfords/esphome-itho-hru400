// ITHO HRU400 control for Home Assistant ESPHome
//
// ESPHome component that lets an ESP32 + CC1101 868 MHz radio talk to an Itho Daalderop HRU400
// (and compatible) heat-recovery ventilation unit, acting as a virtual RF remote control.
//
// Credits:
//   * Radio driver (CC1101 + Itho protocol): Klusjesman, supersjimmie and Arjen Hiemstra
//     (https://github.com/arjenhiemstra/ithowifi) - original copyright and license remain applicable.
//   * ESPHome Itho implementation that served as starting point: Ronald Hiemstra.
//   * ESPHome port: John Halfords, with debugging, protocol analysis and rework by Claude (Anthropic).
#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

#include "IthoCC1101.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace itho_hru400 {

// What a configured RF device (identified by its 24-bit ID) reports.
enum DeviceType : uint8_t {
  DEVICE_CO2 = 0,          // RFT-CO2 sensor, opcode 0x1298, value in ppm
  DEVICE_HUMIDITY = 1,     // RFT-RV humidity sensor, opcode 0x12A0, value in %RH
  DEVICE_TEMPERATURE = 2,  // Spider / zone controller, opcode 0x30C9, value in degrees Celsius
};

struct ConfiguredDevice {
  uint32_t id;
  DeviceType type;
  sensor::Sensor *sensor;
};

class IthoBridge : public Component {
 public:
  // --- ESPHome lifecycle -------------------------------------------------------------------------------
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // --- Actions, called from YAML button lambdas ---------------------------------------------------------
  // Send a command (see IthoCommand in IthoPacket.h). The frames are sent from loop(), so this returns at once.
  void command(int command_code);
  // Pairing: only call while the HRU400 is in pairing mode. Leave removes our virtual remote from the unit.
  void join();
  void leave();

  // --- Configuration, set from __init__.py (all options are documented in README.md) ----------------------
  void set_pins(uint8_t cs, uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t gdo0) {
    cs_pin_ = cs; sck_pin_ = sck; miso_pin_ = miso; mosi_pin_ = mosi; gdo0_pin_ = gdo0;
  }
  void set_hru_id(uint32_t id) { hru_id_ = id; }
  void set_remote_id(uint32_t id) { remote_id_ = id; }
  void set_tx_repeats(uint8_t n) { tx_repeats_ = n; }
  void set_tx_gap(uint8_t ms) { tx_gap_ms_ = ms; }
  void set_status_poll_ms(uint32_t ms) { poll_interval_ms_ = ms; }
  void set_freq_offset(int8_t o) { freq_offset_ = o; }
  void set_command_attempts(uint8_t n) { max_attempts_ = n; }
  void set_debug_rx(bool b) { debug_rx_ = b; }
  void set_discover_devices(bool b) { discover_devices_ = b; }

  // --- Entities, set from __init__.py -------------------------------------------------------------------
  void set_fan_speed_sensor(sensor::Sensor *s) { fan_speed_ = s; }
  void set_humidity_sensor(sensor::Sensor *s) { humidity_ = s; }
  void set_timer_remaining_sensor(sensor::Sensor *s) { timer_remaining_ = s; }
  void set_timer_text_sensor(text_sensor::TextSensor *s) { timer_ = s; }
  void set_last_command_text_sensor(text_sensor::TextSensor *s) { last_command_ = s; }
  void set_last_control_source_text_sensor(text_sensor::TextSensor *s) { last_source_ = s; }
  void add_device(uint32_t id, uint8_t type, sensor::Sensor *s) {
    devices_.push_back({id, static_cast<DeviceType>(type), s});
  }

 protected:
  // --- Receive ----------------------------------------------------------------------------------------
  void handle_rx_();

  // --- Interrupt (GDO0 goes high when the CC1101 detects a sync word, i.e. a packet starts) ---------------
  static void IRAM_ATTR on_gdo_edge_();
  static volatile bool rx_ready_;
  static volatile uint32_t isr_count_;
  void attach_rx_irq_();
  void detach_rx_irq_();

  // --- Transmit: frames are sent one per loop() pass so no single pass blocks for long ------------------
  struct TxJob {
    bool active{false};
    IthoCommand cmd{IthoUnknown};
    uint8_t frames_sent{0};
    uint32_t next_ms{0};
    bool is_remote_command{false};  // true for speed/timer commands that are verified via the HRU400 status
  };
  void start_tx_job_(IthoCommand cmd, bool is_remote_command);
  void send_next_frame_();
  void on_tx_job_done_();

  // --- Commands that are verified (and repeated until the HRU400 confirms them) ---------------------------
  void send_command_();
  struct PendingCommand {
    bool active{false};
    int code{0};
    int expect_info{-1};  // fan_info value the HRU400 will report once it has accepted the command
    int expect_rem{-1};   // expected timer minutes (timer commands only), or -1
    uint8_t attempts{0};
    uint32_t deadline_ms{0};
    uint32_t sent_ms{0};
  } pend_;

  // --- Status from the HRU400 -------------------------------------------------------------------------
  void request_status_(const char *why);
  void handle_hru_status_(IthoPacket *packet);
  void handle_device_report_(IthoPacket *packet);
  void discover_(IthoPacket *packet);
  void start_timer_countdown_(int minutes);
  void stop_timer_countdown_();
  void update_timer_countdown_();
  void log_status_();

  // --- Helpers ------------------------------------------------------------------------------------------
  IthoCC1101 radio_;
  HighFrequencyLoopRequester high_freq_;

  // configuration
  uint8_t cs_pin_{22}, sck_pin_{18}, miso_pin_{19}, mosi_pin_{23}, gdo0_pin_{16};
  uint32_t hru_id_{0};
  uint32_t remote_id_{0x0A3AC4};
  uint8_t tx_repeats_{3};
  uint8_t tx_gap_ms_{50};
  uint32_t poll_interval_ms_{60000};
  int8_t freq_offset_{0};
  uint8_t max_attempts_{5};
  bool debug_rx_{false};
  bool discover_devices_{true};

  // transmit state
  TxJob tx_job_;
  uint32_t rq_due_ms_[2]{0, 0};
  bool rq_pending_[2]{false, false};
  uint32_t next_poll_ms_{0};

  // pairing (bind) state
  bool bind_confirm_pending_{false};
  uint32_t bind_confirm_due_ms_{0};
  uint32_t bind_confirm_target_{0};

  // statistics, shown in the periodic STATUS log line
  uint32_t last_status_ms_{0};
  uint32_t last_rx_ms_{0};
  uint32_t rx_count_{0}, rx_ok_{0}, rx_crc_err_{0}, rx_parse_fail_{0}, tx_count_{0};
  uint32_t cmd_ok_{0}, cmd_ok_first_{0}, cmd_fail_{0}, cmd_attempts_total_{0};
  int32_t fe_sum_all_{0}, fe_sum_hru_{0};
  uint32_t fe_n_all_{0}, fe_n_hru_{0};
  uint8_t chip_version_{0};

  // timer countdown (the HRU400 reports the programmed timer value but does not count it down)
  uint32_t timer_end_ms_{0};
  int hru_timer_raw_{-1};
  int last_timer_published_{-1};
  uint32_t last_timer_tick_ms_{0};

  // de-duplication of published/logged values
  float last_speed_{-1};
  float last_logged_speed_{-1};
  int last_hum_{-1};
  int last_info_{-1};
  int last_rem_{-1};
  std::string last_source_str_, last_cmd_str_, last_timer_text_;

  // discovery: every sender ID is logged once
  static constexpr uint8_t MAX_SEEN = 64;
  uint32_t seen_[MAX_SEEN]{};
  uint8_t seen_count_{0};
  bool seen_table_full_logged_{false};
  bool has_seen_(uint32_t key);

  // entities
  std::vector<ConfiguredDevice> devices_;
  sensor::Sensor *fan_speed_{nullptr};
  sensor::Sensor *humidity_{nullptr};
  sensor::Sensor *timer_remaining_{nullptr};
  text_sensor::TextSensor *timer_{nullptr};
  text_sensor::TextSensor *last_command_{nullptr};
  text_sensor::TextSensor *last_source_{nullptr};
};

}  // namespace itho_hru400
}  // namespace esphome
