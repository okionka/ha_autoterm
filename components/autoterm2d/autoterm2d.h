#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Autoterm2DClimate  (UART2 – physical heater side)
//
//  Bridge mode  (ControllerPanelComponent present, ctrl_uart_ set):
//    • Forwards every heater byte to controller panel (UART1 TX)
//    • Physical panel drives the poll cycle (0x0F / 0x11 / 0x02)
//    • HA commands injected on UART2 TX alongside panel commands
//
//  Virtual-panel mode  (no ControllerPanelComponent, ctrl_uart_ == nullptr):
//    • ESP32 drives the poll cycle itself every 2 s
//    • Heater logging works identically
//    • HA commands injected normally
//
//  In both modes:
//    • All heater responses are parsed and published to HA
//    • Climate entity (mode, fan, preset, target/current temperature)
//    • Diagnostic sensors + status report button
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include "esphome.h"
#include "esphome/core/version.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
// ── Diagnose-Proxy (TCP-Server für Autoterm Test Software) ───────────────────
#include "diag_proxy.h"

namespace esphome {
namespace autoterm2d {

static const char *PRESET_T_HEATER = "By T Heater";
static const char *PRESET_T_PANEL  = "By T Panel";
static const char *PRESET_T_AIR    = "By T Air";
static const char *PRESET_POWER    = "By Power";

static constexpr uint8_t  CMD_START          = 0x01;
static constexpr uint8_t  CMD_SETTINGS       = 0x02;
static constexpr uint8_t  CMD_STOP           = 0x03;
static constexpr uint8_t  CMD_VERSION        = 0x06;
static constexpr uint8_t  CMD_STATUS         = 0x0F;
static constexpr uint8_t  CMD_PANEL_TEMP     = 0x11;

static constexpr int      MAX_PAYLOAD_LEN    = 64;
static constexpr uint32_t PARSER_TIMEOUT_MS  = 500;
static constexpr float    AIR_TEMP_MAX_JUMP  = 2.0f;

// Virtual panel poll cycle (ms between each request step)
static constexpr uint32_t VPANEL_POLL_MS     = 2000;


class Autoterm2DClimate : public climate::Climate,
                            public Component,
                            public uart::UARTDevice,
                            public DiagProxyMixin {
 public:
  Autoterm2DClimate() = default;

  // ── Optional: UART1 ref for bridge-mode forwarding ─────────────────────────
  // When set  → bridge mode  (ControllerPanelComponent present)
  // When null → virtual-panel mode
  void set_controller_uart(uart::UARTComponent *u) { ctrl_uart_ = u; }

  // ── Sensor wiring ──────────────────────────────────────────────────────────
  void set_room_temperature_sensor(sensor::Sensor *s) {
    s->add_on_state_callback([this](float v) {
      this->current_temperature = v;
      this->publish_state();
    });
  }
  void set_air_temp_source_sensor(sensor::Sensor *s) {
    s_air_temp_source_ = s;
    s->add_on_state_callback([this](float t) { on_air_temp_update(t); });
  }
  void set_heater_board_temperature_sensor(sensor::Sensor *s) { s_intake_temp_    = s; }
  void set_battery_voltage_sensor(sensor::Sensor *s)          { s_battery_voltage_= s; }
  void set_air_temperature_sensor(sensor::Sensor *s)          { s_air_temp_       = s; }
  void set_panel_temperature_sensor(sensor::Sensor *s)        { s_panel_temp_     = s; }
  void set_power_level_sensor(sensor::Sensor *s)              { s_power_level_    = s; }
  void set_ventilation_power_sensor(sensor::Sensor *s)        { s_fan_actual_     = s; }
  void set_status_sensor(sensor::Sensor *s)                   { s_status_code_    = s; }
  void set_error_code_sensor(sensor::Sensor *s)               { s_error_code_     = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s)     { s_status_text_    = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s)      { s_error_text_     = s; }
  void set_software_version_sensor(text_sensor::TextSensor *s){ s_sw_version_     = s; }
  void set_status_report_sensor(text_sensor::TextSensor *s)   { s_status_report_  = s; }
  void set_remaining_time_sensor(sensor::Sensor *s)           { s_remaining_time_ = s; }

  // ── Timer control (called from YAML number set_action) ───────────────────
  // minutes == 0 → unlimited (sends flag 0xFF)
  // minutes  > 0 → timed run (sends flag 0x00 + minutes)
  void set_work_time(uint8_t minutes) {
    work_time_minutes_ = minutes;
    ESP_LOGD("autoterm2d", "Timer set: %s",
             minutes == 0 ? "unlimited" : (std::to_string(minutes) + " min").c_str());
  }

  // ── Diagnostic button: format last-known status into HA text sensor ────────
  void publish_status_report() {
    if (!s_status_report_) return;
    const char *mode_str = ctrl_uart_ ? "Bridge (physical panel)" : "Virtual panel";
    char buf[300];
    snprintf(buf, sizeof(buf),
      "Mode: %s\n"
      "State: %s | Err: %s\n"
      "Regulation: %s | Level: %d/10 | Vent: %s\n"
      "T-intake: %d°C | T-out: %s | Flame: %.0f°C\n"
      "Battery: %.1fV | Fan: %d/%d Hz (%d/%d RPM) | Pump: %.2f Hz",
      mode_str,
      state_description(snap_major_, snap_minor_),
      error_description(snap_error_),
      mode_description(snap_mode_), snap_level_ + 1,
      snap_vent_ == 1 ? "On" : "Off",
      snap_t1_,
      snap_t2_ok_ ? (std::to_string(snap_t2_) + "°C").c_str() : "n/a",
      snap_flame_c_,
      snap_volts_,
      snap_fan_sp_, snap_fan_act_,
      snap_fan_sp_ * 60, snap_fan_act_ * 60,
      snap_pump_hz_);
    s_status_report_->publish_state(buf);
    ESP_LOGI("autoterm2d", "Status report:\n%s", buf);
  }

  // ── ESPHome Component ──────────────────────────────────────────────────────
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    reset_parser();
    this->set_supported_custom_presets({
        PRESET_T_HEATER, PRESET_T_PANEL, PRESET_T_AIR, PRESET_POWER});
    this->mode               = climate::CLIMATE_MODE_OFF;
    this->target_temperature = 15.0f;
    this->fan_mode           = climate::CLIMATE_FAN_ON;
    this->set_custom_preset_(PRESET_T_HEATER);
    this->publish_state();

    if (ctrl_uart_) {
      ESP_LOGI("autoterm2d", "Starting in BRIDGE mode (physical panel on UART1)");
    } else {
      ESP_LOGI("autoterm2d", "Starting in VIRTUAL-PANEL mode (ESP32 drives poll cycle)");
    }
    diag_setup_();  // TCP-Diagnoseserver starten (Port 8888)
  }

