#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <mbedtls/md.h>
#include <Update.h>
#include "LGFX_ESP32S3_RGB_TFT_SPI_ST7701_GT911.h"

// Instantiate the display driver
LGFX gfx;

#define GRAPH_HISTORY_SIZE 450

#define BUILD_VERSION "1.0.34"
const char ota_signature[] = "CGM-OTA-SIGNATURE:" BUILD_VERSION;


// Configuration variables
char llu_email[64] = "";
char llu_password[64] = "";
char llu_region[16] = "eu";
char llu_units[10] = "mmol/L";
int llu_poll_interval = 2;
bool is_configured = false;

// Diabetes:M configurations
bool dm_enable_connection = false;
char dm_email[64] = "";
char dm_password[64] = "";
bool dm_enable_2fa = false;
char dm_note_text[64] = "Sent by ESP32";
bool dm_auto_send = false;
int dm_api_interval = 30; // in minutes
int dm_random_offset = 2; // in minutes
char dm_start_sending[32] = ""; // "YYYY-MM-DDTHH:MM"
char dm_stop_sending[32] = "";  // "YYYY-MM-DDTHH:MM"
char dm_timezone_json[32] = "Europe/London";
char dm_timezone_posix[64] = "GMT0BST,M3.5.0/1,M10.5.0/2";

// Category Assignment configurations
bool dm_auto_category = false;
int dm_fallback_category = 8;
int dm_b_start = 480;
int dm_b_end = 510;
int dm_l_start = 720;
int dm_l_end = 750;
int dm_d_start = 1140;
int dm_d_end = 1170;
bool dm_2fa_pending = false;
char device_name[32] = "ESP32-CGM-Display";


struct DMCategory {
  int id;
  char name[32];
};

struct CategoryTimeRule {
  int category_id;
  int start_min;
  int end_min;
  bool enabled;
};

DMCategory dm_categories[32];
int dm_custom_input_ids[16] = {0};
int dm_categories_count = 0;

CategoryTimeRule dm_cat_rules[24];
int dm_cat_rules_count = 0;

void initializeDefaultCategories() {
  const char* default_names[] = {
    "Snack", "Before breakfast", "After breakfast", "Before lunch", "After lunch",
    "Before dinner", "After dinner", "Night", "Other", "Fasting glucose",
    "Before bed", "Breakfast", "Lunch", "Dinner", "Before exercise", "After exercise"
  };
  for (int i = 0; i < 16; i++) {
    dm_categories[i].id = i;
    strncpy(dm_categories[i].name, default_names[i], sizeof(dm_categories[i].name) - 1);
    dm_categories[i].name[sizeof(dm_categories[i].name) - 1] = '\0';
  }
  dm_categories_count = 16;
}

void initializeDefaultCategoryRules() {
  int after_b_end = dm_l_start - 61;
  if (after_b_end < dm_b_end + 1) after_b_end = dm_b_end + 1;
  
  int after_l_end = dm_d_start - 61;
  if (after_l_end < dm_l_end + 1) after_l_end = dm_l_end + 1;
  
  int after_d_end = 21 * 60 - 1; // 20:59
  if (after_d_end < dm_d_end + 1) after_d_end = dm_d_end + 1;
  
  int night_end = dm_b_start - 61;
  if (night_end < 0) night_end = 0;

  dm_cat_rules[0] = {1,  dm_b_start - 60, dm_b_start - 1, true};
  dm_cat_rules[1] = {11, dm_b_start,      dm_b_end,       true};
  dm_cat_rules[2] = {2,  dm_b_end + 1,    after_b_end,    true};
  
  dm_cat_rules[3] = {3,  dm_l_start - 60, dm_l_start - 1, true};
  dm_cat_rules[4] = {12, dm_l_start,      dm_l_end,       true};
  dm_cat_rules[5] = {4,  dm_l_end + 1,    after_l_end,    true};
  
  dm_cat_rules[6] = {5,  dm_d_start - 60, dm_d_start - 1, true};
  dm_cat_rules[7] = {13, dm_d_start,      dm_d_end,       true};
  dm_cat_rules[8] = {6,  dm_d_end + 1,    after_d_end,    true};
  
  dm_cat_rules[9] =  {10, 21 * 60, 24 * 60 - 1, true};
  dm_cat_rules[10] = {7,  0,       night_end,   true};
  
  dm_cat_rules_count = 11;
}

String formatMinutesToHM(int minutes) {
  int h = (minutes / 60) % 24;
  int m = minutes % 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  return String(buf);
}

// Diabetes:M auth caches
char dm_token[512] = "";
char dm_cookies[256] = "";
char dm_user_id[32] = "";
char dm_last_uploaded_libre_ts[32] = "";

// Diabetes:M runtime state
time_t dm_next_send_epoch = 0;
time_t dm_last_sent_epoch = 0;
float dm_last_sent_value = 0.0;
String dm_last_sent_status = "Not Sent";

struct DMLogEntry {
  time_t timestamp;
  float value;
  String status;
};
DMLogEntry dm_logs[10];
int dm_logs_count = 0;

// Heartbeat configurations
bool dm_enable_heartbeat = false;
int dm_heartbeat_interval = 15; // in minutes
time_t dm_next_heartbeat_epoch = 0;
time_t dm_last_heartbeat_epoch = 0;
String dm_last_heartbeat_status = "Not Run";

struct DMHeartbeatLogEntry {
  time_t timestamp;
  String status;
};
DMHeartbeatLogEntry dm_heartbeat_logs[10];
int dm_heartbeat_logs_count = 0;

void addDMHeartbeatLog(time_t ts, const String &status) {
  for (int i = 9; i > 0; i--) {
    dm_heartbeat_logs[i] = dm_heartbeat_logs[i-1];
  }
  dm_heartbeat_logs[0].timestamp = ts;
  dm_heartbeat_logs[0].status = status;
  if (dm_heartbeat_logs_count < 10) dm_heartbeat_logs_count++;
}

// Color tolerances and graph range limits (all stored in mg/dL internally)
float c_low = 72.0;         // Critical Low (Red below)
float w_low = 90.0;         // Warning Low (Yellow below)
float w_high = 180.0;       // Warning High (Yellow above)
float c_high = 216.0;       // Critical High (Red above)
float graph_min = 40.0;     // Graph Y-Axis Min
float graph_max = 250.0;    // Graph Y-Axis Max

// Helpers for unit conversions and timestamp formatting
float toUserUnit(float val) {
  if (strcmp(llu_units, "mmol/L") == 0) return val / 18.0182;
  return val;
}
float fromUserUnit(float val) {
  if (strcmp(llu_units, "mmol/L") == 0) return val * 18.0182;
  return val;
}
String formatUserUnitValue(float val) {
  float converted = toUserUnit(val);
  if (strcmp(llu_units, "mmol/L") == 0) {
    return String(converted, 1);
  } else {
    return String((int)round(converted));
  }
}
String formatTimestamp(const String &ts) {
  int space_idx = ts.indexOf(' ');
  if (space_idx == -1) space_idx = ts.indexOf('T');
  if (space_idx == -1) return "00:00";
  
  String timePart = ts.substring(space_idx + 1);
  int colon1 = timePart.indexOf(':');
  if (colon1 == -1) return "00:00";
  
  int hour = timePart.substring(0, colon1).toInt();
  int colon2 = timePart.indexOf(':', colon1 + 1);
  String minStr = (colon2 != -1) ? timePart.substring(colon1 + 1, colon2) : timePart.substring(colon1 + 1);
  int minute = minStr.toInt();
  
  bool isPM = (timePart.indexOf("PM") != -1 || timePart.indexOf("pm") != -1);
  bool isAM = (timePart.indexOf("AM") != -1 || timePart.indexOf("am") != -1);
  if (isPM && hour < 12) hour += 12;
  if (isAM && hour == 12) hour = 0;
  
  char buf[6];
  sprintf(buf, "%02d:%02d", hour, minute);
  return String(buf);
}

// NTP and local timezone time helpers
void initTimeNTP() {
  Serial.printf("Initializing NTP with timezone POSIX: %s\n", dm_timezone_posix);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", dm_timezone_posix, 1);
  tzset();
}

bool isTimeSynced() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  if (!localtime_r(&now, &timeinfo)) return false;
  return timeinfo.tm_year > 120; // Year > 2020 means NTP synced successfully
}

time_t parseDateTime(const String &str) {
  if (str.length() < 10) return 0;
  
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  // HTML5 datetime-local formats: "YYYY-MM-DDTHH:MM" or "YYYY-MM-DD HH:MM" or with seconds
  int parsed = sscanf(str.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  if (parsed < 5) {
    parsed = sscanf(str.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  }
  if (parsed < 5) {
    parsed = sscanf(str.c_str(), "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute);
    second = 0;
  }
  if (parsed < 5) {
    parsed = sscanf(str.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute);
    second = 0;
  }
  
  if (parsed < 5) return 0;
  
  struct tm tm_info;
  memset(&tm_info, 0, sizeof(tm_info));
  tm_info.tm_year = year - 1900;
  tm_info.tm_mon = month - 1;
  tm_info.tm_mday = day;
  tm_info.tm_hour = hour;
  tm_info.tm_min = minute;
  tm_info.tm_sec = second;
  tm_info.tm_isdst = -1; // let the system determine DST
  
  return mktime(&tm_info);
}

String formatLocalTime(time_t epoch, const char* format) {
  if (epoch == 0) return "--:--";
  struct tm timeinfo;
  if (!localtime_r(&epoch, &timeinfo)) {
    return "--:--";
  }
  char buf[64];
  strftime(buf, sizeof(buf), format, &timeinfo);
  return String(buf);
}

void recalculateNextSendTime() {
  if (!isTimeSynced()) {
    dm_next_send_epoch = 0;
    return;
  }
  
  if (strlen(dm_stop_sending) > 0) {
    time_t stop_t = parseDateTime(dm_stop_sending);
    if (stop_t > 0 && time(nullptr) > stop_t) {
      dm_next_send_epoch = 0;
      return;
    }
  }
  
  long base_sec = (long)dm_api_interval * 60;
  long max_offset_sec = (long)dm_random_offset * 60;
  long offset = 0;
  if (max_offset_sec > 0) {
    offset = random(-max_offset_sec, max_offset_sec + 1);
  }
  time_t next_send = time(nullptr) + base_sec + offset;
  
  if (strlen(dm_stop_sending) > 0) {
    time_t stop_t = parseDateTime(dm_stop_sending);
    if (stop_t > 0 && next_send > stop_t) {
      dm_next_send_epoch = 0;
      return;
    }
  }
  
  dm_next_send_epoch = next_send;
  Serial.printf("Next Diabetes:M scheduled send in %ld seconds (offset: %ld seconds). Next epoch: %ld\n", 
                base_sec + offset, offset, (long)dm_next_send_epoch);
}

void addDMLog(time_t ts, float val, const String &status) {
  for (int i = 9; i > 0; i--) {
    dm_logs[i] = dm_logs[i-1];
  }
  dm_logs[0].timestamp = ts;
  dm_logs[0].value = val;
  dm_logs[0].status = status;
  if (dm_logs_count < 10) dm_logs_count++;
}

char admin_password[32] = "";
bool is_temp_password = true;

struct MessageRule {
  char type[16];
  float val1;
  float val2;
  char text[64];
};

MessageRule msg_rules[10];
int msg_rules_count = 0;

void generateRandomPassword() {
  const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  // Seed random generator using analog noise and micros
  randomSeed(analogRead(0) + micros());
  for (int i = 0; i < 9; i++) {
    if (i == 4) {
      admin_password[i] = '-';
    } else {
      admin_password[i] = charset[random(0, sizeof(charset) - 1)];
    }
  }
  admin_password[9] = '\0';
  is_temp_password = true;
}

// Global state variables
String auth_token = "";
uint32_t token_expires = 0;
String account_id_hash = "";
float last_glucose = 0.0;
int last_trend = 3; // 3 = flat/stable
String last_timestamp = "";
unsigned long last_fetch_time = 0;
time_t llu_last_fetch_attempt_epoch = 0;
time_t llu_last_fetch_success_epoch = 0;
String llu_last_fetch_status = "Never Synced";

// History buffer for the chart
float glucose_history[GRAPH_HISTORY_SIZE];
int history_count = 0;

// WiFiManager configuration portal parameters
WiFiManager wm;
bool shouldSaveConfig = false;

// Local WebServer on home WiFi (port 80)
WebServer localServer(80);

// Forward declarations
void drawSplashScreen();
void drawDashboard();
void drawHistoryGraph();
void drawTrendArrow(int x, int y, int trend, uint32_t color);
void drawThickLine(int x1, int y1, int x2, int y2, int thickness, uint32_t color);
void drawTopStatusBar();
bool libreLinkUpLogin();
bool libreLinkUpFetchData();
int mapGlucoseToY(float val);
void drawPendingConfigScreen();

// Local WebServer handlers
void handleLocalRoot();
void handleLocalParam();
void handleLocalSave();
void handleLocalTest();
void handleLocalReboot();
void handleLocalUpdateGet();
void handleLocalUpdatePost();
void handleLocalUpdateUpload();
void handleLocalChangePasswordGet();
void handleLocalSavePasswordPost();
void handleLocalFactoryReset();
void handleLocalGeneralGet();
void handleLocalSaveGeneralPost();
void handleLocalHardwareGet();
void handleLocalExportConfig();
void handleLocalWifiGet();
void handleLocalWifiSave();
void showDiagnosticsScreen();
void handleLocalImportConfigGet();
void handleLocalImportConfig();
const char* getResetReasonStr(esp_reset_reason_t reason);
const char* getWiFiModeStr(wifi_mode_t mode);
const char* getTrendStr(int trend);
String getUptimeStr();
bool confirmFactoryResetHMI();
void handleLocalDebugGet();
String obfuscate(const String &input);
String deobfuscate(const String &input);

// Diabetes:M forward declarations
void handleLocalDMGet();
void handleLocalDMSave();
void handleLocalDMTestConnection();
void handleLocalDMTestUpload();
void handleLocalDebugDM();
void handleAuthExpired();
void initTimeNTP();
bool isTimeSynced();
time_t parseDateTime(const String& str);
String formatLocalTime(time_t epoch, const char* format = "%H:%M");
void recalculateNextSendTime();
void addDMLog(time_t ts, float val, const String &status);
void addDMHeartbeatLog(time_t ts, const String &status);
bool diabetesMLogin(const char* two_fa_code, String &out_err);
bool diabetesMGetProfile(String &out_info);
bool diabetesMGetCategories(String &out_err);
bool diabetesMUploadReading(float glucose_mgdl, const String &notes, String &out_err);
bool uploadWithRetry(float glucose_mgdl, const String &notes, String &out_err);
void parseAndSaveSettings(JsonVariant settings);
int getSuggestedCategory(int hour, int minute);
void set2FAPending(bool pending);




bool checkAuth();
bool checkForceReset();

void saveConfigCallback() {
  Serial.println("WiFiManager config saved callback triggered.");
  shouldSaveConfig = true;
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered WiFiManager config portal.");
  
  // Render configuration instructions on the screen
  gfx.fillScreen(0x1A1A1A); // Dark background
  
  gfx.setFont(&fonts::DejaVu24);
  gfx.setTextColor(0xFFFFFF);
  gfx.drawCenterString("WiFi Configuration", 240, 60);
  
  gfx.setFont(&fonts::DejaVu18);
  gfx.drawCenterString("Connect your phone/PC to WiFi:", 240, 140);
  
  gfx.setTextColor(0x5CB85C); // Green
  gfx.setFont(&fonts::DejaVu24);
  gfx.drawCenterString("SSID: ESP32-CGM-Config", 240, 180);
  
  gfx.setTextColor(0xFFFFFF);
  gfx.setFont(&fonts::DejaVu18);
  gfx.drawCenterString("Then open your browser and go to:", 240, 260);
  
  gfx.setTextColor(0x33B5E5); // Blue
  gfx.setFont(&fonts::DejaVu24);
  gfx.drawCenterString("http://192.168.4.1", 240, 300);
  
  gfx.setTextColor(0xAAAAAA);
  gfx.setFont(&fonts::DejaVu18);
  gfx.drawCenterString("Enter WiFi & LibreLinkUp details", 240, 380);

  gfx.setTextColor(0x666666);
  gfx.setFont(&fonts::DejaVu18);
  gfx.drawCenterString("Build: " BUILD_VERSION, 240, 440);
}

// Convert a region code into the corresponding LibreLinkUp API hostname
String getApiHost(String region) {
  region.toLowerCase();
  region.trim();
  if (region == "us" || region == "") return "api.libreview.io";
  if (region == "ru") return "api.libreview.ru";
  
  // If they entered a full custom hostname/domain, use it directly
  if (region.indexOf('.') != -1) return region;
  return "api-" + region + ".libreview.io";
}

String sha256(const String &payload) {
  byte shaResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *)payload.c_str(), payload.length());
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  char str[65];
  for (int i = 0; i < 32; i++) {
    sprintf(&str[i*2], "%02x", (int)shaResult[i]);
  }
  str[64] = '\0';
  return String(str);
}

void loadPreferences() {
  Preferences preferences;
  preferences.begin("cgm-config", true); // Open in read-only mode
  
  String dev_name = preferences.getString("dev_name", "ESP32-CGM-Display");
  dev_name.toCharArray(device_name, sizeof(device_name));
  
  String email = preferences.getString("email", "");
  String password = preferences.getString("password", "");
  String region = preferences.getString("region", "eu");
  String units = preferences.getString("units", "mmol/L");
  llu_poll_interval = preferences.getInt("poll", 2);
  if (llu_poll_interval < 1) llu_poll_interval = 1;
  is_configured = preferences.getBool("configured", false);
  
  c_low = preferences.getFloat("c_low", 72.0);
  w_low = preferences.getFloat("w_low", 90.0);
  w_high = preferences.getFloat("w_high", 180.0);
  c_high = preferences.getFloat("c_high", 216.0);
  graph_min = preferences.getFloat("g_min", 40.0);
  graph_max = preferences.getFloat("g_max", 250.0);
  
  String admin_pwd = preferences.getString("admin_pwd", "");
  
  history_count = preferences.getInt("hist_count", 0);
  if (history_count > GRAPH_HISTORY_SIZE || history_count < 0) history_count = 0;
  if (history_count > 0) {
    size_t read_len = preferences.getBytes("history", glucose_history, sizeof(glucose_history));
    size_t expected_len = history_count * sizeof(float);
    if (read_len < expected_len) {
      history_count = read_len / sizeof(float);
    }
  }
  
  msg_rules_count = preferences.getInt("msg_rules_cnt", 0);
  if (msg_rules_count > 10 || msg_rules_count < 0) msg_rules_count = 0;
  if (msg_rules_count > 0) {
    size_t read_len = preferences.getBytes("msg_rules", msg_rules, sizeof(msg_rules));
    size_t expected_len = msg_rules_count * sizeof(MessageRule);
    if (read_len < expected_len) {
      msg_rules_count = read_len / sizeof(MessageRule);
    }
  }
  
  // Load Diabetes:M parameters
  dm_enable_connection = preferences.getBool("dm_conn_en", false);
  String dm_email_str = preferences.getString("dm_email", "");
  String dm_password_str = preferences.getString("dm_password", "");
  dm_enable_2fa = preferences.getBool("dm_enable_2fa", false);
  String dm_note_str = preferences.getString("dm_note", "Sent by ESP32");
  dm_auto_send = preferences.getBool("dm_auto_send", false);
  dm_api_interval = preferences.getInt("dm_poll", 30);
  if (dm_api_interval < 1) dm_api_interval = 30;
  dm_random_offset = preferences.getInt("dm_offset", 2);
  if (dm_random_offset < 0) dm_random_offset = 2;
  dm_enable_heartbeat = preferences.getBool("dm_hb_en", false);
  dm_heartbeat_interval = preferences.getInt("dm_hb_int", 15);
  if (dm_heartbeat_interval < 1) dm_heartbeat_interval = 15;
  String dm_start_str = preferences.getString("dm_start", "");
  String dm_stop_str = preferences.getString("dm_stop", "");
  String dm_tz_json_str = preferences.getString("dm_tz_json", "Europe/London");
  String dm_tz_posix_str = preferences.getString("dm_tz_posix", "GMT0BST,M3.5.0/1,M10.5.0/2");
  
  // Load cached auth
  String dm_token_str = preferences.getString("dm_token", "");
  String dm_cookies_str = preferences.getString("dm_cookies", "");
  String dm_user_id_str = preferences.getString("dm_user_id", "");
  String dm_last_ts_str = preferences.getString("dm_last_ts", "");
  
  dm_auto_category = preferences.getBool("dm_auto_cat", false);
  dm_fallback_category = preferences.getInt("dm_fallback_cat", 8);
  dm_b_start = preferences.getInt("dm_b_start", 480);
  dm_b_end = preferences.getInt("dm_b_end", 510);
  dm_l_start = preferences.getInt("dm_l_start", 720);
  dm_l_end = preferences.getInt("dm_l_end", 750);
  dm_d_start = preferences.getInt("dm_d_start", 1140);
  dm_d_end = preferences.getInt("dm_d_end", 1170);
  dm_2fa_pending = preferences.getBool("dm_2fa_pend", false);
  
  initializeDefaultCategories();
  
  int custom_count = preferences.getInt("dm_custom_cnt", 0);
  if (custom_count > 16) custom_count = 16;
  if (custom_count > 0) {
    size_t read_len = preferences.getBytes("dm_cats", &dm_categories[16], 16 * sizeof(DMCategory));
    int loaded_cnt = read_len / sizeof(DMCategory);
    if (loaded_cnt < custom_count) custom_count = loaded_cnt;
    dm_categories_count = 16 + custom_count;
    preferences.getBytes("dm_input_ids", dm_custom_input_ids, sizeof(dm_custom_input_ids));
  }
  
  dm_cat_rules_count = preferences.getInt("dm_cat_rules_cnt", 0);
  if (dm_cat_rules_count > 24 || dm_cat_rules_count < 0) dm_cat_rules_count = 0;
  if (dm_cat_rules_count > 0) {
    size_t read_len = preferences.getBytes("dm_cat_rules", dm_cat_rules, sizeof(dm_cat_rules));
    int loaded_rules = read_len / sizeof(CategoryTimeRule);
    if (loaded_rules < dm_cat_rules_count) dm_cat_rules_count = loaded_rules;
  } else {
    initializeDefaultCategoryRules();
  }
  
  preferences.end();
  
  email.toCharArray(llu_email, sizeof(llu_email));
  password.toCharArray(llu_password, sizeof(llu_password));
  region.toCharArray(llu_region, sizeof(llu_region));
  units.toCharArray(llu_units, sizeof(llu_units));
  
  dm_email_str.toCharArray(dm_email, sizeof(dm_email));
  dm_password_str.toCharArray(dm_password, sizeof(dm_password));
  dm_note_str.toCharArray(dm_note_text, sizeof(dm_note_text));
  dm_start_str.toCharArray(dm_start_sending, sizeof(dm_start_sending));
  dm_stop_str.toCharArray(dm_stop_sending, sizeof(dm_stop_sending));
  dm_tz_json_str.toCharArray(dm_timezone_json, sizeof(dm_timezone_json));
  dm_tz_posix_str.toCharArray(dm_timezone_posix, sizeof(dm_timezone_posix));
  
  dm_token_str.toCharArray(dm_token, sizeof(dm_token));
  dm_cookies_str.toCharArray(dm_cookies, sizeof(dm_cookies));
  dm_user_id_str.toCharArray(dm_user_id, sizeof(dm_user_id));
  dm_last_ts_str.toCharArray(dm_last_uploaded_libre_ts, sizeof(dm_last_uploaded_libre_ts));

  
  if (admin_pwd.length() > 0) {
    admin_pwd.toCharArray(admin_password, sizeof(admin_password));
    is_temp_password = false;
  } else {
    generateRandomPassword();
  }
  
  // Sanitization / safety check
  if (c_low <= 0.0) c_low = 72.0;
  if (w_low <= 0.0) w_low = 90.0;
  if (w_high <= 0.0) w_high = 180.0;
  if (c_high <= 0.0) c_high = 216.0;
  if (graph_min <= 0.0) graph_min = 40.0;
  if (graph_max <= 0.0) graph_max = 250.0;
  
  Serial.printf("Loaded config. Configured: %d, Region: %s, Units: %s, Poll: %d min, Email: %s\n", 
                is_configured, llu_region, llu_units, llu_poll_interval, llu_email);
  Serial.printf("Tolerances: crit_low=%.1f, warn_low=%.1f, warn_high=%.1f, crit_high=%.1f, graph_min=%.1f, graph_max=%.1f\n",
                c_low, w_low, w_high, c_high, graph_min, graph_max);
}

void pushToHistory(float val) {
  if (history_count < GRAPH_HISTORY_SIZE) {
    glucose_history[history_count] = val;
    history_count++;
  } else {
    // Shift left
    for (int i = 0; i < GRAPH_HISTORY_SIZE - 1; i++) {
      glucose_history[i] = glucose_history[i + 1];
    }
    glucose_history[GRAPH_HISTORY_SIZE - 1] = val;
  }
  
  Preferences preferences;
  preferences.begin("cgm-config", false);
  preferences.putBytes("history", glucose_history, sizeof(glucose_history));
  preferences.putInt("hist_count", history_count);
  preferences.end();
}

void runLibreLinkUpTest(String &logOut) {
  logOut += "<h3>LibreLinkUp API Test Connection</h3>";
  
  if (WiFi.status() != WL_CONNECTED) {
    logOut += "<p style='color:#D9534F;'><b>Error: WiFi not connected!</b> Please configure and connect WiFi first.</p>";
    return;
  }
  
  logOut += "<p>WiFi Status: Connected (IP: " + WiFi.localIP().toString() + ")</p>";
  
  // Test 1: Resolve hostname
  String host = getApiHost(llu_region);
  logOut += "<p>Selected Region: <b>" + String(llu_region) + "</b> (Host: " + host + ")</p>";
  
  logOut += "<p>Resolving host: " + host + "... ";
  IPAddress ip;
  if (WiFi.hostByName(host.c_str(), ip)) {
    logOut += "<span style='color:#5CB85C;'>OK</span> (IP: " + ip.toString() + ")</p>";
  } else {
    logOut += "<span style='color:#D9534F;'>FAILED</span></p>";
    logOut += "<p style='color:#D9534F;'><b>DNS Error:</b> Could not resolve hostname. Check your internet connection or region selection.</p>";
    return;
  }
  
  // Test 2: Login (with redirect handling)
  String token = "";
  String test_account_id_hash = "";
  
  int login_attempts = 0;
  while (login_attempts < 2) {
    login_attempts++;
    logOut += "<p>Attempting Login to LibreLinkUp... URL: https://" + host + "/llu/auth/login</p>";
    logOut += "<p>Using Email: " + String(llu_email) + "</p>";
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    
    String url = "https://" + host + "/llu/auth/login";
    if (!http.begin(client, url)) {
      logOut += "<p style='color:#D9534F;'><b>HTTP Error:</b> Could not initialize HTTP client.</p>";
      return;
    }
    
    http.addHeader("Content-Type", "application/json");
    http.addHeader("product", "llu.android");
    http.addHeader("version", "4.16.0");
    http.addHeader("Accept", "application/json");
    
    DynamicJsonDocument req_doc(512);
    req_doc["email"] = llu_email;
    req_doc["password"] = llu_password;
    
    String req_body;
    serializeJson(req_doc, req_body);
    
    int http_code = http.POST(req_body);
    logOut += "<p>HTTP Response Code: <b>" + String(http_code) + "</b></p>";
    
    if (http_code != 200) {
      logOut += "<p style='color:#D9534F;'><b>Login Failed!</b> Status code: " + String(http_code) + "</p>";
      String resp_err = http.getString();
      logOut += "<pre style='background:#1a1a1a;color:#eee;padding:10px;border:1px solid #444;overflow:auto;max-height:150px;'>" + resp_err + "</pre>";
      http.end();
      return;
    }
    
    String response = http.getString();
    http.end();
    
    logOut += "<p style='color:#5CB85C;'><b>Login HTTP Success!</b> Parsing JSON response...</p>";
    
    logOut += "<details><summary style='color:#33B5E5;cursor:pointer;'>Show Raw Login Response JSON (Redact token if sharing)</summary>";
    logOut += "<pre style='background:#1a1a1a;color:#eee;padding:10px;border:1px solid #444;overflow:auto;max-height:200px;white-space:pre-wrap;word-break:break-all;'>" + response + "</pre>";
    logOut += "</details><br/>";

    DynamicJsonDocument resp_doc(16384);
    DeserializationError err = deserializeJson(resp_doc, response);
    if (err) {
      logOut += "<p style='color:#D9534F;'><b>JSON Parse Error:</b> " + String(err.c_str()) + "</p>";
      return;
    }
    
    int status = resp_doc["status"].as<int>();
    logOut += "<p>JSON response status code: <b>" + String(status) + "</b></p>";
    if (status != 0) {
      logOut += "<p style='color:#D9534F;'><b>LibreLinkUp API Error:</b> Returned status " + String(status) + "</p>";
      return;
    }
    
    // Check for redirect
    if (resp_doc["data"].containsKey("redirect") && resp_doc["data"]["redirect"].as<bool>() == true) {
      String new_region = resp_doc["data"]["region"].as<String>();
      logOut += "<p style='color:#F0AD4E;'><b>API Redirected</b> to region: <b>" + new_region + "</b></p>";
      
      strncpy(llu_region, new_region.c_str(), sizeof(llu_region));
      Preferences preferences;
      preferences.begin("cgm-config", false);
      preferences.putString("region", llu_region);
      preferences.end();
      
      host = getApiHost(llu_region);
      logOut += "<p>Retrying login with new host: <b>" + host + "</b>...</p>";
      continue;
    }
    
    token = resp_doc["data"]["authTicket"]["token"].as<String>();
    logOut += "<p style='color:#5CB85C;'><b>Token obtained successfully!</b> Length: " + String(token.length()) + "</p>";
    
    String user_id = resp_doc["data"]["user"]["id"].as<String>();
    if (user_id != "null" && user_id.length() > 0) {
      test_account_id_hash = sha256(user_id);
      logOut += "<p>Obtained User ID: <b>" + user_id + "</b></p>";
      logOut += "<p>Computed Account-Id Hash: <b>" + test_account_id_hash + "</b></p>";
    } else {
      logOut += "<p style='color:#F0AD4E;'><b>Warning:</b> user.id not found in login response.</p>";
    }
    break;
  }
  
  if (token == "" || token == "null") {
    logOut += "<p style='color:#D9534F;'><b>Error:</b> Failed to obtain a valid auth token after login.</p>";
    return;
  }
  
  // Test 3: Fetch connections
  logOut += "<p>Attempting to fetch connections... URL: https://" + host + "/llu/connections</p>";
  String url = "https://" + host + "/llu/connections";
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, url)) {
    logOut += "<p style='color:#D9534F;'>HTTP Client start failed for connections.</p>";
    return;
  }
  
  http.addHeader("product", "llu.android");
  http.addHeader("version", "4.16.0");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + token);
  if (test_account_id_hash.length() > 0) {
    http.addHeader("Account-Id", test_account_id_hash);
  }
  
  int http_code = http.GET();
  logOut += "<p>HTTP Response Code: <b>" + String(http_code) + "</b></p>";
  
  String response = http.getString();
  http.end();
  
  if (http_code != 200) {
    logOut += "<p style='color:#D9534F;'><b>Connections Fetch Failed!</b> Code: " + String(http_code) + "</p>";
    logOut += "<pre style='background:#1a1a1a;color:#eee;padding:10px;border:1px solid #444;overflow:auto;max-height:150px;white-space:pre-wrap;'>" + response + "</pre>";
    return;
  }
  
  logOut += "<p style='color:#5CB85C;'><b>Connections HTTP Success!</b> Parsing JSON response...</p>";
  
  logOut += "<details><summary style='color:#33B5E5;cursor:pointer;'>Show Raw Connections Response JSON</summary>";
  logOut += "<pre style='background:#1a1a1a;color:#eee;padding:10px;border:1px solid #444;overflow:auto;max-height:200px;white-space:pre-wrap;word-break:break-all;'>" + response + "</pre>";
  logOut += "</details><br/>";

  DynamicJsonDocument conn_doc(16384);
  DeserializationError err = deserializeJson(conn_doc, response);
  if (err) {
    logOut += "<p style='color:#D9534F;'><b>JSON Parse Error:</b> " + String(err.c_str()) + "</p>";
    return;
  }
  
  int conn_status = conn_doc["status"].as<int>();
  if (conn_status != 0) {
    logOut += "<p style='color:#D9534F;'><b>Connections API Error status:</b> " + String(conn_status) + "</p>";
    return;
  }
  
  JsonArray connections = conn_doc["data"].as<JsonArray>();
  logOut += "<p>Found connections: <b>" + String(connections.size()) + "</b></p>";
  
  if (connections.size() == 0) {
    logOut += "<p style='color:#F0AD4E;'><b>Warning:</b> No connections linked to this follower account. Please ensure the patient has shared data with your follower email.</p>";
    return;
  }
  
  JsonObject connection = connections[0];
  String patient_name = connection["firstName"].as<String>() + " " + connection["lastName"].as<String>();
  logOut += "<p>First Connection Patient: <b>" + patient_name + "</b></p>";
  
  if (!connection.containsKey("glucoseMeasurement")) {
    logOut += "<p style='color:#D9534F;'><b>Error:</b> Connection data does not contain a glucoseMeasurement. Check if the sensor is active.</p>";
    return;
  }
  
  JsonObject measurement = connection["glucoseMeasurement"];
  float val = measurement.containsKey("ValueInMgPerDl") ? measurement["ValueInMgPerDl"].as<float>() : 0.0;
  if (val == 0.0) {
    val = measurement["Value"].as<float>();
  }
  if (val < 30.0 && val > 0.0) {
    val = val * 18.0182;
  }
  int trend = measurement["TrendArrow"].as<int>();
  String ts = measurement["Timestamp"].as<String>();
  
  logOut += "<div style='background:#2d4f2d;color:#a6d7a6;padding:15px;border:1px solid #3c763d;border-radius:4px;margin-top:15px;'>";
  logOut += "<h4>Connection Verified & Successful!</h4>";
  logOut += "<p>Latest Glucose Value: <b>" + String(val) + " mg/dL</b> (" + String(val / 18.0182, 1) + " mmol/L)</p>";
  logOut += "<p>Trend Arrow Index: <b>" + String(trend) + "</b></p>";
  logOut += "<p>Timestamp: <b>" + ts + "</b></p>";
  logOut += "</div>";
}

