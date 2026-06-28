# Changelog

All notable changes to the ESP32 CGM Display project between **Build 1.0.26** and **Build 1.0.34**.

## [1.0.34] - 2026-06-27

### Added
- **Internet Loss Fallback to Zero Glucose Reading**: Resets `last_glucose` to `0.0` when LLU fetches fail.
- **Clean Disconnected Display State**: If `last_glucose <= 0.0`, the screen displays `"0.0"` (or `"0"` in mg/dL), draws a neutral **Grey** background banner
- **Diabetes:M Custom Category Upload Mapping**: Custom category selections
- **LCD Touch Screen Long Press Diagnostics**: Press and hold display for quick diagnostics
- **Network / WiFi Configuration Webpage**: Added a dedicated `/wifi` settings webpage allowing you to reconfigure Wi-Fi SSID, Password, and Device Name (hostname) on the fly.
- **DHCP Client Hostname**: Set customized hostname dynamically on boot using `WiFi.setHostname(device_name)`.