  void loop() override {
    const uint32_t now = millis();

    // ── Diagnose-Proxy: Client verwalten + TCP→UART senden ──────────────────
    diag_loop_tick_();
    diag_drain_tx_([this](uint8_t b) { write_byte(b); });
    // ────────────────────────────────────────────────────────────────────────

    // Parser timeout
    if (read_state_ != 0 && (now - parse_start_ms_) > PARSER_TIMEOUT_MS) {
      ESP_LOGW("autoterm2d", "Parser timeout – reset");
      reset_parser();
    }

    // Virtual panel: Poll-Zyklus pausiert wenn Diagnose-Client verbunden
    if (ctrl_uart_ == nullptr && !is_diagnostic_active())
      virtual_panel_poll(now);

    // Read heater bytes, forward, log, parse
    bool got_byte = false;
    while (available()) {
      uint8_t b;
      read_byte(&b);
      if (ctrl_uart_) ctrl_uart_->write_byte(b);   // bridge: forward to panel
      diag_forward_rx_(b);                          // Diagnose-Client mitlauschen
      heater_frame_buf_.push_back(b);
      process_byte(b);
      got_byte = true;
    }
    if (got_byte) last_heater_rx_ms_ = now;

    if (!heater_frame_buf_.empty() && (now - last_heater_rx_ms_) > 50) {
      log_heater_frame();
      heater_frame_buf_.clear();
    }
  }

