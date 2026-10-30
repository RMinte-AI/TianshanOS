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

### Check Where Automation Is Saved

For a command-line check, enter `auto --config status` to view the current configuration location and backup status. Use `auto --help` for the available options. The old `config --sync` option has been removed. Use the web steps in Chapter 13 for routine backup and recovery.

## 11. Manage and Run Remote Commands

“Commands” stores reusable SSH commands and runs them on a selected remote host. Remote hosts and their authentication information are managed on the “Security” page. See the Security Guide for those procedures.

### Check the Host Fingerprint Before Connecting

A fingerprint helps identify the host you are connecting to. These steps apply to SSH host tests on the web “Security” page, not to the Terminal SSH Shell confirmation flow. If the message says that the fingerprint is unregistered or has changed, do not keep executing the command. Open “Security,” find the target SSH host, and select “Test.”

For a first connection, the confirmation dialog shows the host address and fingerprint. Check them with the host administrator through a trusted channel. Select “Trust & connect” only after confirming a match.

If the fingerprint has changed, the dialog shows both the stored and current fingerprints, with “Cancel” selected by default. Reinstalling the host can change its fingerprint, but a change may also mean that you have reached another host or an impersonator.

Check the address and confirm the reason through a trusted channel. Select “Cancel” if you cannot confirm it. Do not delete the old fingerprint to skip this check. Select “Update Host Key” only after confirming that the new fingerprint is trusted.

### Select a Host and View Commands

1. Select “Commands” in the top navigation.
2. Choose a host currently shown under “Select Host.”
3. Review its saved items under “Command List.”

“Orphan Commands” reference hosts that no longer exist and cannot run. Check the host configuration. To keep the task, recreate the command under a valid host, update the templates and rules that use it, and check the result before removing the orphan.

### Create or Edit a Command

A saved command runs on a remote host. Verify the command and required permissions on that host before saving it. Take extra care with commands that delete data, shut down or restart a host, or overwrite files.

1. Select a host, then select “New Command.” Use the edit button on an existing command to change it.
2. Enter a unique “Command ID.” It may contain letters, numbers, underscores, and hyphens, and cannot begin or end with an underscore or hyphen.
3. Enter “Command Name” and “Command.” Put each command on a separate line when using multiple lines.
4. Add a description and choose an icon or an image from the SD Card if needed.
5. Review the run mode and output-matching options, then select “Save.”

The name helps you recognize a command; Automation uses its ID to find it. You cannot change an existing command's ID. To use a new ID, create a command and update the templates and sources that use it.

### Execute a Command and Review the Result

1. Select the execute control on a command card.
2. Watch the output and status under “Execution Result.”
3. When “Cancel” is shown, use it to request interruption of the current session. Check the state and output to confirm whether it stopped. Completed remote operations are not undone.
4. Select “Clear” to remove the current result display.

“Clear” does not undo work already completed on the remote host. Success, failure, extracted content, and final status depend on the command's matching settings.

### Configure Result Matching

Result matching converts remote output into a status that is easier to use.

- “Expected match”: Marks the result successful when the output contains the configured text.
- “Fail Match”: Marks the result failed when the output contains the configured text.
- “Extract regex”: Uses one `(.*)` capture group to save selected output.
- “Stop on match”: Ends a continuing command after a successful match.
- “Timeout (s)”: Sets how long to wait for a match, in seconds. It applies only when a success or failure pattern is set, or “Stop on match” is enabled.
- “Variable Name”: Saves status and extracted output for use on the Automation page.

Choose stable, specific success and failure text. Broad text can produce incorrect matches. Execute the command once and review “Match Results” before using its variables in a rule.

### Use Background Execution and Service Mode

nohup means a command continues to run in the remote host's background after the SSH connection closes. Closing the WebUI does not stop a background task.

Background runs require SSH key authentication. Deploy the public key on “Security,” then select the associated host. Do not save plaintext passwords in background commands. The same key must remain available to stop or verify the original task.

After enabling “Run in background (nohup)” and executing the command, select “Background runs” on Commands. The root-only control on System and the control on Automation open the same list.

1. Find the original task by its name, user, host address, and port.
2. Select “Verify remote state” to check this run on the original host. “Refresh” only reads known records; it does not replace verification.
3. Select “View Log” to read the last 100 lines from the original host. Select “Follow log” for updates. “Stop following” stops only the page updates.
4. To end the task, select “Stop” on its record and confirm. Wait for the result, then verify that it has stopped. Do not start it again without checking.

