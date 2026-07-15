#include "watch_os.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <limits>

extern "C" {
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sys/stat.h"
#include "sys/unistd.h"
#include "cJSON.h"

void start_cmd_receiver(void);
}

namespace buddhas_watch {

namespace {
constexpr const char *kTag = "WATCH_OS";
constexpr const char *kDataDir = "/data";
constexpr const char *kSettingsPath = "/data/settings_config.json";
constexpr const char *kKnownNetworksPath = "/data/known_networks.json";
constexpr const char *kFirmwareVersion = "2.0.0";
constexpr uint16_t kMinScreenTimeoutMs = 1000;
constexpr uint16_t kMaxScreenTimeoutMs = 60000;

void ensure_data_dir() {
    struct stat st = {};
    if (stat(kDataDir, &st) == -1) {
        mkdir(kDataDir, 0755);
    }
}

const char *theme_to_string(Theme theme) {
    switch (theme) {
        case Theme::Light:
            return "light";
        case Theme::Dark:
            return "dark";
        case Theme::Custom:
            return "custom";
    }
    return "dark";
}

const char *vibro_to_string(VibroIntensity intensity) {
    switch (intensity) {
        case VibroIntensity::Weak:
            return "weak";
        case VibroIntensity::Medium:
            return "medium";
        case VibroIntensity::Strong:
            return "strong";
    }
    return "medium";
}

bool is_valid_hhmm(uint16_t value) {
    const uint16_t hours = value / 100U;
    const uint16_t minutes = value % 100U;
    return hours < 24U && minutes < 60U;
}

void write_json_file(const char *path, cJSON *root) {
    char *json = cJSON_Print(root);
    if (!json) {
        ESP_LOGE(kTag, "Failed to serialize json for %s", path);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(kTag, "Failed opening %s", path);
        cJSON_free(json);
        return;
    }

    const size_t json_len = strlen(json);
    if (fwrite(json, 1, json_len, f) != json_len) {
        ESP_LOGE(kTag, "Failed writing %s", path);
    }
    fclose(f);
    cJSON_free(json);
}

void save_unified_settings(const ConnectivitySettings &connectivity, uint8_t brightness,
                           uint16_t screen_timeout_ms, Theme theme, bool aod_enabled,
                           uint8_t ringtone_volume, uint8_t media_volume,
                           bool vibration_enabled, VibroIntensity vibration_intensity,
                           uint16_t sleep_timeout_seconds, bool deep_sleep_enabled,
                           bool microphone_enabled, bool dnd_enabled, uint16_t dnd_start,
                           uint16_t dnd_end, bool privacy_mode) {
    ensure_data_dir();

    cJSON *root = cJSON_CreateObject();
    cJSON *connectivity_json = cJSON_AddObjectToObject(root, "connectivity");
    cJSON_AddBoolToObject(connectivity_json, "wifi_enabled", true);
    cJSON_AddBoolToObject(connectivity_json, "ble_enabled", connectivity.ble_enabled());
    cJSON *known = cJSON_AddArrayToObject(connectivity_json, "known_networks");
    for (const auto &network : connectivity.known_networks()) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", network.ssid.c_str());
        cJSON_AddBoolToObject(entry, "credentials_saved", false);
        cJSON_AddItemToArray(known, entry);
    }

    cJSON *display = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(display, "brightness", brightness);
    cJSON_AddNumberToObject(display, "screen_timeout_ms", screen_timeout_ms);
    cJSON_AddStringToObject(display, "theme", theme_to_string(theme));
    cJSON_AddBoolToObject(display, "aod_enabled", aod_enabled);

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddNumberToObject(audio, "ringtone_volume", ringtone_volume);
    cJSON_AddNumberToObject(audio, "media_volume", media_volume);
    cJSON_AddBoolToObject(audio, "vibration_enabled", vibration_enabled);
    cJSON_AddStringToObject(audio, "vibration_intensity", vibro_to_string(vibration_intensity));