  // ── Climate traits ─────────────────────────────────────────────────────────
  climate::ClimateTraits traits() override {
    auto t = climate::ClimateTraits();
    // ESPHome 2026.5.0 deprecated set_supports_current_temperature() and
    // removed it in 2026.7.0 – replaced by the ClimateFeature bit flags.
#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 5, 0)
    t.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
#else
    t.set_supports_current_temperature(true);
#endif
    t.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
    t.set_supported_fan_modes({climate::CLIMATE_FAN_ON, climate::CLIMATE_FAN_OFF});
    t.set_visual_min_temperature(0.0f);
    t.set_visual_max_temperature(30.0f);
    t.set_visual_temperature_step(1.0f);
    return t;
  }

  // ── Climate control ────────────────────────────────────────────────────────
  void control(const climate::ClimateCall &call) override {
    bool needs_send = false;
    if (call.get_mode().has_value()) {
      auto m = *call.get_mode();
      if (m == climate::CLIMATE_MODE_OFF) {
        send_shutdown_command();
        this->mode = climate::CLIMATE_MODE_OFF;
      } else {
        this->mode = climate::CLIMATE_MODE_HEAT;
        needs_send = true;
      }
    }
    if (call.get_target_temperature().has_value()) {
      temp_set_ = static_cast<uint8_t>(*call.get_target_temperature());
      this->target_temperature = temp_set_;
      needs_send = true;
    }
    if (call.get_fan_mode().has_value()) {
      auto fm = *call.get_fan_mode();
      vent_cmd_ = (fm == climate::CLIMATE_FAN_ON) ? 1 : 2;
      this->fan_mode = fm;
      needs_send = true;
    }
    auto preset_ref = call.get_custom_preset();
    if (!preset_ref.empty()) {
      power_mode_ = preset_to_power_mode(preset_ref.c_str());
      this->set_custom_preset_(preset_ref.c_str());
      last_sent_air_temp_ = NAN;
      needs_send = true;
    }
    if (needs_send && this->mode == climate::CLIMATE_MODE_HEAT)
      send_control_command();
    this->publish_state();
  }

 private:
  // ── UART / mode ────────────────────────────────────────────────────────────
  uart::UARTComponent *ctrl_uart_{nullptr};       // null = virtual-panel mode
  sensor::Sensor      *s_air_temp_source_{nullptr};

  // ── Sensors ────────────────────────────────────────────────────────────────
  sensor::Sensor         *s_intake_temp_{nullptr};
  sensor::Sensor         *s_battery_voltage_{nullptr};
  sensor::Sensor         *s_air_temp_{nullptr};
  sensor::Sensor         *s_panel_temp_{nullptr};
  sensor::Sensor         *s_power_level_{nullptr};
  sensor::Sensor         *s_fan_actual_{nullptr};
  sensor::Sensor         *s_status_code_{nullptr};
  sensor::Sensor         *s_error_code_{nullptr};
  text_sensor::TextSensor *s_status_text_{nullptr};
  text_sensor::TextSensor *s_error_text_{nullptr};
  text_sensor::TextSensor *s_sw_version_{nullptr};
  text_sensor::TextSensor *s_status_report_{nullptr};
  sensor::Sensor         *s_remaining_time_{nullptr};

  // ── Control state ──────────────────────────────────────────────────────────
  uint8_t temp_set_    {15};
  uint8_t power_mode_  {1};
  uint8_t vent_cmd_    {1};
  uint8_t power_level_ {4};
  uint8_t major_state_ {0};
  float   last_sent_air_temp_{NAN};
  bool    version_requested_{false};
  uint8_t work_time_minutes_{0};   // 0 = unlimited (sends 0xFF)

  // ── Virtual panel state ────────────────────────────────────────────────────
  uint32_t last_vpanel_poll_ms_{0};
  uint8_t  vpanel_step_{0};         // 0=0x0F  1=0x11  2=0x02

