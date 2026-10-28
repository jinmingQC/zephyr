# 用户态线程与同步回归

本目录验证真正的 Zephyr `K_USER` 执行路径。每个测试入口使用
`ZTEST_USER`，子线程及 TLS 析构函数另外检查 `k_is_user_context()`，
避免只打开 `CONFIG_USERSPACE`、实际仍在 supervisor 模式运行。

测试的共享状态分配在 libc malloc 分区中，或者放在 Ztest 应用分区中。
没有把父线程私有栈地址传给子线程。父子线程虽然属于同一个应用内存域，
各自的用户栈仍然受到独立保护。

套件 setup 在 supervisor 模式调用
`cpp_runtime_userspace_init(&k_mem_domain_default)`，注册单一 C++ 应用内存域。
Ztest 已经把自己的共享数据及 malloc 分区加入这个域。
`src/static_local.cpp` 单独组成一个应用库，并使用 Zephyr 的库到应用分区映射，把可写数据放进 `ztest_mem_partition`；
这也包含编译器自动生成的静态局部变量 guard，不能只标注 C++ 对象本身。

覆盖内容：

- 用户父线程、子线程、孙线程的权限和应用内存域继承。
- 较低优先级用户线程不能通过 `std::thread` 创建更高优先级线程。
- 首次在兄弟线程中初始化的互斥量，以及竞争、递归锁、定时锁、读写锁。
- 条件变量通知与超时，以及同一地址反复构造和析构同步对象。
- `call_once`、首次在用户态初始化的静态局部变量、supervisor 预初始化的静态对象。
- promise/future、`std::async` 的用户态执行。
- TLS 析构的逆序、用户态权限、join 和线程退出通知的完成顺序。
- 原生 `k_wakeup` 打断等待时，`sleep_for` 仍满足最小睡眠时长；
  另外用硬件周期计数器检查非整 tick 起点，避免粗粒度时钟掩盖提前返回。
- join/detach 重复复用、线程池满时返回 `EAGAIN`、释放后的恢复。
- 空句柄和自我 join 的错误返回。
- 互斥量和条件变量池耗尽返回 `ENOMEM`，释放后可以继续使用；与等待超时区分。
- 同步句柄复制、过时代际值、清零伪造、换类型伪造、无效参数与不可访问内存。
- 被持有的互斥量、有等待者的条件变量拒绝销毁。
- 未注册的内存域通过原始 syscall 请求同步对象时被拒绝。

异常关闭时，标准 `std::thread` 构造失败会终止程序。因此资源耗尽测试直接
调用 libc++ 原生后端的创建接口检查错误码；普通功能测试仍使用标准 C++ API。

从已经配置好的 WSL Zephyr 工作区运行，例如：

```sh
source /home/linux/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=/home/linux/zephyrproject/zephyr-cpp-userspace-kernel
export ZEPHYR_SDK_INSTALL_DIR=/home/linux/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
python "$ZEPHYR_BASE/scripts/twister" \
  -T /home/linux/zephyr-cpp-runtime/tests/thread_userspace \
  -p mps2/an385 --inline-logs \
  -O /home/linux/cpp-runtime-userspace-build/arm-gcc \
  --extra-args=EXTRA_ZEPHYR_MODULES=/home/linux/zephyr-cpp-runtime \
  --extra-args=LIBCXX_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxx-18.1.8.src \
  --extra-args=LIBCXXABI_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxxabi-18.1.8.src
```

使用 `qemu_riscv32` 替换平台名称可以验证 RISC-V PMP 用户态。
选择 SDK LLVM 时设置 `export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/llvm`，并使用
新的构建目录。具体实际编译器版本必须以构建报告为准。
SMP 验证使用同目录 `smp2.conf`，并选择支持 SMP 和 userspace 的平台。

此套件验证一个被授权的 C++ 应用域。跨不可信应用域的独立 libc++ 全局状态、
异步取消和任意原生线程的 C++ 退出清理不由这些测试证明。

ARM mps2/an385 只有 8 个 MPU 区域。Ztest 自身增加了一个测试数据分区，
加上 libc、malloc 和运行时后，超过同时启用独立硬件栈 guard 的槽位预算。
板级测试配置因此关闭 Ztest 默认启用的硬件栈溢出 guard；用户态权限、
内核内存保护与用户线程各自私有的栈映射仍然开启。这是测试框架的额外开销，
不能把本套件的结果解读为已验证此平台的硬件栈溢出 guard。
