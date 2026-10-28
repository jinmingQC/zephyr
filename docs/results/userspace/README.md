# 原生 userspace 最终验证记录

本轮最终通过 **25 个镜像配置、331 次用例执行**，无失败、无跳过。
这里不计开发调试过程，也不将有意失败的负对照计作通过。

| 测试集合 | 配置数 | 每配置用例 | 实际通过 |
| --- | ---: | ---: | ---: |
| 原有线程与同步 | 6 | 22 | 132 |
| 生命周期与压力 | 6 | 9 | 54 |
| 真正 userspace 与权限拒绝 | 6 | 23 | 138 |
| 用户态资源耗尽终止 | 6 个独立镜像 | 1 | 6 |
| 带硬件栈保护的独立示例 | 1 | 1 | 1 |
| 合计 | 25 | — | 331 |

六种功能配置均为 GCC/Clang × ARM mps2/an385、RV32 单核、RV32 双核。
故障注入分别在 ARM GCC、ARM Clang 的三个独立镜像中执行。
所有最终运行使用干净的 Zephyr ec98918869caeb2ddaef6e698f9ff36fb60a97ac；
未应用 POSIX 分支的试验补丁，CONFIG_POSIX_THREADS=n。

实际编译器为 SDK 1.0.1 的 GCC 14.3.0、Clang 19.1.7，运行库源码版本18.1.8。
内部修改版 LLVM 16/GCC 以及真实硬件尚未完成验收。

## 怎么读证据

- [summary.json](summary.json)：逐镜像结果、工具链、内核、关键配置和日志哈希。
- [source-manifest.json](source-manifest.json)：交付源码与构建兼容步骤的 SHA-256。
- [independent-userspace-audit.json](independent-userspace-audit.json)：同一23用例在六种配置全部执行，
  并核对编译输入、权限、SMP、时钟和工具链。
- [supervisor-final-summary.json](supervisor-final-summary.json)：两套 supervisor 测试的独立复核。
- runs/：最终 Twister 原始 JSON、完整配置和控制台。
  控制台归档仅将 CRLF 规范为 LF；原始与归档哈希同时记录。
- audits/：执行时的命令和输入快照。
- [failfast-gcc-layout.json](failfast-gcc-layout.json)、[failfast-llvm-layout.json](failfast-llvm-layout.json)：
  故障镜像的 ELF/日志哈希、运行库分区范围及内核池完整范围检查。
- llvm-layout/：LLD 的运行库与测试辅助归档分区证据、生成脚本和符号表。
  分区修正后，普通 data/bss 探针和 libc++ 内部 once 锁均在用户分区内，
  内核 mutex、CV、TCB 和各对象池均在分区外。
- [sleep-negative-control-result.json](sleep-negative-control-result.json)：
  私有副本仅去掉一个 tick 补偿，新增亚 tick 用例因提前返回而失败，其余8项通过。
  该对照使用与交付相同的测试源码，不修改共享模块，也不计入331次通过。
- [style-only-deltas.json](style-only-deltas.json)：最后两处注释换行的精确前后哈希与替换内容。
  它们不改变逻辑；用户态最终矩阵已使用最终格式。
  supervisor 快照中的相关用户态头文件未参与编译；失败注入的代码逻辑也未变化。

## 重要配置边界

全部六个 userspace 功能镜像都实际以用户态执行，测试入口、子孙线程和 TLS 析构均检查权限。
均使用100Hz系统 tick；RV32 双核确认 CONFIG_MP_MAX_NUM_CPUS=2。
亚 tick 测试使用独立 cycle 计时，通过测试专用系统调用读取，不能直接从用户态读内核计时器。
SMP 注入允许有限次数重新寻找唤醒窗口，但必须实际提前唤醒，且每次成功对齐的等待均不得提前返回。

ARM Ztest 镜像需要 libc、malloc、运行库和测试框架四个分区，
因此关闭测试镜像的硬件栈 guard 以满足8槽 MPU 预算；内核权限保护仍开启。
RV32 功能镜像保持硬件栈保护。独立 ARM 示例和全部六个失败注入镜像均启用
CONFIG_HW_STACK_PROTECTION=y、CONFIG_MPU_STACK_GUARD=y。

Clang/LLD 构建日志含 .symtab、.shstrtab、.strtab 的 orphan-section 诊断，
这些是 ELF 内部元数据；Twister 计数没有把它们作为测试告警。
不能据 Twister 的“0 warnings”声称构建日志完全没有诊断。
完整本机构建日志及 ELF 位于 /home/linux/cpp-runtime-userspace-build。

失败注入不能只看到 abort 就算通过：必须先看到准确的 -ENOMEM 诊断，
再证明指定用户线程实际执行 abort，并核对 fatal reason。随后停止整个镜像，不尝试原地恢复。
早期 LLD 映射错误触发的权限失败未纳入最终通过记录。

## 产品验收仍需要什么

本轮交付实现和可复现回归，尚不能替代真实产品验收。
当前支持一个共享运行库/堆的 C++ 应用信任域；特权服务不消费该用户可修改状态。
正常退出、合作式停止受到支持；强制终止/fault 后按设备或固件重启处理，
未实现任意 C++ 线程或应用域的原地恢复。

后续需在实际内部工具链与板卡上核对 MPU/PMP/MMU 配置、栈余量、
资源峰值、长时间运行和故障重启策略。历史 native/POSIX 性能结果不代表新的用户态路径。
复现命令见 [用户态说明](../../userspace.zh-CN.md) 和 [失败注入说明](../../../tests/thread_userspace_failfast/README.md)。
