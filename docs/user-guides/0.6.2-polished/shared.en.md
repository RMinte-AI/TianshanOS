## 1. Getting Started

### Open the WebUI

1. Open the device’s web interface (WebUI) using the address provided by your administrator.
2. Select “Login” in the upper-right corner.
3. Keep `admin` as the username and enter the admin password supplied with the device.
4. Select “Login.” After a successful login, the current username appears in the upper-right corner.

Logging in with the default password opens a “Security Reminder.” Enter the current password, then enter the new password twice and select “Change Now.” Both new-password entries must match. Select “Later” to close the reminder.

When you have finished, select “Logout” in the upper-right corner. You will need to sign in again to use the device.

### Switch Languages

Select the language button at the top of the page, then choose Chinese or English. Page content and control labels switch immediately.

### Page Navigation

The main admin pages are:

- “System”: View device status, control modules, fans, and LEDs, and open OTA Update.
- “Network”: Check Ethernet status and DHCP clients, configure WiFi, and manage NAT forwarding.
- “Files”: Manage files on the SD Card and SPIFFS.
- “Security”: Open the separate security management page. See the Security Guide for instructions.

## 2. System Status and Routine Operations

Select “System” in the top navigation.

### View Resource and Service Status

“Management Chip Resources” shows CPU, DRAM, and PSRAM usage on the management chip. These readings do not show resource usage on AGX or LPMU.

- Select “Details” to view total, used, and free memory and other memory information.
- “Services” shows the number of running services and the total. Select it to view each service’s state and startup stage.
- If a service reports a problem, refresh its status. To restart it, follow “Restart One Service” below.

### View System and Power Information

“System Overview” shows the chip, firmware version, ESP-IDF version, and uptime. For routine use, check the firmware version. ESP-IDF is the version of the system framework used by the device.

The right side of the card shows input voltage, internal voltage, current, power, and protection status.

The switch beside the protection status enables or disables low-voltage protection. When enabled, the device uses the saved voltages and delays to shut down at low voltage and restart after power recovers.

### View Network and Time

“Network & Time” shows Ethernet, WiFi, IP address, current time, sync status, time source, and timezone.

- Select “Sync Time” to copy the browser's current time to the device.
- Select “Timezone,” choose a preset or enter a supported timezone setting, then save.
- Select “OTA Update” to open the firmware update page.

### Advanced Operations

#### Reboot TianshanOS

A reboot temporarily interrupts the WebUI and device management. Finish active operations, then select “Reboot” and confirm. Wait for the device to recover, then reopen the WebUI.

#### Restart One Service

Restarting a service temporarily interrupts the function it provides. Open “Service Status,” find the affected service, and select “Reboot” on that row. Check its status again when the operation finishes.

#### Change Shutdown Settings

These settings control when the device shuts down after a voltage drop and when it starts again after power recovers. Use values that match your device’s power requirements.

Select “Shutdown Settings” to change:

- “Low Voltage Threshold”: Starts the shutdown countdown below this voltage.
- “Recovery voltage threshold”: Starts recovery above this voltage.
- “Shutdown Countdown”: Sets the delay before shutdown after low voltage is detected.
- “Recovery hold time”: Sets the wait time used to confirm stable power recovery.
- “Fan Stop Delay”: Sets the delay before fans stop after shutdown.

Save the form to apply the updated protection settings.

#### Switch the Top USB Target

Switching the USB target may temporarily disconnect an attached device. Confirm the target and finish active work before continuing.

Devices that support USB switching show a “USB” button. Each click switches the top USB port to the next target: ESP, AGX, then LPMU. Check the target shown on the button and the page message to confirm the result.

## 3. Device Panel

The “Device Panel” is on the “System” page. It contains module power controls, Quick Actions, and data widgets.

### Control AGX and LPMU

Save work on the module and complete its normal shutdown process before removing power. Forced power-off can cause loss of unsaved data.