void drawPendingConfigScreen() {
  gfx.fillScreen(0x181A1B);
  
  gfx.setFont(&fonts::DejaVu24);
  gfx.setTextColor(0xFFFFFF);
  gfx.drawCenterString("Configuration Pending", 240, 100);
  
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0xAAAAAA);
  gfx.drawCenterString("Please configure LibreLinkUp settings", 240, 160);
  gfx.drawCenterString("via the local admin portal:", 240, 190);
  
  gfx.setFont(&fonts::DejaVu24);
  gfx.setTextColor(0x33B5E5);
  gfx.drawCenterString("http://" + WiFi.localIP().toString(), 240, 240);
  
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0xAAAAAA);
  gfx.drawCenterString("Username: admin", 240, 300);
  
  if (is_temp_password) {
    gfx.setTextColor(0xF0AD4E); // Highlight password in yellow/orange
    gfx.drawCenterString("Password: " + String(admin_password), 240, 335);
  } else {
    gfx.setTextColor(0x5CB85C); // Green
    gfx.drawCenterString("Password: <custom password set>", 240, 335);
  }
  
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0x666666);
  gfx.drawCenterString("Build: " BUILD_VERSION, 240, 410);
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Firmware signature: %s\n", ota_signature);
  
  // Initialize the LovyanGFX display
  gfx.init();
  gfx.setRotation(0);
  
  // Set backlight pin GPIO 38 as output and turn it ON
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
  
  // Render splash screen
  drawSplashScreen();
  
  // Wait 3 seconds and require continuous screen touch to allow resetting configurations
  bool touch_detected = false;
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0x888888);
  
  int touch_count = 0;
  for (int i = 0; i < 30; i++) {
    uint16_t tx, ty;
    if (gfx.getTouch(&tx, &ty) && (tx > 0 || ty > 0)) {
      touch_count++;
    }
    int remaining = 3 - (i / 10);
    gfx.fillRect(0, 360, 480, 40, 0x121212);
    gfx.drawCenterString("Hold screen to reset ( " + String(remaining) + "s )", 240, 370);
    delay(100);
  }
  
  if (touch_count >= 25) { // Must be held for at least 2.5 seconds (25 out of 30 polls)
    touch_detected = true;
  }
  
  if (touch_detected) {
    if (confirmFactoryResetHMI()) {
      gfx.fillScreen(0x181A1B);
      gfx.setTextColor(0xD9534F); // Red
      gfx.setFont(&fonts::DejaVu24);
      gfx.drawCenterString("Resetting configurations...", 240, 240);
      
      // Clear NVS configuration
      Preferences preferences;
      preferences.begin("cgm-config", false);
      preferences.clear();
      preferences.end();
      
      // Reset WiFi
      WiFi.disconnect(true, true);
      delay(1500);
      ESP.restart();
    } else {
      gfx.fillScreen(0x181A1B);
      gfx.setTextColor(0x5CB85C); // Green
      gfx.setFont(&fonts::DejaVu24);
      gfx.drawCenterString("Reset Canceled", 240, 200);
      gfx.setFont(&fonts::DejaVu18);
      gfx.setTextColor(0x888888);
      gfx.drawCenterString("Booting normally...", 240, 260);
      delay(1500);
    }
  }
  
  // Load preferences from NVS
  loadPreferences();
  
  // Set defaults in memory if they are not already set
  if (strlen(llu_region) == 0) strcpy(llu_region, "eu");
  if (strlen(llu_units) == 0) strcpy(llu_units, "mmol/L");

  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180); // Timeout portal after 3 minutes if no activity
  
  WiFi.setHostname(device_name);
  
  gfx.fillRect(0, 360, 480, 40, 0x121212);
  gfx.setTextColor(0x888888);
  gfx.drawCenterString("Connecting to WiFi...", 240, 370);
  
  // Attempt autoconnect. If no credentials exist or connection fails, it starts Config Portal.
  if (!wm.autoConnect("ESP32-CGM-Config")) {
    Serial.println("Failed to connect to WiFi. Rebooting...");
    delay(2000);
    ESP.restart();
  }
  
  // Initialize NTP time sync
  initTimeNTP();
  
  // Show successful connection on boot
  gfx.fillRect(0, 360, 480, 40, 0x121212);
  gfx.setTextColor(0x5CB85C);
  gfx.drawCenterString("Connected!", 240, 370);
  delay(1000);
  
  // Start the local WebServer
  localServer.on("/", handleLocalRoot);
  localServer.on("/param", handleLocalParam);
  localServer.on("/save", handleLocalSave);
  localServer.on("/test", handleLocalTest);
  localServer.on("/reboot", handleLocalReboot);
  localServer.on("/update", HTTP_GET, handleLocalUpdateGet);
  localServer.on("/update", HTTP_POST, handleLocalUpdatePost, handleLocalUpdateUpload);
  localServer.on("/change-password", HTTP_GET, handleLocalChangePasswordGet);
  localServer.on("/save-password", HTTP_POST, handleLocalSavePasswordPost);
  localServer.on("/factory-reset", handleLocalFactoryReset);
  localServer.on("/general", HTTP_GET, handleLocalGeneralGet);
  localServer.on("/save-general", HTTP_POST, handleLocalSaveGeneralPost);
  localServer.on("/hardware", HTTP_GET, handleLocalHardwareGet);
  localServer.on("/debug-info", HTTP_GET, handleLocalDebugGet);
  localServer.on("/export-config", HTTP_GET, handleLocalExportConfig);
  localServer.on("/import-config", HTTP_GET, handleLocalImportConfigGet);
  localServer.on("/import-config", HTTP_POST, handleLocalImportConfig);
  localServer.on("/diabetes-m", HTTP_GET, handleLocalDMGet);
  localServer.on("/save-diabetes-m", HTTP_POST, handleLocalDMSave);
  localServer.on("/test-dm-connection", HTTP_POST, handleLocalDMTestConnection);
  localServer.on("/test-dm-upload", HTTP_POST, handleLocalDMTestUpload);
  localServer.on("/debug-dm", HTTP_GET, handleLocalDebugDM);
  localServer.on("/wifi", HTTP_GET, handleLocalWifiGet);
  localServer.on("/save-wifi", HTTP_POST, handleLocalWifiSave);
  localServer.begin();
  Serial.print("Local WebServer started on IP: ");
  Serial.println(WiFi.localIP());
  
  // Make initial API call if credentials exist
  if (strlen(llu_email) > 0 && strlen(llu_password) > 0) {
    gfx.fillScreen(0x181A1B);
    gfx.setFont(&fonts::DejaVu24);
    gfx.setTextColor(0xFFFFFF);
    gfx.drawCenterString("Fetching CGM Data...", 240, 240);
    
    libreLinkUpFetchData();
    drawDashboard();
  } else {
    drawPendingConfigScreen();
  }
}

void loop() {
  static unsigned long last_poll = millis();
  static unsigned long last_sec_update = millis();
  
  bool has_credentials = (strlen(llu_email) > 0 && strlen(llu_password) > 0);

  // Periodically poll LibreLinkUp API every llu_poll_interval minutes
  if (has_credentials && (millis() - last_poll >= (unsigned long)llu_poll_interval * 60000)) {
    last_poll = millis();
    Serial.println("Timer triggered. Polling LibreLinkUp API...");
    
    // Draw visual indicator during fetch
    gfx.setFont(&fonts::DejaVu18);
    gfx.setTextColor(0x33B5E5);
    gfx.drawCenterString("Syncing...", 240, 310);
    
    bool success = libreLinkUpFetchData();
    drawDashboard(); // Redraw dashboard to update values/graphics
    
    if (!success) {
      gfx.setFont(&fonts::DejaVu18);
      gfx.setTextColor(0xD9534F);
      gfx.drawCenterString("Update Failed!", 240, 310);
    }
  }
  
  // Update top status bar every 5 seconds
  if (has_credentials && (millis() - last_sec_update >= 5000)) {
    last_sec_update = millis();
    drawTopStatusBar();
  }
  
  // Touch screen interaction: Force immediate poll/refresh or diagnostics
  uint16_t tx, ty;
  static bool was_touched = false;
  static unsigned long touch_start_time = 0;
  static bool long_press_triggered = false;
  
  bool is_touched = gfx.getTouch(&tx, &ty);
  
  // Filter out invalid/phantom 0,0 touches
  if (is_touched && tx == 0 && ty == 0) {
    is_touched = false;
  }
  
  if (has_credentials) {
    if (is_touched) {
      if (!was_touched) {
        touch_start_time = millis();
        long_press_triggered = false;
      } else {
        if (!long_press_triggered && (millis() - touch_start_time >= 1500)) {
          long_press_triggered = true;
          Serial.println("Long press detected. Showing diagnostics screen...");
          showDiagnosticsScreen();
          is_touched = false;
        }
      }
    } else {
      if (was_touched && !long_press_triggered) {
        static unsigned long last_touch = 0;
        if (millis() - last_touch > 60000) { // Throttle manual refreshes to 60-second intervals
          last_touch = millis();
          Serial.println("Screen touched. Forcing immediate update...");
          
          gfx.setFont(&fonts::DejaVu18);
          gfx.setTextColor(0x33B5E5);
          gfx.drawCenterString("Refreshing...", 240, 310);
          
          bool success = libreLinkUpFetchData();
          drawDashboard();
          
          if (!success) {
            gfx.setFont(&fonts::DejaVu18);
            gfx.setTextColor(0xD9534F);
            gfx.drawCenterString("Refresh Failed!", 240, 310);
          }
          
          last_poll = millis(); // Reset regular timer
        }
      }
    }
  }
  was_touched = is_touched;
  
  // Diabetes:M background upload task
  if (dm_enable_connection && dm_auto_send && !dm_2fa_pending) {
    if (dm_next_send_epoch == 0 && isTimeSynced()) {
      recalculateNextSendTime();
    }
    
    if (dm_next_send_epoch > 0 && isTimeSynced() && time(nullptr) >= dm_next_send_epoch) {
      time_t now = time(nullptr);
      bool in_window = true;
      
      if (strlen(dm_start_sending) > 0) {
        time_t start_t = parseDateTime(dm_start_sending);
        if (start_t > 0 && now < start_t) {
          in_window = false;
          dm_last_sent_status = "Awaiting start time";
        }
      }
      
      if (strlen(dm_stop_sending) > 0) {
        time_t stop_t = parseDateTime(dm_stop_sending);
        if (stop_t > 0 && now > stop_t) {
          in_window = false;
          dm_last_sent_status = "Auto send ended";
          dm_next_send_epoch = 0; // stop scheduling checks
        }
      }
      
      if (in_window) {
        bool can_send = true;
        String fail_reason = "";
        
        if (last_fetch_time == 0 || last_timestamp.length() == 0) {
          can_send = false;
          fail_reason = "No Libre reading";
        } else if (strcmp(dm_last_uploaded_libre_ts, last_timestamp.c_str()) == 0) {
          can_send = false;
          fail_reason = "Duplicate reading";
        }
        
        if (can_send) {
          String upload_err = "";
          Serial.printf("[DM] Auto uploading glucose %.1f mg/dL\n", last_glucose);
          bool success = uploadWithRetry(last_glucose, dm_note_text, upload_err);
          
          dm_last_sent_epoch = time(nullptr);
          dm_last_sent_value = last_glucose;
          
          if (success) {
            dm_last_sent_status = "Sent OK";
            strncpy(dm_last_uploaded_libre_ts, last_timestamp.c_str(), sizeof(dm_last_uploaded_libre_ts));
            
            // Save last ts to NVS
            Preferences preferences;
            preferences.begin("cgm-config", false);
            preferences.putString("dm_last_ts", dm_last_uploaded_libre_ts);
            preferences.end();
          } else {
            dm_last_sent_status = upload_err;
          }
          
          addDMLog(dm_last_sent_epoch, last_glucose, dm_last_sent_status);
          drawTopStatusBar();
        } else {
          Serial.printf("[DM] Skip upload: %s\n", fail_reason.c_str());
          if (last_fetch_time == 0 || last_timestamp.length() == 0) {
            // Wait for initial LibreLinkUp poll
            dm_next_send_epoch = time(nullptr) + 60;
          }
        }
      }
      
      if (dm_next_send_epoch > 0) {
        recalculateNextSendTime();
      }
    }
  }

  // Diabetes:M background heartbeat task
  if (dm_enable_connection && dm_enable_heartbeat && !dm_2fa_pending && strlen(dm_email) > 0 && strlen(dm_password) > 0) {
    if (dm_next_heartbeat_epoch == 0 && isTimeSynced()) {
      dm_next_heartbeat_epoch = time(nullptr) + (dm_heartbeat_interval * 60);
      Serial.printf("[DM] Heartbeat initialized. Next check at epoch: %ld\n", (long)dm_next_heartbeat_epoch);
    }
    
    if (dm_next_heartbeat_epoch > 0 && isTimeSynced() && time(nullptr) >= dm_next_heartbeat_epoch) {
      Serial.println("[DM] Running scheduled heartbeat...");
      String profile_info = "";
      bool profile_ok = diabetesMGetProfile(profile_info);
      
      dm_last_heartbeat_epoch = time(nullptr);
      
      if (!profile_ok) {
        if (dm_enable_2fa) {
          dm_last_heartbeat_status = "2FA Re-auth required";
          set2FAPending(true);
        } else {
          Serial.println("[DM] Heartbeat failed. Token may be expired. Attempting silent re-login...");
          String login_err = "";
          bool login_ok = diabetesMLogin(nullptr, login_err);
          if (login_ok) {
            Serial.println("[DM] Heartbeat re-login successful! Retrying profile fetch...");
            profile_ok = diabetesMGetProfile(profile_info);
            if (profile_ok) {
              dm_last_heartbeat_status = "Success (Silent Re-auth)";
              String cat_err = "";
              diabetesMGetCategories(cat_err);
            } else {
              dm_last_heartbeat_status = "Profile failed after re-login";
            }
          } else {
            dm_last_heartbeat_status = "Re-login failed: " + login_err;
            Serial.printf("[DM] Heartbeat silent re-login failed: %s\n", login_err.c_str());
          }
        }
      } else {
        dm_last_heartbeat_status = "Success (Active)";
        Serial.println("[DM] Heartbeat success. Session is active.");
        String cat_err = "";
        diabetesMGetCategories(cat_err);
      }
      
      addDMHeartbeatLog(dm_last_heartbeat_epoch, dm_last_heartbeat_status);
      dm_next_heartbeat_epoch = time(nullptr) + (dm_heartbeat_interval * 60);
      Serial.printf("[DM] Next heartbeat scheduled in %d minutes.\n", dm_heartbeat_interval);
    }
  }

  // Handle background WiFi reconnection if connection drops
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long last_reconnect = 0;
    if (millis() - last_reconnect > 10000) {
      last_reconnect = millis();
      Serial.println("WiFi disconnected. Reconnecting...");
      WiFi.begin();
    }
  }
  
  // Handle local WebServer client connections
  if (WiFi.status() == WL_CONNECTED) {
    localServer.handleClient();
  }
  
  // If not configured, keep showing the pending screen
  if (!has_credentials) {
    static unsigned long last_config_draw = 0;
    if (millis() - last_config_draw > 5000) {
      last_config_draw = millis();
      drawPendingConfigScreen();
    }
  }
  
  delay(50);
}

void drawSplashScreen() {
  gfx.fillScreen(0x121212); // Deep black
  
  gfx.setFont(&fonts::DejaVu40);
  gfx.setTextColor(0xFFFFFF);
  gfx.drawCenterString("Libre Monitor", 240, 160);
  
  gfx.setFont(&fonts::DejaVu24);
  gfx.setTextColor(0x5CB85C); // Green
  gfx.drawCenterString("Standalone CGM", 240, 220);
  
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0x666666);
  gfx.drawCenterString("Guition ESP32 HMI", 240, 270);
  gfx.drawCenterString("Build: " BUILD_VERSION, 240, 310);
}

void drawDashboard() {
  // Clear the screen with deep black
  gfx.fillScreen(0x000000); 
  
  // Set alert color coding based on thresholds and stale status
  uint32_t status_color = 0x888888; // Default Grey
  unsigned long elapsed_m = (millis() - last_fetch_time) / 60000;
  bool is_stale = (elapsed_m >= 15) || (last_fetch_time == 0);
  
  if (is_stale) {
    status_color = 0x888888; // Grey
  } else {
    if (last_glucose < c_low || last_glucose > c_high) {
      status_color = 0xD9534F; // Red (Critical)
    } else if ((last_glucose >= c_low && last_glucose < w_low) || (last_glucose > w_high && last_glucose <= c_high)) {
      status_color = 0xF0AD4E; // Orange/Yellow (Warning)
    } else {
      status_color = 0x5CB85C; // Green (Normal)
    }
  }
  
  // Evaluate custom messages
  String banner_msg = "";
  for (int i = 0; i < msg_rules_count; i++) {
    bool match = false;
    if (strcmp(msg_rules[i].type, "gt") == 0) {
      if (last_glucose > msg_rules[i].val1) match = true;
    } else if (strcmp(msg_rules[i].type, "lt") == 0) {
      if (last_glucose < msg_rules[i].val1) match = true;
    } else if (strcmp(msg_rules[i].type, "between") == 0) {
      if (last_glucose >= msg_rules[i].val1 && last_glucose <= msg_rules[i].val2) match = true;
    }
    if (match) {
      banner_msg = String(msg_rules[i].text);
      break;
    }
  }
  
  int banner_top = (dm_enable_connection && dm_auto_send) ? (dm_2fa_pending ? 72 : 56) : 40;
  int banner_bottom = 180;
  int banner_height = banner_bottom - banner_top;
  
  // 1. Draw the Banner Background: Y = [banner_top, banner_bottom]
  gfx.fillRect(0, banner_top, 480, banner_height, status_color);
  
  // 2. Display the glucose reading in black text on the banner background
  gfx.setTextColor(0x000000);
  gfx.setFont(&fonts::DejaVu72); // Massive anti-aliased digits
  
  String glucose_str = "---";
  if (!is_stale) {
    if (strcmp(llu_units, "mmol/L") == 0) {
      float mmol = last_glucose / 18.0182;
      glucose_str = String(mmol, 1);
    } else {
      glucose_str = String((int)last_glucose);
    }
  }
  
  int val_y = banner_top + 30;
  int arrow_y = banner_top + 70;
  if (banner_msg.length() > 0 && !is_stale) {
    val_y = banner_top + 15;
    arrow_y = banner_top + 55;
  }
  gfx.drawCenterString(glucose_str.c_str(), 210, val_y);
  
  // 3. Draw the trend arrow in black on the banner background
  if (!is_stale) {
    drawTrendArrow(330, arrow_y, last_trend, 0x000000);
  }
  
  // 4. Draw the custom short message centered under the value
  if (banner_msg.length() > 0 && !is_stale) {
    gfx.setFont(&fonts::DejaVu18);
    gfx.setTextColor(0x000000);
    gfx.drawCenterString(banner_msg.c_str(), 240, banner_top + 105);
  }
  
  // 4. Render the expanded history graph (on black background)
  drawHistoryGraph();
  
  // 5. Draw the top status bar (black-text-on-white)
  drawTopStatusBar();
}

