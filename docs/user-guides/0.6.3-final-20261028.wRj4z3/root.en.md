## 10. Terminal and System Logs

“Terminal” is shown only to root. It can run device console commands directly and display device logs. A command may change device state immediately. Confirm its source, parameters, and impact before entering it.

### Connect to and Use Terminal

1. Select “Terminal” in the top navigation.
2. Wait for “Connected to device” and the `tianshan>` prompt.
3. Enter `help` to view the commands provided by the current firmware.
4. Enter a command and press Enter. Wait for its output to finish and the prompt to return.

Input is not executed while the page shows “Not connected to device.” After a disconnect, wait for the reconnection message before submitting a command again to avoid duplicate execution.

Terminal supports these keyboard controls:

| Control | Purpose |
| --- | --- |
| Ctrl+C | Clear the current input and request an interrupt. The command must support interruption for it to stop. |
| Ctrl+L | Clear the screen. |
| ↑ / ↓ | Browse command history from the current page session. |
| ← / → | Move the cursor within the current input. |

“Clear” at the top of the page only clears the display. It does not undo commands that have already run. “Disconnect” ends the current Terminal connection.

### Open a Remote SSH Shell

An SSH Shell sends subsequent keyboard input to a remote host. Confirm the target address, user, and authentication method, and complete the SSH preparation described in the Security Guide before connecting.

1. Enter `ssh --help` to view the SSH command options.
2. Enter `ssh --host <host> --user <user> --shell`, replacing each placeholder, including its angle brackets, with the actual value. To specify a port, add `--port <port>` before --shell.
3. Wait for the remote connection confirmation before entering remote commands.
4. Press Ctrl+\ to leave the SSH Shell and return to the tianshan> prompt.

Do not enter plaintext credentials while the terminal is being shared, recorded, or viewed by another person.

### View System Logs

Select “System Logs” at the top of Terminal to open the log window.

- “Level” sets the minimum displayed level. Use ERROR, WARN+, INFO+, or DEBUG+ to narrow the output.
- “TAG” filters by log source.
- “Search” filters the current logs by keyword.
- “Auto Scroll” follows new log entries when enabled.
- The refresh button reloads historical logs.
- The clear button clears only the logs currently shown in the window.

If filtering produces no output, clear TAG and Search first, then change Level. Closing the log window does not stop device services.


## 11. Manage and Run Remote Commands

“Commands” stores reusable SSH commands and runs them on a selected host. A remote host is a device reached through SSH, such as AGX or LPMU. Manage hosts and keys on “Security”; preparation is covered in the Security Guide.

### Check the Host Fingerprint Before Connecting

A fingerprint helps confirm a host’s identity. These steps apply to SSH host tests on the web “Security” page, rather than the Terminal SSH Shell confirmation.

1. If a fingerprint is unknown or has changed, stop repeating the command.
2. Open “Security,” find the target SSH host, and select “Test.”
3. For a first connection, confirm the address and fingerprint with the host administrator through a trusted channel. Select “Trust & connect” only if they match.
4. For a changed fingerprint, the dialog shows the saved and current values, with “Cancel” selected by default. Check why it changed. Cancel if you cannot confirm it; select “Update Host Key” only after confirming the new fingerprint.

A system reinstall may change a fingerprint, but a change can also mean the wrong host or an impersonated host. Do not delete the old fingerprint to skip this check.

### Select a Host and View Commands

1. Select “Commands” in the top navigation.
2. Choose the target in “Select Host.”
3. View its commands in “Command List.”

“Orphan Commands” refer to hosts that no longer exist and cannot run. Check whether they are still needed before deleting them or recreating them under a valid host. The current import dialog does not offer host reassignment.

### Create or Edit a Command

Confirm the command and permissions on the target host first. Avoid making destructive operations, power-off, reboot, or file replacement easy to trigger by mistake.