  // ── Status snapshot ────────────────────────────────────────────────────────
  uint8_t snap_major_{0}, snap_minor_{0}, snap_error_{0};
  int     snap_t1_{0};
  bool    snap_t2_ok_{false};
  int     snap_t2_{0};
  float   snap_volts_{0}, snap_flame_c_{0}, snap_pump_hz_{0};
  uint8_t snap_fan_sp_{0}, snap_fan_act_{0};
  uint8_t snap_level_{0}, snap_mode_{0}, snap_vent_{0};

  // ── Parser ─────────────────────────────────────────────────────────────────
  uint8_t  read_state_{0};
  int      message_length_{0};
  uint8_t  command_id_{0};
  std::vector<uint8_t> message_data_;
  uint32_t parse_start_ms_{0};

  // ── Heater frame log buffer ─────────────────────────────────────────────────
  std::vector<uint8_t> heater_frame_buf_;
  uint32_t             last_heater_rx_ms_{0};

  // ── Virtual panel poll cycle ───────────────────────────────────────────────
  void virtual_panel_poll(uint32_t now) {
    if (now - last_vpanel_poll_ms_ < VPANEL_POLL_MS) return;
    last_vpanel_poll_ms_ = now;

    switch (vpanel_step_) {
      case 0:   // Status request (0x0F)
        send_poll_request(CMD_STATUS);
        ESP_LOGD("autoterm2d", "VPANEL 0x0F status request");
        break;

      case 1: {  // Panel temperature (0x11)
        // Prefer current_temperature (room sensor); fall back to target
        float temp_f = std::isfinite(this->current_temperature)
                         ? this->current_temperature
                         : this->target_temperature;
        int8_t temp_byte = static_cast<int8_t>(std::round(temp_f));
        send_temp_0x11(temp_byte, " (virtual panel)");
        break;
      }

      case 2:  // Settings request (0x02)
        send_poll_request(CMD_SETTINGS);
        ESP_LOGD("autoterm2d", "VPANEL 0x02 settings request");
        break;
    }
    vpanel_step_ = (vpanel_step_ + 1) % 3;
  }

  // ── Raw frame logger ───────────────────────────────────────────────────────
  void log_heater_frame() {
    std::string hex;
    hex.reserve(heater_frame_buf_.size() * 3);
    for (size_t i = 0; i < heater_frame_buf_.size(); i++) {
      char b[4];
      snprintf(b, sizeof(b), i ? ":%02X" : "%02X", heater_frame_buf_[i]);
      hex += b;
    }
    ESP_LOGD("heater", "→ %s: %s",
             ctrl_uart_ ? "controller" : "virtual-panel", hex.c_str());
  }

  // ── String tables ──────────────────────────────────────────────────────────
  static const char *state_description(uint8_t major, uint8_t minor) {
    if (major == 0 && minor == 0) return "Sleep/Off";
    if (major == 0 && minor == 1) return "Standby";
    if (major == 1 && minor == 0) return "Purge: cooling flame sensor";
    if (major == 1 && minor == 1) return "Purge: ventilating chamber";
    if (major == 2 && minor == 1) return "Pre-heat (glow plug on)";
    if (major == 2 && minor == 2) return "Ignition seq 1";
    if (major == 2 && minor == 3) return "Ignition seq 2";
    if (major == 2 && minor == 4) return "Ramp up";
    if (major == 3 && minor == 0) return "Heating (PID active)";
    if (major == 3 && minor == 35) return "Ventilation only";
    if (major == 3 && minor == 4) return "Cooling down";
    if (major == 4 && minor == 0) return "Shutdown complete";
    return "Unknown state";
  }
  static const char *error_description(uint8_t err) {
    switch (err) {
      case  0: return "OK";
      case  1: return "Overheat";
      case  2: return "Potential overheat";
      case  5: return "Flame sensor fault";
      case  6: return "Temperature sensor fault";
      case  9: return "Glow plug fault";
      case 10: return "Motor RPM fault";
      case 11: return "Air temperature fault";
      case 12: return "Over voltage";
      case 13: return "No start – ignition failed";
      case 15: return "Under voltage";
      case 17: return "Fuel pump fault";
      case 20: return "No communication";
      case 29: return "Flame blowout";
      case 33: return "Control lockout (3× overheat)";
      case 37: return "Hard lockout – send unlock cmd";
      default: return "Unknown error";
    }
  }
  static const char *mode_description(uint8_t m) {
    switch (m) {
      case 1: return "By T Heater";
      case 2: return "By T Panel";
      case 3: return "By T Air";
      case 4: return "By Power";
      default: return "Unknown";
    }
  }

