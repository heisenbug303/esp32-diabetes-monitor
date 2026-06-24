# Post-Flashing Configuration Guide

Follow these step-by-step instructions to configure your ESP32 CGM Monitor after successful installation.

---

## 📶 Step 1: Initial Wi-Fi Setup (Access Point Mode)

On first boot (or after a factory reset), the device will not have any stored Wi-Fi credentials and will automatically start up in Access Point (AP) mode.

1.  Look at the device screen. It should display a message: *"Connect to Wi-Fi AP: ESP32-CGM-Config"* and show an IP address (`192.168.4.1`).
2.  On your phone or computer, scan for available Wi-Fi networks and connect to **`ESP32-CGM-Config`** (there is no password).
3.  A Captive Portal login screen should automatically open. If it does not open within a few seconds, open your browser and navigate to:
    ```
    http://192.168.4.1/
    ```
4.  Click **Configure WiFi**.
5.  Select your local Wi-Fi network from the list, enter the Wi-Fi password, and click **Save**.
6.  The device will save the credentials and reboot to join your local network. Once connected, the screen will display your local IP address (e.g. `192.168.1.150`).

---

## 🔒 Step 2: Access the Web Admin Portal & Set Password

All settings are configured through a local Web Admin portal hosted by the device.

1.  Ensure your phone/computer is connected to the *same* Wi-Fi network as the device.
2.  Open your browser and navigate to the IP address shown on the device screen:
    ```
    http://<device-ip>/
    ```
3.  You will be prompted for authentication. Enter the default credentials:
    *   **Username**: `admin`
    *   **Password**: `<Temp password on device display>`
4.  **Important Security Step**: Since you are using the default temporary password, the system will immediately redirect you to the **Change Portal Password** (`/change-password`) screen.
5.  Enter a new secure password (minimum 6 characters), confirm it, and click **Save Password**. The system will save the password and redirect you back to the home landing page.

---

## 📈 Step 3: Configure LibreLinkUp Follower Settings

To fetch your glucose values, the device needs access to a LibreLinkUp follower account.

> [!IMPORTANT]
> **LibreLinkUp vs. LibreLink Accounts**:
> - This project requires a **LibreLinkUp** account, which is different from a standard **LibreLink** account.
> - If you only have a LibreLink account, you must register a separate LibreLinkUp account and invite it as a follower from the main LibreLink app.
> - **Gmail Hack**: If you are a Gmail user, you can register a follower using your same email address by adding a period `.` anywhere in your username (e.g. `mrbob@gmail.com` $\rightarrow$ `mr.bob@gmail.com`). Google will route the verification email to your main inbox, but LibreLinkUp will treat it as a distinct follower account, simplifying registration.

1.  On the Home Page, click **Configure LibreLinkUp**.
2.  Enter the follower credentials:
    *   **Username or Email**: Your follower account email.
    *   **Password**: Your follower account password.
    *   **Region**: Select your Libreview account region (e.g., `eu` for Europe, `us` for United States).
    *   **API Poll Interval**: Set the refresh frequency in minutes (default is 2 minutes).
3.  Click **Test LibreLinkUp Connection**.
    *   If successful, you will see a success message with the current glucose reading.
    *   If it fails, double-check your follower email/password and region.
4.  Click **Save Settings**. The device will return to the Home page and begin fetching live data.

---

## ⚙️ Step 4: Configure General Settings (Units & Tolerances)

Set up your display preferences, limits, and custom alerts.

1.  On the Home Page, click **Configure General Settings** (or navigate to `/general`).
2.  **Display Units**: Select **`mmol/L`** or **`mg/dL`**.
    *   *Note: Switching units will dynamically convert all numeric limit thresholds and alert rules client-side, adapting step sizes automatically.*
3.  **Colour Tolerances**: Adjust the low and high warning thresholds. These control the colors of the instantaneous display value and the status banner (Red for Critical, Yellow for Warning, Green for Target).
4.  **Graph Y-Axis Range**: Set the minimum and maximum scale limits for the display trend graph.
5.  **Custom Messages**: Add conditional status messages to display when certain thresholds are met.
    *   Click **+ Add Message Condition**.
    *   Select the logic (e.g., `>` greater than, `<` less than, or `between`).
    *   Set the threshold value and type the message text (max 63 characters).
    *   Click **Save Configuration** at the bottom when done.

---

## 📊 Step 5: Configure Diabetes:M Integration (Optional)

If you track your data in Diabetes:M, you can configure the device to automatically push readings.

1.  On the Home Page, click **Configure Diabetes:M** (or navigate to `/diabetes-m`).
2.  Check the **"Enable Diabetes:M Connection Support"** box at the top. This will reveal the configuration fields.
3.  Enter your credentials:
    *   **Username or Email**: Your Diabetes:M account email.
    *   **Password**: Your Diabetes:M account password.
    *   **Note Text**: A brief label (e.g., `Sent by ESP32`) to attach to each entry.
    *   **Timezone**: Select your active timezone for proper scheduler alignment.
4.  **Enable Auto Send**: Check this box if you want the device to automatically upload readings.
    *   Specify the **Start Sending** and **Stop Sending** time-windows if you want to restrict uploads (e.g. only upload during daytime).
5.  **Enable Heartbeat**: Check this box to perform periodic background session renewals. This keeps your API login token alive, preventing expiration and eliminating repeated 2FA request prompts.
6.  **Test Connection & 2FA**:
    *   Click **Test Connection**.
    *   *Note: If your account has 2FA active, the test will trigger an email code. The page will reveal a 2FA input box. Enter the code sent to your email, and click Test Connection again.*
7.  Click **Save Configuration**.
    *   *Note: When connection support is unchecked, all background upload/heartbeat tasks are paused, and the Home page reports "Disabled" for Diabetes:M status.*

---

## 🛠️ Step 6: Backup, Restore, and Maintenance

Navigate to the **Hardware Control** (`/hardware`) page to manage administrative commands.

*   **Save Configuration**: Click this to download a `cgm_config.json` backup file of all your portal settings (tolerances, accounts, custom rules). Passwords are cipher-obfuscated inside the file.
*   **Load Configuration**: Click this to upload your JSON backup file, and restore your configurations.
*   **Update Firmware (OTA)**: Click this to select and write a compiled `firmware.bin` file over the air.
*   **Reboot Device**: Restart the ESP32. You will be prompted with a confirmation dialog, and the browser will redirect to the Home page after a 10-second countdown.
*   **Reset to Factory Default**: Clear all settings, Wi-Fi details, and passwords from NVS. This can also be triggered at boot time by holding the touch screen for 3 seconds and touching `[RESET]` on the HMI confirmation screen.