After a device restart, old records show “Not checked.” “Remote state unconfirmed” means neither stopped nor running. “Last result” describes the last confirmed result, not a guarantee of the current state.

A failed read does not mean the log is empty or the process has exited. Check the original host connection, log path, and SSH key. Verify again after resolving the issue; do not restart a task just because its status check failed.

Deleting a command, changing its host configuration, or switching Automation configurations does not end a remote process. Manage the original task from “Background runs.” Recreating a command with the same name or ID does not take over the original task. Keep any key still needed to control it.

“Service Mode” watches a background task until it becomes available. Provide “Ready match” and “Variable Name.” Adjust the optional failure pattern and timing settings as needed:
- “Ready match”: Marks the service ready when the configured text appears. Use `|` to separate multiple patterns.
- “Fail Match”: Marks the service failed when the configured text appears.
- “Ready timeout (s)”: Sets the longest wait for the ready state, in seconds.
- “Check interval (ms)”: Sets how often the log is checked, in milliseconds; 1000 ms is 1 second.
- “Variable Name”: Stores states such as checking, ready, and timeout.

Service-mode tasks may be starting, verifying, stopping, or unconfirmed. Wait for the final result after a request is accepted. If a background run record already exists, verify that original run and confirm it has ended before starting again. An enabled automatic rule may start the service again. Disable that rule first if the service must stay stopped.

### Import and Export Commands

- Use the export button on a command to export it and, when selected, include its dependent host configuration.
- Select “Import Command,” choose a `.tscfg` configuration package, preview its contents, and decide whether to overwrite existing configuration.

Import may include a remote host and replace configuration with the same ID. Review the command, host, and replacements in the preview before confirming. Saving updates the current configuration without a restart. Existing remote processes do not stop or move to the new host.

New imports check the source, signature, signing permission, and target device. A successful export does not guarantee a later import. A device certificate intended only for HTTPS may lack permission to sign configuration packages. Ask the configuration provider to resolve this; do not disable verification.

### Check the Impact Before Deleting a Command

After selecting delete, review the linked templates and rules. Choose an available replacement command, or unlink the affected configuration and disable its rules. See Chapter 12 for the steps. Deletion changes only the configuration in use; it does not update the other emergency copy.

Deleting configuration and stopping a remote service are separate operations. Deleting does not stop the service or undo completed operations. To end a remote task, stop it from “Background runs” and confirm the result.

## 12. Automation Management

“Automation” connects data and operations into repeatable workflows. Create data sources and action templates, then use rules to decide when to run them.

```text
Automatic: Data source → Variable → Rule evaluation → Actions
Manual: System-page Quick Action → Actions
```

Sources read data, variables hold values, rules evaluate conditions, and templates define tasks. Showing a rule on the panel and allowing manual execution are separate settings. Automatic rules can also appear on the panel.

### View and Control the Automation Engine

The top of the page shows the engine state, rule, variable, and source counts, trigger count, and runtime.

| Control | Effect |
| --- | --- |
| “Start” | Starts a stopped engine. Enabled rules run only if the current configuration permits execution. |
| “Pause” | Pauses further automatic evaluation. Existing actions may continue. To resume through the page, stop the engine, then start it. |
| “Stop” | Attempts to stop rule checks, data reading, and scheduling later actions, while keeping configuration. Wait for confirmation; a timeout does not mean the engine has stopped. |
| “Reload” | Reads configuration again from the location in use. Save edits and check the result. An existing configuration pause remains in place. |

Check whether cooling, alerts, or other ongoing tasks depend on Automation. Stopping the engine does not stop a remote background process. Stop remote tasks from “Background runs” and confirm the result.

“Disable automation” under “Configuration & backup” is separate from the engine's “Pause” control. If the configuration is paused, review it before selecting “Enable automation.” Restarting the device, reloading, or stopping and starting the engine does not clear this configuration pause. See Chapter 13.

### Build a Minimal Automation Workflow

