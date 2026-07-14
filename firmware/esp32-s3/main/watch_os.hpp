#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "esp_err.h"
}

namespace buddhas_watch {

struct NetworkInfo {
    std::string ssid;
    int rssi;
};

struct KnownNetwork {
    std::string ssid;
};

enum class Theme {
    Light,
    Dark,
    Custom,
};

enum class VibroIntensity {
    Weak,
    Medium,
    Strong,
};

enum class ActivityType {
    Static,
    Walking,
    Running,
};

class ConnectivitySettings {
public:
    void render_wifi_list();
    void connect_to_network(const char *ssid, const char *password);
    void show_connection_status();
    void toggle_ble();
    void scan_ble_devices();
    void save_known_networks();

    bool ble_enabled() const { return ble_enabled_; }
    std::string current_network() const { return current_network_; }
    int current_rssi() const { return current_rssi_; }
    const std::vector<KnownNetwork> &known_networks() const { return known_networks_; }

private:
    bool ble_enabled_ = true;
    std::string current_network_ = "";
    int current_rssi_ = -127;
    std::vector<NetworkInfo> scanned_networks_;
    std::vector<KnownNetwork> known_networks_;
};

class DisplaySettings {
public:
    void set_brightness(uint8_t percent);
    void set_screen_timeout(uint16_t milliseconds);
    void select_watch_face(const char *face_name);
    void set_theme(Theme theme);
    void toggle_aod();
    void save_display_prefs();
    uint8_t brightness() const { return brightness_; }
    uint16_t screen_timeout_ms() const { return screen_timeout_ms_; }
    Theme theme() const { return theme_; }
    bool aod_enabled() const { return aod_enabled_; }

private:
    uint8_t brightness_ = 80;
    uint16_t screen_timeout_ms_ = 30000;
    std::string watch_face_ = "default";
    Theme theme_ = Theme::Dark;
    bool aod_enabled_ = false;
};

class AudioSettings {
public:
    void set_ringtone_volume(uint8_t percent);
    void set_media_volume(uint8_t percent);
    void toggle_vibration(bool enabled);
    void set_vibration_intensity(VibroIntensity intensity);
    void test_notification_sound();
    void test_vibration_pattern();
    void save_audio_prefs();
    uint8_t ringtone_volume() const { return ringtone_volume_; }
    uint8_t media_volume() const { return media_volume_; }
    bool vibration_enabled() const { return vibration_enabled_; }
    VibroIntensity vibration_intensity() const { return vibration_intensity_; }

private:
    uint8_t ringtone_volume_ = 70;
    uint8_t media_volume_ = 60;
    bool vibration_enabled_ = true;
    VibroIntensity vibration_intensity_ = VibroIntensity::Medium;
};

class PowerSettings {
public:
    uint8_t get_battery_percent();
    bool is_charging();
    void set_sleep_timeout(uint16_t seconds);
    void trigger_deep_sleep();
    void show_battery_stats();
    uint32_t estimate_remaining_time();
    void save_power_prefs();
    uint16_t sleep_timeout_seconds() const { return sleep_timeout_seconds_; }
    bool deep_sleep_enabled() const { return deep_sleep_enabled_; }

private:
    uint16_t sleep_timeout_seconds_ = 60;
    bool deep_sleep_enabled_ = true;
};

class SensorSettings {
public:
    void display_imu_data();
    uint32_t get_step_count();
    ActivityType detect_activity();
    void sync_time_ntp();
    void calibrate_rtc();
    bool get_microphone_enabled();
    void toggle_microphone_access();
    void test_microphone();
    void save_sensor_prefs();

private:
    uint32_t step_count_ = 0;
    bool microphone_enabled_ = true;
};

class SystemSettings {
public:
    void display_firmware_info();
    void display_storage_usage();
    void browse_sd_files();
    void delete_file(const char *path);
    void clear_all_logs();
    void factory_reset(bool preserve_sd);
    void show_system_stats();
    void save_system_log();
};

class SecuritySettings {
public:
    void set_dnd_schedule(uint16_t start_time, uint16_t end_time);
    void toggle_dnd();
    bool is_in_dnd_period();
    void toggle_privacy_mode();
    bool is_privacy_mode_enabled();
    void show_app_permissions();
    void grant_app_permission(const char *app_id, const char *permission);
    void revoke_app_permission(const char *app_id, const char *permission);
    void save_security_prefs();
    bool dnd_enabled() const { return dnd_enabled_; }
    uint16_t dnd_start_time() const { return dnd_start_time_; }
    uint16_t dnd_end_time() const { return dnd_end_time_; }

private:
    bool dnd_enabled_ = false;
    uint16_t dnd_start_time_ = 2200;
    uint16_t dnd_end_time_ = 700;
    bool privacy_mode_ = false;
};

class SettingsApp {
public:
    void render();
    void launch_connectivity_settings();
    void launch_display_settings();
    void launch_audio_settings();
    void launch_power_settings();
    void launch_sensor_settings();
    void launch_system_settings();
    void launch_security_settings();
    void persist_all();

private:
    ConnectivitySettings connectivity_;
    DisplaySettings display_;
    AudioSettings audio_;
    PowerSettings power_;
    SensorSettings sensors_;
    SystemSettings system_;
    SecuritySettings security_;
};

class StreamingManager {
public:
    void start_ble_server();
    void start_wifi_tcp_server();
    void start_wifi_udp_broadcast();
    void start_usb_cdc_server();
    void start_mdns_discovery();
    void start_all();
};

class AppLauncher {
public:
    void render();
};

class WatchOS {
public:
    esp_err_t boot();

private:
    AppLauncher launcher_;
    StreamingManager streaming_;
    SettingsApp settings_;
};

}  // namespace buddhas_watch
