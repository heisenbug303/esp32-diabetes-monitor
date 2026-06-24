# Firmware Installation Guide

This document describes how to flash the initial firmware binaries onto your ESP32-S3 hardware.

---

## 📦 Required Binary Files

Flashing requires three compiled binaries. If using pre-compiled versions, they can be obtained from the firmware-release folder:
1.  **`bootloader.bin`** (Flash Offset: `0x0`)
2.  **`partitions.bin`** (Flash Offset: `0x8000`)
3.  **`firmware.bin`**   (Flash Offset: `0x10000`)

---

## 🌐 Method 1: Web Browser Flashing (Easiest)

You can flash your device directly from a WebUSB-compatible browser (Google Chrome, Microsoft Edge, Opera) without installing any development utilities.

### Recommended Web Flashers
*   **Official Espressif Web tool (recommended)**: [espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/)
*   **Adafruit ESPTool Web**: [adafruit.github.io/web-esptool](https://adafruit.github.io/web-esptool/)

### Flashing Steps
1.  Connect your ESP32-S3 board to your computer using a USB-C data cable (make sure it is connected to the USB/UART port, not just a power port).
2.  Open one of the web flashers linked above.
3.  Select a baud rate of **`921600`** (or `460800` if you experience failures).
4.  Click **Connect** (or *Connect Device*). A browser dialog will prompt you to select the serial COM port matching your ESP32-S3. Select it and click **Pair**.
5.  *(Optional but recommended)*: Click **Erase Flash** (or *Console -> Erase*) to clear NVS memory from previous configurations.
6.  Configure the file offsets and choose the binary files from your computer:
    *   Offset **`0x0`**: Select `bootloader.bin`
    *   Offset **`0x8000`**: Select `partitions.bin`
    *   Offset **`0x10000`**: Select `firmware.bin`
7.  Click **Program** (or *Program Device* / *Write*).
8.  Wait for the progress bars to reach 100%. Once completed, reboot.

---

## 💻 Method 2: Command Line Flashing (`esptool.py`)

If you prefer using the command line, you can use the official Python-based Espressif flash tool.

### Prerequisites
1.  Ensure you have **Python 3** installed on your system.
2.  Install `esptool` using `pip`:
    ```bash
    pip install esptool
    ```
    *For manual downloads and detailed CLI guides, visit the official repository: [github.com/espressif/esptool](https://github.com/espressif/esptool).*

### Flashing Commands
1.  Connect the device and locate its serial COM port (e.g., `COM3` on Windows, `/dev/ttyUSB0` on Linux).
2.  **Erase the flash memory** (highly recommended before first install):
    ```bash
    esptool.py --chip esp32s3 --port COMx erase_flash
    ```
    *(Replace `COMx` with your actual serial port number).*
3.  **Flash the binaries**:
    Navigate to the directory containing the binary files and run:
    ```bash
    esptool.py --chip esp32s3 --port COMx --baud 921600 write_flash -z 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
    ```
4.  Once completed, reboot the device.
