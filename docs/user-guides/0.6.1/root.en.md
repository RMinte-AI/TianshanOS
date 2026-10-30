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

The name helps you recognize a command; Automation uses its ID to refer to it. The ID is read-only when you edit an existing command. To use a different ID, create a new command and update any action templates or data sources that should use it.

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

Import can overwrite an item with the same ID and can include remote-host information. Follow the Security Guide when evaluating signatures, certificate trust, and package sources. If the page says a restart is required, finish Terminal and Automation tasks before scheduling it.

Before deleting a command, check its data-source, action-template, and rule references. Deletion may be blocked while a bound service is running, its state is unknown, or an operation is in progress. Verify or stop the service and confirm it has stopped before deleting. Deletion cannot be undone from the Commands page.

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
| “Stop” | Attempts to stop rule processing, source polling, and further action scheduling. Configuration is retained. Wait for confirmation; a timeout means stopping has not completed. |
| “Reload” | Reads the saved configuration again. An engine that was running resumes after loading. Save edits first. |

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

Source rows provide an enable switch, variable viewer, export, and delete controls. Disabling or deleting a source affects dependent rules; check references first.

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
| “Webhook” | Sends a request to an HTTP address. |

Enter a unique ID, a name, and the type-specific parameters. Set a delay or “Async Execution” if needed. An asynchronous action continues in the background; check its final result through logs, variables, or the target device.

- CLI: Enter the command, with an optional result variable and timeout.
- SSH: Choose a configured host and command, and check the preview.
- LED: Choose a device and supported color, effect, brightness, text, image, QR-code, or filter operation.
- Log: Choose a level and message; the message can reference variables.
- Set Variable: Enter the variable name and value.
- Webhook: Enter the method, URL, and request content. JSON is the text format used for structured content.

“Test” performs the action. Check the effect of power, reboot, remote-command, or external-request operations before testing. After editing, save and review the parameters. If saving fails, keep the draft and correct the reported issue. Imports can replace templates. Check rule references before deleting; verify or stop any bound service whose stopped state is unconfirmed.

### Create a Rule

1. Select “Add” under “Rules” and enter a unique ID, name, and icon.
2. Choose “Logic”: AND requires all conditions; OR requires any condition.
3. Set the cooldown for automatic triggers. Values are in ms; 1000 ms is 1 second.
4. Add conditions and choose a variable, comparison, and value. Comparisons include equal, not equal, greater than, greater or equal, less than, less or equal, value changed, and contains. Match the value type to the variable.
5. Add templates and set any action delays, repetition, and action-level conditions.
6. Set “Show on panel” and “Allow manual trigger.” The first controls card visibility on System; the second controls manual execution.
7. Review, save, and then enable the rule. Leave “Enable immediately” off while configuring it for the first time.

### Configure Repetition and Action Conditions

| Mode | Meaning |
| --- | --- |
| “Once” | Runs once per trigger. |
| “Repeat while true” | Repeats while the action’s condition holds, up to 100 times per run. |
| “Fixed count” | Repeats for the configured count and interval, checking the action condition before each execution. |

Action conditions are separate from rule-trigger conditions. Without an action condition, “Repeat while true” continues to the per-run limit. The rule may trigger another run later.

Disabling a rule prevents later triggers, but a run already in progress may continue. To end further scheduling, use the engine’s “Stop” control and wait for confirmation. Check submitted actions and remote processes separately. Completed operations are not undone.

### Create a Quick Action

“Manual Trigger Only,” “Show on panel,” and “Allow manual trigger” control whether a rule runs only manually, whether its card is visible, and whether users may execute it.

1. Enter the ID, name, and icon, then enable “Manual Trigger Only.” Such a rule needs no automatic trigger conditions.
2. Add templates and review delays, repetition, and action conditions.
3. Enable “Show on panel” and “Allow manual trigger,” then save and enable the rule.
4. Return to the System Device Panel. Check the card’s name and state, run it once, and review the result.

Automatic rules can also appear on the panel. Showing a card does not make its rule manual-only. A disabled rule may remain visible but cannot start. Give admin-facing actions clear names and appropriate log or stop controls.

### Maintain Rules and Configuration

- Manual execution requires an enabled rule that permits manual triggers, with no run of the same rule already in progress. It bypasses automatic trigger conditions and cooldown, but still applies action conditions, delays, and repetition.
- Disabling does not undo completed actions. Deletion cannot be undone from the page.
- Check source, template, and remote-command references before editing. Some changes or deletions are blocked while a bound service is running, its state is unknown, or an operation is in progress. Verify or stop the service first.
- Preview IDs, conditions, actions, and replacements before import, and follow the Security Guide for package trust.
- If saving fails, read the message and retain the draft. If saving succeeded but refreshing failed, refresh and check the list before creating another item.

After changes or import, check engine state, enable switches, and actual results. If the page requests a reload or reboot, review running tasks before proceeding.