- “AGX Power” shows the power-control state. Select it to switch power on or off, then wait for confirmation.
- “LPMU Power” shows “Online,” “Offline,” or “Unknown.” These states come from a network check. Online means the module is reachable; offline means it is not. Neither state alone confirms whether the module is powered on or off.
- Selecting the LPMU button triggers the same action as pressing its physical power button. Wait for the check and the displayed result. If the result is unknown, check the module and network before pressing the button again.

### Use Quick Actions

A root user configures Quick Action cards. Card visibility and manual execution are separate settings. Seeing a card does not mean you can start its task.

1. Check the card’s name, state, and available controls to identify the task.
2. Select an available card and wait for its state to update. Do not start it again while it is processing.
3. Background tasks may provide log, status-verification, or stop controls. Read the log to see task output. After selecting stop, check that the task has stopped.
4. If the state is unknown or starting is blocked, use the available verification control, or ask a root user to check the rule, remote command, and host connection.

If starting is blocked, follow the reason shown. Do not keep selecting start:

- Rule disabled or manual triggering not allowed: Ask a root user to check the rule settings. A visible card is not permission to run it.
- Task executing, starting, verifying, or stopping: Wait for the state to update.
- Linked service already running: Do not start it again. To restart it, stop it first and confirm the result.
- Service not confirmed stopped: Verify its state. If it remains unclear, ask a root user to check the host connection.
- Rule not loaded or awaiting removal after restart: Ask a root user to check the rule version and pending-restart message.
- Automation not accepting start requests: Ask a root user to check the engine and configuration state. Do not restart the device yourself.

Confirm that a service has stopped before restarting it. An accepted start or stop request may still be in progress; wait for the final state. After triggering one action, wait a few seconds before starting another.

Press and hold a card until the reorder indicator appears, then drag it to change the display order.

If no cards are available, check which message the page shows:

- “Loading quick actions”: Wait for configuration to finish loading. If the message persists, refresh the page or ask a root user to investigate.
- “Quick actions unavailable”: Ask a root user for help. Do not keep selecting start.
- No configured Quick Actions: If you need one, ask a root user to check the rules’ “Show on panel” setting.

After importing a rule package, a card may still run the old task until the device restarts. Newly imported rules or rules awaiting removal may not start. If a task was recently changed, ask a root user to confirm which version is in use before running it.

Do not restart a device providing active services just to restore a card.

### Manage Data Widgets

Data widgets continuously display device data in the “Device Panel.”

1. Select “Widget Manager.”
2. Choose a refresh interval or disable automatic refresh.
3. Add a preset widget, or choose a component style and data source offered by the page.
4. Edit its label, display style, and unit as needed, then save.

Existing widgets can be edited, deleted, and reordered. You can also select a widget card to edit it. Press and hold a widget, then drag it to a new position.

## 4. Fan Management

“Fan Control” is on the “System” page. Fan cards show the fans provided by your device. Check the fan number before changing a curve.

### View Fan Status

The status bar shows “Effective Temp” and “Current output.” The large percentage is the control setting confirmed by the device, not the measured speed. RPM is the measured speed in revolutions per minute and is hidden without a valid reading. A large `--` means the current output is unconfirmed.

While you drag the manual slider, the value beside it shows the proposed setting. The large percentage continues to show the current output. Release the slider, then check the message and updated reading to confirm the change. If a requested setting differs from the current output, do not assume it has already taken effect.

Smart mode shows its operating state, such as “Smart Mode,” “Following curve,” protection, or an invalid temperature. It also provides a safety reference, a 45-second forecast, and the rate of temperature change.

If the temperature becomes invalid, check that its source is still updating. The device switches to protective control for temperature loss. Select “TTI” for an explanation of Smart thermal control.

Use the refresh button in the section header to fetch the current state. If adjustment fails or the output is unconfirmed, check the device before deciding whether to retry.

### Select an Operating Mode

| Mode | Purpose |
| --- | --- |
| “Off” | Stops the fan. |
| “Manual” | Uses a fixed control percentage set with the 0-100% slider. |
| “Curve” | Follows the configured temperature-to-control-percentage curve. |
| “Smart” | Uses the base curve, temperature trends, and past adjustments to control cooling. Uses protective control when needed. |

