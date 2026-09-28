# Gate 0（2026-09-24）

HEAD d6ed947a592265fa12754828bc79803fc50c1db2。交接文档已完整读取。
原工作区证书/HTTPS/time/PKI/API/WebUI 补丁及 tests/certificate 保留；开工 diff 和修改文件 SHA 保存在 /tmp/tianshan-runtime-start。原有 output/、tmp/ 不动。未找到额外仓库 AGENTS.md；遵守用户提供约束。

实际构建配置目标 esp32s3，SDK /Users/massif/esp/v5.5.2/esp-idf，已有 Python3.12 构建环境；PSRAM OCT/80MHz、内部保留16KiB来自 sdkconfig，未作为硬件检测结果。无板卡采样/现场完整 JSON；使用合成配置。

已核实仍存在：卡片 manual 过滤、手动保存丢条件、delete/add 编辑、无 revision、借用指针解锁使用、先 free 再分配、保存错误丢失、日志512截断/共享 scratch、watcher 裸句柄与强删、SSH认证前缺少信任检查、EAGAIN无总deadline。

当前规则权威加载优先 SD目录（同名tscfg优先）>旧单文件> NVS。当前 register/enable/delete 全量保存；普通 SD JSON、NVS需要新恢复协议；不可安全写回的加密/旧单文件来源在改内存前拒绝写，不删除或明文降级。

调用清单 /tmp/tianshan-runtime-start/{rule,ssh}-callers.txt：规则调用来自自动评估任务、API、CLI、启动加载；SSH来自API/CLI/自动动作/日志监控/shell。API规则详情和导出借用内部指针；规则/模板执行跨网络等待。共同路径修改需逐个回归。

批次 A 日志；B 规则所有权与存储；C 字段/UI与服务准入；D SSH生命周期与信任；E 构建和综合验收。原证书修复不作为本轮功能测试替代。
资源目标：新增常驻任务0，常规评估/日志新增分配0；日志工作区<=2KiB，规则元数据<=64B/条，新增内部常驻<=8KiB。实际sizeof/链接图与实机未测分开记录。

收尾核对：开工已有的证书/HTTPS/时间/PKI/服务管理器文件 SHA 保持不变；共同 Web 文件在本轮叠加修改。期间另一任务继续修改 index.html、api.js、app.js、router.js、terminal.js 和语言文件；均保留。最终构建前后源码 SHA manifest 相同，记录于工件目录，不能把这个联合工作区等同于已提交版本。构建生成的 bin 保留为本轮新工件，未刷写。