    const uint32_t max_seconds = std::numeric_limits<uint32_t>::max() / 1000U;
    const uint32_t safe_seconds = std::min<uint32_t>(sleep_timeout_seconds, max_seconds);
    const uint32_t sleep_timeout_ms = safe_seconds * 1000U;

    cJSON *power = cJSON_AddObjectToObject(root, "power");
    cJSON_AddNumberToObject(power, "sleep_timeout_ms", sleep_timeout_ms);
    cJSON_AddBoolToObject(power, "deep_sleep_enabled", deep_sleep_enabled);
    cJSON_AddBoolToObject(power, "battery_saver_mode", false);

    cJSON *sensors = cJSON_AddObjectToObject(root, "sensors");
    cJSON_AddBoolToObject(sensors, "step_tracking_enabled", true);
    cJSON_AddBoolToObject(sensors, "rtc_synced", true);
    cJSON_AddBoolToObject(sensors, "microphone_enabled", microphone_enabled);

    cJSON *security = cJSON_AddObjectToObject(root, "security");
    cJSON_AddBoolToObject(security, "dnd_enabled", dnd_enabled);
    cJSON_AddNumberToObject(security, "dnd_start_time", dnd_start);
    cJSON_AddNumberToObject(security, "dnd_end_time", dnd_end);
    cJSON_AddBoolToObject(security, "privacy_mode", privacy_mode);

    write_json_file(kSettingsPath, root);
    cJSON_Delete(root);
}

}  // namespace

void ConnectivitySettings::render_wifi_list() {
    scanned_networks_ = {
        {"Buddhas-Net", -40},
        {"Lab-Router", -61},
        {"Jetson-Hotspot", -55},
    };
    for (const auto &entry : scanned_networks_) {
        ESP_LOGI(kTag, "Wi-Fi %s RSSI=%d", entry.ssid.c_str(), entry.rssi);
    }
}

void ConnectivitySettings::connect_to_network(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) {
        return;
    }
    (void)password;
    current_network_ = ssid;
    current_rssi_ = -42;
    known_networks_.push_back({ssid});
    ESP_LOGI(kTag, "Connected to %s", ssid);
}

void ConnectivitySettings::show_connection_status() {
    ESP_LOGI(kTag, "Wi-Fi=%s RSSI=%d BLE=%s", current_network_.c_str(), current_rssi_,
             ble_enabled_ ? "on" : "off");
}

void ConnectivitySettings::toggle_ble() {
    ble_enabled_ = !ble_enabled_;
    ESP_LOGI(kTag, "BLE %s", ble_enabled_ ? "enabled" : "disabled");
}

void ConnectivitySettings::scan_ble_devices() {
    ESP_LOGI(kTag, "BLE scan: Phone, Jetson, SteamDeck");
}

void ConnectivitySettings::save_known_networks() {
    ensure_data_dir();
    cJSON *root = cJSON_CreateObject();
    cJSON *known = cJSON_AddArrayToObject(root, "known_networks");
    for (const auto &entry : known_networks_) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", entry.ssid.c_str());
        cJSON_AddBoolToObject(network, "credentials_saved", false);
        cJSON_AddItemToArray(known, network);
    }
    write_json_file(kKnownNetworksPath, root);
    cJSON_Delete(root);
}

void DisplaySettings::set_brightness(uint8_t percent) {
    brightness_ = std::min<uint8_t>(100, percent);
    ESP_LOGI(kTag, "Brightness=%u%%", brightness_);
}

void DisplaySettings::set_screen_timeout(uint16_t milliseconds) {
    screen_timeout_ms_ = std::clamp(milliseconds, kMinScreenTimeoutMs, kMaxScreenTimeoutMs);
    ESP_LOGI(kTag, "Screen timeout=%u ms", screen_timeout_ms_);
}

void DisplaySettings::select_watch_face(const char *face_name) {
    watch_face_ = (face_name && strlen(face_name)) ? face_name : "default";
    ESP_LOGI(kTag, "Watch face=%s", watch_face_.c_str());
}