1. Select a host and choose “New Command.” To edit, select an existing command’s edit button.
2. Enter a unique “Command ID.” Use letters, numbers, underscores, and hyphens. Do not start or end with an underscore or hyphen.
3. Enter “Name” and “Command.” Put each command on its own line.
4. Add a description and choose an icon or an image on the SD card if needed.
5. Check the run mode and result-matching settings, then select “Save.”

The name helps users identify a task; the ID lets Automation find it. An existing command’s ID cannot be changed. To use a new ID, create a command and update the templates and sources that use it.

If configuration has changed, input in the open dialog is not a saved draft. Copy anything you need to keep before closing and reopening the editor. Review the latest configuration before saving. Do not keep submitting or assume that closing saves a draft.

### Run and Check Results

1. Select the execute button on the command card.
2. Watch the output and status in “Execution Result.”
3. If “Cancel” is available, use it to request interruption of the current session. Continue checking output and status to confirm whether it stopped.
4. “Clear” only removes the displayed result.

Neither “Cancel” nor “Clear” undoes remote operations that have finished. Success, failure, extracted content, and the final state also depend on the matching settings below.

### Configure Result Matching

- “Expected match”: Marks success when output contains the specified text.
- “Fail Match”: Marks failure when output contains the specified text.
- “Extract regex”: Uses one `(.*)` capture group to save the required content.
- “Stop on match”: Ends a command that is still running after a successful match.
- “Timeout (s)”: Sets how long to wait for matching. It applies only when success or failure matching is configured, or “Stop on match” is enabled.
- “Variable Name”: Stores status and extracted results for use in Automation.

Choose stable, specific matching text. Broad text can produce a wrong result. After saving, run once and check “Match Results” before using the variables in a rule.

### Use Background Execution and Service Mode

“Run in background (nohup)” keeps the command running on the remote host after the WebUI closes or SSH disconnects. Background execution requires SSH key authentication. Deploy the public key as described in the Security Guide and use its associated host configuration.

“Service Mode” checks whether a background task has become ready. Enter “Ready match” and “Variable Name,” then adjust these settings as needed:

- “Ready match”: Marks readiness when these patterns appear in the log. Separate multiple patterns with `|`.
- “Fail Match”: Marks failure when the log contains this text.
- “Ready timeout (s)”: Sets the longest readiness wait.
- “Check interval (ms)”: Sets the log-check interval. 1000 ms is 1 second.
- “Variable Name”: Stores states such as checking, ready, and timeout.

Starting, verifying, and stopping may take time. Acceptance of a request does not confirm completion. If the state is unknown, check the connection before starting again.

### Manage Tasks in “Background runs”

“Commands” and “Automation” provide a “Background runs” entry. Root users can also open it from System. Retained run records remain available even if the original command has been deleted.

1. Open “Background runs.” Check the task name, host address, and account to find the run you need.
2. For an unchecked or unconfirmed state, select “Verify remote state” and wait. Verify again after a device reboot. Refreshing the list only reloads records; it does not connect to the host to check the process.
3. Select “View Log” for the last 100 lines from the original host. “Follow log” refreshes the log; “Stop following” only stops refresh, not the task.
4. To end the task, select “Stop” and confirm. Wait for confirmation that it stopped before deciding whether to start again.

A failed log or status check does not mean that the log is empty or the process has exited. Check the host connection and verify again. Do not start another task because a check failed.

Deleting a command, switching configuration, or rebooting TianshanOS does not end background processes on remote hosts. Records retain the original host information. Recreating a command with the same name or changing its host does not move an old task to the new host. Check the original address shown here.

An automatic rule may start a task again after you stop it. To keep it stopped, check and disable the related rule as needed before stopping the task.

### Import and Export Commands

- Use a command’s export button to save a package. Include the associated host if the export options offer it and you need it.
- Select “Import Command,” choose a `.tscfg` file, and review the command and any host information. Enable overwrite only if you intend to replace the same ID.
- Check the source, certificate, and signature as described in the Security Guide. If the file, credentials, or configuration changes after preview, select “Check package again” and review it before importing.