Stopping a fan or setting a low manual value reduces cooling. Check the load and temperature first, and keep monitoring them. The slider is available only in Manual mode when the current output is confirmed.

### Configure a Fan Curve

Select “Curve” in the Fan Control section header to open “Fan Curve Management.” The “Curve” button inside a card only changes the operating mode.

1. Under “Fan,” choose the number to adjust and check it against the System page.
2. Under “Temperature variable binding,” add temperature variables and assign weights.
3. Select “Bind.” The temperature source is shared by fans in Curve and Smart modes, so changing it affects the fans using that source.
4. Add or edit “Curve nodes.” Each curve needs at least 2 nodes and supports up to 10.
5. Set “Minimum speed” and “Maximum speed” as control percentages. The minimum must not exceed the maximum.
6. Set “Temperature hysteresis” and “Minimum interval.” Hysteresis accepts 0-20°C and reduces frequent adjustments caused by small temperature changes. The interval accepts 500-30000 ms; 1000 ms is 1 second.
7. Select “Save curve.” After a successful save, the selected fan switches to Curve mode. To use Smart thermal control, return to the card and select “Smart.”

If saving fails, read the message and check the current state before retrying. After unbinding temperature variables, check the effective temperature and fan state as well.

### Import and Export a Curve

- Select “Import Config” and choose a curve JSON file. JSON is the file format used to store curve data. Review the nodes, limits, and fan number, then select “Save curve” to apply them.
- Select “Export Config” to download the current curve. The page also attempts to save a copy under `/sdcard/config` on the SD card. Check the browser download and the reported SD-card result separately.

### Use a Test Temperature

A test temperature temporarily replaces the normal source and affects Curve or Smart control. Monitor the fan and device throughout the test.

1. Enter 0-100°C under “Test Temp.”
2. Select “Test” and observe the current output, state, and fan response.
3. Select “Clear Test” when finished. Check that the effective temperature has returned to its normal source.

## 5. LED Management

“LED Control” is on the “System” page and shows the LEDs provided by the device. Available features vary by LED.

### Routine Controls

- Use the light-bulb button at the bottom of a card to turn the LED on or off.
- Move the “Brightness” slider, or choose a color or preset.
- Use the animation button to open “LED Settings.” Select an animation under “Programmatic Animation,” then use its play or stop control.
- Select the save icon at the bottom right of the card to save the current LED configuration.
- Select “All Off” to turn off all LEDs and check the reported result. If some fail, check those devices.

### Advanced LED Matrix Features

Matrix cards provide icons for the available features. Open one, then switch between these groups in “LED Settings”:

- “Programmatic Animation”: Select and run an animation.
- “Image/QR Code”: Choose an image from the SD card or enter content for a QR code.
- “Text Display”: Set text, font, alignment, scrolling, foreground, and background.
- “Post-processing Filter”: Choose a filter and parameters, then apply or stop it.
- “Color Correction”: Adjust the display and use the available reset, import, or export controls.

Check the actual light or matrix display after applying a change. Use only the features shown. If a setting fails, follow the message to check the file, input, or device state.

## 6. Network Management

Select “Network” in the top navigation to open “Network Settings.” Changing the network mode, hotspot, or NAT settings may interrupt the current WebUI connection. Before saving, make sure you can reconnect through the new network address.

### View Network Status

The top of the page shows the state of Ethernet, the WiFi client, and the WiFi AP. Open the related panel to view IP address, subnet mask, gateway, DNS, MAC address, SSID, signal, and connected-device counts when available.

The Ethernet panel displays the current link and address information. It does not provide address editing controls.

### Select a WiFi Mode

| Mode | Purpose |
| --- | --- |
| “Off” | Turns WiFi off. |
| “Station (STA)” | Connects the device to an existing WiFi network. |
| “Access Point (AP)” | Makes the device provide a WiFi hotspot. |
| “STA+AP” | Connects to an existing WiFi network while keeping the device hotspot available. |

