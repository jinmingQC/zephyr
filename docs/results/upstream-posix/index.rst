Upstream POSIX 修复回归
======================

在 Zephyr ec98918869caeb2ddaef6e698f9ff36fb60a97ac 上应用两份 POSIX
prerequisite patch，使用 SDK 1.0.1 / GCC 14.3.0，运行原版
posix.threads_base.tls。没有加载 C++ module，也没有修改测试源码。

* ARM mps2/an385：47 passed、2 skipped、0 failed/error。
* RV32 qemu_riscv32 SMP2：47 passed、2 skipped、0 failed/error。
  仅额外启用 SMP、2 CPU 并关闭 QEMU icount。

两项 skip 来自原始测试条件，不计入 passed：动态线程模式不适用的
pthread_attr_static_corner_cases，以及无法分配 2^23 字节大栈的
pthread_attr_large_stacksize。

实际资源为 6 个 POSIX 线程槽、6 个静态动态栈、512 个 TSS key；
栈大小、heap、TLS、SMP 等最终选中值和配置哈希见 selected-config.json。
index.json 保存完整运行命令、版本、补丁哈希、干净基线顺序
git apply --check 结果，以及隔离 kernel git diff --check 的退出码 0。

测试包含 create/join/detach、描述符复用、keys/析构、mutex 和 cond。
其中 mutex timedlock 明确断言经 pthread_join 返回的 ETIMEDOUT 和 0；
tryjoin/timedjoin 覆盖失败后重新 join 的恢复路径。

两份 *-twister.json 是原始 Twister 结果。此回归与共同 22 项 C++ 测试
分别报告，不把原配置 skip 合并为通过数。