1. Create a source and use its test to check the connection and selected fields.
2. Enable it, then use the row’s “View Variables” icon to check values and update times.
3. Create a template, review its parameters, and select “Test.” Testing runs the action immediately; confirm that the device and host can accept it.
4. Create a rule with “Enable immediately” off. Set conditions, cooldown, action order, delays, and repetition.
5. Set “Show on panel” and “Allow manual trigger” as needed.
6. Save, then enable the rule. Check variables, trigger counts, and actual results.

### Manage Data Sources

Select “Add” under “Data Sources” and choose a type:

| Type | Purpose |
| --- | --- |
| “REST API” | Periodically reads data from an HTTP address. |
| “WebSocket” | Receives data pushed over a persistent connection. |
| “Socket.IO” | Receives events from a Socket.IO service. |
| “Command Variable” | Reads the results of a configured remote command. |

1. Enter a unique ID, display label, and the required connection information.
2. Use the test to check the connection. Obtain addresses, authentication details, and field information from the data provider.
3. For the first three types, select fields from the test response. Leaving a Socket.IO event name blank lets the test attempt event discovery.
4. For a Command Variable, select the host and a command with a configured variable name, then set the polling interval.
5. Check the enable switch in the form, save, and use “View Variables” on its row to check the result.

Source rows provide an enable switch, variable viewer, export, and delete controls. Check which rules use a source before disabling or deleting it, so you do not disrupt their tasks.

Import and export use configuration packages. Preview IDs, types, and replacements before import, and follow the Security Guide for source and trust requirements.

### View Variables

Select the eye-shaped “View Variables” icon on a source row. The window shows names, types, values, and update times. Remote-command variables can also be viewed from the command’s variable control.

- Confirm the names and source, and check that values and types suit the rule’s comparisons.
- Check whether update times match the expected data frequency.
- If no data is available, check that the source is enabled, then test the connection and fields. For command variables, check the command’s execution result too.

### Create and Test Action Templates

Select “Add” under “Action Templates” and choose a task type:

| Type | Purpose |
| --- | --- |
| “CLI Command” | Runs a local device-console command. |
| “SSH Command” | Runs a remote command saved on the Commands page. |
| “LED Control” | Controls LED colors, effects, or matrix content. |
| “Log” | Writes a message at the selected level. |
| “Set Variable” | Assigns a value to an Automation variable. |
| “Webhook” | The option remains visible, but action execution is not implemented. It cannot currently send a request. |

Enter a unique ID, a name, and the type-specific parameters. Set a delay or “Async Execution” if needed. An asynchronous action continues in the background; check its final result through logs, variables, or the target device.

- CLI: Enter the command, with an optional result variable and timeout.
- SSH: Choose a configured host and command, and check the preview.
- LED: Choose a device and supported color, effect, brightness, text, image, QR-code, or filter operation.
- Log: Choose a level and message; the message can reference variables.
- Set Variable: Enter the variable name and value.
- Webhook: Currently unavailable. Choose a supported action instead. Rule packages containing Webhook actions cannot be imported either.

“Test” runs the action. Check the effect of power, reboot, remote-command, or external-request operations before testing. A queued message means the task was accepted, not that it succeeded. Check System Logs and the target device too.

After editing, save and check the parameters. If saving fails, keep the draft and correct the reported issue. Imports may replace templates; review the deletion impact before removing one. If an image or font is missing, insert the original SD card or select an available resource. The emergency backup does not save these files.

### Delete Configuration and Handle Its Links

Deleting a rule, template, SSH command, or host can affect other configuration. Save an emergency backup first if you will need it. Deletion cannot be undone from the page.

1. Select delete in the relevant list. Review the target name and affected items.
2. Confirm deletion if there are no links. Otherwise, choose an available replacement or “Unlink and disable affected rules.”
3. Refresh the list afterward. Confirm removal and check the linked templates, rules, and enable states.

Unlinking a deleted template removes its action from affected rules and disables those rules. Deleting an SSH command leaves linked templates as disabled drafts with the command reference cleared; affected rules are unlinked and disabled. Before choosing a replacement, check its target and action, not just a similar name.

When deleting a host, also review affected commands and sources. A command without a host cannot run. Recreate needed commands under a valid host and update their links, then check them before use. Deleting local host configuration is separate from revoking a remote public key; see the Security Guide.

### Use Force Delete When Needed

If ordinary deletion cannot complete, select “Force delete.” Read the explanation before confirming. It deletes the target, clears links it can identify, and disables affected rules. It does not stop remote background services.