A successful import is saved to the current configuration without requiring a reboot. Existing Automation pauses remain in place. Check the host and task impact before running. To update the internal copy, follow Chapter 13.

### Delete a Command

Select delete and review the affected templates and rules. Follow Chapter 12 to replace the association, or unlink it and disable related rules, before confirming deletion.

Deletion does not require proof that a remote service has stopped, and it does not stop that service. To end a retained task, find it in “Background runs” and stop it. Deleting a local host configuration does not revoke the remote public key; see the Security Guide for revocation.

## 12. Automation Management

“Automation” connects data and actions into repeatable tasks. Create sources and action templates first, then use rules to decide when to run them.

```text
Automatic: Source → Variable → Rule condition → Action
Manual: System Quick Action → Action
```

Sources read data, variables hold values, rules evaluate conditions, and templates define tasks. “Show on panel” and “Allow manual trigger” are separate settings. Automatic rules can also appear on the panel.

### View and Control the Automation Engine

The page shows engine state, rule count, variable count, source count, trigger count, and uptime.

| Control | Effect |
| --- | --- |
| “Start” | Starts a stopped engine and processes rules that are allowed to run. |
| “Pause” | Pauses further automatic evaluation. Existing actions may continue. To resume, stop and then start the engine. |
| “Stop” | Attempts to stop rule evaluation, source reads, and further action scheduling while retaining configuration. Wait for confirmation; a timeout does not confirm it stopped. |
| “Reload” | Reads the saved configuration at the current location. Save edits first and check the result. This does not restore a different backup. |

“Enable automation” and “Disable automation” in “Configuration & backup” save an explicit choice for the configuration. An engine pause or stop does not mean that all rules are disabled or remote services have ended. See Chapter 13.

Check cooling, alarms, and other ongoing tasks before changing engine state. Stop remote background tasks in “Background runs” and confirm the result.

### Complete a Small Automation Flow

1. Create a source and test its connection and required fields.
2. Enable it, select “View Variables,” and check the values and update time.
3. Create an action template, review its parameters, and select “Test.” Testing sends a real execution request. Confirm that the device and remote host can accept it.
4. Create a rule with “Enable immediately” off. Set conditions, cooldown, action order, delays, and repeats.
5. Set “Show on panel” and “Allow manual trigger” as needed.
6. Save, enable, and observe variables, trigger count, and actual results.

### Manage Sources

Under “Data Sources,” select “Add” and choose a type:

| Type | Purpose |
| --- | --- |
| “REST API” | Periodically reads data returned by an HTTP address. |
| “WebSocket” | Receives data pushed through a persistent connection. |
| “Socket.IO” | Receives events from a Socket.IO service. |
| “Command Variables” | Reads the results of configured remote commands. |

1. Enter a unique ID, display label, and the connection details required for the type.
2. Test the connection. Use the address, authentication, and fields supplied by the data provider.
3. For the first three types, choose fields from the test result. Leaving a Socket.IO event name empty allows event discovery.
4. For “Command Variables,” choose a host and a command with a variable name, then set the check interval.
5. Save, enable, and check the result through “View Variables” on that row.

Each source row offers enable, variable view, export, and delete controls. Check which rules use a source before disabling or deleting it. Before importing, review the type, ID, overwrite contents, and the source and trust requirements in the Security Guide.

### View Variables

Select the eye icon, “View Variables,” on a source row to see names, types, values, and update times. Remote commands also provide a variable view.

- Check the name and source. Confirm that the value and type suit the rule comparison.
- Use the update time to see whether readings are still arriving.
- If data is missing, check that the source is enabled, then test its connection and fields. For command variables, also check the command result.

### Create and Test an Action Template

Under “Action Templates,” select “Add” and choose a type:

| Type | Effect |
| --- | --- |
| “CLI Command” | Runs a local device console command. |
| “SSH Command” | Runs a remote command saved on “Commands.” |
| “LED Control” | Controls LED colors, animations, or matrix content. |
| “Log Record” | Writes a message at the selected log level. |
| “Set Variable” | Assigns a value to an Automation variable. |
| “Webhook” | Sends an HTTP request to the specified address. |

Enter a unique ID, name, and the required parameters. Set a delay and “Async Execution” if needed. Async actions continue in the background; use logs, variables, or the target device to confirm the final result.

- CLI: Enter a command, with a result variable and timeout if needed.
- SSH: Choose a host and command, then check the preview.
- LED: Choose a device and supported color, animation, brightness, text, image, QR code, or filter.
- Log: Select a level and enter a message. Variables can be included.
- Set Variable: Enter the variable name and value.
- Webhook: Enter the provider’s URL and choose GET, POST, or PUT. Add request content if needed. Testing sends a request to that address; confirm that the recipient permits it.

Local Webhook actions can run, but signed rule packages containing Webhook are still unsupported. This restriction applies to package import.

“Test” may power off, reboot, run a remote command, or send an external request. “Queued” means waiting to execute, not completed. Check “System Logs” in “Terminal” or the target device for results.

If an image or font cannot be read, check the original SD card and path, or select an available resource. The emergency copy stores task configuration, not those files.

### Create a Rule

1. Under “Rules,” select “Add” and enter a unique ID, name, and icon.
2. Choose “Logic”: AND requires all conditions; OR requires any condition.
3. Set “Cooldown” to limit automatic trigger frequency. The unit is ms; 1000 ms is 1 second.
4. Add conditions, choosing a variable, comparison, and value. Supported comparisons are equal, not equal, greater, greater or equal, less, less or equal, changed, and contains. Match the value type to the variable.
5. Add templates and set their order, delay, repeat mode, and action conditions.
6. Choose whether to enable “Show on panel” and “Allow manual trigger.”
7. Save and review before enabling. Leave “Enable immediately” off during initial setup if needed.

Up to 64 rules can be saved. This does not mean 64 tasks can run at once. Command length and the number of conditions and actions also affect configuration capacity.

Numbers must be valid, finite values. Use a single comparison value, rather than an array or an object. Follow the selected value type and correct any field errors before saving.

A successful save updates the current configuration without requiring a reboot to switch rules. “Does not include the latest changes” for the internal copy does not mean the save failed. Check the changes, then save an emergency backup as described in Chapter 13.

### Set Action Repeats and Conditions

| Mode | Meaning |
| --- | --- |
| “Once” | Runs once per trigger. |
| “Repeat while true” | Repeats according to the individual action’s conditions, up to 100 times per run. |
| “Fixed count” | Repeats for the chosen count and interval, checking action conditions before each execution. |

Action conditions are separate from rule trigger conditions. Without action conditions, “Repeat while true” repeats up to the per-run limit. The rule may be triggered again later.

Disabling a rule blocks new triggers. Check actions already sent and remote tasks separately. Stopping, deleting, or changing configuration does not undo completed operations.

### Create a Quick Action

“Manual trigger only,” “Show on panel,” and “Allow manual trigger” control whether a rule runs only manually, whether its card appears, and whether users may run it.

1. Enter the ID, name, and icon, then enable “Manual trigger only.” This rule does not need automatic trigger conditions.
2. Add templates and check delays, repeats, and action conditions.
3. Enable “Show on panel” and “Allow manual trigger,” then save and enable the rule.
4. Return to System and check the card name, state, and actual result.

An automatic rule can still run by its conditions when displayed on the panel. Disabled cards may remain visible but cannot start. Give admin-facing cards names that explain their actual tasks.

### Import a Rule Package

Use the import entry in “Rules” to select a `.tscfg` file. File upload and Security’s verify-only function are not substitutes. Before confirming, check:

- The current storage location is writable. Check the card in SD mode or internal storage in emergency mode.
- The device certificate, matching private key, and CA chain are available, and device time is confirmed.
- The deployment administrator has configured a trusted configuration signer. Root login or an HTTPS certificate does not replace this requirement.
- Required templates, commands, and hosts exist. Rule import does not create them. For enabled rules, check that the required configuration is enabled too.

1. Select the file and wait for “Signing trust, target device and rule content verified.”
2. Check the name, ID, and “Rule summary.” Open “View rule content” to review conditions, actions, targets, and enable state.
3. For an existing ID, review the previewed contents and enable state. Enable “Overwrite existing config” only if you intend to replace it.
4. If the file, credentials, or configuration changes after preview, select “Check package again” and review it.
5. Confirm import and wait for the save result, then check the rule and actual task result.

A successful import takes effect in the current configuration without a reboot. An existing pause is not lifted. If execution is allowed, enabled automatic rules may run when their conditions are met. Check task impact before confirming import.

Imported rules can be edited, enabled, disabled, or deleted on the device. Local edits do not mean that the original signer approved those changes. Every new package import still requires signature verification; an older result does not bypass it.

A certificate usable for HTTPS does not necessarily authorize package signing. Ask the provider for a package signed with the required permission; do not disable verification. Signed rule packages containing Webhook still cannot be imported.

### Delete a Rule, Template, Command, or Host

You can delete configuration without first stopping a remote service. Deletion does not stop that service. Check which rules, templates, and tasks will be affected before deciding to delete.

1. Select the target’s delete button and check its name and affected items.
2. Under normal “Delete,” choose a replacement if offered, or “Unlink and disable affected rules.”
3. Review what will change or be disabled, then confirm deletion.
4. Refresh the lists to check the result. To end an old background task, stop it separately in “Background runs” and confirm.

A deleted rule is removed from the current configuration without waiting for a reboot. Deleting a template removes its related actions and disables affected rules. Deleting a command retains related templates as disabled drafts and clears their command association; affected rules are unlinked and disabled. Deleting a host may also affect commands and sources. Check the confirmation list and the resulting lists.

Before replacing an association, confirm the replacement’s target and purpose. A similar name is not enough. Check affected configuration after deletion, correct it, and confirm that execution is safe before enabling related rules.

In SD mode, deletion changes only the current SD configuration. The old internal copy is unchanged. Save a new emergency backup after checking the result if you do not want an emergency restore to bring back old tasks.

### Use “Force delete”

If deletion is needed and normal association handling cannot complete, choose “Force delete” in the delete dialog. It removes the target, clears confirmed associations, and disables affected rules. It does not stop remote tasks.

When the full impact cannot be confirmed, the page requires confirmation to pause Automation and continue deleting. Proceed only if Automation may be interrupted. It stays paused afterward. Check the remaining configuration, then select “Enable automation” in “Configuration & backup” when it is safe to run.

Force deletion does not bypass management permissions, configuration-change checks, or storage-write failures. A disappearing card is not proof of a successful permanent deletion. Read the result and reload the lists. Do not delete SD files to bypass protection.

If configuration has changed, select “Review deletion impact.” Read the new list, choose how to handle associations again, and then confirm. This button rechecks the impact; it does not delete automatically.

### An Incomplete Save or Import

| Situation | Next step |
| --- | --- |
| Configuration changed | Keep unsaved input. Reopen the editor, or recheck the import or deletion impact, before confirming. |
| Save result unconfirmed | Reload the lists and storage location first. Check System Logs if still unclear. Do not submit again before checking. |
| Storage is unwritable or full | Check the current card or internal storage, resolve the problem, and recheck. If writing failed, do not assume your changes were saved. |
| Template, command, or host missing | Add the item named in the message, check its enable state, then verify again. |
| Signer untrusted, wrong certificate purpose, or package for another device | Ask the deployment administrator or provider to check. Do not rename package contents or disable verification. |
| Device time unconfirmed | Sync and check time under “Network & Time” on System, then verify again. |