After choosing a mode, wait for the page to refresh the state. The current wireless connection may be interrupted during the switch.

### Connect to WiFi

1. Set the WiFi mode to “Station (STA)” or “STA+AP.”
2. Under “Station,” select “Scan.”
3. Choose a network from the list. The list shows SSID, signal strength, channel, and authentication type.
4. Enter the password and confirm. Leave the password blank for an open network.
5. Wait for the state to change to “Connected,” then confirm the new IP address.

Select “Disconnect” to end the current WiFi client connection.

### Configure the WiFi AP

1. Set the WiFi mode to “Access Point (AP)” or “STA+AP.”
2. Under “Hotspot,” select “Config.”
3. Enter the SSID. A blank password creates an open hotspot; a protected hotspot requires at least 8 characters.
4. Select a channel and enable “Hide SSID” if needed.
5. Select “Apply” and wait for the hotspot state to update.

Select “Devices” to view clients currently connected to the hotspot.

### Set the Hostname

Enter a new name in the “Hostname” section under “Network Services,” then select “Set.” The page shows the current hostname after it updates.

### View DHCP Clients

DHCP automatically assigns network addresses to connected devices. Select “Clients,” choose “WiFi AP” or “Ethernet,” and view the current leases. Use the refresh button to reload the list.

### Advanced Network Operations

#### Configure the NAT Gateway

NAT forwards network traffic between the device's network interfaces. Enable or disable NAT, then select “Save” to retain the setting. Check the WiFi and Ethernet status afterward.

#### Access the Upstream Network via LPMU

This operation uses the configured LPMU host to set up network access. Before starting, confirm that LPMU is reachable and the network cables are connected. Obtain the sudo password for the SSH account on that host. This password authorizes system changes and may differ from the WebUI password.

1. Under “Upstream Network Access,” select “Access via LPMU.”
2. Enter the LPMU sudo password and select the access button. The password is used only for this run and is not saved; enter it again for a later run.
3. Wait for the operation to finish. Do not start it again while processing.
4. Access is confirmed only when the result reports both an internet connection and completed network configuration.
5. If it fails or the result is unconfirmed, read the reason first. Expand “Execution Log” for more detail. Check the password for a password error, or the cabling and upstream network if no internet connection is found.

A failed status request does not mean the script has stopped. Use the available “Refresh Status” control to check the current run before deciding whether to try again.

## 7. File Management

Select “Files” in the top navigation to open “File Manager.”

The SD Card is removable storage, and SPIFFS is internal device file storage. Select “SD Card” or “SPIFFS” to switch locations. The path at the top shows your current folder. Select a folder name in the path to go back to it.

### Browse and Manage Files

- Select a folder name to open it.
- Select the download button beside a file to save it to the browser's download location.
- Select the rename button, enter a new name, and confirm.
- Select “New Folder,” enter a folder name, and create it.
- Select the refresh button to reload the current directory and storage state.

### Upload Files

Files can only be uploaded to the SD Card, not SPIFFS. Confirm that the SD Card is mounted first.

1. Select “SD Card” and open the folder where you want to upload the files.
2. Select “Upload Files.”
3. Select one or more files, or drag files into the upload area.
4. Review the upload list and remove unwanted files.
5. Select “Upload” and wait for each file to show completion.

Uploading a `.tscfg` package opens verification and application steps. However, this file-upload page does not yet apply the package's settings. Successful upload or verification does not mean the settings are in effect.

Give rule packages to a root user to import on Automation. Ordinary file upload is not a substitute for rule import. See the Security Guide for source and signature guidance for other packages.

### Batch Operations

After selecting files or folders, the batch toolbar appears.

- “Batch Download” downloads selected files. Folders are not included.
- “Batch Delete” deletes selected files and folders.
- “Clear Selection” clears the current selection.

### Delete a File or Folder

Deletion cannot be undone in the WebUI. Deleting a folder also deletes all of its contents. Confirm the name and path before selecting “Delete” or “Batch Delete” and approving the prompt.