void DisplaySettings::set_theme(Theme theme) {
    theme_ = theme;
    ESP_LOGI(kTag, "Theme=%s", theme_to_string(theme_));
}

void DisplaySettings::toggle_aod() {
    aod_enabled_ = !aod_enabled_;
    ESP_LOGI(kTag, "AOD=%s", aod_enabled_ ? "on" : "off");
}

void DisplaySettings::save_display_prefs() {
    ESP_LOGI(kTag, "Display settings saved");
}

void AudioSettings::set_ringtone_volume(uint8_t percent) {
    ringtone_volume_ = std::min<uint8_t>(100, percent);
    ESP_LOGI(kTag, "Ringtone volume=%u%%", ringtone_volume_);
}

void AudioSettings::set_media_volume(uint8_t percent) {
    media_volume_ = std::min<uint8_t>(100, percent);
    ESP_LOGI(kTag, "Media volume=%u%%", media_volume_);
}

void AudioSettings::toggle_vibration(bool enabled) {
    vibration_enabled_ = enabled;
    ESP_LOGI(kTag, "Vibration=%s", vibration_enabled_ ? "enabled" : "disabled");
}

void AudioSettings::set_vibration_intensity(VibroIntensity intensity) {
    vibration_intensity_ = intensity;
    ESP_LOGI(kTag, "Vibration intensity=%s", vibro_to_string(vibration_intensity_));
}

void AudioSettings::test_notification_sound() {
    ESP_LOGI(kTag, "Notification tone preview");
}

void AudioSettings::test_vibration_pattern() {
    ESP_LOGI(kTag, "Vibration pattern preview");
}

void AudioSettings::save_audio_prefs() {
    ESP_LOGI(kTag, "Audio settings saved");
}

uint8_t PowerSettings::get_battery_percent() {
    return static_cast<uint8_t>(40U + (esp_random() % 56U));
}

bool PowerSettings::is_charging() {
    return false;
}

void PowerSettings::set_sleep_timeout(uint16_t seconds) {
    sleep_timeout_seconds_ = seconds;
    ESP_LOGI(kTag, "Sleep timeout=%u sec", sleep_timeout_seconds_);
}

void PowerSettings::trigger_deep_sleep() {
    ESP_LOGW(kTag, "Deep sleep requested");
}

void PowerSettings::show_battery_stats() {
    ESP_LOGI(kTag, "Battery=%u%% charging=%s", get_battery_percent(), is_charging() ? "yes" : "no");
}

uint32_t PowerSettings::estimate_remaining_time() {
    return static_cast<uint32_t>(get_battery_percent()) * 6U * 60U;
}

void PowerSettings::save_power_prefs() {
    ESP_LOGI(kTag, "Power settings saved");
}

void SensorSettings::display_imu_data() {
    ESP_LOGI(kTag, "IMU accel=(0.01,-0.02,0.99) gyro=(0.1,0.2,0.1)");
}

uint32_t SensorSettings::get_step_count() {
    step_count_ += 3;
    return step_count_;
}

ActivityType SensorSettings::detect_activity() {
    return ActivityType::Walking;
}

void SensorSettings::sync_time_ntp() {
    ESP_LOGI(kTag, "NTP sync requested");
}

void SensorSettings::calibrate_rtc() {
    ESP_LOGI(kTag, "RTC calibrated");
}

bool SensorSettings::get_microphone_enabled() {
    return microphone_enabled_;
}

void SensorSettings::toggle_microphone_access() {
    microphone_enabled_ = !microphone_enabled_;
    ESP_LOGI(kTag, "Microphone=%s", microphone_enabled_ ? "enabled" : "disabled");
}

void SensorSettings::test_microphone() {
    ESP_LOGI(kTag, "Microphone loopback test");
}

void SensorSettings::save_sensor_prefs() {
    ESP_LOGI(kTag, "Sensor settings saved");
}

void SystemSettings::display_firmware_info() {
    ESP_LOGI(kTag, "Firmware v%s build=%s %s", kFirmwareVersion, __DATE__, __TIME__);
}