  // ── Utilities ──────────────────────────────────────────────────────────────
  void reset_parser() { read_state_ = 0; message_data_.clear(); }
  uint8_t safe_byte(size_t i) const {
    return i < message_data_.size() ? message_data_[i] : 0;
  }
  static int to_signed_temp(uint8_t raw) {
    return (raw > 127) ? (static_cast<int>(raw) - 256) : static_cast<int>(raw);
  }
  template<typename T> static void publish_if(sensor::Sensor *s, T v) {
    if (s) s->publish_state(static_cast<float>(v));
  }
  static void publish_text(text_sensor::TextSensor *s, const char *v) {
    if (s) s->publish_state(v);
  }
  static uint8_t preset_to_power_mode(const char *p) {
    if (strcmp(p, PRESET_T_PANEL) == 0) return 2;
    if (strcmp(p, PRESET_T_AIR)   == 0) return 3;
    if (strcmp(p, PRESET_POWER)   == 0) return 4;
    return 1;
  }
  static const char *power_mode_to_preset(uint8_t m) {
    switch (m) {
      case 2: return PRESET_T_PANEL;
      case 3: return PRESET_T_AIR;
      case 4: return PRESET_POWER;
      default: return PRESET_T_HEATER;
    }
  }

  // ── CRC-16 Modbus ──────────────────────────────────────────────────────────
  static uint16_t crc16_modbus(const std::vector<uint8_t> &data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t b : data) {
      crc ^= b;
      for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
  }

  // ── Frame senders ──────────────────────────────────────────────────────────

  // Poll request with no payload (0x0F, 0x02)
  void send_poll_request(uint8_t cmd) {
    std::vector<uint8_t> frame = {0xAA, 0x03, 0x00, 0x00, cmd};
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
  }

  // Start / update command (0x01 / 0x02 with payload)
  void send_frame(uint8_t cmd) {
    // work_time_minutes_ == 0 → unlimited: flag=0xFF, time=0xFF
    // work_time_minutes_  > 0 → timed:    flag=0x00, time=minutes
    uint8_t wt_flag = (work_time_minutes_ == 0) ? 0xFF : 0x00;
    uint8_t wt_min  = (work_time_minutes_ == 0) ? 0xFF : work_time_minutes_;
    std::vector<uint8_t> frame = {
        0xAA, 0x03, 0x06, 0x00, cmd, wt_flag, wt_min,
        power_mode_, temp_set_, vent_cmd_, power_level_};
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
    ESP_LOGD("autoterm2d", "TX %-8s mode=%s temp=%d°C vent=%s level=%d timer=%s",
             cmd == CMD_START ? "START" : "UPDATE",
             mode_description(power_mode_), temp_set_,
             vent_cmd_ == 1 ? "On" : "Off", power_level_ + 1,
             work_time_minutes_ == 0 ? "unlimited"
                                     : (std::to_string(work_time_minutes_) + "min").c_str());
  }
  void send_control_command() { send_frame(CMD_START); }

  void send_shutdown_command() {
    const uint8_t raw[] = {0xAA, 0x03, 0x00, 0x00, 0x03, 0x5D, 0x7C};
    write_array(raw, sizeof(raw));
    ESP_LOGD("autoterm2d", "TX SHUTDOWN");
  }

  // Panel temperature (0x11)
  void send_temp_0x11(int8_t temp_c, const char *note = "") {
    std::vector<uint8_t> frame = {0xAA, 0x03, 0x01, 0x00, CMD_PANEL_TEMP,
                                   static_cast<uint8_t>(temp_c)};
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
    ESP_LOGD("autoterm2d", "TX 0x11 %d°C%s", temp_c, note);
  }

