2026-11-01实施授权：用户已明确批准v3本地阶段、独立分支、两个隔离目标模式对话及三方调度/复核/整合，来源见approval-local-phase.json。当前只允许本地验证和构建；所有对话包括监控均禁止设备访问、推送、发布。实机前另请用户确认。以下历史“待批”指准备时状态，以本段和launch-manifest批准记录为准。

TianShanOS 修复准备包 v3（方案反查后；尚未批准实施）

先读 five-designs.txt（五处具体选定设计），再读 repair-plan.txt 和 normal-use-acceptance.txt；five-design-closure.json列闭合状态。plan-review-report.txt保留前版问题和本次复核结论。
代码基线已本地提交2844de64，未推送；当前目录是待审文档，不含48项新修复。

用户审阅：
- five-designs.txt：五处完整选定设计与正常使用；design-review-v3.txt：最终Reviewer/用户视角复核。
- repair-plan.txt：范围、分组、责任、关键修法、监控与实机维护授权。
- normal-use-acceptance.txt：26条用户流程、明确预期、真实体验与未验证边界。
- batch-repair-designs.txt：23批全部48项的修法/证据/正常流程对应关系。

三对话指令：
- monitor-instructions.txt：当前对话协调、阶段复核、整合及验收。
- p1-goal-instructions.txt：13项P1及F01/F64/F66依赖，共16项。
- p2-goal-instructions.txt：另外32项P2。
- heartbeat-instructions.txt：审批后五分钟补查，尚未创建定时任务。

记录：
- baseline-commit.json与baseline-commit-audit.txt：本地提交范围、检查及残余边界。
- baseline-service-control.log、baseline-ws-adapter.log：本次实际运行的针对性本机检查。
- device-observations.txt / device-observations.json：Safari实机准备阶段操作、未做事项与原状态。
- review-record-corrections.json：复裁摘要F37/F59/F74及F58用词更正，原判定和范围不变。
- issues-in-scope.json：48项唯一归属；batches.json：依赖与预计文件。
- normal-use-cases.json：验收记录来源，目前修复后结果全部NOT_RUN。
- launch-manifest.json：待批启动清单，提交后冻结；runtime_state_path为未来唯一实时状态，尚未创建。
- batch-plan-review.json：23批逐批裁定和兼容边界；五处细化已由v3闭合。
- review-evidence/：旧公式真实函数22条隔离兼容探针及结果，不是修复后验收。

用户确认前禁止：创建/调度执行对话、开启heartbeat、实施48项修复、刷机或改变真实运行配置。
所有后续状态须与实际动作一致；不因为准备包写完就标修复或验收完成。

review-dependency-evidence/包含本范围需要的SDK静态摘录，保留原源路径/行号及哈希；这不是SDK已运行验证。handoff-template.txt为简短交接格式，不是产品新框架。
