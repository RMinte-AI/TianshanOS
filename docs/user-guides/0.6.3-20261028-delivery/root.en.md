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

“Orphan Commands” reference hosts that no longer exist and cannot be executed. Delete an orphan command, or use the host binding option during import to associate it with a valid host.

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

After enabling “Run in background (nohup),” you can use:

- “View Log”: Read the current background-task log.
- “Tail Log”: Continuously refresh the log.
- “Stop Tail”: Stop refreshing the page without stopping the remote task.
- “Check Process”: Check whether the background task is still running.
- “Stop Process”: Request termination of the background task, then check its state.

If “View Log” or “Check Process” fails, read the reason and check the host connection. A failed read does not mean the log is empty or the process has exited. Check again after restoring the connection; do not restart a task because its status check failed.

“Service Mode” watches a background task until it becomes available. Provide “Ready match” and “Variable Name.” Adjust the optional failure pattern and timing settings as needed:
- “Ready match”: Marks the service ready when the configured text appears. Use `|` to separate multiple patterns.
- “Fail Match”: Marks the service failed when the configured text appears.
- “Ready timeout (s)”: Sets the longest wait for the ready state, in seconds.
- “Check interval (ms)”: Sets how often the log is checked, in milliseconds; 1000 ms is 1 second.
- “Variable Name”: Stores states such as checking, ready, and timeout.

“Stop Tail” only stops log updates in the page. “Stop Process” requests termination of the background task; check the process state afterward. Service-mode tasks may be starting, being verified, stopping, or unconfirmed. An accepted request is not a final result. If the state is unknown, verify it; confirm the task has stopped before starting it again.

### Import and Export Commands

- Use the export button on a command to export it and, when selected, include its dependent host configuration.
- Select “Import Command,” choose a `.tscfg` configuration package, preview its contents, and choose whether to overwrite an existing configuration or bind it to a host currently shown on the page.

Import can replace configuration with the same ID and may include remote-host information. Follow the Security Guide to check the source and signature. If a restart is required, finish Terminal and Automation tasks before restarting.

### Check Which Rules Use a Command Before Deleting It

Deleting a command does not undo completed operations on the remote host and cannot be undone from the Commands page. First check whether sources, templates, or rules still need it.

- Service running or state unknown: Select the command's refresh icon to verify its state. Stop it if needed and confirm the result. Wait for any start, stop, or verification to finish. The delete button does not stop the remote task.
- Service stopped but deletion blocked: Check which imported rules still use the command. Update those rules first. Stopping a service or the Automation engine leaves the command in those rules. Changing the command's host or replacing host configuration may also be blocked.
- Rule read-only or awaiting restart: Follow Chapter 12. Do not delete SD files to bypass protection. Check the revised rules and connections before removing unneeded configuration.

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
| “Start” | Starts a stopped engine and begins processing enabled rules. |
| “Pause” | Pauses further automatic evaluation. Existing actions may continue. To resume through the page, stop the engine, then start it. |
| “Stop” | Attempts to stop rule checks, data reading, and scheduling later actions, while keeping configuration. Wait for confirmation; a timeout does not mean the engine has stopped. |
| “Reload” | Reads saved configuration again; an engine that was running resumes after loading. Save edits first. Blocked while rules await restart; it cannot replace a device restart. |

Check whether cooling, alerts, or other ongoing tasks depend on Automation. Stopping the engine does not stop a remote background process. Stop remote tasks from Commands or the relevant service card and confirm the result.

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
5. Save and enable the source. Use its “View Variables” control to check the result.

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

“Test” runs the action. Check the effect of power, reboot, remote-command, or external-request operations before testing.

After editing, save and check the parameters. If saving fails, keep the draft and correct the reported issue. Imports may replace templates; check which rules use a template before deleting it. If a linked service is not confirmed stopped, verify it. Stop it if needed and confirm the result.

### Follow Action-Template Deletion Messages

If “Can't delete action template” appears, read the reason first. The template has not been deleted:

1. Rules still use the template: Select “View rules.” Remove the template from those rules, or delete unneeded rules that permit deletion, then delete the template. Disabling a rule or stopping the engine does not remove the template from the rule.
2. Service not stopped: Select “View commands.” Verify its state, stop it, and confirm the result before returning to delete the template.
3. Configuration updating or loading: Wait for it to finish. Recovery needed or usage check unavailable: Open “System Logs” on Terminal, find the cause, and restore configuration. Do not keep selecting delete.

You cannot edit an imported read-only rule directly. Ask the configuration provider to revise its package and remove the unneeded template.

If saving a rule reports that a template no longer exists, the save did not succeed. Select an existing template, review it, and save again.

### Create a Rule

1. Select “Add” under “Rules” and enter a unique ID, name, and icon.
2. Choose “Logic”: AND requires all conditions; OR requires any condition.
3. Set the cooldown for automatic triggers. Values are in ms; 1000 ms is 1 second.
4. Add conditions and choose a variable, comparison, and value. Comparisons include equal, not equal, greater than, greater or equal, less than, less or equal, value changed, and contains. Match the value type to the variable.
5. Add templates and set any action delays, repetition, and action-level conditions.
6. Set “Show on panel” and “Allow manual trigger.” The first controls card visibility on System; the second controls manual execution.
7. Review, save, and then enable the rule. Leave “Enable immediately” off while configuring it for the first time.

If saving, enabling, or disabling reports that the “backup copy is not synced,” the operation succeeded, but the backup copy did not sync. This is not a failed save or a requirement to restart. Do not remove or replace the SD card. Refresh the rule list and check its content and enable state; do not create the rule again. If the warning persists, open “System Logs” on Terminal to check for storage problems.

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