  void request_version() {
    const uint8_t req[] = {0xAA, 0x03, 0x00, 0x00, 0x06, 0x5E, 0xBC};
    write_array(req, sizeof(req));
    ESP_LOGD("autoterm2d", "TX 0x06 version request");
  }

  // ── By T Air smoothing ─────────────────────────────────────────────────────
  void on_air_temp_update(float new_temp) {
    if (power_mode_ != 3) return;
    if (major_state_ == 0) return;
    if (!std::isfinite(new_temp)) return;
    if (new_temp < -40.0f || new_temp > 80.0f) return;
    float target = new_temp;
    if (std::isfinite(last_sent_air_temp_)) {
      float d = target - last_sent_air_temp_;
      if (std::abs(d) > AIR_TEMP_MAX_JUMP)
        target = last_sent_air_temp_ + (d > 0 ? AIR_TEMP_MAX_JUMP : -AIR_TEMP_MAX_JUMP);
    }
    int8_t t = static_cast<int8_t>(std::round(target));
    send_temp_0x11(t, " (By T Air / HA)");
    last_sent_air_temp_ = static_cast<float>(t);
  }

  // ── State machine ──────────────────────────────────────────────────────────
  void process_byte(uint8_t byte) {
    switch (read_state_) {
      case 0:
        if (byte == 0xAA) {
          message_data_.clear(); parse_start_ms_ = millis(); read_state_ = 1;
        }
        break;
      case 1: read_state_ = 2; break;
      case 2:
        if (byte == 0 || byte > MAX_PAYLOAD_LEN) {
          ESP_LOGW("autoterm2d", "Bad length 0x%02X", byte); reset_parser();
        } else { message_length_ = byte; read_state_ = 3; }
        break;
      case 3: read_state_ = 4; break;
      case 4: command_id_ = byte; read_state_ = 5; break;
      case 5:
        message_data_.push_back(byte);
        if (message_data_.size() > MAX_PAYLOAD_LEN) { reset_parser(); break; }
        if ((int)message_data_.size() >= message_length_) {
          process_message(); reset_parser();
        }
        break;
      default: reset_parser(); break;
    }
  }

  void process_message() {
    switch (command_id_) {
      case CMD_STATUS:     handle_status();     break;
      case CMD_PANEL_TEMP: handle_panel_temp(); break;
      case CMD_SETTINGS:   handle_settings();   break;
      case CMD_VERSION:    handle_version();    break;
      default:             handle_unknown();    break;
    }
  }

  void handle_status() {
    uint8_t major = safe_byte(0), minor = safe_byte(1), error = safe_byte(2);
    int     t1    = to_signed_temp(safe_byte(3));
    uint8_t t2r   = safe_byte(4);
    float   volts = static_cast<float>((safe_byte(5) << 8) | safe_byte(6)) / 10.0f;
    uint16_t flk  = (safe_byte(7) << 8) | safe_byte(8);
    float   flame = (flk > 273) ? (flk - 273.15f) : 0.0f;
    uint8_t fsp   = safe_byte(11), fac = safe_byte(12);
    float   pump  = safe_byte(14) / 100.0f;
    bool    t2ok  = (t2r != 0x7F);
    int     t2    = to_signed_temp(t2r);

    if (t2ok) {
      ESP_LOGD("autoterm2d",
        "STATUS %d.%d (%s) Err:%d(%s) T-in:%d°C T-out:%d°C %.1fV Flame:%.0f°C Fan:%d/%dHz Pump:%.2fHz",
        major, minor, state_description(major, minor), error, error_description(error),
        t1, t2, volts, flame, fsp, fac, pump);
    } else {
      ESP_LOGD("autoterm2d",
        "STATUS %d.%d (%s) Err:%d(%s) T-in:%d°C T-out:n/a %.1fV Flame:%.0f°C Fan:%d/%dHz Pump:%.2fHz",
        major, minor, state_description(major, minor), error, error_description(error),
        t1, volts, flame, fsp, fac, pump);
    }

    if (!version_requested_) { version_requested_ = true; request_version(); }

    major_state_ = major;
    snap_major_ = major; snap_minor_ = minor; snap_error_ = error;
    snap_t1_ = t1; snap_t2_ok_ = t2ok; snap_t2_ = t2;
    snap_volts_ = volts; snap_flame_c_ = flame;
    snap_fan_sp_ = fsp; snap_fan_act_ = fac; snap_pump_hz_ = pump;

    publish_if(s_status_code_,   static_cast<float>((major << 8) | minor));
    publish_text(s_status_text_, state_description(major, minor));
    publish_if(s_error_code_,    static_cast<float>(error));
    publish_text(s_error_text_,  error_description(error));
    publish_if(s_intake_temp_,   static_cast<float>(t1));
    if (s_air_temp_) s_air_temp_->publish_state(t2ok ? static_cast<float>(t2) : NAN);
    publish_if(s_battery_voltage_, volts);
    publish_if(s_fan_actual_,      static_cast<float>(fac));

    auto new_mode = (major != 0) ? climate::CLIMATE_MODE_HEAT : climate::CLIMATE_MODE_OFF;
    if (new_mode != this->mode) { this->mode = new_mode; this->publish_state(); }
  }