### Mount and Unmount the SD Card

Unmounting makes SD files temporarily unavailable. Finish uploads, downloads, and other file operations, then select “Unmount SD.”

Removing or replacing a card that stores Automation configuration may prevent it from loading after restart. Ask a root user to confirm that unmounting is safe. Do not unmount or remove the card when the page tells you to keep it installed.

When the SD Card is not mounted, the page shows “Mount SD.” Select it, wait for the state to change to “Mounted,” then open the SD Card directory again.

## 8. OTA Updates

On the “System” page, select “OTA Update” in the “Network & Time” section to open “Firmware Upgrade.”

The device reboots during an update, temporarily disconnecting the WebUI. Save active work and keep device power stable before starting. Do not power off the device while update progress is incomplete.

### Check for Updates from an OTA Server

1. Review the “Current Version.”
2. Enter the OTA server address supplied by an administrator or publisher.
3. Select “Save,” then select “Check Update.”
4. The page reports “Update Available,” “Already up to date,” an older server version, or an error.
5. Confirm the target version, then select “Upgrade Now” or the upgrade control shown by the page.
6. Wait for download, installation, and reboot to finish. To cancel, use the “Abort” button when it is available. Not every stage supports cancellation.
7. Reconnect to the WebUI after the device comes back online and verify “Current Version.”

When “Also upgrade www” is enabled, the firmware and WebUI are updated in sequence. For 0.6.2, use the matched main firmware and WebUI resources supplied by the publisher, both from the same release. If the old interface remains after upgrading, force-refresh the browser and check the version and page again.

### Manual Upgrade

Expand “Manual Upgrade” and choose one of these methods:

- “Upgrade from URL”: Enter the firmware URL, set “Also upgrade www” according to the release instructions, then select “Upgrade.” Here, www refers to the device’s web-interface resources.
- “Upgrade from SD Card”: Enter a firmware path such as `/sdcard/firmware.bin`. If “Also upgrade www” is enabled, ensure that `www.bin` from the same release is in that directory.

For an upgrade from a URL, “Skip certificate check” skips verification of the HTTPS server certificate. This removes the certificate check used to confirm the download server’s identity. Leave it unchecked for routine updates. If you see a certificate error, ask your administrator to check the server address and certificate.

### Partition Management and Rollback

“Partition Management” shows the running partition and other available partitions.

- “Mark Valid” confirms the running version and disables automatic rollback protection for that version. Use it after confirming that the current version operates correctly.
- “Rollback to This Version” selects another bootable version and switches through a reboot. Rollback interrupts current services. Confirm the target version and data compatibility first.

After the operation and reboot complete, reopen the WebUI and verify the current version and device state.

Version 0.6.2 changes how Automation configurations are saved. Before returning to older firmware, ask a root user or the device provider whether it can read these configurations, and prepare a backup and recovery method.

Rollback changes the firmware, not the configuration format. Existing rules may no longer work after a downgrade.

## 9. Security Management Entry

Select “Security” in the top navigation to open security management. Use the TianshanOS Security Guide for SSH keys, remote hosts, known-host fingerprints, HTTPS certificates, configuration packages, and account management. These procedures are not repeated here.

### Failed or Unconfirmed Operations

Forms may mark a field in red and show an error beside it. Correct that field as instructed, then save. A cleared error mark does not confirm a successful save; check the save message and list too.

Check which operation failed, then read the reason and next step. If the message gives no reason, do not guess.

If “Details” is available, select it to see more information. Select “Hide” to hide the details. If a × button is shown, select it to dismiss the message. When asking for help, keep a record of the message and details for a root user to check.

If the result is unconfirmed, refresh and check the device, file, or task state. A timeout or lost connection does not mean the operation failed to run. Do not immediately repeat power, delete, task-start, or upgrade operations. Ask a root user to investigate if the result remains unclear.

For batch actions, check successful, failed, and unconfirmed items separately. Confirm downloaded files in the browser’s download list.