void SystemSettings::display_storage_usage() {
    ESP_LOGI(kTag, "Storage flash=42%% psram=31%% sd=15%%");
}

void SystemSettings::browse_sd_files() {
    ESP_LOGI(kTag, "Browse /data/csi_logs /data/battery_logs");
}

void SystemSettings::delete_file(const char *path) {
    if (!path || strlen(path) == 0) {
        return;
    }
    if (unlink(path) == 0) {
        ESP_LOGI(kTag, "Deleted %s", path);
        return;
    }
    ESP_LOGE(kTag, "Failed to delete %s", path);
}

void SystemSettings::clear_all_logs() {
    ESP_LOGI(kTag, "Cleared all logs");
}

void SystemSettings::factory_reset(bool preserve_sd) {
    ESP_LOGW(kTag, "Factory reset preserve_sd=%s", preserve_sd ? "true" : "false");
}

void SystemSettings::show_system_stats() {
    ESP_LOGI(kTag, "Uptime=%llu ms free_heap=%u", esp_timer_get_time() / 1000ULL, esp_get_free_heap_size());
}

void SystemSettings::save_system_log() {
    ensure_data_dir();
    FILE *f = fopen("/data/system_changes.log", "a");
    if (!f) {
        return;
    }
    fprintf(f, "settings change at %llu\n", esp_timer_get_time());
    fclose(f);
}

void SecuritySettings::set_dnd_schedule(uint16_t start_time, uint16_t end_time) {
    if (!is_valid_hhmm(start_time) || !is_valid_hhmm(end_time)) {
        ESP_LOGW(kTag, "Rejected invalid DND schedule %04u-%04u", start_time, end_time);
        return;
    }
    dnd_start_time_ = start_time;
    dnd_end_time_ = end_time;
}

void SecuritySettings::toggle_dnd() {
    dnd_enabled_ = !dnd_enabled_;
    ESP_LOGI(kTag, "DND=%s", dnd_enabled_ ? "on" : "off");
}

bool SecuritySettings::is_in_dnd_period() {
    std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        return false;
    }
    std::tm tm_now = {};
    if (localtime_r(&now, &tm_now) == nullptr) {
        return false;
    }
    const uint16_t current = static_cast<uint16_t>(
        (static_cast<uint16_t>(tm_now.tm_hour) * 100U) + static_cast<uint16_t>(tm_now.tm_min));

    if (dnd_start_time_ <= dnd_end_time_) {
        return current >= dnd_start_time_ && current <= dnd_end_time_;
    }

    return current >= dnd_start_time_ || current <= dnd_end_time_;
}

void SecuritySettings::toggle_privacy_mode() {
    privacy_mode_ = !privacy_mode_;
    ESP_LOGI(kTag, "Privacy mode=%s", privacy_mode_ ? "enabled" : "disabled");
}

bool SecuritySettings::is_privacy_mode_enabled() {
    return privacy_mode_;
}

void SecuritySettings::show_app_permissions() {
    ESP_LOGI(kTag, "permissions: csi_collector[sensors,network], system_monitor[read_only]");
}

void SecuritySettings::grant_app_permission(const char *app_id, const char *permission) {
    ESP_LOGI(kTag, "grant %s -> %s", app_id ? app_id : "", permission ? permission : "");
}

void SecuritySettings::revoke_app_permission(const char *app_id, const char *permission) {
    ESP_LOGI(kTag, "revoke %s -> %s", app_id ? app_id : "", permission ? permission : "");
}

void SecuritySettings::save_security_prefs() {
    ESP_LOGI(kTag, "Security settings saved");
}

void SettingsApp::render() {
    ESP_LOGI(kTag, "Settings modules:");
    ESP_LOGI(kTag, "  1) Connectivity");
    ESP_LOGI(kTag, "  2) Display");
    ESP_LOGI(kTag, "  3) Sound & Haptics");
    ESP_LOGI(kTag, "  4) Power");
    ESP_LOGI(kTag, "  5) Sensors & Health");
    ESP_LOGI(kTag, "  6) System & Storage");
    ESP_LOGI(kTag, "  7) Security & Privacy");
}