If the full impact cannot be confirmed, the page asks you to check “I agree to pause automation and continue deleting.” Automation stays paused afterward. Review and repair the remaining configuration, then select “Enable automation” in “Configuration & backup.” Do not rely on a restart to resume it.

If configuration changes during confirmation, select “Review deletion impact,” review the new list, and confirm again. Force delete cannot guarantee completion when storage writes fail. Read the list again to check; a disappearing card does not prove the deletion was saved.

### Create a Rule

1. Select “Add” under “Rules” and enter a unique ID, name, and icon.
2. Choose “Logic”: AND requires all conditions; OR requires any condition.
3. Set the cooldown for automatic triggers. Values are in ms; 1000 ms is 1 second.
4. Add conditions and choose a variable, comparison, and value. Comparisons include equal, not equal, greater than, greater or equal, less than, less or equal, value changed, and contains. Match the value type to the variable.
5. Add templates and set any action delays, repetition, and action-level conditions.
6. Set “Show on panel” and “Allow manual trigger.” The first controls card visibility on System; the second controls manual execution.
7. Review, save, and then enable the rule. Leave “Enable immediately” off while configuring it for the first time.

Saving, enabling, or disabling a rule updates the current configuration without a restart. If Automation is paused, saving does not resume it. Refresh the list and check the result. Update the emergency backup after important changes, as described in Chapter 13.

You can save up to 64 rules. This is not a guarantee that 64 tasks can run at once. Command length and the number of conditions and actions also affect storage and memory. If capacity is insufficient, remove unneeded configuration or reduce its content before saving.

Match the value to the selected type. For a number, enter a specific value, not infinity or NaN (an invalid number). Enter text, a boolean (true or false), or null for the other types. Do not enter a whole JSON object or array. If the page reports an error, check the variable, comparison, and value before saving again.

### Configure Repetition and Action Conditions

| Mode | Meaning |
| --- | --- |
| “Once” | Runs once per trigger. |
| “Repeat while true” | Repeats while the action’s condition holds, up to 100 times per run. |
| “Fixed count” | Repeats for the configured count and interval, checking the action condition before each execution. |

Action conditions are separate from rule-trigger conditions. Without an action condition, “Repeat while true” continues to the per-run limit. The rule may trigger another run later.

Disabling a rule prevents new triggers; a run already started may continue. To stop scheduling later actions, select the engine's “Stop” control and wait for confirmation. Check actions already sent and remote processes separately. Completed operations are not undone.

### Create a Quick Action

“Manual Trigger Only,” “Show on panel,” and “Allow manual trigger” control whether a rule runs only manually, whether its card is visible, and whether users may execute it.

1. Enter the ID, name, and icon, then enable “Manual Trigger Only.” Such a rule needs no automatic trigger conditions.
2. Add templates and review delays, repetition, and action conditions.
3. Enable “Show on panel” and “Allow manual trigger,” then save and enable the rule.
4. Return to the System Device Panel. Check the card’s name and state, run it once, and review the result.

Automatic rules can also appear on the panel. Showing a card does not make its rule manual-only. A disabled rule may remain visible but cannot start. Give admin-facing actions names that explain their effect, and tell users whom to contact to verify or stop background tasks. Root users view logs, verify, and stop services from “Background runs.”

### Import a Rule Configuration Package

Import rule packages on Automation, not through Security's “Verify Only” or file upload. This control checks the signer, target device, and required configuration. Verify each package through its own import control; another package's result does not apply.

Before importing, prepare:

- Confirm whether the SD card or internal emergency configuration is in use, and that it can be saved. See Chapter 13.
- The device has a valid certificate, its matching private key, and a CA chain; its time is verified. A CA chain checks certificate origins. See the Security Guide for installation steps.
- The deployment administrator has selected a trusted signer. Root login or installing HTTPS certificates does not complete this setting. Ask the deployment administrator to configure it if it is missing.
- The rule's templates, commands, and hosts are ready. Import does not install these for you. Add any missing configuration first. If the rule is enabled, also check the enable state of the configuration it uses.

Saving an import updates the current configuration without a restart. Before confirming, check the enable state, conditions, actions, and targets, and confirm that execution is safe. An existing configuration pause remains in place. To prevent execution, disable Automation first or ask the provider for a disabled version.