void drawThickLine(int x1, int y1, int x2, int y2, int thickness, uint32_t color) {
  if (thickness <= 1) {
    gfx.drawLine(x1, y1, x2, y2, color);
    return;
  }
  
  for (int i = -thickness/2; i <= thickness/2; i++) {
    if (abs(x1 - x2) > abs(y1 - y2)) { // Horizontal-ish
      gfx.drawLine(x1, y1 + i, x2, y2 + i, color);
    } else { // Vertical-ish
      gfx.drawLine(x1 + i, y1, x2 + i, y2, color);
    }
  }
}

void drawTrendArrow(int x, int y, int trend, uint32_t color) {
  int len = 25;       // Made larger to match value size
  int arrow_sz = 12;  // Made larger to match value size
  int thickness = 6;  // Thicker lines
  
  switch (trend) {
    case 1: // Down-Down (Falling rapidly) -> straight down
      drawThickLine(x, y - len, x, y + len, thickness, color);
      gfx.fillTriangle(x, y + len + 3, x - arrow_sz, y + len - arrow_sz, x + arrow_sz, y + len - arrow_sz, color);
      break;
      
    case 2: // Down (Falling) -> diagonal down-right
      drawThickLine(x - len/2 - 4, y - len/2 - 4, x + len/2 + 4, y + len/2 + 4, thickness, color);
      gfx.fillTriangle(x + len/2 + 5, y + len/2 + 5,
                       x + len/2 - arrow_sz, y + len/2,
                       x + len/2, y + len/2 - arrow_sz, color);
      break;
      
    case 3: // Flat (Stable) -> horizontal right
      drawThickLine(x - len, y, x + len, y, thickness, color);
      gfx.fillTriangle(x + len + 3, y, x + len - arrow_sz, y - arrow_sz, x + len - arrow_sz, y + arrow_sz, color);
      break;
      
    case 4: // Up (Rising) -> diagonal up-right
      drawThickLine(x - len/2 - 4, y + len/2 + 4, x + len/2 + 4, y - len/2 - 4, thickness, color);
      gfx.fillTriangle(x + len/2 + 5, y - len/2 - 5,
                       x + len/2 - arrow_sz, y - len/2,
                       x + len/2, y - len/2 + arrow_sz, color);
      break;
      
    case 5: // Up-Up (Rising rapidly) -> straight up
      drawThickLine(x, y + len, x, y - len, thickness, color);
      gfx.fillTriangle(x, y - len - 3, x - arrow_sz, y - len + arrow_sz, x + arrow_sz, y - len + arrow_sz, color);
      break;
      
    default: // Unknown / Flat
      drawThickLine(x - len, y, x + len, y, thickness, color);
      break;
  }
}

int mapGlucoseToY(float val) {
  // Graph bounds: Y = [200, 465] (height = 265px)
  if (val < graph_min) val = graph_min;
  if (val > graph_max) val = graph_max;
  float range = graph_max - graph_min;
  if (range <= 0.0) range = 1.0;
  return 465 - (int)((val - graph_min) * (265.0 / range));
}

void drawHistoryGraph() {
  int startX = 15;
  int endX = 465;
  int graphW = endX - startX;
  int startY = 200;
  int endY = 465;
  
  // Draw base axis line
  gfx.drawLine(startX, endY, endX, endY, 0x444444);
  
  // Set threshold strings based on configured units
  String low_label = "";
  String high_label = "";
  if (strcmp(llu_units, "mmol/L") == 0) {
    low_label = String(w_low / 18.0182, 1);
    high_label = String(w_high / 18.0182, 1);
  } else {
    low_label = String((int)w_low);
    high_label = String((int)w_high);
  }

  if (history_count == 0) {
    gfx.setFont(&fonts::DejaVu18);
    gfx.setTextColor(0x666666);
    gfx.drawCenterString("Awaiting history data...", 240, startY + 100);
    return;
  }
  
  // Plot historical bars (no pixel gap)
  int barW = graphW / GRAPH_HISTORY_SIZE; // 450 / 90 = 5 pixels
  
  for (int i = 0; i < history_count; i++) {
    int px = startX + i * barW;
    int py = mapGlucoseToY(glucose_history[i]);
    
    // Choose color code for each specific point
    uint32_t pt_color = 0x5CB85C; // Green
    if (glucose_history[i] < c_low || glucose_history[i] > c_high) {
      pt_color = 0xD9534F; // Red (Critical)
    } else if ((glucose_history[i] >= c_low && glucose_history[i] < w_low) || (glucose_history[i] > w_high && glucose_history[i] <= c_high)) {
      pt_color = 0xF0AD4E; // Orange/Yellow (Warning)
    }
    
    int barH = endY - py;
    if (barH > 0) {
      gfx.fillRect(px, py, barW, barH, pt_color);
    }
  }

  // Draw dotted threshold line for Low Warning (w_low) on top of bars
  int y_low = mapGlucoseToY(w_low);
  for (int x = startX; x < endX; x += 6) {
    gfx.drawFastHLine(x, y_low, 3, 0xAA2222); // Darker red dash overlay
  }
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0xD9534F, 0x000000); // Bright red text on black background for overlay readability
  gfx.drawString(low_label, startX + 8, y_low - 7);
  
  // Draw dotted threshold line for High Warning (w_high) on top of bars
  int y_high = mapGlucoseToY(w_high);
  for (int x = startX; x < endX; x += 6) {
    gfx.drawFastHLine(x, y_high, 3, 0xAA7722); // Darker yellow/orange dash overlay
  }
  gfx.setTextColor(0xF0AD4E, 0x000000); // Bright orange/yellow text on black background
  gfx.drawString(high_label, startX + 8, y_high - 7);
}

void drawTopStatusBar() {
  int bar_height = (dm_enable_connection && dm_auto_send) ? (dm_2fa_pending ? 72 : 56) : 40;
  gfx.fillRect(0, 0, 480, bar_height, 0xFFFFFF); // White background
  
  // Line 1: LibreLinkUp status & WiFi
  gfx.setFont(&fonts::DejaVu18);
  String msg = "";
  if (last_fetch_time == 0) {
    gfx.setTextColor(0x000000); // Black text
    msg = "Awaiting Data (" + String(llu_units) + ")";
  } else {
    unsigned long elapsed_m = (millis() - last_fetch_time) / 60000;
    if (elapsed_m >= 15) {
      gfx.setTextColor(0xCC0000); // Dark red warning for stale readings
    } else {
      gfx.setTextColor(0x000000); // Black text
    }
    String timeStr = formatTimestamp(last_timestamp);
    msg = "Updated " + timeStr + " (" + String(llu_units) + ")";
  }
  gfx.drawString(msg, 15, (dm_enable_connection && dm_auto_send) ? 6 : 10);
  
  int rssi = WiFi.RSSI();
  uint32_t wifi_color = 0x008800; // Dark green
  
  if (WiFi.status() != WL_CONNECTED) {
    gfx.setTextColor(0xCC0000); // Dark red
    gfx.drawString("WiFi Disconnected", 310, (dm_enable_connection && dm_auto_send) ? 6 : 10);
  } else {
    if (rssi < -78) {
      wifi_color = 0xBB5500; // Dark orange if signal is weak
    }
    gfx.setTextColor(wifi_color);
    gfx.drawString("WiFi Connected", 330, (dm_enable_connection && dm_auto_send) ? 6 : 10);
  }
  
  // Line 2: Diabetes:M background status
  if (dm_enable_connection && dm_auto_send) {
    gfx.setFont(&fonts::DejaVu18); // anti-aliased 18px font (DejaVu14 not available)
    gfx.setTextColor(0x000000);
    
    String next_str = "--:--";
    if (dm_next_send_epoch > 0) {
      if (isTimeSynced()) {
        next_str = formatLocalTime(dm_next_send_epoch, "%H:%M");
      } else {
        next_str = "Pending Sync";
      }
    }
    
    String last_sent_str = "";
    if (dm_last_sent_epoch > 0) {
      String time_s = formatLocalTime(dm_last_sent_epoch, "%H:%M");
      String val_s = "";
      if (strcmp(llu_units, "mmol/L") == 0) {
        val_s = String(dm_last_sent_value / 18.0182, 1);
      } else {
        val_s = String((int)dm_last_sent_value);
      }
      last_sent_str = time_s + " (" + val_s + ") ";
    } else {
      last_sent_str = "None ";
    }
    
    String line2_left = "Last: " + last_sent_str;
    String line2_right = "Next: " + next_str;
    
    gfx.drawString(line2_left, 15, 31);
    gfx.drawString(line2_right, 330, 31);
    
    if (dm_2fa_pending) {
      gfx.fillRect(0, 52, 480, 20, 0xCC0000); // Solid red background
      gfx.setTextColor(0xFFFFFF); // White text
      gfx.drawString("Diabetes:M - Please re-authenticate", 15, 53);
    }
  }
}

bool libreLinkUpLogin() {
  WiFiClientSecure client;
  client.setInsecure(); // Bypass cert verification due to low memory constraints
  HTTPClient http;
  
  String host = getApiHost(llu_region);
  String url = "https://" + host + "/llu/auth/login";
  
  Serial.printf("Connecting to LLU Login: %s\n", url.c_str());
  
  if (!http.begin(client, url)) {
    Serial.println("HTTP start failed on login.");
    return false;
  }
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("product", "llu.android");
  http.addHeader("version", "4.16.0");
  http.addHeader("Accept", "application/json");
  
  // Assemble the login request payload
  DynamicJsonDocument req_doc(512);
  req_doc["email"] = llu_email;
  req_doc["password"] = llu_password;
  
  String req_body;
  serializeJson(req_doc, req_body);
  
  int http_code = http.POST(req_body);
  if (http_code != 200) {
    Serial.printf("LLU Login POST failed, HTTP code: %d\n", http_code);
    http.end();
    return false;
  }
  
  String response = http.getString();
  http.end();
  
  DynamicJsonDocument resp_doc(16384);
  DeserializationError err = deserializeJson(resp_doc, response);
  if (err) {
    Serial.printf("JSON parse failed on login response: %s\n", err.c_str());
    return false;
  }
  
  int status = resp_doc["status"].as<int>();
  if (status != 0) {
    Serial.printf("LLU Login API returned error status: %d\n", status);
    return false;
  }
  
  // Check for redirect
  if (resp_doc["data"].containsKey("redirect") && resp_doc["data"]["redirect"].as<bool>() == true) {
    String new_region = resp_doc["data"]["region"].as<String>();
    Serial.printf("API redirected. New region: %s\n", new_region.c_str());
    strncpy(llu_region, new_region.c_str(), sizeof(llu_region));
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putString("region", llu_region);
    preferences.end();
    
    static int redirect_count = 0;
    if (redirect_count < 2) {
      redirect_count++;
      bool success = libreLinkUpLogin();
      redirect_count = 0;
      return success;
    }
    return false;
  }
  
  auth_token = resp_doc["data"]["authTicket"]["token"].as<String>();
  token_expires = resp_doc["data"]["authTicket"]["expires"].as<uint32_t>();
  
  String user_id = resp_doc["data"]["user"]["id"].as<String>();
  if (user_id != "null" && user_id.length() > 0) {
    account_id_hash = sha256(user_id);
    Serial.printf("LLU Login Success! Token saved. Account-Id Hash: %s\n", account_id_hash.c_str());
  } else {
    account_id_hash = "";
    Serial.println("LLU Login Success! Token saved. Warning: user.id not found.");
  }
  return true;
}

bool libreLinkUpFetchData() {
  last_glucose = 0.0; // Reset to 0 on new fetch attempt (internet loss check)
  llu_last_fetch_attempt_epoch = time(nullptr);
  llu_last_fetch_status = "Fetching...";

  // Verify token validity or log in
  if (auth_token == "" || (token_expires > 0 && (uint32_t)(millis() / 1000) >= token_expires)) {
    Serial.println("No valid LLU token. Logging in first...");
    if (!libreLinkUpLogin()) {
      llu_last_fetch_status = "Login Failed";
      return false;
    }
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  String host = getApiHost(llu_region);
  String url = "https://" + host + "/llu/connections";
  
  Serial.printf("Connecting to LLU Connections: %s\n", url.c_str());
  
  if (!http.begin(client, url)) {
    Serial.println("HTTP start failed on connections.");
    llu_last_fetch_status = "HTTP Connections Begin Failed";
    return false;
  }
  
  http.addHeader("product", "llu.android");
  http.addHeader("version", "4.16.0");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + auth_token);
  if (account_id_hash.length() > 0) {
    http.addHeader("Account-Id", account_id_hash);
  }
  
  int http_code = http.GET();
  
  // If authorization expired, re-login and retry request
  if (http_code == 401) {
    Serial.println("Received 401 Unauthorized. Retrying Login...");
    http.end();
    
    if (libreLinkUpLogin()) {
      if (!http.begin(client, url)) {
        llu_last_fetch_status = "HTTP Connections Retry Begin Failed";
        return false;
      }
      http.addHeader("product", "llu.android");
      http.addHeader("version", "4.16.0");
      http.addHeader("Accept", "application/json");
      http.addHeader("Authorization", "Bearer " + auth_token);
      if (account_id_hash.length() > 0) {
        http.addHeader("Account-Id", account_id_hash);
      }
      http_code = http.GET();
    } else {
      llu_last_fetch_status = "Retry Login Failed";
      return false;
    }
  }
  
  if (http_code != 200) {
    Serial.printf("LLU Connections GET failed, HTTP code: %d\n", http_code);
    http.end();
    llu_last_fetch_status = "HTTP GET Error: " + String(http_code);
    return false;
  }
  
  String response = http.getString();
  http.end();
  
  DynamicJsonDocument resp_doc(16384);
  DeserializationError err = deserializeJson(resp_doc, response);
  if (err) {
    Serial.println("JSON parse failed on LLU connections response.");
    llu_last_fetch_status = "JSON Parse Error";
    return false;
  }
  
  int status = resp_doc["status"].as<int>();
  if (status != 0) {
    Serial.printf("LLU Connections API returned error status: %d\n", status);
    llu_last_fetch_status = "API Error status: " + String(status);
    return false;
  }
  
  JsonArray connections = resp_doc["data"].as<JsonArray>();
  if (connections.size() == 0) {
    Serial.println("Connections array is empty.");
    llu_last_fetch_status = "Connections Empty";
    return false;
  }
  
  JsonObject connection = connections[0];
  if (!connection.containsKey("glucoseMeasurement")) {
    Serial.println("Connection data does not contain a glucoseMeasurement.");
    llu_last_fetch_status = "No glucoseMeasurement";
    return false;
  }
  
  JsonObject measurement = connection["glucoseMeasurement"];
  float value = measurement.containsKey("ValueInMgPerDl") ? measurement["ValueInMgPerDl"].as<float>() : 0.0;
  if (value == 0.0) {
    value = measurement["Value"].as<float>();
  }
  if (value < 30.0 && value > 0.0) {
    value = value * 18.0182;
  }
  int trend = measurement["TrendArrow"].as<int>();
  String timestamp = measurement["Timestamp"].as<String>();
  
  // Store global values
  last_glucose = value;
  last_trend = trend;
  last_timestamp = timestamp;
  last_fetch_time = millis();
  llu_last_fetch_success_epoch = time(nullptr);
  llu_last_fetch_status = "Success";
  
  // Push reading to historical buffer
  pushToHistory(value);
  
  Serial.printf("Parsed Glucose: %.1f mg/dL, Trend: %d, Time: %s\n", value, trend, timestamp.c_str());
  return true;
}

// Local WebServer implementations
void handleLocalRoot() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2, h3 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:15px; border-radius:8px; margin-bottom:20px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += ".btn { display:block; padding:12px; margin:10px 0; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; font-size:16px; width:100%; box-sizing:border-box; }";
  html += ".btn-blue { background:#33B5E5; }";
  html += ".btn-orange { background:#f0ad4e; }";
  html += ".btn-grey { background:#555; }";
  html += "</style></head><body>";
  html += "<h2>Libre Monitor Local Admin</h2>";
  html += "<p>Build: " BUILD_VERSION "</p>";
  
  html += "<div class='card'>";
  html += "<h3>Status</h3>";
  html += "<p>WiFi Signal: <b>" + String(WiFi.RSSI()) + " dBm</b></p>";
  
  // LibreLinkUp Status
  String llu_status_text = "";
  if (strlen(llu_email) == 0 || strlen(llu_password) == 0) {
    llu_status_text = "Awaiting configuration";
  } else if (llu_last_fetch_status != "Success" && llu_last_fetch_status != "Never Synced" && llu_last_fetch_status != "Fetching...") {
    llu_status_text = "Error (" + llu_last_fetch_status + ")";
  } else if (last_fetch_time > 0) {
    unsigned long elapsed_m = (millis() - last_fetch_time) / 60000;
    String glucoseStr = "";
    if (strcmp(llu_units, "mmol/L") == 0) {
      glucoseStr = String(last_glucose / 18.0182, 1) + " mmol/L";
    } else {
      glucoseStr = String((int)last_glucose) + " mg/dL";
    }
    llu_status_text = "Last Sync " + String(elapsed_m) + "m ago, " + glucoseStr;
  } else {
    llu_status_text = llu_last_fetch_status;
  }
  html += "<p>LibreLinkUp Status: <b>" + llu_status_text + "</b></p>";

  // Diabetes:M Status
  String dm_status_text = "";
  if (!dm_enable_connection) {
    dm_status_text = "Disabled";
  } else if (dm_2fa_pending) {
    dm_status_text = "2FA Authentication Required";
  } else if (strlen(dm_email) == 0 || strlen(dm_password) == 0) {
    dm_status_text = "Awaiting configuration";
  } else if (dm_last_sent_epoch > 0) {
    unsigned long elapsed_m = (time(nullptr) - dm_last_sent_epoch) / 60;
    String glucoseStr = "";
    if (strcmp(llu_units, "mmol/L") == 0) {
      glucoseStr = String(dm_last_sent_value / 18.0182, 1) + " mmol/L";
    } else {
      glucoseStr = String((int)dm_last_sent_value) + " mg/dL";
    }
    if (dm_last_sent_status == "Sent OK") {
      dm_status_text = "Last Sync " + String(elapsed_m) + "m ago, " + glucoseStr;
    } else {
      dm_status_text = "Error (" + dm_last_sent_status + ")";
    }
  } else {
    dm_status_text = dm_last_sent_status;
  }
  html += "<p>Diabetes:M Status: <b>" + dm_status_text + "</b></p>";
  html += "</div>";
  
  html += "<div style='max-width:600px; margin-left:auto; margin-right:auto;'>";
  html += "<a href='/param' class='btn btn-blue'>Configure LibreLinkUp</a>";
  html += "<a href='/diabetes-m' class='btn btn-blue'>Configure Diabetes:M</a>";
  html += "<a href='/general' class='btn btn-blue'>Configure General Settings</a>";
  html += "<div style='height:45px;'></div>";
  html += "<a href='/hardware' class='btn btn-orange'>Hardware Control</a>";
  html += "<a href='/change-password' class='btn btn-grey'>Change Portal Password</a>";
  html += "</div>";
  
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalParam() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:15px; border-radius:8px; margin-bottom:20px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += ".form-group { margin-bottom:15px; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += "input, select { width:100%; padding:10px; border-radius:4px; border:1px solid #444; background:#111; color:#fff; box-sizing:border-box; font-size:16px; }";
  html += ".btn { display:block; width:100%; padding:12px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; margin-top:20px; font-size:16px; box-sizing:border-box; }";
  html += ".btn-blue { background:#33B5E5; margin-top:10px; }";
  html += ".btn-grey { background:#555; margin-top:10px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Configure LibreLinkUp</h2>";
  html += "<form action='/save' method='POST'>";
  
  html += "<div class='form-group'>";
  html += "<label>Username or Email</label>";
  html += "<input type='text' name='email' value='" + String(llu_email) + "' maxlength='64'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Password</label>";
  html += "<input type='password' id='password' name='password' value='" + String(llu_password) + "' maxlength='64'>";
  html += "<div style='margin:5px 0;'><input type='checkbox' onclick='var x=document.getElementById(\"password\");x.type=this.checked?\"text\":\"password\";' style='width:auto;'> Show Password</div>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Region</label>";
  html += "<select name='region'>";
  
  struct RegionOpt { const char* val; const char* label; };
  RegionOpt regions[] = {
    {"eu", "Europe (eu)"},
    {"us", "United States (us)"},
    {"de", "Germany (de)"},
    {"jp", "Japan (jp)"},
    {"ap", "Asia Pacific (ap)"},
    {"ae", "Middle East (ae)"}
  };
  
  for (auto& r : regions) {
    html += "<option value='";
    html += r.val;
    html += "'";
    if (strcmp(llu_region, r.val) == 0) {
      html += " selected";
    }
    html += ">";
    html += r.label;
    html += "</option>";
  }
  html += "</select>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>API Poll Interval (minutes, min 1)</label>";
  html += "<input type='number' name='poll' value='" + String(llu_poll_interval) + "' min='1' max='60'>";
  html += "</div>";
  
  html += "<a href='/test' class='btn btn-blue'>Test LibreLinkUp Connection</a>";
  html += "<button type='submit' class='btn'>Save Settings</button>";
  html += "</form>";
  
  html += "<a href='/' class='btn btn-grey'>Return to Home Page</a>";
  html += "</div>";
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalSave() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  if (localServer.hasArg("email") && localServer.hasArg("password") && localServer.hasArg("region")) {
    String email = localServer.arg("email");
    String password = localServer.arg("password");
    String region = localServer.arg("region");
    int poll = localServer.hasArg("poll") ? localServer.arg("poll").toInt() : 2;
    if (poll < 1) poll = 1;
    
    email.toCharArray(llu_email, sizeof(llu_email));
    password.toCharArray(llu_password, sizeof(llu_password));
    region.toCharArray(llu_region, sizeof(llu_region));
    llu_poll_interval = poll;

    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putString("email", llu_email);
    preferences.putString("password", llu_password);
    preferences.putString("region", llu_region);
    preferences.putInt("poll", llu_poll_interval);
    preferences.putBool("configured", true);
    preferences.end();
    
    Serial.println("Local web server LLU settings updated and saved to NVS.");
    
    // Reset LibreLinkUp state so it fetches immediately using new credentials
    auth_token = "";
    token_expires = 0;
    
    // Fetch data immediately
    libreLinkUpFetchData();
    drawDashboard();
    
    String html = "<html><head><meta http-equiv='refresh' content='3;url=/param'><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#5cb85c;}</style></head><body>";
    html += "<h2>LibreLinkUp Settings Saved!</h2>";
    html += "<p>Updating follower credentials and returning to configuration page...</p>";
    html += "</body></html>";
    
    localServer.send(200, "text/html", html);
  } else {
    localServer.send(400, "text/plain", "Bad Request: Missing parameters");
  }
}

void handleLocalTest() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  String logOut = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  logOut += "body{font-family:sans-serif;background:#222;color:#fff;padding:20px;}";
  logOut += "h3,h4{color:#33B5E5;}";
  logOut += "p{margin:10px 0;}";
  logOut += ".button{display:block;width:100%;box-sizing:border-box;padding:12px;text-align:center;background:#33B5E5;color:#fff;text-decoration:none;border-radius:4px;font-weight:bold;margin-top:20px;border:none;cursor:pointer;}";
  logOut += ".button-back{background:#555;}";
  logOut += "</style></head><body>";
  
  runLibreLinkUpTest(logOut);
  
  logOut += "<br/><a href='/param' class='button button-back'>Back to Menu</a>";
  logOut += "</body></html>";
  
  localServer.send(200, "text/html", logOut);
}

