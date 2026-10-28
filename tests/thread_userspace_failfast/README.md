# 用户态失败策略回归

每个场景构建独立镜像，并且只产生一次预期的致命错误。发生错误后立即停止整个镜像，不恢复线程或应用域。

| CPP_RUNTIME_FAILURE_CASE | 注入方式 | 必须停止的位置 |
| --- | --- | --- |
| 1 | 通过内部 syscall 分配到 mutex 池返回 ENOMEM | 首次 std::call_once 获取内部锁，不能进入回调 |
| 2 | 通过内部 syscall 分配到 condition-variable 池返回 ENOMEM | 新 std::condition_variable 的 notify_one，不能忽略错误后返回 |
| 3 | 用户线程持续 malloc(1)，直到真实 heap 分配失败 | 首次初始化有析构函数的 thread_local，不能静默漏注册析构 |

同步场景还必须先观察到适配器输出 libc++ synchronization failure: -12，证明目标操作收到的确实是 ENOMEM。
TLS 场景同样要求 libc++ TLS destructor registration failure: -12。
只有随后出现对应的 fatal PASS，console harness 才通过。EFAULT 等其他错误导致的 abort 不能冒充资源耗尽测试成功。

测试保留 CONFIG_HW_STACK_PROTECTION=y。测试状态使用简单的编译器 TLS，不增加应用数据 MPU 分区。privileged bootstrap 只使用 C/Zephyr API。

链接器 --wrap=abort 只记录执行确实到达了用户态 abort，然后立即调用真正的 libc abort。它不替换错误处理行为。fatal handler 必须同时验证：

- 当前线程是本镜像唯一的 application_thread；
- 注入前已经验证用户模式，以及资源池或 heap 确实耗尽；
- 只在目标操作前设置的 armed 标志与镜像场景一致；
- abort 包装器确实在用户模式执行；
- 错误原因是 K_ERR_KERNEL_OOPS。当前 ARM 和 RISC-V 都将用户态 panic 降为这个错误原因。

任意其他故障都打印 FAIL。只有满足全部条件，才打印对应场景的唯一 PASS 行，随后调用 k_fatal_halt，永不继续执行。console harness 的 ignore_faults=true 仅允许出现这次预期的内核 fatal 输出，不能把任意 fault 变成通过。

## 运行全部三个场景

按本工作区目录示例：

~~~sh
export ZEPHYR_BASE=/home/linux/zephyrproject/zephyr-cpp-userspace-kernel
export ZEPHYR_SDK_INSTALL_DIR=/home/linux/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
export PATH=/home/linux/zephyrproject/.venv/bin:$PATH

python3 "$ZEPHYR_BASE/scripts/twister" \
  -T /home/linux/zephyr-cpp-runtime/tests/thread_userspace_failfast \
  -p mps2/an385 -j 2 -O /tmp/cpp-failfast \
  -x EXTRA_ZEPHYR_MODULES=/home/linux/zephyr-cpp-runtime \
  -x LIBCXX_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxx-18.1.8.src \
  -x LIBCXXABI_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxxabi-18.1.8.src
~~~

Twister 为三项 scenario 分别传入 CMake 参数 1、2、3，分别构建、启动、停止镜像。最终应为三个 scenario 全部通过，不能只搜索任意一个 PASS 字样。

单独用 CMake 实验时，把 CPP_RUNTIME_FAILURE_CASE 设置为 1、2 或 3，并为每个场景使用不同构建目录。QEMU 输出预期 PASS 后停在 fatal halt 中，需要结束模拟器。