1. Select the import icon under “Rules” to open “Import Rule Configuration,” then choose the `.tscfg` file. Verification starts and the page displays a preview.
2. Wait for “Signing trust, target device and rule content verified.” Check the name, rule ID, and “Rule summary.” Expand “View rule content” to review conditions, actions, targets, and enable state; do not rely on the filename.
3. If the same ID exists, review what will be replaced. Select “Overwrite existing configuration” only if you intend to replace it. If the file, credentials, or configuration change after preview, select “Check package again.” Close and reopen the editor if the page asks you to, then verify again.
4. Select “Confirm Import” and wait for the save result. Check the rule list, enable state, and required variables. A successful save does not prove successful task execution.
5. If the rule permits a safe manual check, run it once and check the device or host. For other rules, watch the logs and results after a normal trigger. To clear a configuration pause, review the configuration before selecting “Enable automation.”

Imported rules can be edited, enabled, disabled, and deleted. Check the current configuration after edits and update the emergency backup if needed. A later import still requires signature verification; permission to edit locally does not bypass it.

A certificate intended only for HTTPS may not authorize configuration signing. A successful local export may still be rejected on import. Ask the provider to resolve the certificate-purpose issue; do not disable verification or repeat the import.

Imports and edits do not undo completed actions or stop remote background processes. End those tasks separately from “Background runs” and confirm the result.

### If Import Does Not Complete

#### Verification Failed

| Message or issue | Next step |
| --- | --- |
| Signing root not selected, signer untrusted, or signer unauthorized | Ask the deployment administrator or configuration provider to check the signing source and certificate permissions. Do not substitute Security's “Verify Only” or disable verification. |
| Package targets another device | Ask the provider to export it using this device's certificate; do not change the name inside the package. Arrange matching replacement packages before changing this device's certificate too. |
| Device time is unverified | Check and synchronize time under “Network & Time” on System, then verify the file again. |
| Configuration loading | Wait for it to finish, then verify the package again. |
| Configuration failed to load | Check System Logs, resolve the cause, and restore configuration before verifying again. |
| Required configuration missing | Add the template, command, or host named by the page, then verify again. |
| Required configuration disabled | Check whether it should be enabled. Once it is safe to run, enable it and verify again. |
| Unsupported action | Ask the provider to revise the package. Rule-package import currently rejects Webhook actions; their presence in the template selector does not mean the package can be imported. |
| File, credentials, or configuration changed after preview | Select “Check package again.” Reopen the editor or select the file as instructed, then review and confirm. |

#### Save Incomplete

| Message or issue | Next step |
| --- | --- |
| Current storage unavailable or full | Check the location in “Configuration & backup.” Resolve the storage issue or reduce unneeded content, then verify again. |
| Current configuration or save location changed | Keep and copy the draft. Reopen the editor, review the latest configuration, and submit again. Do not keep selecting save. |
| Save outcome unconfirmed | Refresh and check the rule content and enable state. If still unclear, inspect System Logs. Keep the original package and do not repeat the import. |

A condition variable without a sample is not necessarily missing configuration; check when it will update. If configuration cannot be read, keep the original files and check System Logs. Do not overwrite damaged content as though it were empty.

### Maintain Rules and Configuration

- Manual execution requires an enabled rule that permits manual triggers, with no run of the same rule already in progress. It bypasses automatic trigger conditions and cooldown, but still applies action conditions, delays, and repetition.
- Disabling does not undo completed actions. Deletion cannot be undone from the page.
- Before editing sources, templates, or commands, check which rules use them. Configuration edits do not stop existing remote tasks. Verify and stop those tasks separately if needed.
- Import each package through its relevant list and review the preview. See the Security Guide for full signature and account procedures.
- If saving fails, read the message and retain the draft. If saving succeeded but refreshing failed, refresh and check the list before creating another item.

## 13. Configuration and Backup

Select “Configuration & backup” on Automation. Check “Currently using” before saving, restoring, or choosing a configuration. First review any effect on cooling, alerts, or remote tasks.

### Distinguish the Three Types of Content

| Content | How it is saved |
| --- | --- |
| System settings such as networking, fans, temperature, and lighting | Saved inside the device. An Automation backup restore does not change them. |
| Current Automation configuration | Additions, edits, and deletions go to the SD card during normal SD use, or to internal storage in emergency mode. |
| Internal emergency backup | While using the SD card, it updates only when you select “Save emergency backup.” Emergency-mode edits update this internal configuration directly. |