void handleLocalReboot() {
  if (!checkAuth()) return;
  localServer.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='10;url=/'></head><body style='background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;'><h2>Rebooting device...</h2><p>Returning to Home Page in 10 seconds...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleLocalUpdateGet() {
  if (!checkAuth()) return;
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; }";
  html += ".card { background:#222; padding:20px; border-radius:8px; border:1px solid #333; display:inline-block; text-align:left; max-width:400px; width:100%; box-sizing:border-box; }";
  html += ".btn { display:block; width:100%; padding:12px; margin-top:20px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; }";
  html += ".btn-grey { background:#555; }";
  html += "input[type=file] { background:#111; color:#ccc; padding:10px; border:1px solid #333; width:100%; box-sizing:border-box; border-radius:4px; margin-top:10px; }";
  html += ".progress-container { width:100%; background-color:#333; border-radius:4px; margin-top:15px; display:none; }";
  html += ".progress-bar { width:0%; height:20px; background-color:#5cb85c; border-radius:4px; text-align:center; line-height:20px; color:white; font-size:12px; }";
  html += ".info-box { background:#2a2a2a; border-left:4px solid #33B5E5; padding:10px; margin-top:15px; font-size:14px; border-radius:0 4px 4px 0; }";
  html += ".warn-box { background:#d9534f; color:#fff; padding:12px; border-radius:4px; margin-top:15px; font-size:14px; display:none; }";
  html += "</style></head><body>";
  html += "<h2>Firmware Update</h2>";
  html += "<div class='card'>";
  html += "<p>Current Version: <b>" BUILD_VERSION "</b></p>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>";
  html += "<label>Select firmware.bin file:</label>";
  html += "<input type='file' name='update' accept='.bin' required onchange='inspectFile(this)'>";
  html += "<div id='new_ver_info' class='info-box' style='display:none;'></div>";
  html += "<div id='warn_box' class='warn-box'><b>Warning:</b> This is not a pre-prepared OTA firmware and caution should be advised.</div>";
  html += "<button type='button' class='btn' onclick='startUpload()'>Update Firmware</button>";
  html += "</form>";
  html += "<div class='progress-container' id='prg_container'><div class='progress-bar' id='prg_bar'>0%</div></div>";
  html += "<p style='color:#f0ad4e;font-size:14px;margin-top:15px;'><b>Note:</b> The device screen may flicker or go black during the update. This is normal functionality. Please keep the device powered ON during the process.</p>";
  html += "</div>";
  html += "<br/><br/><a href='/' class='btn btn-grey' style='display:inline-block;max-width:200px;'>Return to Home Page</a>";
  
  html += "<script>";
  html += "function inspectFile(input) {";
  html += "  var file = input.files[0];";
  html += "  if (!file) return;";
  html += "  var reader = new FileReader();";
  html += "  reader.onload = function(e) {";
  html += "    var bytes = new Uint8Array(e.target.result);";
  html += "    var sig = [67, 71, 77, 45, 79, 84, 65, 45, 83, 73, 71, 78, 65, 84, 85, 82, 69, 58];";
  html += "    var foundIdx = -1;";
  html += "    for (var i = 0; i <= bytes.length - sig.length; i++) {";
  html += "      var match = true;";
  html += "      for (var j = 0; j < sig.length; j++) {";
  html += "        if (bytes[i + j] !== sig[j]) { match = false; break; }";
  html += "      }";
  html += "      if (match) { foundIdx = i; break; }";
  html += "    }";
  html += "    var newVerBox = document.getElementById('new_ver_info');";
  html += "    var warnBox = document.getElementById('warn_box');";
  html += "    newVerBox.style.display = 'block';";
  html += "    if (foundIdx !== -1) {";
  html += "      var ver = '';";
  html += "      var start = foundIdx + sig.length;";
  html += "      for (var k = 0; k < 16; k++) {";
  html += "        var charCode = bytes[start + k];";
  html += "        if (charCode === 0 || charCode === 34 || charCode === 59) break;";
  html += "        ver += String.fromCharCode(charCode);";
  html += "      }";
  html += "      newVerBox.innerHTML = 'Uploaded Version: <b>' + ver + '</b>';";
  html += "      warnBox.style.display = 'none';";
  html += "    } else {";
  html += "      newVerBox.innerHTML = 'Uploaded Version: <b>Unknown</b>';";
  html += "      warnBox.style.display = 'block';";
  html += "    }";
  html += "  };";
  html += "  reader.readAsArrayBuffer(file.slice(0, 1048576));";
  html += "}";
  html += "function startUpload() {";
  html += "  var fileInput = document.querySelector('input[type=file]');";
  html += "  if (fileInput.files.length === 0) { alert('Please select a file first.'); return; }";
  html += "  document.getElementById('prg_container').style.display = 'block';";
  html += "  var formData = new FormData(document.getElementById('upload_form'));";
  html += "  var xhr = new XMLHttpRequest();";
  html += "  xhr.open('POST', '/update', true);";
  html += "  xhr.upload.onprogress = function(e) {";
  html += "    if (e.lengthComputable) {";
  html += "      var pct = Math.round((e.loaded / e.total) * 100);";
  html += "      var bar = document.getElementById('prg_bar');";
  html += "      bar.style.width = pct + '%';";
  html += "      bar.innerHTML = pct + '%';";
  // Fix the slide issues
  html += "    }";
  html += "  };";
  html += "  xhr.onload = function() {";
  html += "    if (xhr.status === 200) {";
  html += "      document.body.innerHTML = '<div style=\"background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;\"><h2 style=\"color:#5cb85c;\">Update Success!</h2><p>Rebooting device... Page will refresh in 10 seconds.</p></div>';";
  html += "      setTimeout(function() { window.location.href = '/'; }, 10000);";
  html += "    } else {";
  html += "      document.body.innerHTML = '<div style=\"background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;\"><h2 style=\"color:#d9534f;\">Update Failed!</h2><p>Error: \\'\\' + xhr.responseText + \\'\\\'</p><br/><a href=\"/update\" style=\"color:#33B5E5;\">Try Again</a></div>';";
  html += "    }";
  html += "  };";
  html += "  xhr.send(formData);";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalUpdatePost() {
  if (!checkAuth()) return;
  localServer.sendHeader("Connection", "close");
  localServer.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
  delay(1000);
  ESP.restart();
}

void handleLocalUpdateUpload() {
  if (!checkAuth()) return;
  HTTPUpload& upload = localServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update Start: %s\n", upload.filename.c_str());
    gfx.fillScreen(0x181A1B);
    gfx.setFont(&fonts::DejaVu24);
    gfx.setTextColor(0x33B5E5);
    gfx.drawCenterString("Updating Firmware...", 240, 180);
    gfx.setFont(&fonts::DejaVu18);
    gfx.setTextColor(0xAAAAAA);
    gfx.drawCenterString("Please keep device powered ON", 240, 240);
    
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
      gfx.fillScreen(0x181A1B);
      gfx.setFont(&fonts::DejaVu24);
      gfx.setTextColor(0x5CB85C);
      gfx.drawCenterString("Update Successful!", 240, 200);
      gfx.setTextColor(0xFFFFFF);
      gfx.setFont(&fonts::DejaVu18);
      gfx.drawCenterString("Rebooting device...", 240, 260);
    } else {
      Update.printError(Serial);
      gfx.fillScreen(0x181A1B);
      gfx.setFont(&fonts::DejaVu24);
      gfx.setTextColor(0xD9534F);
      gfx.drawCenterString("Update Failed!", 240, 200);
    }
  }
}

bool checkAuth() {
  if (!localServer.authenticate("admin", admin_password)) {
    localServer.requestAuthentication(BASIC_AUTH, "Libre Monitor", "Authentication failed");
    return false;
  }
  return true;
}

bool checkForceReset() {
  if (is_temp_password) {
    localServer.sendHeader("Location", "/change-password");
    localServer.send(302, "text/plain", "Redirecting to password change page...");
    return true;
  }
  return false;
}

void handleLocalChangePasswordGet() {
  if (!checkAuth()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:20px; border-radius:8px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += ".form-group { margin-bottom:15px; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += "input { width:100%; padding:10px; border-radius:4px; border:1px solid #444; background:#111; color:#fff; box-sizing:border-box; font-size:16px; }";
  html += ".btn { display:block; width:100%; padding:12px; margin-top:20px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; font-size:16px; box-sizing:border-box; }";
  if (!is_temp_password) {
    html += ".btn-grey { background:#555; margin-top:10px; }";
  }
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Change Portal Password</h2>";
  if (is_temp_password) {
    html += "<p style='color:#f0ad4e;'><b>Security Notice:</b> You are currently logged in with a temporary password. You must set a custom password to proceed.</p>";
  }
  
  html += "<form method='POST' action='/save-password' id='pwd_form'>";
  html += "<div class='form-group'>";
  html += "<label>New Password (min 6 characters)</label>";
  html += "<input type='password' id='new_pwd' name='new_pwd' required minlength='6'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Confirm Password</label>";
  html += "<input type='password' id='confirm_pwd' name='confirm_pwd' required minlength='6'>";
  html += "<div style='margin:5px 0;'><input type='checkbox' onclick='var x1=document.getElementById(\"new_pwd\"),x2=document.getElementById(\"confirm_pwd\");x1.type=this.checked?\"text\":\"password\";x2.type=this.checked?\"text\":\"password\";' style='width:auto;'> Show Passwords</div>";
  html += "</div>";
  
  html += "<button type='submit' class='btn'>Save Password</button>";
  html += "</form>";
  if (!is_temp_password) {
    html += "<a href='/' class='btn btn-grey'>Return to Home Page</a>";
  }
  html += "</div>";
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalSavePasswordPost() {
  if (!checkAuth()) return;
  
  if (localServer.hasArg("new_pwd") && localServer.hasArg("confirm_pwd")) {
    String new_pwd = localServer.arg("new_pwd");
    String confirm_pwd = localServer.arg("confirm_pwd");
    
    new_pwd.trim();
    confirm_pwd.trim();
    
    if (new_pwd.length() < 6) {
      localServer.send(400, "text/plain", "Bad Request: Password must be at least 6 characters");
      return;
    }
    
    if (new_pwd != confirm_pwd) {
      localServer.send(400, "text/plain", "Bad Request: Passwords do not match");
      return;
    }
    
    // Save to NVS
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putString("admin_pwd", new_pwd);
    preferences.end();
    
    new_pwd.toCharArray(admin_password, sizeof(admin_password));
    is_temp_password = false;
    
    Serial.println("Admin portal password successfully updated.");
    
    String html = "<html><head><meta http-equiv='refresh' content='3;url=/'><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#5cb85c;}</style></head><body>";
    html += "<h2>Password Saved Successfully!</h2>";
    html += "<p>Updating admin credentials and returning to landing page...</p>";
    html += "</body></html>";
    
    localServer.send(200, "text/html", html);
  } else {
    localServer.send(400, "text/plain", "Bad Request: Missing parameters");
  }
}

void handleLocalFactoryReset() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#d9534f;}</style></head><body>";
  html += "<h2>Factory Reset Initiated</h2>";
  html += "<p>All settings, Wi-Fi credentials, passwords, and data are being erased.</p>";
  html += "<p>The device will reboot shortly. Please connect to <b>ESP32-CGM-Config</b> Wi-Fi to reconfigure.</p>";
  html += "</body></html>";
  localServer.send(200, "text/html", html);
  
  delay(1500);
  
  // Clear NVS configuration
  Preferences preferences;
  preferences.begin("cgm-config", false);
  preferences.clear();
  preferences.end();
  
  // Reset WiFi credentials
  WiFi.disconnect(true, true);
  delay(1500);
  
  ESP.restart();
}

void handleLocalGeneralGet() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:15px; border-radius:8px; margin-bottom:20px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += "details { background:#111; border:1px solid #333; border-radius:6px; margin-bottom:15px; padding:10px 15px; }";
  html += "summary { color:#33B5E5; font-size:18px; font-weight:bold; cursor:pointer; outline:none; padding:5px 0; }";
  html += "summary::-webkit-details-marker { color:#33B5E5; }";
  html += ".form-group { margin-bottom:15px; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += "input, select { width:100%; padding:10px; border-radius:4px; border:1px solid #444; background:#222; color:#fff; box-sizing:border-box; font-size:16px; }";
  html += ".btn { display:block; width:100%; padding:12px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; margin-top:20px; font-size:16px; box-sizing:border-box; }";
  html += ".btn-blue { background:#33B5E5; }";
  html += ".btn-grey { background:#555; margin-top:10px; }";
  html += ".btn-red { background:#d9534f; margin-top:0; }";
  html += ".rule-row { display:flex; align-items:center; gap:8px; margin-bottom:10px; border-bottom:1px solid #333; padding-bottom:10px; flex-wrap:wrap; }";
  html += ".rule-row select { width:auto; }";
  html += ".rule-row input[type=number] { width:80px; }";
  html += ".rule-row input[type=text] { width:280px; }";
  html += ".val2-container { display:inline-flex; align-items:center; gap:8px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Configure General Settings</h2>";
  html += "<form action='/save-general' method='POST'>";
  
  // 1. Display units section
  html += "<details>";
  html += "<summary>Display units</summary>";
  html += "<div style='padding-top:10px;'>";
  html += "<div class='form-group'>";
  html += "<label>Units</label>";
  html += "<select name='units' id='units_select' onchange='handleUnitsChange()'>";
  html += "<option value='mmol/L'";
  if (strcmp(llu_units, "mmol/L") == 0) html += " selected";
  html += ">mmol/L</option>";
  html += "<option value='mg/dL'";
  if (strcmp(llu_units, "mg/dL") == 0) html += " selected";
  html += ">mg/dL</option>";
  html += "</select>";
  html += "</div>";
  html += "</div>";
  html += "</details>";
  
  String stepStr = (strcmp(llu_units, "mmol/L") == 0) ? "0.1" : "1";

  // 2. Colour Tolerances section
  html += "<details>";
  html += "<summary id='colour_tolerances_summary'>Colour Tolerances (" + String(llu_units) + ")</summary>";
  html += "<div style='padding-top:10px;'>";
  html += "<div class='form-group'>";
  html += "<label>Critical Low Limit (Red warning below this)</label>";
  html += "<input type='number' step='" + stepStr + "' name='c_low' value='" + formatUserUnitValue(c_low) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Warning Low Limit (Yellow warning below this)</label>";
  html += "<input type='number' step='" + stepStr + "' name='w_low' value='" + formatUserUnitValue(w_low) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Warning High Limit (Yellow warning above this)</label>";
  html += "<input type='number' step='" + stepStr + "' name='w_high' value='" + formatUserUnitValue(w_high) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Critical High Limit (Red warning above this)</label>";
  html += "<input type='number' step='" + stepStr + "' name='c_high' value='" + formatUserUnitValue(c_high) + "'>";
  html += "</div>";
  html += "</div>";
  html += "</details>";
  
  // 3. Graph Y-Axis Range section
  html += "<details>";
  html += "<summary id='graph_range_summary'>Graph Y-Axis Range (" + String(llu_units) + ")</summary>";
  html += "<div style='padding-top:10px;'>";
  html += "<div class='form-group'>";
  html += "<label>Graph Y-Axis Minimum (clamped if below)</label>";
  html += "<input type='number' step='" + stepStr + "' name='g_min' value='" + formatUserUnitValue(graph_min) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Graph Y-Axis Maximum (clamped if above)</label>";
  html += "<input type='number' step='" + stepStr + "' name='g_max' value='" + formatUserUnitValue(graph_max) + "'>";
  html += "</div>";
  html += "</div>";
  html += "</details>";
  
  // 4. Custom Messages section
  html += "<details>";
  html += "<summary>Custom Messages (max 10)</summary>";
  html += "<div style='padding-top:10px;'>";
  html += "<div id='rules_container' style='margin-bottom:15px;'>";
  
  // Render existing rules
  for (int i = 0; i < msg_rules_count; i++) {
    html += "<div class='rule-row'>";
    html += "<select class='type-select' name='msg_type_" + String(i) + "' onchange='reindexRules()'>";
    html += "<option value='gt'"; if (strcmp(msg_rules[i].type, "gt") == 0) html += " selected"; html += ">&gt; (greater than)</option>";
    html += "<option value='lt'"; if (strcmp(msg_rules[i].type, "lt") == 0) html += " selected"; html += ">&lt; (less than)</option>";
    html += "<option value='between'"; if (strcmp(msg_rules[i].type, "between") == 0) html += " selected"; html += ">between</option>";
    html += "</select> ";
    
    html += "<input type='number' step='" + stepStr + "' class='val1-input' name='msg_val1_" + String(i) + "' value='" + formatUserUnitValue(msg_rules[i].val1) + "' required> ";
    
    String display_style = (strcmp(msg_rules[i].type, "between") == 0) ? "inline-flex" : "none";
    html += "<span class='val2-container' style='display:" + display_style + ";'>";
    html += "and <input type='number' step='" + stepStr + "' class='val2-input' name='msg_val2_" + String(i) + "' value='" + formatUserUnitValue(msg_rules[i].val2) + "'> ";
    html += "</span> ";
    
    html += "<input type='text' class='text-input' name='msg_text_" + String(i) + "' value='" + String(msg_rules[i].text) + "' placeholder='Message' maxlength='63' required> ";
    html += "<button type='button' class='btn btn-red' onclick='this.parentNode.remove();reindexRules();' style='width:auto;'>Delete</button>";
    html += "</div>";
  }
  
  html += "</div>";
  html += "<button type='button' id='add_btn' class='btn btn-blue' onclick='addRule()' style='width:auto;margin-bottom:20px;'>+ Add Message Condition</button><br/>";
  html += "</div>";
  html += "</details>";
  
  html += "<button type='submit' class='btn'>Save Configuration</button>";
  html += "</form>";
  html += "<a href='/' class='btn btn-grey'>Return to Home Page</a>";
  html += "</div>";
  
  html += "<script>";
  html += "function reindexRules() {";
  html += "  var rows = document.querySelectorAll('.rule-row');";
  html += "  rows.forEach(function(row, idx) {";
  html += "    row.querySelector('.type-select').name = 'msg_type_' + idx;";
  html += "    row.querySelector('.val1-input').name = 'msg_val1_' + idx;";
  html += "    var val2Input = row.querySelector('.val2-input');";
  html += "    if (val2Input) val2Input.name = 'msg_val2_' + idx;";
  html += "    row.querySelector('.text-input').name = 'msg_text_' + idx;";
  html += "    var type = row.querySelector('.type-select').value;";
  html += "    var val2Container = row.querySelector('.val2-container');";
  html += "    if (type === 'between') {";
  html += "      val2Container.style.display = 'inline-flex';";
  html += "      if (val2Input) val2Input.required = true;";
  html += "    } else {";
  html += "      val2Container.style.display = 'none';";
  html += "      if (val2Input) val2Input.required = false;";
  html += "    }";
  html += "  });";
  html += "  document.getElementById('add_btn').style.display = (rows.length >= 10) ? 'none' : 'block';";
  html += "}";
  
  html += "function addRule() {";
  html += "  var container = document.getElementById('rules_container');";
  html += "  var newRow = document.createElement('div');";
  html += "  newRow.className = 'rule-row';";
  html += "  var isMmol = (document.getElementById('units_select').value === 'mmol/L');";
  html += "  var stepAttr = isMmol ? '0.1' : '1';";
  html += "  var inner = \"<select class='type-select' onchange='reindexRules()'>\";";
  html += "  inner += \"<option value='gt'>&gt; (greater than)</option>\";";
  html += "  inner += \"<option value='lt'>&lt; (less than)</option>\";";
  html += "  inner += \"<option value='between'>between</option>\";";
  html += "  inner += \"</select> \";";
  html += "  inner += \"<input type='number' step='\" + stepAttr + \"' class='val1-input' required> \";";
  html += "  inner += \"<span class='val2-container' style='display:none;'> \";";
  html += "  inner += \"and <input type='number' step='\" + stepAttr + \"' class='val2-input'> \";";
  html += "  inner += \"</span> \";";
  html += "  inner += \"<input type='text' class='text-input' placeholder='Message' maxlength='63' required> \";";
  html += "  inner += \"<button type='button' class='btn btn-red' onclick='this.parentNode.remove();reindexRules();' style='width:auto;'>Delete</button>\";";
  html += "  newRow.innerHTML = inner;";
  html += "  container.appendChild(newRow);";
  html += "  reindexRules();";
  html += "}";
  
  html += "function handleUnitsChange() {";
  html += "  var selectEl = document.getElementById('units_select');";
  html += "  var prevUnit = selectEl.getAttribute('data-prev') || (selectEl.value === 'mmol/L' ? 'mg/dL' : 'mmol/L');";
  html += "  var currentUnit = selectEl.value;";
  html += "  if (prevUnit === currentUnit) return;";
  html += "  var colSum = document.getElementById('colour_tolerances_summary');";
  html += "  if (colSum) colSum.textContent = 'Colour Tolerances (' + currentUnit + ')';";
  html += "  var grSum = document.getElementById('graph_range_summary');";
  html += "  if (grSum) grSum.textContent = 'Graph Y-Axis Range (' + currentUnit + ')';";
  html += "  var factor = 18.0182;";
  html += "  var isMmol = (currentUnit === 'mmol/L');";
  html += "  var names = ['c_low', 'w_low', 'w_high', 'c_high', 'g_min', 'g_max'];";
  html += "  names.forEach(function(name) {";
  html += "    var input = document.querySelector(\"input[name='\" + name + \"']\");";
  html += "    if (input && input.value !== '') {";
  html += "      var val = parseFloat(input.value);";
  html += "      if (!isNaN(val)) {";
  html += "        if (isMmol) {";
  html += "          var newVal = val / factor;";
  html += "          input.value = (Math.round(newVal * 10) / 10).toFixed(1);";
  html += "          input.step = '0.1';";
  html += "        } else {";
  html += "          var newVal = val * factor;";
  html += "          input.value = Math.round(newVal);";
  html += "          input.step = '1';";
  html += "        }";
  html += "      }";
  html += "    }";
  html += "  });";
  html += "  var ruleInputs = document.querySelectorAll('.val1-input, .val2-input');";
  html += "  ruleInputs.forEach(function(input) {";
  html += "    if (input && input.value !== '') {";
  html += "      var val = parseFloat(input.value);";
  html += "      if (!isNaN(val)) {";
  html += "        if (isMmol) {";
  html += "          var newVal = val / factor;";
  html += "          input.value = (Math.round(newVal * 10) / 10).toFixed(1);";
  html += "          input.step = '0.1';";
  html += "        } else {";
  html += "          var newVal = val * factor;";
  html += "          input.value = Math.round(newVal);";
  html += "          input.step = '1';";
  html += "        }";
  html += "      }";
  html += "    }";
  html += "  });";
  html += "  selectEl.setAttribute('data-prev', currentUnit);";
  html += "}";
  
  html += "window.onload = function() {";
  html += "  var selectEl = document.getElementById('units_select');";
  html += "  selectEl.setAttribute('data-prev', selectEl.value);";
  html += "  var isMmol = (selectEl.value === 'mmol/L');";
  html += "  var stepAttr = isMmol ? '0.1' : '1';";
  html += "  var names = ['c_low', 'w_low', 'w_high', 'c_high', 'g_min', 'g_max'];";
  html += "  names.forEach(function(name) {";
  html += "    var input = document.querySelector(\"input[name='\" + name + \"']\");";
  html += "    if (input) input.step = stepAttr;";
  html += "  });";
  html += "  var ruleInputs = document.querySelectorAll('.val1-input, .val2-input');";
  html += "  ruleInputs.forEach(function(input) {";
  html += "    if (input) input.step = stepAttr;";
  html += "  });";
  html += "  reindexRules();";
  html += "};";
  html += "</script>";
  
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalSaveGeneralPost() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  if (localServer.hasArg("units") && localServer.hasArg("c_low") && localServer.hasArg("w_low") && localServer.hasArg("w_high") && localServer.hasArg("c_high") && localServer.hasArg("g_min") && localServer.hasArg("g_max")) {
    String units = localServer.arg("units");
    units.toCharArray(llu_units, sizeof(llu_units));
    
    float user_c_low = localServer.arg("c_low").toFloat();
    float user_w_low = localServer.arg("w_low").toFloat();
    float user_w_high = localServer.arg("w_high").toFloat();
    float user_c_high = localServer.arg("c_high").toFloat();
    float user_g_min = localServer.arg("g_min").toFloat();
    float user_g_max = localServer.arg("g_max").toFloat();

    c_low = fromUserUnit(user_c_low);
    w_low = fromUserUnit(user_w_low);
    w_high = fromUserUnit(user_w_high);
    c_high = fromUserUnit(user_c_high);
    graph_min = fromUserUnit(user_g_min);
    graph_max = fromUserUnit(user_g_max);

    // Parse message rules from POST
    int rule_idx = 0;
    for (int i = 0; i < 10; i++) {
      String type_key = "msg_type_" + String(i);
      if (localServer.hasArg(type_key)) {
        String type = localServer.arg(type_key);
        float val1 = localServer.arg("msg_val1_" + String(i)).toFloat();
        float val2 = localServer.arg("msg_val2_" + String(i)).toFloat();
        String text = localServer.arg("msg_text_" + String(i));
        text.trim();
        
        if (text.length() > 0) {
          strncpy(msg_rules[rule_idx].type, type.c_str(), sizeof(msg_rules[rule_idx].type));
          msg_rules[rule_idx].val1 = fromUserUnit(val1);
          msg_rules[rule_idx].val2 = fromUserUnit(val2);
          strncpy(msg_rules[rule_idx].text, text.c_str(), sizeof(msg_rules[rule_idx].text));
          rule_idx++;
        }
      }
    }
    if (rule_idx < 10) {
      memset(&msg_rules[rule_idx], 0, (10 - rule_idx) * sizeof(MessageRule));
    }
    msg_rules_count = rule_idx;

    // Sanitization & bounds checking
    if (c_low < 30.0) c_low = 30.0;
    if (w_low < c_low) w_low = c_low;
    if (w_high < w_low) w_high = w_low;
    if (c_high < w_high) c_high = w_high;
    if (graph_min < 30.0) graph_min = 30.0;
    if (graph_max < graph_min) graph_max = graph_min + 50.0;
    
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putString("units", llu_units);
    preferences.putFloat("c_low", c_low);
    preferences.putFloat("w_low", w_low);
    preferences.putFloat("w_high", w_high);
    preferences.putFloat("c_high", c_high);
    preferences.putFloat("g_min", graph_min);
    preferences.putFloat("g_max", graph_max);
    preferences.putBytes("msg_rules", msg_rules, sizeof(msg_rules));
    preferences.putInt("msg_rules_cnt", msg_rules_count);
    preferences.end();
    
    Serial.println("Local web server updated and saved general configuration to NVS.");
    
    drawDashboard();
    
    String html = "<html><head><meta http-equiv='refresh' content='3;url=/general'><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#5cb85c;}</style></head><body>";
    html += "<h2>Configuration Saved Successfully!</h2>";
    html += "<p>Updating local display thresholds and returning to configuration page...</p>";
    html += "</body></html>";
    
    localServer.send(200, "text/html", html);
  } else {
    localServer.send(400, "text/plain", "Bad Request: Missing parameters");
  }
}

void handleLocalHardwareGet() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:15px; border-radius:8px; margin-bottom:20px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += ".btn { display:block; width:100%; padding:12px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; margin-top:20px; font-size:16px; box-sizing:border-box; }";
  html += ".btn-blue { background:#33B5E5; }";
  html += ".btn-orange { background:#f0ad4e; }";
  html += ".btn-red { background:#d9534f; }";
  html += ".btn-grey { background:#555; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Hardware Control</h2>";
  html += "<a href='/update' class='btn btn-blue'>Update Firmware (OTA)</a>";
  html += "<a href='/wifi' class='btn btn-blue'>Network & Wi-Fi Settings</a>";
  html += "<a href='/debug-info' class='btn btn-blue'>Debug Info</a>";
  html += "<a href='/reboot' class='btn btn-red' onclick='return confirm(\"Are you sure you want to reboot the device?\");'>Reboot Device</a>";
  html += "<a href='/factory-reset' class='btn btn-red' style='margin-top:40px;' onclick='return confirm(\"Are you sure you want to perform a factory reset? This will erase all Wi-Fi, password, settings and historical readings, and require a full reconfiguration.\");'>Reset to Factory Default</a>";
  html += "</div>";
  
  html += "<div class='card'>";
  html += "<h2>Configuration Management</h2>";
  html += "<a href='/export-config' class='btn btn-blue' style='text-decoration:none;'>Save Configuration</a>";
  html += "<a href='/import-config' class='btn btn-orange' style='text-decoration:none;margin-top:10px;'>Load Configuration</a>";
  html += "</div>";
  
  html += "<div style='max-width:600px; margin-left:auto; margin-right:auto;'>";
  html += "<a href='/' class='btn btn-grey'>Return to Home Page</a>";
  html += "</div>";
  
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

const char* getResetReasonStr(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:   return "Unknown";
    case ESP_RST_POWERON:   return "Power On / Vbat";
    case ESP_RST_EXT:       return "External Pin";
    case ESP_RST_SW:        return "Software Restart";
    case ESP_RST_PANIC:     return "Software Panic / Exception";
    case ESP_RST_INT_WDT:   return "Interrupt Watchdog";
    case ESP_RST_TASK_WDT:  return "Task Watchdog";
    case ESP_RST_WDT:       return "Watchdog Reset";
    case ESP_RST_DEEPSLEEP: return "Deep Sleep Wakeup";
    case ESP_RST_BROWNOUT:  return "Brownout Reset";
    case ESP_RST_SDIO:      return "SDIO Reset";
    default:                return "Other";
  }
}

const char* getWiFiModeStr(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL:  return "NULL";
    case WIFI_MODE_STA:   return "Station (STA)";
    case WIFI_MODE_AP:    return "Soft Access Point (AP)";
    case WIFI_MODE_APSTA: return "Station + AP (STA+AP)";
    default:              return "Unknown";
  }
}