Automatic rules can also appear on the panel. Showing a card does not make its rule manual-only. A disabled rule may remain visible but cannot start. Give admin-facing actions clear names and appropriate log or stop controls.

### Import a Rule Configuration Package

Import rule packages on Automation, not through Security's “Verify Only” or file upload. This control checks whether the signer is trusted, whether the package belongs to this device, and whether required configuration is present. Its verification result does not apply to other package types.

Before importing, prepare:

- The SD card is mounted and writable. Packages are saved on the card. Keep it installed after import; do not remove or replace it while a restart is pending.
- The device has a valid certificate, its matching private key, and a CA chain; its time is verified. A CA chain checks certificate origins. See the Security Guide for installation steps.
- The deployment administrator has selected a trusted signer. Root login or installing HTTPS certificates does not complete this setting. Ask the deployment administrator to configure it if it is missing.
- The rule's templates, commands, and hosts are ready. Import does not install these for you. Add any missing configuration first. If the rule is enabled, also check the enable state of the configuration it uses.

A restart interrupts WebUI and device-management services. Check whether the rule is enabled and will run automatically. Confirm that it may run after restart before importing it. If it must remain inactive, ask the provider for a disabled version. Restarting TianshanOS does not stop background processes on remote hosts.

1. Select the import icon under “Rules” to open “Import Rule Config,” then choose the `.tscfg` file. Verification starts and the page displays a preview.
2. Wait for “Signing trust, target device and rule content verified.” Check the name, rule ID, and “Rule summary.” Expand “View rule content” to review conditions, actions, targets, and enable state; do not rely on the filename.
3. If the same ID exists, read “Overwrite impact.” Select “Overwrite existing config” only if you want to replace the saved version. If the file, credentials, or configuration change after preview, select the file again and reverify it.
4. Select “Confirm Import” and wait. “Saved; takes effect after restart. The running version has not changed.” confirms storage, not that the new rule is running. If the page says the configuration is already saved and active, that repeated import does not require another restart.
5. If a restart is required, save other work and review Automation and remote tasks, then restart TianshanOS from System. Keep the same SD card installed and wait for WebUI to return.
6. Return to Automation and confirm that the rule's pending-restart label has disappeared. Check its enable state, required variables, and logs. If the rule permits a safe manual check, run it once and check the device or remote-host result. For rules that cannot run manually, watch the logs and results after a normal trigger. Do not rely on the save message alone.

Imported rule packages are read-only. You cannot edit, enable, disable, or delete them directly. Ask the provider to issue a revised package for this device, then import it as described here. To disable a rule, obtain a disabled version and restart as instructed; the old version may still run until restart.

The engine's “Stop” control stops scheduling later actions. It does not change rules or undo completed operations. Stop remote background tasks separately and confirm the result.

After editing or importing, check the engine, rules, and task results. Reload or restart as instructed, then confirm that tasks work. The table below explains read-only and pending-restart restrictions.

### Distinguish Saved and Running Versions

| Page message or state | What to do |
| --- | --- |
| “Saved; restart required” | The new configuration is saved but not yet in use. Ordinary edits, enable switches, and deletion are unavailable. Arrange a device restart. |
| An older version is already running | Until restart, the old rule may still run. The manual button says “Run the current, older version”; check its effect before running it. Export returns the current running version, not the pending new package. |
| New rule not yet loaded | Manual execution is unavailable. Restart, then check its enable state and manual permissions. |
| “Deletion saved; removal requires restart” | The rule leaves the running list after restart. No new manual or automatic run can start. Check actions already sent and remote tasks separately. Confirm removal after restart. |
| Read-only rule | Ordinary edit, enable, disable, and delete controls remain unavailable after the pending-restart label disappears. Obtain and import a revised package as described above. |

Not every save needs a restart. Creating, editing, or enabling an editable rule updates the current rule when the operation succeeds. Disabling it does too. Check the operation result and rule state.

“Reload” is unavailable while rules await restart. Stopping and starting the engine does not switch those versions; restart the device.

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
| A bound service is running or its state is unconfirmed | Verify the service on Commands. Stop it if needed and confirm the result. Wait for any start, stop, or verification in progress to finish, then select the file again and reverify it. |
| Unsupported action | Ask the provider to revise the package. Rule-package import currently rejects Webhook actions; their presence in the template selector does not mean the package can be imported. |
| File, credentials, or configuration changed after preview | Select the file again, reverify it, and review overwrite impact before confirming. |

#### Save Incomplete

| Message or issue | Next step |
| --- | --- |
| SD card unavailable | Check that the card is inserted, mounted, and writable, then verify again. |
| Configuration source read-only | Restore a writable source. Do not delete files to bypass protection. |
| Save unreliable | Check storage, resolve the issue, and verify again. |
| Save outcome is unconfirmed | Refresh the rule list and check the rule and pending-restart state. If it remains uncertain, inspect System Logs. Keep the original file and do not repeat the import. |

A condition variable without a sample is not necessarily missing configuration; check when it will update. If old storage objects need cleanup, keep the SD card installed and check after restart.

### Maintain Rules and Configuration

- Manual execution requires an enabled rule that permits manual triggers, with no run of the same rule already in progress. It bypasses automatic trigger conditions and cooldown, but still applies action conditions, delays, and repetition.
- Disabling does not undo completed actions. Deletion cannot be undone from the page.
- Before editing sources, templates, or commands, check which rules use them. Verify or stop any linked service that is running, unknown, or still processing an operation. If a rule still uses the configuration, revise the rule too; stopping the service does not remove that use.
- Use this chapter's dedicated flow for rule packages. Follow the Security Guide for other packages; the rule-package verification result does not apply to them.
- If saving fails, read the message and retain the draft. If saving succeeded but refreshing failed, refresh and check the list before creating another item.