## 13. Configuration and Backup

Check which configuration the device is using before saving, restoring, or switching. Open “Configuration & backup” on “Automation.” These operations are for root users.

### Understand the Current Configuration Location

1. Open “Automation” and select “Configuration & backup.”
2. Under “Currently using,” check whether it is the SD card or internal emergency configuration.
3. Check SD card availability, the last internal save time, and “Backup status.”

| Current state | Meaning |
| --- | --- |
| Using the SD card configuration | Additions, edits, and deletions are saved to the card. The internal copy does not update automatically. |
| Using the internal emergency configuration | Additions, edits, and deletions are saved inside the device. No separate emergency-copy save is needed. |
| Does not include the latest changes | The internal copy differs from the current configuration. This is not necessarily a save error. Check the tasks, then update the copy manually. |

The copy includes rules, templates, SSH commands, associated hosts, sources, and user-variable definitions that need saving. It excludes private keys, images, fonts, logs, and remote run state. It is not a full-device backup.

System settings for networking, fans, temperature, and lighting are saved separately inside the device. Restoring an Automation copy does not restore those settings or keys to older values. Inserting an old card does not restore system settings either.

### Save an Emergency Backup

While using the SD card configuration:

1. Check rules, hosts, and actual tasks to confirm that this is the configuration to retain.
2. Select “Save emergency backup” and wait for the success message.
3. Check the last internal save time and backup status.

Saving replaces the previous internal copy; it does not retain multiple historical versions. Later SD edits leave that copy unchanged until you save again. Saving a copy neither stops remote tasks nor backs up their current run state.

### Restore from an Emergency Backup

While using the SD card configuration with a valid internal copy:

1. Check its save time for outdated or unwanted tasks.
2. Select “Restore emergency backup” and review additions, changes, removals, and enable-state changes.
3. Confirm only if the copy may replace the entire current SD configuration.
4. Wait for completion, then check rules, templates, commands, hosts, and the current enable or pause state. A reboot is not required.

Restore replaces the whole set; it does not merge. An older copy may bring back deleted rules or remove newer tasks. Restore requires a usable copy. A failed read must not be treated as empty configuration to overwrite.

The confirmation explains whether automatic rules may run and whether Automation stays paused. Automatic rules can run by their conditions without another click; manual rules still require one. Confirm that tasks may run before lifting a pause.

### Continue Using and Editing Without a Card

If the card is missing or unreadable and a usable internal configuration exists, the device enters emergency mode. Enabled automatic rules run by their conditions, while an existing pause remains. If no usable configuration exists, follow the reason shown; do not assume tasks were restored.

You can add, edit, and delete in emergency mode. Saving writes internally. Images, fonts, and logs may still depend on the card. A successful configuration restore does not make those resources available.

### Choose After Reinserting a Card

1. Insert a usable card and select “Check SD card” in “Configuration & backup.”
2. Confirm that internal configuration is still in use and review the two choices.
3. To use tasks already on the card, select “Use SD card configuration.” The internal copy is retained.
4. To bring changes made without a card back to it, select “Save current configuration to SD card and use it.” Review the differences and confirm replacement.

Inserting a card does not switch, overwrite, or merge automatically. You may close the dialog and continue using internal configuration. Rebooting before choosing does not make the choice for you.

Saving the current configuration to the card replaces its whole configuration. Check the current location afterward. If saving succeeded but switching did not, the device still uses internal configuration. Read the status, then choose “Use SD card configuration”; do not immediately overwrite again.

### Enable or Disable Automation

Use “Disable automation” or “Enable automation” in “Configuration & backup” to save an explicit choice for the configuration. After a forced deletion that requires a pause, check the configuration before enabling it manually. Imports and location changes do not automatically lift an existing pause.

Restoring or switching can bring different rules and enable states. Read the preview before confirming, then check the resulting state. Configuration operations do not stop remote background services. End them separately in “Background runs.”