const char* getTrendStr(int trend) {
  switch (trend) {
    case 1:  return "Rising Quickly (1)";
    case 2:  return "Rising (2)";
    case 3:  return "Stable / Flat (3)";
    case 4:  return "Falling (4)";
    case 5:  return "Falling Quickly (5)";
    default: return "Unknown";
  }
}

String getUptimeStr() {
  unsigned long total_sec = millis() / 1000;
  unsigned long days = total_sec / 86400;
  unsigned long hours = (total_sec % 86400) / 3600;
  unsigned long minutes = (total_sec % 3600) / 60;
  unsigned long seconds = total_sec % 60;
  
  String res = "";
  if (days > 0) {
    res += String(days) + "d ";
  }
  if (hours > 0 || days > 0) {
    res += String(hours) + "h ";
  }
  res += String(minutes) + "m " + String(seconds) + "s";
  return res;
}

bool confirmFactoryResetHMI() {
  gfx.fillScreen(0x181A1B); // Dark background
  gfx.setTextColor(0xD9534F); // Red
  gfx.setFont(&fonts::DejaVu24);
  gfx.drawCenterString("FACTORY RESET", 240, 60);
  
  gfx.setTextColor(0xFFFFFF); // White
  gfx.setFont(&fonts::DejaVu18);
  gfx.drawCenterString("Are you sure you want to", 240, 130);
  gfx.drawCenterString("erase all settings & Wi-Fi?", 240, 160);
  gfx.drawCenterString("This cannot be undone.", 240, 190);
  
  // Draw OK button (Red)
  int ok_x = 50, ok_y = 260, ok_w = 160, ok_h = 70;
  gfx.fillRoundRect(ok_x, ok_y, ok_w, ok_h, 8, 0xD9534F);
  gfx.setTextColor(0xFFFFFF);
  gfx.setFont(&fonts::DejaVu24);
  gfx.drawCenterString("RESET", ok_x + ok_w/2, ok_y + ok_h/2 - 12);
  
  // Draw Cancel button (Grey)
  int cc_x = 270, cc_y = 260, cc_w = 160, cc_h = 70;
  gfx.fillRoundRect(cc_x, cc_y, cc_w, cc_h, 8, 0x555555);
  gfx.setTextColor(0xFFFFFF);
  gfx.setFont(&fonts::DejaVu24);
  gfx.drawCenterString("CANCEL", cc_x + cc_w/2, cc_y + cc_h/2 - 12);
  
  unsigned long start_time = millis();
  int timeout_seconds = 10;
  int last_printed_remaining = -1;
  
  // Clear any existing touch flags
  uint16_t dummy_x, dummy_y;
  for (int i = 0; i < 5; i++) {
    gfx.getTouch(&dummy_x, &dummy_y);
    delay(10);
  }
  
  while (true) {
    unsigned long elapsed = millis() - start_time;
    if (elapsed >= (unsigned long)timeout_seconds * 1000) {
      break; // Timeout defaults to cancel
    }
    
    int remaining = timeout_seconds - (int)(elapsed / 1000);
    if (remaining != last_printed_remaining) {
      last_printed_remaining = remaining;
      gfx.fillRect(0, 370, 480, 50, 0x181A1B);
      gfx.setTextColor(0x888888);
      gfx.setFont(&fonts::DejaVu18);
      gfx.drawCenterString("Defaulting to CANCEL in " + String(remaining) + "s", 240, 385);
    }
    
    uint16_t tx = 0, ty = 0;
    bool touched = gfx.getTouch(&tx, &ty);
    
    if (touched && (tx > 0 || ty > 0)) {
      // Check OK button
      if (tx >= ok_x && tx <= (ok_x + ok_w) && ty >= ok_y && ty <= (ok_y + ok_h)) {
        gfx.fillRoundRect(ok_x, ok_y, ok_w, ok_h, 8, 0x5CB85C); // Green
        gfx.setTextColor(0xFFFFFF);
        gfx.setFont(&fonts::DejaVu24);
        gfx.drawCenterString("RESET", ok_x + ok_w/2, ok_y + ok_h/2 - 12);
        delay(300);
        return true;
      }
      
      // Check Cancel button
      if (tx >= cc_x && tx <= (cc_x + cc_w) && ty >= cc_y && ty <= (cc_y + cc_h)) {
        gfx.fillRoundRect(cc_x, cc_y, cc_w, cc_h, 8, 0x888888); // Light grey
        gfx.setTextColor(0xFFFFFF);
        gfx.setFont(&fonts::DejaVu24);
        gfx.drawCenterString("CANCEL", cc_x + cc_w/2, cc_y + cc_h/2 - 12);
        delay(300);
        return false;
      }
    }
    
    delay(50);
  }
  
  return false; // Default to cancel
}

void handleLocalDebugGet() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;

  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; margin:0; }";
  html += ".container { max-width:800px; margin:0 auto; text-align:left; }";
  html += "h2 { color:#33B5E5; text-align:center; margin-bottom:20px; }";
  html += "h3 { color:#33B5E5; border-bottom:1px solid #33B5E5; padding-bottom:5px; margin-top:30px; }";
  html += "table { width:100%; border-collapse:collapse; margin-bottom:20px; font-size:15px; background:#222; border-radius:8px; overflow:hidden; border:1px solid #333; }";
  html += "th, td { padding:12px 15px; text-align:left; border-bottom:1px solid #333; }";
  html += "th { background:#2d3032; font-weight:bold; color:#33B5E5; width:40%; }";
  html += "tr:last-child td { border-bottom:none; }";
  html += ".btn { display:block; padding:12px; margin:20px 0; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; }";
  html += ".btn-grey { background:#555; }";
  html += ".card { background:#222; padding:20px; border-radius:8px; border:1px solid #333; margin-top:20px; }";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h2>System Diagnostics</h2>";

  // Table 1: System & Chip Information
  html += "<h3>1. System & Chip Information</h3>";
  html += "<table>";
  html += "<tr><th>Chip Model</th><td>" + String(ESP.getChipModel()) + "</td></tr>";
  html += "<tr><th>CPU Frequency</th><td>" + String(ESP.getCpuFreqMHz()) + " MHz</td></tr>";
  html += "<tr><th>Free Heap</th><td>" + String(ESP.getFreeHeap() / 1024.0, 1) + " KB (" + String(ESP.getFreeHeap()) + " B)</td></tr>";
  html += "<tr><th>Min Free Heap</th><td>" + String(ESP.getMinFreeHeap() / 1024.0, 1) + " KB (" + String(ESP.getMinFreeHeap()) + " B)</td></tr>";
  html += "<tr><th>Total Heap Size</th><td>" + String(ESP.getHeapSize() / 1024.0, 1) + " KB</td></tr>";
  
  #if defined(BOARD_HAS_PSRAM) || defined(ARDUINO_BOARD_HAS_PSRAM)
  html += "<tr><th>Total PSRAM</th><td>" + String(ESP.getPsramSize() / 1024.0, 1) + " KB</td></tr>";
  html += "<tr><th>Free PSRAM</th><td>" + String(ESP.getFreePsram() / 1024.0, 1) + " KB</td></tr>";
  #else
  html += "<tr><th>PSRAM</th><td>Not Compiled / Supported</td></tr>";
  #endif

  html += "<tr><th>Flash Size</th><td>" + String(ESP.getFlashChipSize() / (1024.0 * 1024.0), 1) + " MB</td></tr>";
  html += "<tr><th>Flash Speed</th><td>" + String(ESP.getFlashChipSpeed() / 1000000.0, 1) + " MHz</td></tr>";
  html += "<tr><th>SDK Version</th><td>" + String(ESP.getSdkVersion()) + "</td></tr>";
  html += "<tr><th>Reset Reason</th><td>" + String(getResetReasonStr(esp_reset_reason())) + "</td></tr>";
  html += "<tr><th>Device Uptime</th><td>" + getUptimeStr() + "</td></tr>";
  html += "<tr><th>Firmware Version</th><td>" + String(BUILD_VERSION) + " (" + String(ota_signature) + ")</td></tr>";
  html += "<tr><th>Compile Time</th><td>" + String(__DATE__) + " " + String(__TIME__) + "</td></tr>";
  html += "</table>";

  // Table 2: WiFi & Network Configuration
  html += "<h3>2. WiFi & Network Configuration</h3>";
  html += "<table>";
  html += "<tr><th>SSID</th><td>" + WiFi.SSID() + "</td></tr>";
  html += "<tr><th>BSSID</th><td>" + WiFi.BSSIDstr() + "</td></tr>";
  html += "<tr><th>Signal Strength (RSSI)</th><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><th>WiFi Mode</th><td>" + String(getWiFiModeStr(WiFi.getMode())) + "</td></tr>";
  html += "<tr><th>IP Address</th><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><th>Gateway IP</th><td>" + WiFi.gatewayIP().toString() + "</td></tr>";
  html += "<tr><th>Subnet Mask</th><td>" + WiFi.subnetMask().toString() + "</td></tr>";
  html += "<tr><th>DNS IP</th><td>" + WiFi.dnsIP().toString() + "</td></tr>";
  html += "<tr><th>STA MAC Address</th><td>" + WiFi.macAddress() + "</td></tr>";
  if (WiFi.getMode() & WIFI_MODE_AP) {
    html += "<tr><th>SoftAP IP Address</th><td>" + WiFi.softAPIP().toString() + "</td></tr>";
    html += "<tr><th>SoftAP MAC Address</th><td>" + WiFi.softAPmacAddress() + "</td></tr>";
  }
  html += "</table>";

  // Table 3: Time & NTP Status
  html += "<h3>3. NTP & Time Synchronization</h3>";
  html += "<table>";
  html += "<tr><th>NTP Synced</th><td>" + String(isTimeSynced() ? "Yes" : "No") + "</td></tr>";
  html += "<tr><th>Local System Time</th><td>" + formatLocalTime(time(nullptr), "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "<tr><th>Timezone Name</th><td>" + String(dm_timezone_json) + "</td></tr>";
  html += "<tr><th>Timezone POSIX TZ</th><td>" + String(dm_timezone_posix) + "</td></tr>";
  html += "</table>";

  // Table 4: LibreLinkUp Connection
  html += "<h3>4. LibreLinkUp Connection</h3>";
  html += "<table>";
  html += "<tr><th>Configured</th><td>" + String(strlen(llu_email) > 0 ? "Yes" : "No") + "</td></tr>";
  html += "<tr><th>Email</th><td>" + String(llu_email) + "</td></tr>";
  html += "<tr><th>Region</th><td>" + String(llu_region) + "</td></tr>";
  html += "<tr><th>Units</th><td>" + String(llu_units) + "</td></tr>";
  html += "<tr><th>Poll Interval</th><td>" + String(llu_poll_interval) + " min</td></tr>";
  html += "<tr><th>Last Attempt Time</th><td>" + formatLocalTime(llu_last_fetch_attempt_epoch, "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "<tr><th>Last Success Time</th><td>" + formatLocalTime(llu_last_fetch_success_epoch, "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "<tr><th>Last Fetch Status</th><td>" + llu_last_fetch_status + "</td></tr>";
  html += "<tr><th>Last Glucose Value</th><td>" + (last_glucose > 0 ? (strcmp(llu_units, "mmol/L") == 0 ? String(last_glucose / 18.0182, 1) + " mmol/L" : String((int)last_glucose) + " mg/dL") : "--") + "</td></tr>";
  html += "<tr><th>Last Trend</th><td>" + String(getTrendStr(last_trend)) + "</td></tr>";
  html += "<tr><th>Last API Timestamp</th><td>" + last_timestamp + "</td></tr>";
  html += "<tr><th>Active Session Ticket</th><td>" + String(auth_token.length() > 0 ? "Yes" : "No") + "</td></tr>";
  html += "<tr><th>Session Token Expiry</th><td>" + (token_expires > 0 ? formatLocalTime(token_expires, "%Y-%m-%d %H:%M:%S") : "N/A") + "</td></tr>";
  html += "</table>";

  // Table 5: Diabetes:M Connection
  html += "<h3>5. Diabetes:M Connection</h3>";
  html += "<table>";
  html += "<tr><th>Connection Support Enabled</th><td>" + String(dm_enable_connection ? "Yes" : "No") + "</td></tr>";
  html += "<tr><th>Configured</th><td>" + String(strlen(dm_email) > 0 ? "Yes" : "No") + "</td></tr>";
  if (strlen(dm_email) > 0) {
    html += "<tr><th>Email</th><td>" + String(dm_email) + "</td></tr>";
  }
  html += "<tr><th>Auto Send Enabled</th><td>" + String(dm_auto_send ? "Yes" : "No") + "</td></tr>";
  html += "<tr><th>API Interval</th><td>" + String(dm_api_interval) + " min</td></tr>";
  html += "<tr><th>Random Offset Limit</th><td>" + String(dm_random_offset) + " min</td></tr>";
  html += "<tr><th>Note Text</th><td>" + String(dm_note_text) + "</td></tr>";
  html += "<tr><th>Start Sending Window</th><td>" + String(strlen(dm_start_sending) > 0 ? dm_start_sending : "None") + "</td></tr>";
  html += "<tr><th>Stop Sending Window</th><td>" + String(strlen(dm_stop_sending) > 0 ? dm_stop_sending : "None") + "</td></tr>";
  html += "<tr><th>Last Send Attempt</th><td>" + formatLocalTime(dm_last_sent_epoch, "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "<tr><th>Last Send Status</th><td>" + dm_last_sent_status + "</td></tr>";
  html += "<tr><th>Last Send Value</th><td>" + (dm_last_sent_value > 0 ? (strcmp(llu_units, "mmol/L") == 0 ? String(dm_last_sent_value / 18.0182, 1) + " mmol/L" : String((int)dm_last_sent_value) + " mg/dL") : "--") + "</td></tr>";
  html += "<tr><th>Next Scheduled Send</th><td>" + formatLocalTime(dm_next_send_epoch, "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "<tr><th>Heartbeat Enabled</th><td>" + String(dm_enable_heartbeat ? "Yes" : "No") + "</td></tr>";
  html += "<tr><th>Heartbeat Interval</th><td>" + String(dm_heartbeat_interval) + " min</td></tr>";
  html += "<tr><th>Last Heartbeat Time</th><td>" + formatLocalTime(dm_last_heartbeat_epoch, "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "<tr><th>Last Heartbeat Status</th><td>" + dm_last_heartbeat_status + "</td></tr>";
  html += "<tr><th>Next Heartbeat Send</th><td>" + formatLocalTime(dm_next_heartbeat_epoch, "%Y-%m-%d %H:%M:%S") + "</td></tr>";
  html += "</table>";

  // Diabetes:M Heartbeat Logs collapsible
  if (dm_enable_heartbeat) {
    html += "<details class='card'><summary style='font-weight:bold;cursor:pointer;color:#33B5E5;font-size:16px;'>Show Last 10 Diabetes:M Heartbeat Logs</summary>";
    html += "<table style='width:100%;border-collapse:collapse;margin-top:10px;font-size:14px;border:none;'>";
    html += "<thead><tr style='border-bottom:1px solid #444;text-align:left;'><th style='padding:5px;background:none;width:auto;'>Time</th><th style='padding:5px;background:none;width:auto;'>Status</th></tr></thead>";
    html += "<tbody>";
    if (dm_heartbeat_logs_count == 0) {
      html += "<tr><td colspan='2' style='padding:10px;text-align:center;color:#888;'>No heartbeat logs yet.</td></tr>";
    } else {
      for (int i = 0; i < dm_heartbeat_logs_count; i++) {
        String time_str = formatLocalTime(dm_heartbeat_logs[i].timestamp, "%Y-%m-%d %H:%M:%S");
        html += "<tr style='border-bottom:1px solid #333;'>";
        html += "<td style='padding:5px;white-space:nowrap;'>" + time_str + "</td>";
        html += "<td style='padding:5px;color:#ccc;'>" + dm_heartbeat_logs[i].status + "</td>";
        html += "</tr>";
      }
    }
    html += "</tbody></table></details>";
  }

  // Diabetes:M Logs collapsible
  html += "<details class='card'><summary style='font-weight:bold;cursor:pointer;color:#33B5E5;font-size:16px;'>Show Last 10 Diabetes:M Sync Attempts</summary>";
  html += "<table style='width:100%;border-collapse:collapse;margin-top:10px;font-size:14px;border:none;'>";
  html += "<thead><tr style='border-bottom:1px solid #444;text-align:left;'><th style='padding:5px;background:none;width:auto;'>Time</th><th style='padding:5px;background:none;width:auto;'>Glucose</th><th style='padding:5px;background:none;width:auto;'>Status</th></tr></thead>";
  html += "<tbody>";
  if (dm_logs_count == 0) {
    html += "<tr><td colspan='3' style='padding:10px;text-align:center;color:#888;'>No log entries yet.</td></tr>";
  } else {
    for (int i = 0; i < dm_logs_count; i++) {
      String time_str = formatLocalTime(dm_logs[i].timestamp, "%Y-%m-%d %H:%M:%S");
      String val_str = "";
      if (strcmp(llu_units, "mmol/L") == 0) {
        val_str = String(dm_logs[i].value / 18.0182, 1) + " mmol/L";
      } else {
        val_str = String((int)dm_logs[i].value) + " mg/dL";
      }
      html += "<tr style='border-bottom:1px solid #333;'>";
      html += "<td style='padding:5px;white-space:nowrap;'>" + time_str + "</td>";
      html += "<td style='padding:5px;white-space:nowrap;'>" + val_str + "</td>";
      html += "<td style='padding:5px;color:#ccc;'>" + dm_logs[i].status + "</td>";
      html += "</tr>";
    }
  }
  html += "</tbody></table></details>";

  html += "<a href='/hardware' class='btn btn-grey'>Back to Hardware Control</a>";
  html += "</div></body></html>";

  localServer.send(200, "text/html", html);
}

String obfuscate(const String &input) {
  const char key[] = "cgmSecretKey123";
  int key_len = strlen(key);
  String output = "";
  for (int i = 0; i < input.length(); i++) {
    char xor_char = input[i] ^ key[i % key_len];
    char hex[3];
    sprintf(hex, "%02x", xor_char);
    output += hex;
  }
  return output;
}

String deobfuscate(const String &input) {
  if (input.length() % 2 != 0) return "";
  const char key[] = "cgmSecretKey123";
  int key_len = strlen(key);
  String output = "";
  for (int i = 0; i < input.length(); i += 2) {
    String hex_part = input.substring(i, i + 2);
    char xor_char = (char)strtol(hex_part.c_str(), NULL, 16);
    char orig_char = xor_char ^ key[(i / 2) % key_len];
    output += orig_char;
  }
  return output;
}

void handleLocalExportConfig() {
  if (!checkAuth()) return;
  
  DynamicJsonDocument doc(8192);
  doc["device_name"] = device_name;
  doc["email"] = llu_email;
  doc["password"] = obfuscate(llu_password);
  doc["region"] = llu_region;
  doc["poll"] = llu_poll_interval;
  doc["units"] = llu_units;
  
  doc["c_low"] = toUserUnit(c_low);
  doc["w_low"] = toUserUnit(w_low);
  doc["w_high"] = toUserUnit(w_high);
  doc["c_high"] = toUserUnit(c_high);
  doc["g_min"] = toUserUnit(graph_min);
  doc["g_max"] = toUserUnit(graph_max);
  
  JsonArray rules_arr = doc.createNestedArray("msg_rules");
  for (int i = 0; i < msg_rules_count; i++) {
    JsonObject rule_obj = rules_arr.createNestedObject();
    rule_obj["type"] = msg_rules[i].type;
    rule_obj["val1"] = toUserUnit(msg_rules[i].val1);
    rule_obj["val2"] = toUserUnit(msg_rules[i].val2);
    rule_obj["text"] = msg_rules[i].text;
  }
  
  // Diabetes:M settings
  doc["dm_conn_en"] = dm_enable_connection;
  doc["dm_email"] = dm_email;
  doc["dm_password"] = obfuscate(dm_password);
  doc["dm_enable_2fa"] = dm_enable_2fa;
  doc["dm_note"] = dm_note_text;
  doc["dm_auto_send"] = dm_auto_send;
  doc["dm_poll"] = dm_api_interval;
  doc["dm_offset"] = dm_random_offset;
  doc["dm_start"] = dm_start_sending;
  doc["dm_stop"] = dm_stop_sending;
  doc["dm_tz_json"] = dm_timezone_json;
  doc["dm_tz_posix"] = dm_timezone_posix;
  doc["dm_last_ts"] = dm_last_uploaded_libre_ts;
  doc["dm_hb_en"] = dm_enable_heartbeat;
  doc["dm_hb_int"] = dm_heartbeat_interval;
  doc["dm_auto_cat"] = dm_auto_category;
  doc["dm_fallback_cat"] = dm_fallback_category;
  doc["dm_b_start"] = dm_b_start;
  doc["dm_b_end"] = dm_b_end;
  doc["dm_l_start"] = dm_l_start;
  doc["dm_l_end"] = dm_l_end;
  doc["dm_d_start"] = dm_d_start;
  doc["dm_d_end"] = dm_d_end;
  doc["dm_2fa_pend"] = dm_2fa_pending;
  
  int custom_count = dm_categories_count - 16;
  if (custom_count < 0) custom_count = 0;
  doc["dm_custom_cnt"] = custom_count;
  if (custom_count > 0) {
    JsonArray cats_arr = doc.createNestedArray("dm_cats");
    for (int i = 0; i < custom_count; i++) {
      JsonObject cat_obj = cats_arr.createNestedObject();
      cat_obj["id"] = dm_categories[16 + i].id;
      cat_obj["input_id"] = dm_custom_input_ids[i];
      cat_obj["name"] = dm_categories[16 + i].name;
    }
  }
  
  JsonArray dm_rules_arr = doc.createNestedArray("dm_cat_rules");
  for (int i = 0; i < dm_cat_rules_count; i++) {
    JsonObject rule_obj = dm_rules_arr.createNestedObject();
    rule_obj["cat_id"] = dm_cat_rules[i].category_id;
    rule_obj["start"] = dm_cat_rules[i].start_min;
    rule_obj["end"] = dm_cat_rules[i].end_min;
    rule_obj["en"] = dm_cat_rules[i].enabled;
  }
  
  String json_str;
  serializeJson(doc, json_str);
  
  localServer.sendHeader("Content-Disposition", "attachment; filename=cgm_config.json");
  localServer.send(200, "application/json", json_str);
}

void handleLocalImportConfigGet() {
  if (!checkAuth()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:20px; border-radius:8px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += "input[type=file] { background:#111; color:#ccc; padding:10px; border:1px solid #333; width:100%; box-sizing:border-box; border-radius:4px; margin-top:10px; }";
  html += ".btn { display:block; width:100%; padding:12px; margin-top:20px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; font-size:16px; box-sizing:border-box; }";
  html += ".btn-orange { background:#f0ad4e; }";
  html += ".btn-grey { background:#555; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Load Configuration</h2>";
  html += "<form action='/import-config' method='POST' id='import_form' onsubmit='return handleImportSubmit();'>";
  html += "<label>Load Configuration JSON file:</label>";
  html += "<input type='file' id='config_file_input' accept='.json' required>";
  html += "<input type='hidden' name='config_json' id='config_json'>";
  html += "<button type='submit' class='btn btn-orange'>Load Configuration</button>";
  html += "</form>";
  html += "<a href='/hardware' class='btn btn-grey'>Return to Hardware Control</a>";
  html += "</div>";
  
  html += "<script>";
  html += "function handleImportSubmit() {";
  html += "  var fileInput = document.getElementById('config_file_input');";
  html += "  var file = fileInput.files[0];";
  html += "  if (!file) { alert('Please select a file first.'); return false; }";
  html += "  if (!confirm('WARNING: You are about to overwrite your settings. Are you sure you want to proceed?')) { return false; }";
  html += "  var reader = new FileReader();";
  html += "  reader.onload = function(e) {";
  html += "    var text = e.target.result;";
  html += "    try {";
  html += "      JSON.parse(text);";
  html += "      document.getElementById('config_json').value = text;";
  html += "      document.getElementById('import_form').submit();";
  html += "    } catch(err) {";
  html += "      alert('Error: Invalid JSON file format.');";
  html += "    }";
  html += "  };";
  html += "  reader.readAsText(file);";
  html += "  return false;";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalImportConfig() {
  if (!checkAuth()) return;
  
  if (localServer.hasArg("config_json")) {
    String json_str = localServer.arg("config_json");
    
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, json_str);
    if (err) {
      localServer.send(400, "text/plain", "Bad Request: JSON parse error: " + String(err.c_str()));
      return;
    }
    
    String json_units = doc.containsKey("units") ? doc["units"].as<String>() : String(llu_units);
    
    auto importVal = [&](float val) -> float {
      if (json_units == "mmol/L") return val * 18.0182;
      return val;
    };
    
    if (doc.containsKey("device_name")) {
      String dev_name = doc["device_name"].as<String>();
      dev_name.toCharArray(device_name, sizeof(device_name));
    }
    if (doc.containsKey("email")) {
      String email = doc["email"].as<String>();
      email.toCharArray(llu_email, sizeof(llu_email));
    }
    if (doc.containsKey("password")) {
      String password_obf = doc["password"].as<String>();
      String password = deobfuscate(password_obf);
      password.toCharArray(llu_password, sizeof(llu_password));
    }
    if (doc.containsKey("region")) {
      String region = doc["region"].as<String>();
      region.toCharArray(llu_region, sizeof(llu_region));
    }
    if (doc.containsKey("poll")) {
      llu_poll_interval = doc["poll"].as<int>();
      if (llu_poll_interval < 1) llu_poll_interval = 1;
    }
    if (doc.containsKey("units")) {
      String units = doc["units"].as<String>();
      units.toCharArray(llu_units, sizeof(llu_units));
    }
    if (doc.containsKey("c_low")) {
      c_low = importVal(doc["c_low"].as<float>());
    }
    if (doc.containsKey("w_low")) {
      w_low = importVal(doc["w_low"].as<float>());
    }
    if (doc.containsKey("w_high")) {
      w_high = importVal(doc["w_high"].as<float>());
    }
    if (doc.containsKey("c_high")) {
      c_high = importVal(doc["c_high"].as<float>());
    }
    if (doc.containsKey("g_min")) {
      graph_min = importVal(doc["g_min"].as<float>());
    }
    if (doc.containsKey("g_max")) {
      graph_max = importVal(doc["g_max"].as<float>());
    }
    
    if (doc.containsKey("msg_rules")) {
      JsonArray rules_arr = doc["msg_rules"].as<JsonArray>();
      int rule_idx = 0;
      for (JsonObject rule_obj : rules_arr) {
        if (rule_idx >= 10) break;
        if (rule_obj.containsKey("type") && rule_obj.containsKey("text")) {
          String type = rule_obj["type"].as<String>();
          float val1 = rule_obj["val1"].as<float>();
          float val2 = rule_obj["val2"].as<float>();
          String text = rule_obj["text"].as<String>();
          text.trim();
          
          if (text.length() > 0) {
            strncpy(msg_rules[rule_idx].type, type.c_str(), sizeof(msg_rules[rule_idx].type));
            msg_rules[rule_idx].val1 = importVal(val1);
            msg_rules[rule_idx].val2 = importVal(val2);
            strncpy(msg_rules[rule_idx].text, text.c_str(), sizeof(msg_rules[rule_idx].text));
            rule_idx++;
          }
        }
      }
      
      if (rule_idx < 10) {
        memset(&msg_rules[rule_idx], 0, (10 - rule_idx) * sizeof(MessageRule));
      }
      msg_rules_count = rule_idx;
    }
    
    // Deserialize Diabetes:M settings if present
    if (doc.containsKey("dm_conn_en")) {
      dm_enable_connection = doc["dm_conn_en"].as<bool>();
    }
    if (doc.containsKey("dm_email")) {
      String email = doc["dm_email"].as<String>();
      email.toCharArray(dm_email, sizeof(dm_email));
    }
    if (doc.containsKey("dm_password")) {
      String password_obf = doc["dm_password"].as<String>();
      String password = deobfuscate(password_obf);
      password.toCharArray(dm_password, sizeof(dm_password));
    }
    if (doc.containsKey("dm_enable_2fa")) {
      dm_enable_2fa = doc["dm_enable_2fa"].as<bool>();
    }
    if (doc.containsKey("dm_note")) {
      String note = doc["dm_note"].as<String>();
      note.toCharArray(dm_note_text, sizeof(dm_note_text));
    }
    if (doc.containsKey("dm_auto_send")) {
      dm_auto_send = doc["dm_auto_send"].as<bool>();
    }
    if (doc.containsKey("dm_poll")) {
      dm_api_interval = doc["dm_poll"].as<int>();
      if (dm_api_interval < 5) dm_api_interval = 30;
    }
    if (doc.containsKey("dm_offset")) {
      dm_random_offset = doc["dm_offset"].as<int>();
      if (dm_random_offset < 0) dm_random_offset = 2;
    }
    if (doc.containsKey("dm_start")) {
      String start = doc["dm_start"].as<String>();
      start.toCharArray(dm_start_sending, sizeof(dm_start_sending));
    }
    if (doc.containsKey("dm_stop")) {
      String stop = doc["dm_stop"].as<String>();
      stop.toCharArray(dm_stop_sending, sizeof(dm_stop_sending));
    }
    if (doc.containsKey("dm_tz_json")) {
      String tz_json = doc["dm_tz_json"].as<String>();
      tz_json.toCharArray(dm_timezone_json, sizeof(dm_timezone_json));
    }
    if (doc.containsKey("dm_tz_posix")) {
      String tz_posix = doc["dm_tz_posix"].as<String>();
      tz_posix.toCharArray(dm_timezone_posix, sizeof(dm_timezone_posix));
    }
    if (doc.containsKey("dm_last_ts")) {
      String last_ts = doc["dm_last_ts"].as<String>();
      last_ts.toCharArray(dm_last_uploaded_libre_ts, sizeof(dm_last_uploaded_libre_ts));
    }
    if (doc.containsKey("dm_hb_en")) {
      dm_enable_heartbeat = doc["dm_hb_en"].as<bool>();
    }
    if (doc.containsKey("dm_hb_int")) {
      dm_heartbeat_interval = doc["dm_hb_int"].as<int>();
      if (dm_heartbeat_interval < 1) dm_heartbeat_interval = 15;
    }
    if (doc.containsKey("dm_auto_cat")) {
      dm_auto_category = doc["dm_auto_cat"].as<bool>();
    }
    if (doc.containsKey("dm_fallback_cat")) {
      dm_fallback_category = doc["dm_fallback_cat"].as<int>();
    }
    if (doc.containsKey("dm_b_start")) dm_b_start = doc["dm_b_start"].as<int>();
    if (doc.containsKey("dm_b_end")) dm_b_end = doc["dm_b_end"].as<int>();
    if (doc.containsKey("dm_l_start")) dm_l_start = doc["dm_l_start"].as<int>();
    if (doc.containsKey("dm_l_end")) dm_l_end = doc["dm_l_end"].as<int>();
    if (doc.containsKey("dm_d_start")) dm_d_start = doc["dm_d_start"].as<int>();
    if (doc.containsKey("dm_d_end")) dm_d_end = doc["dm_d_end"].as<int>();
    if (doc.containsKey("dm_2fa_pend")) {
      dm_2fa_pending = doc["dm_2fa_pend"].as<bool>();
    }

    // Set TZ environment
    setenv("TZ", dm_timezone_posix, 1);
    tzset();
    
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putString("email", llu_email);
    preferences.putString("password", llu_password);
    preferences.putString("region", llu_region);
    preferences.putInt("poll", llu_poll_interval);
    preferences.putString("units", llu_units);
    preferences.putFloat("c_low", c_low);
    preferences.putFloat("w_low", w_low);
    preferences.putFloat("w_high", w_high);
    preferences.putFloat("c_high", c_high);
    preferences.putFloat("g_min", graph_min);
    preferences.putFloat("g_max", graph_max);
    preferences.putBytes("msg_rules", msg_rules, sizeof(msg_rules));
    preferences.putInt("msg_rules_cnt", msg_rules_count);
    preferences.putString("dev_name", device_name);
    
    // Commit Diabetes:M settings
    preferences.putString("dm_email", dm_email);
    preferences.putString("dm_password", dm_password);
    preferences.putBool("dm_enable_2fa", dm_enable_2fa);
    preferences.putString("dm_note", dm_note_text);
    preferences.putBool("dm_auto_send", dm_auto_send);
    preferences.putInt("dm_poll", dm_api_interval);
    preferences.putInt("dm_offset", dm_random_offset);
    preferences.putString("dm_start", dm_start_sending);
    preferences.putString("dm_stop", dm_stop_sending);
    preferences.putString("dm_tz_json", dm_timezone_json);
    preferences.putString("dm_tz_posix", dm_timezone_posix);
    preferences.putString("dm_last_ts", dm_last_uploaded_libre_ts);
    preferences.putBool("dm_hb_en", dm_enable_heartbeat);
    preferences.putInt("dm_hb_int", dm_heartbeat_interval);
    preferences.putBool("dm_conn_en", dm_enable_connection);
    preferences.putBool("dm_auto_cat", dm_auto_category);
    preferences.putInt("dm_fallback_cat", dm_fallback_category);
    preferences.putInt("dm_b_start", dm_b_start);
    preferences.putInt("dm_b_end", dm_b_end);
    preferences.putInt("dm_l_start", dm_l_start);
    preferences.putInt("dm_l_end", dm_l_end);
    preferences.putInt("dm_d_start", dm_d_start);
    preferences.putInt("dm_d_end", dm_d_end);
    preferences.putBool("dm_2fa_pend", dm_2fa_pending);
    
    if (doc.containsKey("dm_custom_cnt")) {
      int custom_cnt = doc["dm_custom_cnt"].as<int>();
      if (custom_cnt > 16) custom_cnt = 16;
      if (custom_cnt > 0 && doc.containsKey("dm_cats")) {
        JsonArray cats_arr = doc["dm_cats"].as<JsonArray>();
        int count = 0;
        memset(dm_custom_input_ids, 0, sizeof(dm_custom_input_ids));
        for (JsonVariant val : cats_arr) {
          if (count >= custom_cnt) break;
          dm_categories[16 + count].id = val["id"].as<int>();
          dm_custom_input_ids[count] = val.containsKey("input_id") ? val["input_id"].as<int>() : 0;
          String name = val["name"].as<String>();
          name.toCharArray(dm_categories[16 + count].name, sizeof(dm_categories[16 + count].name));
          count++;
        }
        dm_categories_count = 16 + count;
        preferences.putInt("dm_custom_cnt", count);
        preferences.putBytes("dm_cats", &dm_categories[16], count * sizeof(DMCategory));
        preferences.putBytes("dm_input_ids", dm_custom_input_ids, sizeof(dm_custom_input_ids));
      } else {
        dm_categories_count = 16;
        preferences.putInt("dm_custom_cnt", 0);
      }
    }
    
    if (doc.containsKey("dm_cat_rules")) {
      JsonArray dm_rules_arr = doc["dm_cat_rules"].as<JsonArray>();
      int count = 0;
      for (JsonVariant val : dm_rules_arr) {
        if (count >= 24) break;
        dm_cat_rules[count].category_id = val["cat_id"].as<int>();
        dm_cat_rules[count].start_min = val["start"].as<int>();
        dm_cat_rules[count].end_min = val["end"].as<int>();
        dm_cat_rules[count].enabled = val["en"].as<bool>();
        count++;
      }
      dm_cat_rules_count = count;
      preferences.putInt("dm_cat_rules_cnt", dm_cat_rules_count);
      if (dm_cat_rules_count > 0) {
        preferences.putBytes("dm_cat_rules", dm_cat_rules, dm_cat_rules_count * sizeof(CategoryTimeRule));
      }
    }
    
    preferences.end();
    
    // Recalculate scheduling
    recalculateNextSendTime();
    
    Serial.println("Imported configuration successfully committed to NVS.");
    auth_token = "";
    token_expires = 0;
    
    drawDashboard();
    
    String html = "<html><head><meta http-equiv='refresh' content='3;url=/hardware'><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#5cb85c;}</style></head><body>";
    html += "<h2>Configuration Imported Successfully!</h2>";
    html += "<p>Updating settings and returning to landing page...</p>";
    html += "</body></html>";
    
    localServer.send(200, "text/html", html);
  } else {
    localServer.send(400, "text/plain", "Bad Request: Missing config_json argument");
  }
}

// Diabetes:M API Client functions
bool diabetesMLogin(const char* two_fa_code, String &out_err) {
  DynamicJsonDocument loginDoc(1024);
  loginDoc["username"] = dm_email;
  loginDoc["password"] = dm_password;
  loginDoc["device"] = "web";
  loginDoc["client"] = "web";
  if (two_fa_code != nullptr && strlen(two_fa_code) > 0) {
    loginDoc["two_fa_code"] = two_fa_code;
  }
  
  String loginPayload;
  serializeJson(loginDoc, loginPayload);
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, "https://analytics.diabetes-m.com/api/v1/user/authentication/login_v2")) {
    out_err = "Failed to initialize HTTP client";
    return false;
  }
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "Mozilla/5.0");
  
  if (strlen(dm_cookies) > 0) {
    http.addHeader("Cookie", dm_cookies);
  }
  
  const char* collectHeaders[] = {"Set-Cookie"};
  http.collectHeaders(collectHeaders, 1);
  
  int httpCode = http.POST(loginPayload);
  String response = http.getString();
  
  // Capture cookie if sent
  if (http.hasHeader("Set-Cookie")) {
    String setCookie = http.header("Set-Cookie");
    int semi = setCookie.indexOf(';');
    if (semi != -1) {
      setCookie = setCookie.substring(0, semi);
    }
    strncpy(dm_cookies, setCookie.c_str(), sizeof(dm_cookies));
  }
  
  http.end();
  
  if (httpCode == 200) {
    DynamicJsonDocument respDoc(24576);
    DeserializationError err = deserializeJson(respDoc, response);
    if (err) {
      out_err = "JSON Parse Error: " + String(err.c_str());
      return false;
    }
    
    String token = respDoc["token"].as<String>();
    String user_id = "";
    if (respDoc.containsKey("user")) {
      user_id = respDoc["user"]["user_id"].as<String>();
    }
    
    strncpy(dm_token, token.c_str(), sizeof(dm_token));
    strncpy(dm_user_id, user_id.c_str(), sizeof(dm_user_id));
    
    if (respDoc.containsKey("settings")) {
      parseAndSaveSettings(respDoc["settings"]);
    }
    
    // Commit to NVS
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putString("dm_token", dm_token);
    preferences.putString("dm_cookies", dm_cookies);
    preferences.putString("dm_user_id", dm_user_id);
    preferences.end();
    
    set2FAPending(false);
    return true;
  } else {
    if (response.indexOf("EMAIL_2FA_GENERATED") != -1) {
      out_err = "2FA_REQUIRED";
      Preferences preferences;
      preferences.begin("cgm-config", false);
      preferences.putString("dm_cookies", dm_cookies);
      preferences.end();
      set2FAPending(true);
      return false;
    }
    
    out_err = "HTTP Code: " + String(httpCode) + " - " + response;
    return false;
  }
}

void handleAuthExpired() {
  dm_token[0] = '\0';
  dm_cookies[0] = '\0';
  
  Preferences preferences;
  preferences.begin("cgm-config", false);
  preferences.putString("dm_token", "");
  preferences.putString("dm_cookies", "");
  preferences.end();
  
  set2FAPending(true);
  Serial.println("[DM] Authentication expired. Cleared token and set 2FA pending.");
}

bool diabetesMGetProfile(String &out_info) {
  if (strlen(dm_token) == 0) {
    out_info = "Not authenticated";
    return false;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, "https://analytics.diabetes-m.com/api/v1/user/profile/get_profile")) {
    out_info = "Failed to initialize HTTP client";
    return false;
  }
  
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Authorization", "Bearer " + String(dm_token));
  if (strlen(dm_cookies) > 0) {
    http.addHeader("Cookie", dm_cookies);
  }
  
  int httpCode = http.GET();
  String response = http.getString();
  http.end();
  
  if (httpCode == 200) {
    DynamicJsonDocument doc(24576);
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
      out_info = "Profile parse error: " + String(err.c_str());
      return false;
    }
    
    String email = doc["email"].as<String>();
    String user_id = doc["user_id"].as<String>();
    
    if (doc.containsKey("settings")) {
      parseAndSaveSettings(doc["settings"]);
    }
    
    set2FAPending(false);
    
    // Auto-fetch dynamic custom categories
    String cat_err = "";
    diabetesMGetCategories(cat_err);
    
    out_info = "Connected as: " + email + " (User ID: " + user_id + ")";
    return true;
  } else {
    if (httpCode == 401 || response.indexOf("Token validation failed") != -1) {
      handleAuthExpired();
    }
    out_info = "HTTP Code: " + String(httpCode) + " - " + response;
    return false;
  }
}

