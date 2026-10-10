# 删除保护：实际使用的已确认文案

日期：2026-10-09。来源：用户确认的方案 v2 + 第二轮第 6 节；以下逐字取自实际语言包。

| key | 中文 | English |
|---|---|---|
| deleteProtection.actionTitle | 无法删除动作模板 | Can't delete action template |
| deleteProtection.actionInUse | 动作模板“{name}”正被以下规则使用。请先把它从这些规则中移除，或删除不再需要的规则，再删除动作模板。 | Action template “{name}” is used by the rules below. Remove it from those rules, or delete rules you no longer need, before deleting the template. |
| deleteProtection.actionInUseWithoutDetails | 动作模板“{name}”仍被规则使用，无法删除。请先把它从使用它的规则中移除。 | Action template “{name}” is still used by rules, so it can't be deleted. Remove it from those rules first. |
| deleteProtection.viewRules | 查看规则 | View rules |
| deleteProtection.commandTitle | 无法删除指令“{name}” | Can't delete command “{name}” |
| deleteProtection.templateTitle | 无法删除动作模板“{name}” | Can't delete action template “{name}” |
| deleteProtection.boundCommand | 此动作模板使用指令“{command}”。 | This action template uses command “{command}”. |
| deleteProtection.running | 指令“{command}”对应的服务仍在运行。请先停止服务，确认已停止后再删除。 | The service for command “{command}” is still running. Stop the service and confirm it has stopped before deleting. |
| deleteProtection.unconfirmed | 还没有确认指令“{command}”对应的服务是否已停止。请先在「指令」页点这条指令的刷新图标核验状态；如果它仍在运行，请先停止，再删除。 | It hasn't been confirmed that the service for command “{command}” has stopped. On the SSH Commands page, select the refresh icon for this command to verify its state. If it is still running, stop it before deleting. |
| deleteProtection.starting | 正在启动指令“{command}”对应的服务。请等待操作完成后再删除。 | The service for command “{command}” is starting. Wait for the operation to finish before deleting. |
| deleteProtection.stopping | 正在停止指令“{command}”对应的服务。请等待操作完成后再删除。 | The service for command “{command}” is stopping. Wait for the operation to finish before deleting. |
| deleteProtection.verifying | 正在核验指令“{command}”对应的服务状态。请等待核验完成后再删除。 | The state of the service for command “{command}” is being verified. Wait for verification to finish before deleting. |
| deleteProtection.operationPending | 指令“{command}”对应的服务还有操作未完成。请等待操作完成后再删除。 | An operation on the service for command “{command}” is still in progress. Wait for it to finish before deleting. |
| deleteProtection.viewCommand | 查看指令 | View commands |
| deleteProtection.checkBusy | 自动化配置正在更新，本次未删除动作模板。请等待更新完成后再删除。 | The automation configuration is being updated. The action template was not deleted. Wait for the update to finish before deleting. |
| deleteProtection.checkLoading | 规则配置仍在加载，本次未删除动作模板。请等待加载完成后再删除。 | The rule configuration is still loading. The action template was not deleted. Wait for loading to finish before deleting. |
| deleteProtection.checkRecovery | 规则配置需要恢复核对，本次未删除动作模板。请在「终端」页打开「系统日志」查看详情。 | The rule configuration needs a recovery check. The action template was not deleted. Open “System Logs” on the Terminal page for details. |
| deleteProtection.checkFailed | 没能确认是否还有规则在使用这个动作模板，所以没有删除。请在「终端」页打开「系统日志」查看原因。 | Couldn't confirm whether any rule still uses this action template, so it was not deleted. Open “System Logs” on the Terminal page to see why. |
| deleteProtection.actionMissing | 规则没有保存：动作模板“{name}”已不存在，可能已被删除。请重新选择动作模板后再保存。 | The rule was not saved. Action template “{name}” no longer exists; it may have been deleted. Select an existing action template and save again. |
| deleteProtection.quickLoadingTitle | 快捷操作正在加载 | Loading quick actions |
| deleteProtection.quickRecoveryTitle | 快捷操作暂不可用 | Quick actions unavailable |
| deleteProtection.recoveryDetails | 请在「终端」页打开「系统日志」查看详情。 | Open “System Logs” on the Terminal page for details. |
| deleteProtection.goAutomation | 前往自动化 | Go to Automation |
| deleteProtection.loadingDetails | 如果长时间没有变化，请在「终端」页打开「系统日志」查看原因。 | If nothing changes for a long time, open “System Logs” on the Terminal page to see why. |
| deleteProtection.quickLoadingNeedRoot | 如果长时间没有变化，请联系拥有 root 权限的用户查看原因。 | If nothing changes for a long time, contact a user with root access to find out why. |
| deleteProtection.quickRecoveryNeedRoot | 请联系拥有 root 权限的用户处理。 | Contact a user with root access to resolve this. |
| deleteProtection.quickEmptyNeedRoot | 如需添加快捷操作，请联系拥有 root 权限的用户。 | To add quick actions, contact a user with root access. |

C 组不使用 goAutomation；首页三个状态仅 root 使用该跳转入口。指令自身的服务阻断框只有 common.close，模板场景保留 viewCommand。
root 空状态 quickActionsHint 已改为「在首页显示」，加载/恢复正文复用现有 runtimeRepair.configLoading/recovery_required；后端服务兜底复用 promptRepair.serviceDeleteProtected。非 root 加载/恢复的补充句与空状态正文使用上述三个 NeedRoot 新键，用户已确认定稿，无占位符，逐字采用。