void SettingsApp::launch_connectivity_settings() {
    connectivity_.render_wifi_list();
    connectivity_.show_connection_status();
}

void SettingsApp::launch_display_settings() {
    display_.set_brightness(80);
    display_.set_screen_timeout(30000);
    display_.set_theme(Theme::Dark);
}

void SettingsApp::launch_audio_settings() {
    audio_.set_ringtone_volume(70);
    audio_.set_media_volume(60);
    audio_.test_notification_sound();
}

void SettingsApp::launch_power_settings() {
    power_.show_battery_stats();
    power_.set_sleep_timeout(60);
}

void SettingsApp::launch_sensor_settings() {
    sensors_.display_imu_data();
    sensors_.sync_time_ntp();
}

void SettingsApp::launch_system_settings() {
    system_.display_firmware_info();
    system_.display_storage_usage();
}

void SettingsApp::launch_security_settings() {
    security_.show_app_permissions();
}

void SettingsApp::persist_all() {
    connectivity_.save_known_networks();
    display_.save_display_prefs();
    audio_.save_audio_prefs();
    power_.save_power_prefs();
    sensors_.save_sensor_prefs();
    security_.save_security_prefs();
    system_.save_system_log();

    save_unified_settings(connectivity_,
                          display_.brightness(),
                          display_.screen_timeout_ms(),
                          display_.theme(),
                          display_.aod_enabled(),
                          audio_.ringtone_volume(),
                          audio_.media_volume(),
                          audio_.vibration_enabled(),
                          audio_.vibration_intensity(),
                          power_.sleep_timeout_seconds(),
                          power_.deep_sleep_enabled(),
                          sensors_.get_microphone_enabled(),
                          security_.dnd_enabled(),
                          security_.dnd_start_time(),
                          security_.dnd_end_time(),
                          security_.is_privacy_mode_enabled());
}

void StreamingManager::start_ble_server() {
    ESP_LOGI(kTag, "BLE CSI GATT server ready");
}

void StreamingManager::start_wifi_tcp_server() {
    ESP_LOGI(kTag, "Wi-Fi TCP CSI server listening on :5000");
}

void StreamingManager::start_wifi_udp_broadcast() {
    ESP_LOGI(kTag, "Wi-Fi UDP CSI broadcast on :5001");
}

void StreamingManager::start_usb_cdc_server() {
    ESP_LOGI(kTag, "USB-C CDC streaming @ 921600 baud");
}

void StreamingManager::start_mdns_discovery() {
    ESP_LOGI(kTag, "mDNS service _csi._tcp published");
}

void StreamingManager::start_all() {
    start_ble_server();
    start_wifi_tcp_server();
    start_wifi_udp_broadcast();
    start_usb_cdc_server();
    start_mdns_discovery();
}

void AppLauncher::render() {
    ESP_LOGI(kTag, "Apps:");
    ESP_LOGI(kTag, "  • CSI Collector");
    ESP_LOGI(kTag, "  • Settings");
    ESP_LOGI(kTag, "  • System Monitor");
    ESP_LOGI(kTag, "  • BLE Broadcaster");
    ESP_LOGI(kTag, "  • USB Server");
    ESP_LOGI(kTag, "  • Wi-Fi Server");
    ESP_LOGI(kTag, "  • App Store");
}

esp_err_t WatchOS::boot() {
    ensure_data_dir();
    launcher_.render();
    settings_.render();
    settings_.launch_connectivity_settings();
    settings_.launch_display_settings();
    settings_.launch_audio_settings();
    settings_.launch_power_settings();
    settings_.launch_sensor_settings();
    settings_.launch_system_settings();
    settings_.launch_security_settings();
    settings_.persist_all();

    streaming_.start_all();
    start_cmd_receiver();

    ESP_LOGI(kTag, "Buddhas-Watch OS boot complete");
    return ESP_OK;
}

}  // namespace buddhas_watch