void parseAndSaveSettings(JsonVariant settings) {
  if (settings.isNull()) return;
  
  bool updated = false;
  if (settings.containsKey("breakfast_meal_time")) {
    int b_start = settings["breakfast_meal_time"][0].as<int>();
    int b_end = settings["breakfast_meal_time"][1].as<int>();
    if (b_start != dm_b_start || b_end != dm_b_end) {
      dm_b_start = b_start;
      dm_b_end = b_end;
      updated = true;
    }
  }
  if (settings.containsKey("lunch_meal_time")) {
    int l_start = settings["lunch_meal_time"][0].as<int>();
    int l_end = settings["lunch_meal_time"][1].as<int>();
    if (l_start != dm_l_start || l_end != dm_l_end) {
      dm_l_start = l_start;
      dm_l_end = l_end;
      updated = true;
    }
  }
  if (settings.containsKey("dinner_meal_time")) {
    int d_start = settings["dinner_meal_time"][0].as<int>();
    int d_end = settings["dinner_meal_time"][1].as<int>();
    if (d_start != dm_d_start || d_end != dm_d_end) {
      dm_d_start = d_start;
      dm_d_end = d_end;
      updated = true;
    }
  }
  
  if (updated) {
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putInt("dm_b_start", dm_b_start);
    preferences.putInt("dm_b_end", dm_b_end);
    preferences.putInt("dm_l_start", dm_l_start);
    preferences.putInt("dm_l_end", dm_l_end);
    preferences.putInt("dm_d_start", dm_d_start);
    preferences.putInt("dm_d_end", dm_d_end);
    preferences.end();
    Serial.println("Updated and saved meal times from settings.");
  }
}

int getSuggestedCategory(int hour, int minute) {
  int m = hour * 60 + minute;
  
  for (int i = 0; i < dm_cat_rules_count; i++) {
    if (!dm_cat_rules[i].enabled) continue;
    
    int start = dm_cat_rules[i].start_min;
    int end = dm_cat_rules[i].end_min;
    
    if (start <= end) {
      if (m >= start && m <= end) {
        return dm_cat_rules[i].category_id;
      }
    } else {
      if (m >= start || m <= end) {
        return dm_cat_rules[i].category_id;
      }
    }
  }
  
  return dm_fallback_category;
}

void set2FAPending(bool pending) {
  if (dm_2fa_pending != pending) {
    dm_2fa_pending = pending;
    Preferences preferences;
    preferences.begin("cgm-config", false);
    preferences.putBool("dm_2fa_pend", dm_2fa_pending);
    preferences.end();
    Serial.printf("[DM] 2FA pending state changed to: %s\n", pending ? "TRUE" : "FALSE");
  }
}

bool diabetesMGetCategories(String &out_err) {
  if (strlen(dm_token) == 0) {
    out_err = "Not authenticated";
    return false;
  }
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, "https://analytics.diabetes-m.com/api/v1/user/categories/list")) {
    out_err = "Failed to initialize HTTP client";
    return false;
  }
  
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Authorization", "Bearer " + String(dm_token));
  if (strlen(dm_cookies) > 0) {
    http.addHeader("Cookie", dm_cookies);
  }
  
  int httpCode = http.GET();
  String response = http.getString();
  http.end();
  
  if (httpCode == 200) {
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
      out_err = "Categories parse error: " + String(err.c_str());
      return false;
    }
    
    JsonArray arr;
    bool has_cats = false;
    if (doc["categories"].is<JsonArray>()) {
      arr = doc["categories"].as<JsonArray>();
      has_cats = true;
    } else if (doc["categories"]["categories"].is<JsonArray>()) {
      arr = doc["categories"]["categories"].as<JsonArray>();
      has_cats = true;
    }
    
    if (has_cats) {
      int custom_cnt = 0;
      memset(dm_custom_input_ids, 0, sizeof(dm_custom_input_ids));
      for (JsonVariant val : arr) {
        if (custom_cnt >= 16) break;
        if (val["deleted"].as<bool>()) continue;
        
        int cat_id = val["category_id"].as<int>();
        int input_id = val["input_id"].as<int>();
        String name = val["name"].as<String>();
        
        dm_categories[16 + custom_cnt].id = cat_id;
        dm_custom_input_ids[custom_cnt] = input_id;
        name.toCharArray(dm_categories[16 + custom_cnt].name, sizeof(dm_categories[16 + custom_cnt].name));
        custom_cnt++;
      }
      
      dm_categories_count = 16 + custom_cnt;
      
      Preferences preferences;
      preferences.begin("cgm-config", false);
      preferences.putInt("dm_custom_cnt", custom_cnt);
      if (custom_cnt > 0) {
        preferences.putBytes("dm_cats", &dm_categories[16], custom_cnt * sizeof(DMCategory));
        preferences.putBytes("dm_input_ids", dm_custom_input_ids, sizeof(dm_custom_input_ids));
      }
      preferences.end();
      Serial.printf("Downloaded and saved %d custom categories.\n", custom_cnt);
      return true;
    }
    out_err = "No categories key in response";
    return false;
  } else {
    if (httpCode == 401 || response.indexOf("Token validation failed") != -1) {
      handleAuthExpired();
    }
    out_err = "HTTP Code: " + String(httpCode) + " - " + response;
    return false;
  }
}

bool diabetesMUploadReading(float glucose_mgdl, const String &notes, String &out_err) {
  if (strlen(dm_token) == 0) {
    out_err = "Not authenticated";
    return false;
  }
  
  if (!isTimeSynced()) {
    out_err = "Time not synchronized via NTP";
    return false;
  }
  
  uint64_t current_time_ms = (uint64_t)time(nullptr) * 1000;
  
  float glucose_mmol = glucose_mgdl / 18.0182;
  String glucose_str = "";
  if (strcmp(llu_units, "mmol/L") == 0) {
    glucose_str = String(glucose_mmol, 1);
  } else {
    glucose_str = String((int)glucose_mgdl);
  }
  
  DynamicJsonDocument doc(4096);
  doc["entry_id"] = 0;
  doc["user_id"] = dm_user_id;
  doc["input_id"] = 0;
  doc["photo_id"] = 0;
  doc["device_hash"] = 0;
  doc["last_modified"] = 0;
  doc["deleted"] = false;
  doc["entry_time"] = current_time_ms;
  doc["glucose"] = glucose_mmol;
  doc["carbs"] = 0;
  doc["proteins"] = 0;
  doc["fats"] = 0;
  doc["calories"] = 0;
  doc["carb_bolus"] = 0;
  doc["correction_bolus"] = 0;
  doc["extended_bolus"] = 0;
  doc["extended_bolus_duration"] = 0;
  doc["basal"] = 0;
  doc["bolus_insulin_type"] = 13;
  doc["basal_insulin_type"] = 32;
  doc["weight_entry"] = 0;
  doc["weight"] = 0;
  int category_id = dm_fallback_category;
  if (dm_auto_category) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (localtime_r(&now, &timeinfo)) {
      int suggested = getSuggestedCategory(timeinfo.tm_hour, timeinfo.tm_min);
      if (suggested != 8) {
        category_id = suggested;
      }
    }
  }
  
  // Map category_id to input_id if it's a custom category
  int upload_category = category_id;
  for (int i = 0; i < 16; i++) {
    if (dm_categories[16 + i].id == category_id) {
      if (dm_custom_input_ids[i] > 0) {
        upload_category = dm_custom_input_ids[i];
      }
      break;
    }
  }
  doc["category"] = upload_category;
  doc["carb_ratio_factor"] = 0;
  doc["insulin_sensitivity_factor"] = 0;
  doc["notes"] = notes;
  doc["is_sensor"] = false;
  doc["pressure_sys"] = 0;
  doc["pressure_dia"] = 0;
  doc["pulse"] = 0;
  doc["injection_bolus_site"] = -1;
  doc["injection_basal_site"] = -1;
  doc["finger_test_site"] = -1;
  doc["ketones"] = 0;
  doc["timezone"] = dm_timezone_json;
  doc["exercise_index"] = 0;
  doc["exercise_comment"] = "";
  doc["exercise_duration"] = 0;
  
  doc.createNestedArray("medications_list");
  doc.createNestedArray("food_list");
  
  doc["hba1c"] = 0;
  doc["cholesterol_total"] = 0;
  doc["cholesterol_ldl"] = 0;
  doc["cholesterol_hdl"] = 0;
  doc["glucoseInCurrentUnit"] = glucose_str;
  doc["carbsInCurrentUnit"] = 0;
  doc["weightEntryInCurrentUnit"] = 0;
  doc["hba1cInCurrentUnit"] = 0;
  doc["cholesterol_totalInCurrentUnit"] = 0;
  doc["cholesterol_ldlInCurrentUnit"] = 0;
  doc["cholesterol_hdlInCurrentUnit"] = 0;
  doc["ketonesInCurrentUnit"] = 0;
  doc["triglyceridesInCurrentUnit"] = 0;
  doc["microalbumin_test_type"] = 0;
  doc["microalbumin"] = "";
  doc["creatinine_clearance"] = "";
  doc["egfr"] = "";
  doc["cystatin_c"] = "";
  doc["albumin"] = "";
  doc["creatinine"] = "";
  doc["calcium"] = "";
  doc["total_protein"] = "";
  doc["sodium"] = "";
  doc["potassium"] = "";
  doc["bicarbonate"] = "";
  doc["chloride"] = "";
  doc["alp"] = "";
  doc["alt"] = "";
  doc["ast"] = "";
  doc["bilirubin"] = "";
  doc["bun"] = "";
  doc["basal_is_rate"] = false;
  
  String payload;
  serializeJson(doc, payload);
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, "https://analytics.diabetes-m.com/api/v1/diary/entries/save_as_new")) {
    out_err = "Failed to initialize HTTP client";
    return false;
  }
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Authorization", "Bearer " + String(dm_token));
  if (strlen(dm_cookies) > 0) {
    http.addHeader("Cookie", dm_cookies);
  }
  
  int httpCode = http.POST(payload);
  String response = http.getString();
  http.end();
  
  if (httpCode == 200) {
    out_err = "Sent OK";
    return true;
  } else {
    out_err = "Error (" + String(httpCode) + ")";
    return false;
  }
}

bool uploadWithRetry(float glucose_mgdl, const String &notes, String &out_err) {
  bool ok = diabetesMUploadReading(glucose_mgdl, notes, out_err);
  if (!ok && (out_err.indexOf("401") != -1 || out_err.indexOf("Unauthorized") != -1 || out_err.indexOf("authenticated") != -1)) {
    if (dm_enable_2fa) {
      set2FAPending(true);
      out_err = "Auth expired, 2FA re-auth required";
      return false;
    }
    Serial.println("Token expired. Attempting automatic re-login...");
    String login_err;
    if (diabetesMLogin(nullptr, login_err)) {
      Serial.println("Re-login successful! Retrying upload...");
      ok = diabetesMUploadReading(glucose_mgdl, notes, out_err);
    } else {
      out_err = "Auth expired, auto login failed: " + login_err;
    }
  }
  return ok;
}