  void handle_panel_temp() {
    int t = to_signed_temp(safe_byte(0));
    ESP_LOGD("autoterm2d", "PANEL TEMP  %d°C", t);
    publish_if(s_panel_temp_, static_cast<float>(t));
  }

  void handle_settings() {
    if (major_state_ == 0) return;
    bool    use_time = (safe_byte(0) == 0);
    uint8_t work_min = safe_byte(1);
    uint8_t mode     = safe_byte(2);
    uint8_t target   = safe_byte(3);
    uint8_t vent     = safe_byte(4);
    uint8_t level    = safe_byte(5);

    snap_mode_ = mode; snap_level_ = level; snap_vent_ = vent;

    ESP_LOGD("autoterm2d",
      "SETTINGS    mode=%-12s target=%d°C vent=%-3s level=%d/10 time=%s",
      mode_description(mode), target, vent == 1 ? "On" : "Off", level + 1,
      use_time ? (std::to_string(work_min) + "min").c_str() : "unlimited");

    // Publish remaining work time (counts down while heater runs)
    if (s_remaining_time_) {
      s_remaining_time_->publish_state(use_time ? static_cast<float>(work_min) : 0.0f);
    }

    publish_if(s_power_level_, static_cast<float>(level + 1));
    bool changed = false;
    if (temp_set_ != target)   { temp_set_ = target; this->target_temperature = target; changed = true; }
    if (power_level_ != level) { power_level_ = level; changed = true; }
    if (power_mode_ != mode) {
      power_mode_ = mode;
      this->set_custom_preset_(power_mode_to_preset(mode));
      changed = true;
    }
    auto nf = (vent == 1) ? climate::CLIMATE_FAN_ON : climate::CLIMATE_FAN_OFF;
    if (this->fan_mode != nf) {
      vent_cmd_ = (vent == 1) ? 1 : 2;
      this->fan_mode = nf;
      changed = true;
    }
    if (changed) this->publish_state();
  }

  void handle_version() {
    if (message_data_.size() < 4) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             safe_byte(0), safe_byte(1), safe_byte(2), safe_byte(3));
    ESP_LOGI("autoterm2d", "Firmware: %s (bootloader v%d)", buf, safe_byte(4));
    publish_text(s_sw_version_, buf);
  }

  void handle_unknown() {
    std::string hex;
    for (size_t i = 0; i < message_data_.size() && i < 12; i++) {
      char b[4]; snprintf(b, sizeof(b), i ? ":%02X" : "%02X", message_data_[i]); hex += b;
    }
    if (message_data_.size() > 12) hex += "...";
    ESP_LOGD("autoterm2d", "CMD 0x%02X len=%-2d %s", command_id_, message_length_, hex.c_str());
  }
};

}  // namespace autoterm2d
}  // namespace esphome