The emergency backup saves rules, action templates, SSH commands, associated hosts, sources, and initial definitions of user variables together. It does not save private keys, images, fonts, logs, or remote run states. Host settings only remember which key to use; the key itself is not copied.

This is not a full-device backup. Keep important files and keys through their own backup methods. A restore does not mean remote services have stopped or that every file needed by a rule is available.

### Save an Emergency Backup

1. Confirm that “Currently using” shows “SD card configuration,” then review the current rules and tasks.
2. Select “Save emergency backup” and wait for completion.
3. Check “Last internal save” and “Backup status.” “Includes the current changes” means the internal copy matches the current configuration.

Saving replaces the previous emergency backup. Later SD-card edits do not update it. If it does not include the latest changes, review the current content before deciding whether to save again.

While using “Internal emergency configuration,” edits are saved internally. You do not need to select “Save emergency backup” again.

### Restore an Emergency Backup

A restore replaces the current SD-card configuration with the internal copy, which may be older. Export any current configuration and files you need first. Do not save an emergency backup first; that would overwrite the old copy you intend to restore.

1. While using the SD-card configuration, select “Restore emergency backup.”
2. Review added, changed, and removed items, and whether Automation will stay paused.
3. Confirm and wait for completion. The lists update without a device restart.
4. Check rules, hosts, variables, enable states, and task results. If still paused, review the configuration before selecting “Enable automation.”

A restore does not change networking, fans, or keys. It does not end running remote services. Manage them from “Background runs” if needed.

### When No Usable SD Card Is Available

If a usable internal configuration exists, the device switches to it. Enabled automatic rules still follow their conditions; manual rules still need a click. The internal copy may be older, so check which tasks may run before a planned switch.

Emergency mode permits additions, edits, and deletions. Saves go directly to internal storage. If no usable internal configuration exists, read the reason first. You can start with a valid new configuration, but do not overwrite damaged files as though they were empty.

Networking, fans, and other system settings still load from internal storage. Emergency configuration does not restore SD-card images, fonts, or logs. Insert the original card or select available files if resources are missing. A missing card does not mean all functions stop, or that every task can continue.

### Choose a Configuration After Reinserting the Card

Insert a usable SD card and select “Check SD card,” then review the two choices below. Insertion does not switch or merge configurations. Close the panel to keep using internal configuration. Restarting does not make the choice for you.

| Button | What happens |
| --- | --- |
| “Use SD card configuration” | Reads and switches to the card's configuration. Internal configuration is kept, not copied to the card. |
| “Save current configuration to SD card and use it” | Replaces the card's configuration with the current internal configuration, then switches to the card. The old card configuration is overwritten. |

1. Check the current location and decide which configuration to keep.
2. Select the relevant button and review the replacement and enable-state notice. Before writing to the card, check the added, changed, and removed items.
3. Confirm and wait. Check that “Currently using” shows “SD card configuration,” then review the lists and task results.

Missing or empty card configuration is not filled from internal storage automatically. Confirm that an empty configuration is what you want before choosing it. If the card or configuration changes after preview, check again as instructed; do not reuse the old confirmation.

### Distinguish Save, Switch, and Run Results

| Message or situation | Next step |
| --- | --- |
| Saved to SD card, but not switched | Internal configuration is still in use. Select “Use SD card configuration” to finish switching. Do not overwrite the card again. |
| Configuration saved, Automation still paused | Review the configuration, then select “Enable automation.” Do not save again or rely on a restart to clear the pause. |
| Deletion not saved, Automation paused | Check whether the target is still present and storage is available. Repair and review the configuration before deciding whether to enable it. |
| Save outcome unconfirmed | Read the location and content again. Check System Logs if needed. Do not submit the same change again without checking. |
| Current input differs from the latest configuration | Copy any draft you need before closing and reopening the editor. Review the latest content and enter your edits. The page does not merge drafts for you. |

After a successful switch, restore, or save, check whether tasks are allowed to run and whether they finish. A configuration pause remains in place; review and select “Enable automation” to resume. A pause caused by “Disable automation” or force deletion is not cleared by restarting or switching configurations.