void handleLocalDebugDM() {
  if (!checkAuth()) return;
  
  String output = "=== Diabetes:M Categories Debug Info ===\n\n";
  output += "RAM State:\n";
  output += "Connection Enabled: " + String(dm_enable_connection ? "Yes" : "No") + "\n";
  output += "Email: " + String(dm_email) + "\n";
  output += "Token Length: " + String(strlen(dm_token)) + "\n";
  if (strlen(dm_token) > 0) {
    output += "Token Prefix: " + String(dm_token).substring(0, 8) + "...\n";
  }
  output += "Cookies Length: " + String(strlen(dm_cookies)) + "\n";
  if (strlen(dm_cookies) > 0) {
    output += "Cookies Prefix: " + String(dm_cookies).substring(0, 30) + "...\n";
  }
  output += "Categories Count: " + String(dm_categories_count) + "\n";
  output += "\nLoaded Categories in RAM:\n";
  for (int i = 0; i < dm_categories_count; i++) {
    output += String(i) + ". ID=" + String(dm_categories[i].id) + ", Name=\"" + String(dm_categories[i].name) + "\"\n";
  }
  
  output += "\nNVS Saved Count:\n";
  Preferences prefs;
  prefs.begin("cgm-config", true);
  output += "NVS dm_custom_cnt: " + String(prefs.getInt("dm_custom_cnt", -1)) + "\n";
  prefs.end();
  
  output += "\n--- Running Live categories list query ---\n";
  if (strlen(dm_token) == 0) {
    output += "Cannot query: No token in RAM. Try testing connection first.\n";
  } else {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, "https://analytics.diabetes-m.com/api/v1/user/categories/list")) {
      output += "HTTP Client begin failed.\n";
    } else {
      http.addHeader("Accept", "application/json");
      http.addHeader("User-Agent", "Mozilla/5.0");
      http.addHeader("Authorization", "Bearer " + String(dm_token));
      if (strlen(dm_cookies) > 0) {
        http.addHeader("Cookie", dm_cookies);
      }
      int httpCode = http.GET();
      output += "HTTP Status Code: " + String(httpCode) + "\n";
      String response = http.getString();
      output += "Response Length: " + String(response.length()) + "\n";
      output += "Response Content:\n" + response + "\n";
      http.end();
    }
  }
  
  localServer.send(200, "text/plain", output);
}

void handleLocalDMGet() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  // Auto-sync dynamic custom categories on settings page load
  if (dm_enable_connection && !dm_2fa_pending && WiFi.status() == WL_CONNECTED && strlen(dm_email) > 0 && strlen(dm_password) > 0) {
    String cat_err = "";
    bool success = false;
    if (strlen(dm_token) > 0) {
      success = diabetesMGetCategories(cat_err);
    }
    if (!success) {
      if (!dm_enable_2fa) {
        String login_err = "";
        if (diabetesMLogin(nullptr, login_err)) {
          diabetesMGetCategories(cat_err);
        }
      } else {
        set2FAPending(true);
      }
    }
  }
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:15px; border-radius:8px; margin-bottom:20px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += ".form-group { margin-bottom:15px; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += "input, select { width:100%; padding:10px; border-radius:4px; border:1px solid #444; background:#111; color:#fff; box-sizing:border-box; font-size:16px; }";
  html += "input[type='checkbox'] { width:auto; margin-right:10px; vertical-align:middle; }";
  html += ".btn { display:block; width:100%; padding:12px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; margin-top:20px; font-size:16px; box-sizing:border-box; }";
  html += ".btn-blue { background:#33B5E5; margin-top:10px; }";
  html += ".btn-orange { background:#f0ad4e; margin-top:10px; }";
  html += ".btn-grey { background:#555; margin-top:10px; }";
  html += ".btn-row { display:flex; gap:10px; margin-top:15px; }";
  html += ".btn-row button { flex:1; padding:10px; font-weight:bold; cursor:pointer; border-radius:4px; border:none; color:#fff; font-size:14px; }";
  html += "</style></head><body onload='toggleConnectionFields(); initSlots();'>";
  
  html += "<div class='card'>";
  html += "<h2>Configure Diabetes:M</h2>";
  html += "<form action='/save-diabetes-m' method='POST' id='dm_form' onsubmit='return false;'>";
  
  html += "<div class='form-group'>";
  html += "<label><input type='checkbox' id='dm_conn_en' name='dm_conn_en'" + String(dm_enable_connection ? " checked" : "") + " onclick='toggleConnectionFields()'> Enable Diabetes:M Connection Support</label>";
  html += "</div>";
  
  html += "<div id='dm_fields_container'>";
  
  html += "<div class='form-group'>";
  html += "<label>Username or Email</label>";
  html += "<input type='text' id='dm_email' name='email' value='" + String(dm_email) + "' maxlength='64' placeholder='example@email.com'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Password</label>";
  html += "<input type='password' id='dm_password' name='password' value='" + String(dm_password) + "' maxlength='64'>";
  html += "<div style='margin:5px 0;'><input type='checkbox' onclick='var x=document.getElementById(\"dm_password\");x.type=this.checked?\"text\":\"password\";'> Show Password</div>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label><input type='checkbox' id='dm_auto_send' name='auto_send'" + String(dm_auto_send ? " checked" : "") + " onclick='toggleAutoSendFields()'> Enable auto send Libre reading</label>";
  html += "</div>";
  
  html += "<div class='form-group' id='2fa_container' style='display:" + String(dm_2fa_pending ? "block" : "none") + ";border:1px solid #f0ad4e;padding:15px;border-radius:6px;background:#2a251b;'>";
  html += "<label style='color:#f0ad4e;'>2FA Code Required</label>";
  html += "<input type='text' id='two_fa_code' name='two_fa_code' placeholder='Enter code from email' style='margin-bottom:10px;'>";
  html += "<div class='btn-row'>";
  html += "<button type='button' id='btn_2fa_ok' style='background:#5cb85c;margin-top:0;' onclick='submit2FACode()'>OK</button>";
  html += "<button type='button' id='btn_2fa_request' style='background:#f0ad4e;margin-top:0;' onclick='requestNew2FA()'>Request New 2FA</button>";
  html += "</div>";
  html += "</div>";
  
  html += "<hr style='border:0;border-top:1px solid #333;margin:20px 0;'>";
  
  html += "<div class='form-group'>";
  html += "<label>Sending Interval (minutes)</label>";
  html += "<input type='number' name='poll' value='" + String(dm_api_interval) + "' min='5' max='1440'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Sending Random Offset (minutes)</label>";
  html += "<input type='number' name='offset' value='" + String(dm_random_offset) + "' min='0' max='30'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Note Text</label>";
  html += "<input type='text' id='dm_note' name='note' value='" + String(dm_note_text) + "' maxlength='64'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Timezone</label>";
  html += "<select name='timezone' id='timezone'>";
  struct TZOpt { const char* val; const char* label; };
  TZOpt timezones[] = {
    {"Europe/London", "Europe/London"},
    {"Europe/Berlin", "Europe/Berlin"},
    {"UTC", "UTC"},
    {"America/New_York", "America/New_York"},
    {"America/Los_Angeles", "America/Los_Angeles"}
  };
  for (auto& tz : timezones) {
    String selected = (strcmp(dm_timezone_json, tz.val) == 0) ? " selected" : "";
    html += "<option value='" + String(tz.val) + "'" + selected + ">" + String(tz.label) + "</option>";
  }
  html += "</select>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label><input type='checkbox' id='dm_auto_cat' name='dm_auto_cat'" + String(dm_auto_category ? " checked" : "") + "> Enable Auto-Category Assignment</label>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Fallback Category</label>";
  html += "<select name='dm_fallback_cat' id='dm_fallback_cat' onchange='checkCategoriesExist()'>";
  for (int i = 0; i < dm_categories_count; i++) {
    String selected = (dm_fallback_category == dm_categories[i].id) ? " selected" : "";
    html += "<option value='" + String(dm_categories[i].id) + "'" + selected + ">" + String(dm_categories[i].name) + "</option>";
  }
  html += "</select>";
  html += "</div>";

  html += "<details class='card' style='margin-top:20px;' open><summary style='font-weight:bold;cursor:pointer;color:#33B5E5;font-size:18px;'>Configure Category Time Slots (max 24)</summary>";
  html += "<div style='margin-top:10px;'>";
  html += "<table id='slots_table' style='width:100%;border-collapse:collapse;'>";
  html += "<thead><tr style='border-bottom:1px solid #444;text-align:left;color:#888;font-size:12px;'>";
  html += "<th style='padding:5px;width:50px;'>Active</th>";
  html += "<th style='padding:5px;'>Category</th>";
  html += "<th style='padding:5px;width:120px;'>Start Time</th>";
  html += "<th style='padding:5px;width:120px;'>End Time</th>";
  html += "<th style='padding:5px;width:80px;'>Action</th>";
  html += "</tr></thead>";
  html += "<tbody id='slots_body'>";
  html += "</tbody></table>";
  html += "<div class='btn-row' style='display:flex;flex-wrap:wrap;gap:8px;'>";
  html += "<button type='button' class='btn-blue' style='padding:6px 12px;font-size:13px;margin:0;flex:1;min-width:120px;' onclick='addSlotRow()'>+ Add Time Slot</button>";
  html += "<button type='button' class='btn-orange' style='padding:6px 12px;font-size:13px;margin:0;flex:2;min-width:220px;' onclick='grabMealTimes()'>Reset to Diabetes:M Portal Meal Times</button>";
  html += "<button type='button' style='background:#33B5E5;color:#fff;padding:6px 12px;font-size:13px;border:none;border-radius:4px;cursor:pointer;margin:0;flex:1;min-width:100px;' onclick='sortTimes()'>Sort Times</button>";
  html += "<button type='button' style='background:#5bc85c;color:#fff;padding:6px 12px;font-size:13px;border:none;border-radius:4px;cursor:pointer;margin:0;flex:1.5;min-width:160px;' onclick='redownloadCategories()'>Redownload Categories</button>";
  html += "</div>";
  html += "</div>";
  html += "</details>";
  html += "<input type='hidden' id='dm_cat_rules_json' name='dm_cat_rules_json' value=''>";
  
  html += "<div class='form-group'>";
  html += "<label>Start Sending</label>";
  html += "<input type='datetime-local' id='dm_start' name='start' value='" + String(dm_start_sending) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Stop Sending</label>";
  html += "<input type='datetime-local' id='dm_stop' name='stop' value='" + String(dm_stop_sending) + "'>";
  html += "</div>";
  
  html += "<hr style='border:0;border-top:1px solid #333;margin:20px 0;'>";
  
  html += "<div class='form-group'>";
  html += "<label><input type='checkbox' id='dm_hb_en' name='dm_hb_en'" + String(dm_enable_heartbeat ? " checked" : "") + " onclick='toggleHeartbeatFields()'> Enable Heartbeat</label>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>API Heartbeat Time (minutes)</label>";
  html += "<input type='number' id='dm_hb_int' name='dm_hb_int' value='" + String(dm_heartbeat_interval) + "' min='1' max='1440'>";
  html += "</div>";
  
  html += "<div id='status_msg' style='margin:15px 0;'></div>";
  
  html += "<div class='btn-row'>";
  html += "<button type='button' class='btn-orange' onclick='runTestConnection()'>Test Connection</button>";
  html += "<button type='button' class='btn-blue' onclick='runTestUpload()'>Test Upload</button>";
  html += "</div>";
  
  html += "</div>"; // Close dm_fields_container
  
  html += "<button type='button' class='btn' onclick='submitForm()'>Save Configuration</button>";
  html += "<a href='/' class='btn btn-grey'>Return to Home Page</a>";
  html += "</form>";
  html += "</div>";
  
  html += "<div id='dm_logs_container'>";
  
  // Show Heartbeat Log Collapsible
  if (dm_enable_heartbeat) {
    html += "<details class='card' style='margin-top:10px;'><summary style='font-weight:bold;cursor:pointer;color:#33B5E5;font-size:18px;'>Show Heartbeat Log (Last Succeeded: " + formatLocalTime(dm_last_heartbeat_epoch, "%Y-%m-%d %H:%M:%S") + ")</summary>";
    html += "<table style='width:100%;border-collapse:collapse;margin-top:10px;font-size:14px;'>";
    html += "<thead><tr style='border-bottom:1px solid #444;text-align:left;'><th style='padding:5px;'>Time</th><th style='padding:5px;'>Status</th></tr></thead>";
    html += "<tbody>";
    if (dm_heartbeat_logs_count == 0) {
      html += "<tr><td colspan='2' style='padding:10px;text-align:center;color:#888;'>No heartbeat logs yet.</td></tr>";
    } else {
      for (int i = 0; i < dm_heartbeat_logs_count; i++) {
        String time_str = formatLocalTime(dm_heartbeat_logs[i].timestamp, "%Y-%m-%d %H:%M:%S");
        html += "<tr style='border-bottom:1px solid #333;'>";
        html += "<td style='padding:5px;white-space:nowrap;'>" + time_str + "</td>";
        html += "<td style='padding:5px;color:#ccc;'>" + dm_heartbeat_logs[i].status + "</td>";
        html += "</tr>";
      }
    }
    html += "</tbody></table></details>";
  }

  // Show Log Collapsible
  html += "<details class='card' style='margin-top:20px;'><summary style='font-weight:bold;cursor:pointer;color:#33B5E5;font-size:18px;'>Show Communication Log</summary>";
  html += "<table style='width:100%;border-collapse:collapse;margin-top:10px;font-size:14px;'>";
  html += "<thead><tr style='border-bottom:1px solid #444;text-align:left;'><th style='padding:5px;'>Time</th><th style='padding:5px;'>Glucose</th><th style='padding:5px;'>Status</th></tr></thead>";
  html += "<tbody>";
  if (dm_logs_count == 0) {
    html += "<tr><td colspan='3' style='padding:10px;text-align:center;color:#888;'>No log entries yet.</td></tr>";
  } else {
    for (int i = 0; i < dm_logs_count; i++) {
      String time_str = formatLocalTime(dm_logs[i].timestamp, "%Y-%m-%d %H:%M:%S");
      String val_str = "";
      if (strcmp(llu_units, "mmol/L") == 0) {
        val_str = String(dm_logs[i].value / 18.0182, 1) + " mmol/L";
      } else {
        val_str = String((int)dm_logs[i].value) + " mg/dL";
      }
      html += "<tr style='border-bottom:1px solid #333;'>";
      html += "<td style='padding:5px;white-space:nowrap;'>" + time_str + "</td>";
      html += "<td style='padding:5px;white-space:nowrap;'>" + val_str + "</td>";
      html += "<td style='padding:5px;color:#ccc;'>" + dm_logs[i].status + "</td>";
      html += "</tr>";
    }
  }
  html += "</tbody></table></details>";
  html += "</div>"; // Close dm_logs_container
  
  // Scripts
  html += "<script>";
  html += "var dm_categories = [";
  for (int i = 0; i < dm_categories_count; i++) {
    html += "{id:" + String(dm_categories[i].id) + ",name:'" + String(dm_categories[i].name) + "'}";
    if (i < dm_categories_count - 1) html += ",";
  }
  html += "\n];\n";
  html += "var dm_rules = [";
  for (int i = 0; i < dm_cat_rules_count; i++) {
    html += "{cat_id:" + String(dm_cat_rules[i].category_id) + ",";
    html += "start:'" + formatMinutesToHM(dm_cat_rules[i].start_min) + "',";
    html += "end:'" + formatMinutesToHM(dm_cat_rules[i].end_min) + "',";
    html += "enabled:" + String(dm_cat_rules[i].enabled ? "true" : "false") + "}";
    if (i < dm_cat_rules_count - 1) html += ",";
  }
  html += "\n];\n";
  html += "var meal_b_start = " + String(dm_b_start) + ";\n";
  html += "var meal_b_end = " + String(dm_b_end) + ";\n";
  html += "var meal_l_start = " + String(dm_l_start) + ";\n";
  html += "var meal_l_end = " + String(dm_l_end) + ";\n";
  html += "var meal_d_start = " + String(dm_d_start) + ";\n";
  html += "var meal_d_end = " + String(dm_d_end) + ";\n";
  html += "function addSlotRow(catId, startVal, endVal, enabled) {\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  var rowCount = tbody.rows.length;\n";
  html += "  if (rowCount >= 24) { alert('Maximum 24 slots allowed.'); return; }\n";
  html += "  catId = catId !== undefined ? catId : 8;\n";
  html += "  startVal = startVal !== undefined ? startVal : '08:00';\n";
  html += "  endVal = endVal !== undefined ? endVal : '09:00';\n";
  html += "  enabled = enabled !== undefined ? enabled : true;\n";
  html += "  var tr = document.createElement('tr');\n";
  html += "  tr.style.borderBottom = '1px solid #333';\n";
  html += "  var td1 = document.createElement('td');\n";
  html += "  td1.style.padding = '5px';\n";
  html += "  td1.innerHTML = '<input type=\"checkbox\" class=\"slot-en\" ' + (enabled ? 'checked' : '') + ' onchange=\"checkOverlaps()\">';\n";
  html += "  tr.appendChild(td1);\n";
  html += "  var td2 = document.createElement('td');\n";
  html += "  td2.style.padding = '5px';\n";
  html += "  var select = document.createElement('select');\n";
  html += "  select.className = 'slot-cat';\n";
  html += "  select.onchange = function() { checkCategoriesExist(); };\n";
  html += "  dm_categories.forEach(function(cat) {\n";
  html += "    var opt = document.createElement('option');\n";
  html += "    opt.value = cat.id;\n";
  html += "    opt.textContent = cat.name;\n";
  html += "    if (cat.id == catId) opt.selected = true;\n";
  html += "    select.appendChild(opt);\n";
  html += "  });\n";
  html += "  td2.appendChild(select);\n";
  html += "  tr.appendChild(td2);\n";
  html += "  var td3 = document.createElement('td');\n";
  html += "  td3.style.padding = '5px';\n";
  html += "  td3.innerHTML = '<input type=\"time\" class=\"slot-start\" value=\"' + startVal + '\" onchange=\"checkOverlaps()\">';\n";
  html += "  tr.appendChild(td3);\n";
  html += "  var td4 = document.createElement('td');\n";
  html += "  td4.style.padding = '5px';\n";
  html += "  td4.innerHTML = '<input type=\"time\" class=\"slot-end\" value=\"' + endVal + '\" onchange=\"checkOverlaps()\">';\n";
  html += "  tr.appendChild(td4);\n";
  html += "  var td5 = document.createElement('td');\n";
  html += "  td5.style.padding = '5px';\n";
  html += "  td5.innerHTML = '<button type=\"button\" class=\"btn-orange\" style=\"margin-top:0;padding:4px 8px;font-size:12px;\" onclick=\"deleteSlotRow(this)\">Delete</button>';\n";
  html += "  tr.appendChild(td5);\n";
  html += "  tbody.appendChild(tr);\n";
  html += "}\n";
  html += "function deleteSlotRow(btn) {\n";
  html += "  var tr = btn.parentNode.parentNode;\n";
  html += "  tr.parentNode.removeChild(tr);\n";
  html += "  checkOverlaps();\n";
  html += "  checkCategoriesExist();\n";
  html += "}\n";
  html += "function initSlots() {\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  if (tbody) {\n";
  html += "    tbody.innerHTML = '';\n";
  html += "    dm_rules.forEach(function(r) {\n";
  html += "      addSlotRow(r.cat_id, r.start, r.end, r.enabled);\n";
  html += "    });\n";
  html += "    checkOverlaps();\n";
  html += "    checkCategoriesExist();\n";
  html += "  }\n";
  html += "}\n";
  html += "function grabMealTimes() {\n";
  html += "  if (!confirm('WARNING: This will overwrite your current custom time slot settings with the default meal times from your Diabetes:M portal profile. Do you want to proceed?')) return;\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  tbody.innerHTML = '';\n";
  html += "  function toHM(m) {\n";
  html += "    var h = Math.floor(m / 60) % 24;\n";
  html += "    var min = m % 60;\n";
  html += "    return (h < 10 ? '0' + h : h) + ':' + (min < 10 ? '0' + min : min);\n";
  html += "  }\n";
  html += "  var after_b_end = meal_l_start - 61;\n";
  html += "  if (after_b_end < meal_b_end + 1) after_b_end = meal_b_end + 1;\n";
  html += "  var after_l_end = meal_d_start - 61;\n";
  html += "  if (after_l_end < meal_l_end + 1) after_l_end = meal_l_end + 1;\n";
  html += "  var after_d_end = 21 * 60 - 1;\n";
  html += "  if (after_d_end < meal_d_end + 1) after_d_end = meal_d_end + 1;\n";
  html += "  var night_end = meal_b_start - 61;\n";
  html += "  if (night_end < 0) night_end = 0;\n";
  html += "  addSlotRow(1, toHM(meal_b_start - 60), toHM(meal_b_start - 1), true);\n";
  html += "  addSlotRow(11, toHM(meal_b_start), toHM(meal_b_end), true);\n";
  html += "  addSlotRow(2, toHM(meal_b_end + 1), toHM(after_b_end), true);\n";
  html += "  addSlotRow(3, toHM(meal_l_start - 60), toHM(meal_l_start - 1), true);\n";
  html += "  addSlotRow(12, toHM(meal_l_start), toHM(meal_l_end), true);\n";
  html += "  addSlotRow(4, toHM(meal_l_end + 1), toHM(after_l_end), true);\n";
  html += "  addSlotRow(5, toHM(meal_d_start - 60), toHM(meal_d_start - 1), true);\n";
  html += "  addSlotRow(13, toHM(meal_d_start), toHM(meal_d_end), true);\n";
  html += "  addSlotRow(6, toHM(meal_d_end + 1), toHM(after_d_end), true);\n";
  html += "  addSlotRow(10, '21:00', '23:59', true);\n";
  html += "  addSlotRow(7, '00:00', toHM(night_end), true);\n";
  html += "  checkOverlaps();\n";
  html += "  checkCategoriesExist();\n";
  html += "}\n";
  html += "function rangeIntersects(s1, e1, s2, e2) {\n";
  html += "  var m1 = new Array(1440).fill(false);\n";
  html += "  if (s1 <= e1) {\n";
  html += "    for (var m = s1; m <= e1; m++) m1[m] = true;\n";
  html += "  } else {\n";
  html += "    for (var m = s1; m < 1440; m++) m1[m] = true;\n";
  html += "    for (var m = 0; m <= e1; m++) m1[m] = true;\n";
  html += "  }\n";
  html += "  if (s2 <= e2) {\n";
  html += "    for (var m = s2; m <= e2; m++) { if (m1[m]) return true; }\n";
  html += "  } else {\n";
  html += "    for (var m = s2; m < 1440; m++) { if (m1[m]) return true; }\n";
  html += "    for (var m = 0; m <= e2; m++) { if (m1[m]) return true; }\n";
  html += "  }\n";
  html += "  return false;\n";
  html += "}\n";
  html += "function checkOverlaps() {\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  if (!tbody) return false;\n";
  html += "  var rows = tbody.querySelectorAll('tr');\n";
  html += "  rows.forEach(function(row) {\n";
  html += "    var sEl = row.querySelector('.slot-start');\n";
  html += "    var eEl = row.querySelector('.slot-end');\n";
  html += "    if (sEl) { sEl.style.borderColor = ''; sEl.style.backgroundColor = ''; }\n";
  html += "    if (eEl) { eEl.style.borderColor = ''; eEl.style.backgroundColor = ''; }\n";
  html += "  });\n";
  html += "  var parsed = [];\n";
  html += "  rows.forEach(function(row, idx) {\n";
  html += "    var en = row.querySelector('.slot-en');\n";
  html += "    var sEl = row.querySelector('.slot-start');\n";
  html += "    var eEl = row.querySelector('.slot-end');\n";
  html += "    if (en && en.checked && sEl && eEl) {\n";
  html += "      var sVal = sEl.value, eVal = eEl.value;\n";
  html += "      if (sVal && eVal) {\n";
  html += "        var sParts = sVal.split(':'), eParts = eVal.split(':');\n";
  html += "        var sMin = parseInt(sParts[0],10)*60 + parseInt(sParts[1],10);\n";
  html += "        var eMin = parseInt(eParts[0],10)*60 + parseInt(eParts[1],10);\n";
  html += "        parsed.push({ idx: idx, start: sMin, end: eMin, sEl: sEl, eEl: eEl });\n";
  html += "      }\n";
  html += "    }\n";
  html += "  });\n";
  html += "  var overlap = false;\n";
  html += "  for (var i = 0; i < parsed.length; i++) {\n";
  html += "    for (var j = i + 1; j < parsed.length; j++) {\n";
  html += "      if (rangeIntersects(parsed[i].start, parsed[i].end, parsed[j].start, parsed[j].end)) {\n";
  html += "        overlap = true;\n";
  html += "        parsed[i].sEl.style.borderColor = '#d9534f';\n";
  html += "        parsed[i].sEl.style.backgroundColor = '#3d1c1c';\n";
  html += "        parsed[i].eEl.style.borderColor = '#d9534f';\n";
  html += "        parsed[i].eEl.style.backgroundColor = '#3d1c1c';\n";
  html += "        parsed[j].sEl.style.borderColor = '#d9534f';\n";
  html += "        parsed[j].sEl.style.backgroundColor = '#3d1c1c';\n";
  html += "        parsed[j].eEl.style.borderColor = '#d9534f';\n";
  html += "        parsed[j].eEl.style.backgroundColor = '#3d1c1c';\n";
  html += "      }\n";
  html += "    }\n";
  html += "  }\n";
  html += "  return overlap;\n";
  html += "}\n";
  html += "function checkCategoriesExist() {\n";
  html += "  var valid = true;\n";
  html += "  var fbSelect = document.getElementById('dm_fallback_cat');\n";
  html += "  if (fbSelect) { fbSelect.style.borderColor = ''; fbSelect.style.backgroundColor = ''; }\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  if (tbody) {\n";
  html += "    var rows = tbody.querySelectorAll('tr');\n";
  html += "    rows.forEach(function(row) {\n";
  html += "      var select = row.querySelector('.slot-cat');\n";
  html += "      if (select) { select.style.borderColor = ''; select.style.backgroundColor = ''; }\n";
  html += "    });\n";
  html += "  }\n";
  html += "  if (fbSelect) {\n";
  html += "    var fbVal = parseInt(fbSelect.value, 10);\n";
  html += "    var fbFound = dm_categories.some(function(c) { return c.id == fbVal; });\n";
  html += "    if (!fbFound) {\n";
  html += "      fbSelect.style.borderColor = '#d9534f';\n";
  html += "      fbSelect.style.backgroundColor = '#3d1c1c';\n";
  html += "      valid = false;\n";
  html += "    }\n";
  html += "  }\n";
  html += "  if (tbody) {\n";
  html += "    var rows = tbody.querySelectorAll('tr');\n";
  html += "    rows.forEach(function(row) {\n";
  html += "      var select = row.querySelector('.slot-cat');\n";
  html += "      if (select) {\n";
  html += "        var val = parseInt(select.value, 10);\n";
  html += "        var found = dm_categories.some(function(c) { return c.id == val; });\n";
  html += "        if (!found) {\n";
  html += "          select.style.borderColor = '#d9534f';\n";
  html += "          select.style.backgroundColor = '#3d1c1c';\n";
  html += "          valid = false;\n";
  html += "        }\n";
  html += "      }\n";
  html += "    });\n";
  html += "  }\n";
  html += "  return valid;\n";
  html += "}\n";
  html += "function sortTimes() {\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  if (!tbody) return;\n";
  html += "  var rows = tbody.querySelectorAll('tr');\n";
  html += "  var list = [];\n";
  html += "  rows.forEach(function(row) {\n";
  html += "    var en = row.querySelector('.slot-en').checked;\n";
  html += "    var cat = parseInt(row.querySelector('.slot-cat').value, 10);\n";
  html += "    var start = row.querySelector('.slot-start').value;\n";
  html += "    var end = row.querySelector('.slot-end').value;\n";
  html += "    var parts = start.split(':');\n";
  html += "    var mins = parseInt(parts[0],10)*60 + parseInt(parts[1],10);\n";
  html += "    list.push({ en: en, cat: cat, start: start, end: end, mins: mins });\n";
  html += "  });\n";
  html += "  list.sort(function(a, b) { return a.mins - b.mins; });\n";
  html += "  tbody.innerHTML = '';\n";
  html += "  list.forEach(function(item) {\n";
  html += "    addSlotRow(item.cat, item.start, item.end, item.en);\n";
  html += "  });\n";
  html += "  checkOverlaps();\n";
  html += "  checkCategoriesExist();\n";
  html += "}\n";
  html += "function redownloadCategories() {\n";
  html += "  var statusDiv = document.getElementById('status_msg');\n";
  html += "  statusDiv.innerHTML = \"<p style='color:#33B5E5;'>Redownloading categories... Please wait...</p>\";\n";
  html += "  var formData = new FormData();\n";
  html += "  formData.append('email', document.getElementById('dm_email').value);\n";
  html += "  formData.append('password', document.getElementById('dm_password').value);\n";
  html += "  formData.append('two_fa_code', document.getElementById('two_fa_code').value);\n";
  html += "  var xhr = new XMLHttpRequest();\n";
  html += "  xhr.open('POST', '/test-dm-connection', true);\n";
  html += "  xhr.onload = function() {\n";
  html += "    if (xhr.status === 200) {\n";
  html += "      try {\n";
  html += "        var res = JSON.parse(xhr.responseText);\n";
  html += "        statusDiv.innerHTML = \"<div style='color:#5cb85c;border:1px solid #5cb85c;padding:10px;border-radius:4px;background:#1b2a1b;'><b>Categories Redownloaded Successfully!</b></div>\";\n";
  html += "        if (res.categories) {\n";
  html += "          dm_categories = res.categories;\n";
  html += "          var dropdown = document.getElementById('dm_fallback_cat');\n";
  html += "          if (dropdown) {\n";
  html += "            var currentVal = dropdown.value;\n";
  html += "            dropdown.innerHTML = '';\n";
  html += "            dm_categories.forEach(function(cat) {\n";
  html += "              var opt = document.createElement('option');\n";
  html += "              opt.value = cat.id;\n";
  html += "              opt.textContent = cat.name;\n";
  html += "              if (cat.id == currentVal) opt.selected = true;\n";
  html += "              dropdown.appendChild(opt);\n";
  html += "            });\n";
  html += "          }\n";
  html += "          var tbody = document.getElementById('slots_body');\n";
  html += "          var rows = tbody.querySelectorAll('tr');\n";
  html += "          rows.forEach(function(row) {\n";
  html += "            var select = row.querySelector('.slot-cat');\n";
  html += "            if (select) {\n";
  html += "              var currentVal = select.value;\n";
  html += "              select.innerHTML = '';\n";
  html += "              dm_categories.forEach(function(cat) {\n";
  html += "                var opt = document.createElement('option');\n";
  html += "                opt.value = cat.id;\n";
  html += "                opt.textContent = cat.name;\n";
  html += "                if (cat.id == currentVal) opt.selected = true;\n";
  html += "                select.appendChild(opt);\n";
  html += "              });\n";
  html += "            }\n";
  html += "          });\n";
  html += "          checkCategoriesExist();\n";
  html += "        }\n";
  html += "      } catch(e) {\n";
  html += "        statusDiv.innerHTML = \"<div style='color:#5cb85c;border:1px solid #5cb85c;padding:10px;border-radius:4px;background:#1b2a1b;'><b>Success:</b> \" + xhr.responseText + \"</div>\";\n";
  html += "      }\n";
  html += "    } else {\n";
  html += "      if (xhr.responseText.indexOf('2FA_REQUIRED') !== -1) {\n";
  html += "        document.getElementById('2fa_container').style.display = 'block';\n";
  html += "        statusDiv.innerHTML = \"<div style='color:#f0ad4e;border:1px solid #f0ad4e;padding:10px;border-radius:4px;background:#2a251b;'><b>2FA Code Required:</b> A verification code has been generated. Please check your email, enter the code in the 2FA Code input field, and click Redownload Categories again.</div>\";\n";
  html += "      } else {\n";
  html += "        statusDiv.innerHTML = \"<div style='color:#d9534f;border:1px solid #d9534f;padding:10px;border-radius:4px;background:#2a1b1b;'><b>Redownload Failed:</b> \" + xhr.responseText + \"</div>\";\n";
  html += "      }\n";
  html += "    }\n";
  html += "  };\n";
  html += "  xhr.send(formData);\n";
  html += "}\n";
  html += "function submitForm() {\n";
  html += "  if (!checkCategoriesExist()) {\n";
  html += "    alert('One or more selected categories no longer exist. The invalid fields have been highlighted in red. Please correct them before saving.');\n";
  html += "    return;\n";
  html += "  }\n";
  html += "  if (checkOverlaps()) {\n";
  html += "    alert('Time slot configurations overlap. The overlapping fields have been highlighted in red. Please correct them before saving.');\n";
  html += "    return;\n";
  html += "  }\n";
  html += "  var tbody = document.getElementById('slots_body');\n";
  html += "  var rows = tbody.querySelectorAll('tr');\n";
  html += "  var rules = [];\n";
  html += "  for (var i = 0; i < rows.length; i++) {\n";
  html += "    var row = rows[i];\n";
  html += "    var en = row.querySelector('.slot-en').checked;\n";
  html += "    var catId = parseInt(row.querySelector('.slot-cat').value, 10);\n";
  html += "    var start = row.querySelector('.slot-start').value;\n";
  html += "    var end = row.querySelector('.slot-end').value;\n";
  html += "    rules.push({cat_id: catId, start: start, end: end, enabled: en});\n";
  html += "  }\n";
  html += "  document.getElementById('dm_cat_rules_json').value = JSON.stringify(rules);\n";
  html += "  document.getElementById('dm_form').submit();\n";
  html += "}";
  html += "</script></body></html>";
  localServer.send(200, "text/html", html);
}

void handleLocalDMSave() {
  if (!checkAuth()) return;
  
  dm_enable_connection = localServer.hasArg("dm_conn_en");
  
  if (dm_enable_connection) {
    if (localServer.hasArg("email")) {
      String email = localServer.arg("email");
      email.toCharArray(dm_email, sizeof(dm_email));
    }
    if (localServer.hasArg("password")) {
      String password = localServer.arg("password");
      password.toCharArray(dm_password, sizeof(dm_password));
    }
    dm_enable_2fa = localServer.hasArg("enable_2fa");
    
    // Only clear 2FA pending state on save if 2FA is disabled.
    // If 2FA is enabled, they must successfully run "Test Connection" with a code first.
    if (!dm_enable_2fa) {
      set2FAPending(false);
    }
    dm_auto_send = localServer.hasArg("auto_send");
    dm_enable_heartbeat = localServer.hasArg("dm_hb_en");
    if (localServer.hasArg("dm_hb_int")) {
      dm_heartbeat_interval = localServer.arg("dm_hb_int").toInt();
      if (dm_heartbeat_interval < 1) dm_heartbeat_interval = 15;
    }
    
    dm_api_interval = localServer.arg("poll").toInt();
    if (dm_api_interval < 5) dm_api_interval = 30; // min 5 mins
    
    dm_random_offset = localServer.arg("offset").toInt();
    if (dm_random_offset < 0) dm_random_offset = 2;
    
    String note = localServer.arg("note");
    String start = localServer.arg("start");
    String stop = localServer.arg("stop");
    String tz = localServer.arg("timezone");
    
    note.toCharArray(dm_note_text, sizeof(dm_note_text));
    start.toCharArray(dm_start_sending, sizeof(dm_start_sending));
    stop.toCharArray(dm_stop_sending, sizeof(dm_stop_sending));
    
    if (tz == "Europe/London") {
      strcpy(dm_timezone_json, "Europe/London");
      strcpy(dm_timezone_posix, "GMT0BST,M3.5.0/1,M10.5.0/2");
    } else if (tz == "Europe/Berlin") {
      strcpy(dm_timezone_json, "Europe/Berlin");
      strcpy(dm_timezone_posix, "CET-1CEST,M3.5.0,M10.5.0/3");
    } else if (tz == "America/New_York") {
      strcpy(dm_timezone_json, "America/New_York");
      strcpy(dm_timezone_posix, "EST5EDT,M3.2.0,M11.1.0");
    } else if (tz == "America/Los_Angeles") {
      strcpy(dm_timezone_json, "America/Los_Angeles");
      strcpy(dm_timezone_posix, "PST8PDT,M3.2.0,M11.1.0");
    } else {
      strcpy(dm_timezone_json, "UTC");
      strcpy(dm_timezone_posix, "UTC0");
    }
    dm_auto_category = localServer.hasArg("dm_auto_cat");
    if (localServer.hasArg("dm_fallback_cat")) {
      dm_fallback_category = localServer.arg("dm_fallback_cat").toInt();
    }
    
    if (localServer.hasArg("dm_cat_rules_json")) {
      String rules_json = localServer.arg("dm_cat_rules_json");
      DynamicJsonDocument doc(4096);
      DeserializationError err = deserializeJson(doc, rules_json);
      if (!err) {
        JsonArray arr = doc.as<JsonArray>();
        int count = 0;
        for (JsonVariant val : arr) {
          if (count >= 24) break;
          
          int cat_id = val["cat_id"].as<int>();
          String start_str = val["start"].as<String>();
          String end_str = val["end"].as<String>();
          bool enabled = val["enabled"].as<bool>();
          
          int start_h = 0, start_m = 0;
          int end_h = 0, end_m = 0;
          sscanf(start_str.c_str(), "%d:%d", &start_h, &start_m);
          sscanf(end_str.c_str(), "%d:%d", &end_h, &end_m);
          
          dm_cat_rules[count].category_id = cat_id;
          dm_cat_rules[count].start_min = start_h * 60 + start_m;
          dm_cat_rules[count].end_min = end_h * 60 + end_m;
          dm_cat_rules[count].enabled = enabled;
          
          count++;
        }
        dm_cat_rules_count = count;
      }
    }
    
    setenv("TZ", dm_timezone_posix, 1);
    tzset();
  }
  
  Preferences preferences;
  preferences.begin("cgm-config", false);
  preferences.putBool("dm_conn_en", dm_enable_connection);
  if (dm_enable_connection) {
    preferences.putString("dm_email", dm_email);
    preferences.putString("dm_password", dm_password);
    preferences.putBool("dm_enable_2fa", dm_enable_2fa);
    preferences.putString("dm_note", dm_note_text);
    preferences.putBool("dm_auto_send", dm_auto_send);
    preferences.putInt("dm_poll", dm_api_interval);
    preferences.putInt("dm_offset", dm_random_offset);
    preferences.putString("dm_start", dm_start_sending);
    preferences.putString("dm_stop", dm_stop_sending);
    preferences.putString("dm_tz_json", dm_timezone_json);
    preferences.putString("dm_tz_posix", dm_timezone_posix);
    preferences.putBool("dm_hb_en", dm_enable_heartbeat);
    preferences.putInt("dm_hb_int", dm_heartbeat_interval);
    preferences.putBool("dm_auto_cat", dm_auto_category);
    preferences.putInt("dm_fallback_cat", dm_fallback_category);
    preferences.putInt("dm_cat_rules_cnt", dm_cat_rules_count);
    if (dm_cat_rules_count > 0) {
      preferences.putBytes("dm_cat_rules", dm_cat_rules, dm_cat_rules_count * sizeof(CategoryTimeRule));
    }
  }
  preferences.end();
  
  dm_next_heartbeat_epoch = 0; // force immediate rescheduling
  recalculateNextSendTime();
  drawDashboard();
  
  String html = "<html><head><meta http-equiv='refresh' content='2;url=/'><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#5cb85c;}</style></head><body>";
  html += "<h2>Configuration Saved Successfully!</h2>";
  html += "<p>Updating settings and returning to landing page...</p>";
  html += "</body></html>";
  localServer.send(200, "text/html", html);
}

void handleLocalDMTestConnection() {
  if (!checkAuth()) return;
  if (!localServer.hasArg("email") || !localServer.hasArg("password")) {
    localServer.send(400, "text/plain", "Email and password are required.");
    return;
  }
  
  String orig_email = String(dm_email);
  String orig_pwd = String(dm_password);
  String orig_cookies = String(dm_cookies);
  
  String email = localServer.arg("email");
  String password = localServer.arg("password");
  String two_fa_code = localServer.arg("two_fa_code");
  
  // Check if we can reuse the existing active token:
  // If credentials match what's currently in RAM, a token exists, and no new 2FA code is sent,
  // we try to fetch profile directly first.
  bool can_use_existing_token = (email == String(dm_email) && password == String(dm_password) && strlen(dm_token) > 0 && two_fa_code.length() == 0);
  
  String profile_info = "";
  bool profile_ok = false;
  
  if (can_use_existing_token) {
    profile_ok = diabetesMGetProfile(profile_info);
    if (profile_ok) {
      String cat_err = "";
      bool cat_ok = diabetesMGetCategories(cat_err);
      
      DynamicJsonDocument resp_doc(4096);
      if (cat_ok) {
        resp_doc["profile"] = profile_info;
      } else {
        resp_doc["profile"] = profile_info + " (Warning: Failed to fetch categories: " + cat_err + ")";
      }
      
      JsonArray cats_arr = resp_doc.createNestedArray("categories");
      for (int i = 0; i < dm_categories_count; i++) {
        JsonObject cat_obj = cats_arr.createNestedObject();
        cat_obj["id"] = dm_categories[i].id;
        cat_obj["name"] = dm_categories[i].name;
      }
      String resp_str = "";
      serializeJson(resp_doc, resp_str);
      localServer.send(200, "application/json", resp_str);
      return;
    } else {
      can_use_existing_token = false;
    }
  }
  
  email.toCharArray(dm_email, sizeof(dm_email));
  password.toCharArray(dm_password, sizeof(dm_password));
  
  String login_err = "";
  bool login_ok = diabetesMLogin(two_fa_code.c_str(), login_err);
  
  if (!login_ok) {
    if (login_err != "2FA_REQUIRED") {
      orig_email.toCharArray(dm_email, sizeof(dm_email));
      orig_pwd.toCharArray(dm_password, sizeof(dm_password));
      orig_cookies.toCharArray(dm_cookies, sizeof(dm_cookies));
    }
    localServer.send(400, "text/plain", login_err);
    return;
  }
  
  profile_ok = diabetesMGetProfile(profile_info);
  
  if (profile_ok) {
    String cat_err = "";
    bool cat_ok = diabetesMGetCategories(cat_err);
    
    DynamicJsonDocument resp_doc(4096);
    if (cat_ok) {
      resp_doc["profile"] = profile_info;
    } else {
      resp_doc["profile"] = profile_info + " (Warning: Failed to fetch categories: " + cat_err + ")";
    }
    
    JsonArray cats_arr = resp_doc.createNestedArray("categories");
    for (int i = 0; i < dm_categories_count; i++) {
      JsonObject cat_obj = cats_arr.createNestedObject();
      cat_obj["id"] = dm_categories[i].id;
      cat_obj["name"] = dm_categories[i].name;
    }
    String resp_str = "";
    serializeJson(resp_doc, resp_str);
    localServer.send(200, "application/json", resp_str);
  } else {
    orig_email.toCharArray(dm_email, sizeof(dm_email));
    orig_pwd.toCharArray(dm_password, sizeof(dm_password));
    localServer.send(400, "text/plain", "Auth success, but profile fetch failed: " + profile_info);
  }
}

void handleLocalDMTestUpload() {
  if (!checkAuth()) return;
  if (!localServer.hasArg("email") || !localServer.hasArg("password") || !localServer.hasArg("value")) {
    localServer.send(400, "text/plain", "Missing arguments.");
    return;
  }
  
  String orig_email = String(dm_email);
  String orig_pwd = String(dm_password);
  String orig_cookies = String(dm_cookies);
  
  String email = localServer.arg("email");
  String password = localServer.arg("password");
  String two_fa_code = localServer.arg("two_fa_code");
  float val = localServer.arg("value").toFloat();
  String notes = localServer.arg("notes");
  
  float val_mgdl = fromUserUnit(val);
  
  // Check if we can reuse the existing active token:
  // If credentials match what's currently in RAM, a token exists, and no new 2FA code is sent,
  // we try to upload directly first.
  bool can_use_existing_token = (email == String(dm_email) && password == String(dm_password) && strlen(dm_token) > 0 && two_fa_code.length() == 0);
  
  bool upload_ok = false;
  String upload_err = "";
  
  if (can_use_existing_token) {
    upload_ok = diabetesMUploadReading(val_mgdl, notes, upload_err);
    if (!upload_ok && (upload_err.indexOf("401") != -1 || upload_err.indexOf("Unauthorized") != -1 || upload_err.indexOf("authenticated") != -1)) {
      can_use_existing_token = false;
    }
  }
  
  if (!can_use_existing_token) {
    email.toCharArray(dm_email, sizeof(dm_email));
    password.toCharArray(dm_password, sizeof(dm_password));
    
    String login_err = "";
    bool login_ok = diabetesMLogin(two_fa_code.c_str(), login_err);
    
    if (!login_ok) {
      if (login_err != "2FA_REQUIRED") {
        orig_email.toCharArray(dm_email, sizeof(dm_email));
        orig_pwd.toCharArray(dm_password, sizeof(dm_password));
        orig_cookies.toCharArray(dm_cookies, sizeof(dm_cookies));
      }
      localServer.send(400, "text/plain", login_err);
      return;
    }
    
    upload_ok = diabetesMUploadReading(val_mgdl, notes, upload_err);
  }
  
  if (upload_ok) {
    localServer.send(200, "text/plain", "Reading " + String(val, 1) + " " + String(llu_units) + " uploaded successfully.");
  } else {
    localServer.send(400, "text/plain", "Upload failed: " + upload_err);
  }
}

void handleLocalWifiGet() {
  if (!checkAuth()) return;
  if (checkForceReset()) return;
  
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body { font-family:sans-serif; background:#181a1b; color:#fff; padding:20px; text-align:center; }";
  html += "h2 { color:#33B5E5; text-align:center; }";
  html += ".card { background:#222; padding:15px; border-radius:8px; margin-bottom:20px; border:1px solid #333; text-align:left; max-width:600px; margin-left:auto; margin-right:auto; box-sizing:border-box; }";
  html += ".form-group { margin-bottom:15px; }";
  html += "label { display:block; margin-bottom:5px; color:#aaa; font-weight:bold; }";
  html += "input { width:100%; padding:10px; border-radius:4px; border:1px solid #444; background:#111; color:#fff; box-sizing:border-box; font-size:16px; }";
  html += ".btn { display:block; width:100%; padding:12px; text-align:center; background:#5cb85c; color:#fff; text-decoration:none; border-radius:4px; font-weight:bold; border:none; cursor:pointer; margin-top:20px; font-size:16px; box-sizing:border-box; }";
  html += ".btn-blue { background:#33B5E5; }";
  html += ".btn-grey { background:#555; margin-top:10px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Network & Wi-Fi Configuration</h2>";
  html += "<form action='/save-wifi' method='POST' onsubmit='return confirmWifiChange();'>";
  
  html += "<div class='form-group'>";
  html += "<label>Current Network (SSID)</label>";
  html += "<input type='text' id='wifi_ssid' name='ssid' value='" + WiFi.SSID() + "' placeholder='Enter Wi-Fi SSID' required>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Wi-Fi Password</label>";
  html += "<input type='password' id='wifi_pwd' name='password' placeholder='Enter Wi-Fi Password (leave empty to keep current)'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Device Name (Hostname)</label>";
  html += "<input type='text' id='dev_name' name='device_name' value='" + String(device_name) + "' placeholder='e.g. esp32-cgm' required>";
  html += "</div>";
  
  html += "<button type='submit' class='btn'>Save and Apply settings</button>";
  html += "<a href='/hardware' class='btn btn-grey'>Cancel</a>";
  html += "</form>";
  html += "</div>";
  
  html += "<script>";
  html += "function confirmWifiChange() {";
  html += "  var ssid = document.getElementById('wifi_ssid').value;";
  html += "  var name = document.getElementById('dev_name').value;";
  html += "  var origSSID = '" + WiFi.SSID() + "';";
  html += "  var origName = '" + String(device_name) + "';";
  html += "  if (ssid !== origSSID || name !== origName) {";
  html += "    return confirm('WARNING: Updating the Wi-Fi network and/or device name will cause the device to restart and reconnect. You will lose connectivity to this portal and may need to access it via a new IP address or hostname. Do you want to proceed?');";
  html += "  }";
  html += "  return true;";
  html += "}";
  html += "</script>";
  
  html += "</body></html>";
  
  localServer.send(200, "text/html", html);
}

void handleLocalWifiSave() {
  if (!checkAuth()) return;
  
  if (!localServer.hasArg("ssid") || !localServer.hasArg("device_name")) {
    localServer.send(400, "text/plain", "Missing parameters.");
    return;
  }
  
  String ssid = localServer.arg("ssid");
  String password = localServer.arg("password");
  String name = localServer.arg("device_name");
  
  name.toCharArray(device_name, sizeof(device_name));
  
  Preferences preferences;
  preferences.begin("cgm-config", false);
  preferences.putString("dev_name", name);
  preferences.end();
  
  WiFi.setHostname(device_name);
  
  if (password.length() > 0 || ssid != WiFi.SSID()) {
    if (password.length() > 0) {
      WiFi.begin(ssid.c_str(), password.c_str());
    } else {
      WiFi.begin(ssid.c_str());
    }
  }
  
  String html = "<html><head><style>body{background:#181a1b;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}h2{color:#5cb85c;}</style></head><body>";
  html += "<h2>WiFi Settings Saved!</h2>";
  html += "<p>The device is restarting to connect to the new network / apply the new device name.</p>";
  html += "<p>Portal will close. Please reconnect your client to the same network and lookup the device.</p>";
  html += "</body></html>";
  localServer.send(200, "text/html", html);
  
  delay(2000);
  ESP.restart();
}

void showDiagnosticsScreen() {
  gfx.fillScreen(0x181A1B);
  
  gfx.setFont(&fonts::DejaVu24);
  gfx.setTextColor(0x33B5E5);
  gfx.drawCenterString("Device Diagnostics", 240, 20);
  
  gfx.setFont(&fonts::DejaVu18);
  gfx.setTextColor(0xFFFFFF);
  
  int y = 70;
  int dy = 30;
  
  gfx.drawString("Device Name: " + String(device_name), 20, y); y += dy;
  gfx.drawString("IP Address: " + WiFi.localIP().toString(), 20, y); y += dy;
  gfx.drawString("Wi-Fi SSID: " + WiFi.SSID(), 20, y); y += dy;
  gfx.drawString("Wi-Fi Signal: " + String(WiFi.RSSI()) + " dBm", 20, y); y += dy;
  gfx.drawString("Uptime: " + getUptimeStr(), 20, y); y += dy;
  gfx.drawString("Free Memory: " + String(ESP.getFreeHeap() / 1024) + " KB", 20, y); y += dy;
  
  String last_llu = llu_last_fetch_status;
  if (last_llu.length() > 25) last_llu = last_llu.substring(0, 25) + "...";
  gfx.drawString("Libre Sync: " + last_llu, 20, y); y += dy;
  
  String last_dm = dm_last_sent_status;
  if (last_dm.length() > 25) last_dm = last_dm.substring(0, 25) + "...";
  gfx.drawString("Diabetes:M: " + last_dm, 20, y); y += dy;
  
  int btn_w = 160;
  int btn_h = 50;
  int btn_x = 240 - (btn_w / 2);
  int btn_y = 380;
  
  gfx.fillRoundRect(btn_x, btn_y, btn_w, btn_h, 8, 0x5CB85C);
  gfx.setFont(&fonts::DejaVu24);
  gfx.setTextColor(0xFFFFFF);
  gfx.drawCenterString("OK", 240, btn_y + 12);
  
  delay(200);
  uint16_t tx, ty;
  while (gfx.getTouch(&tx, &ty)) {
    delay(50);
  }
  
  bool ok_pressed = false;
  while (!ok_pressed) {
    localServer.handleClient();
    delay(10);
    
    if (gfx.getTouch(&tx, &ty) && (tx > 0 || ty > 0)) {
      if (tx >= btn_x && tx <= (btn_x + btn_w) && ty >= btn_y && ty <= (btn_y + btn_h)) {
        ok_pressed = true;
      }
    }
  }
  
  while (gfx.getTouch(&tx, &ty)) {
    delay(50);
  }
  
  drawDashboard();
}

