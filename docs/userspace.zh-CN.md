# 原生 libc++ 用户态线程与同步

本说明对应 libcxx-native-module-threads 分支。
范围是 C++17 线程、同步及其需要的 TLS、正常退出清理。
异常、RTTI、文件系统和完整 C++ 标准库认证不在本轮范围。

## 用户态解决什么

Zephyr 用户态让应用以较低权限运行，MPU/PMP/MMU 阻止应用直接读写内核内存。
C++ 语法、模板和标准库逻辑仍是普通机器代码；调度、等待和内核对象操作通过系统调用完成。

本实现支持一个 C++ 应用域。域内线程互相信任，共享运行库和堆，彼此不是独立进程。
用户创建的子线程保持用户态，继承内存域与对象权限。
优先级取 max(配置优先级, 调用者当前优先级)；Zephyr 数字越大优先级越低，
因此低优先级用户线程不能借创建子线程提升优先级。

~~~mermaid
flowchart TB
  subgraph U["用户态：同一个 C++ 应用信任域"]
    A["应用 std::thread / mutex / condition_variable"]
    B["libc++ + libc++abi：once、future 等通用逻辑"]
    C["userspace_threading.cpp：句柄、TLS、退出析构"]
    H["应用堆与显式共享数据分区"]
    A --> B --> C
    A --- H
  end
  C --> S["系统调用边界：内存、域、句柄、状态校验"]
  subgraph K["受保护内核内存"]
    S --> D["userspace_kernel.c：对象池、线程槽、使用中计数"]
    D --> E["Zephyr k_thread / k_mutex / k_condvar"]
  end
  E --> F["以 K_USER 启动子线程"]
  F --> U
~~~

libcxx_source_runtime 与 cpp_runtime_kernel 是两个独立归档。
前者可变数据放入 cpp_runtime_partition；后者的 TCB、锁、等待队列和池状态保持受保护。
不能把整个模块的 .bss 或整片 RAM 开放给用户。
启动时还会检查普通 data/bss 探针是否确实进入运行库分区，以及内核池是否留在分区外；
链接布局不正确时返回 ENOEXEC，不启动应用。

当前 Zephyr 生成的分区脚本会给归档通配符加引号；本次 LLD 19 将其按字面匹配，
导致运行库全局变量没有被搬入用户分区。模块只在使用 LLD 时，对构建目录中生成的
两份 app_smem 链接片段去掉这些归档选择器引号，再继续正常链接。
这也修正应用辅助归档和 libc 的同类选择器；内核源码、SDK 文件均不修改。
兼容步骤位于 cmake/userspace_lld.cmake 和 scripts/fix_lld_app_partitions.py。
更新 Zephyr/链接器后应复查此步骤；运行库布局检查会帮助发现失效的集成。

## 为什么不能直接打开 CONFIG_USERSPACE

原来 std::mutex 直接包含 k_mutex，其中有拥有者和等待队列指针。
这些内容不能由用户修改，任意堆/栈对象也不是已登记的 Zephyr 内核对象。
用户态对象因此改为 64 位句柄，真实锁/CV 来自受保护的有界池。

同步句柄绑定对象地址、类型和代次，复制到其他地址、使用旧代次、混用类型都会被拒绝。
55 位代次不会回绕，耗尽后该槽停止复用。
系统调用复制并校验用户参数；阻塞期间保留对象引用，有持有者/等待者时拒绝销毁。
这些检查保护内核，不会使 C++ 标准定义的未定义行为变成合法用法。

call_once、静态局部变量初始化和 libc++ TLS 本身也有可变全局状态。
所以只修线程创建不够，运行库数据分区也必须正确配置。

## 创建与正常退出

1. std::thread 在应用堆中准备调用对象，经系统调用申请线程槽。
2. 服务校验调用者、输出地址和容量，用预留 TCB/栈创建子线程。
   用户调用强制使用 K_USER | K_INHERIT_PERMS，不接受用户提供特权选项。
3. 子线程在用户态执行固定入口，再调用应用函数。
4. 函数返回后先执行语言 thread_local 析构，再处理库 TSS 析构。
   语言 TLS 按逆注册顺序执行；TSS 析构最多重复四轮。
   notify_all_at_thread_exit 和退出时 future 就绪在语言 TLS 析构之后发生。
5. sleep_for 使用可从用户态调用的 uptime 系统调用重检剩余时间，避免提前唤醒缩短睡眠。
   初始预算额外增加一个 tick，用于补偿读取 tick 时的截断；这是有意的保守等待。
   分数 tick 回归用独立硬件 cycle 读数测量，不能用同一套取整时钟自证正确。
6. join 或 detached 回收必须由 k_thread_join 确认线程彻底停止，才能复用栈和槽。

用户回调和析构不能交给系统调用或工作队列在特权态执行。
原生根线程不是 std::thread 创建的，需要显式清理：

~~~cpp
void root_entry(void *, void *, void *)
{
    run_application();             // 返回时自动对象已析构
    cpp_runtime_thread_cleanup();  // 当前根线程的 TLS/TSS
    // 此后直接返回，不再使用运行库
}
~~~

不能替另一个线程清理，也不能把清理放入 abort/fatal hook。

## 安全和生命周期契约

- 一个应用信任域，不支持多个互不信任的 C++ 应用独立状态。
- 用户开始运行后，特权服务使用 C/Zephyr 内核 API，
  不消费同一份用户共享 libc++ 实例、应用 C++ 对象或公共 malloc 堆。
  new-handler、terminate-handler、PMR 默认资源都是可修改回调状态；
  让特权线程继续消费这些状态会破坏隔离。内核服务本身不使用公共 malloc。
- 注册的内存域必须一直有效；运行期间不修改映射、不迁移相关线程。
  同域重复注册成功，注册第二个域返回错误。
- 共享数据放在显式应用分区或共享堆，不能假定子线程可访问父线程栈。
  隔离栈平台上，捕获父栈引用会故障。示例用 shared_ptr 捕获共享堆对象。
- 合作式退出：停止标记、唤醒等待、函数返回、join。
  强制 k_thread_abort 或用户 fault 不运行 C++ 析构，可能留下锁、堆状态、
  阻塞系统调用引用或 join 所有权。**此类故障按设备/固件需要重启处理，当前未实现应用域原地重置，
  不承诺任意 C++ 线程被强制结束后可以单独原地恢复。**
  raw detached abort 测试只证明已停止的线程槽可以复用。
- 当前异常关闭。标准线程构造器资源不足会终止，不能返回错误供应用继续处理。
  同步对象资源不足同样快速终止，防止 libc++ 内部忽略错误后无锁继续。
  TLS 析构注册内存不足也终止，避免编译器忽略返回值后静默漏析构。
- ISR 不可使用这些线程和同步 API。

当前上游基线的 RISC-V 架构头将 C 函数 z_riscv_thread_is_user_mode 声明在
extern "C" 外，C++ 用户态调用会产生链接错误。模块仅在 RISC-V userspace 的 C++ 编译中
预先声明它的 C linkage，避免修改内核或 SDK；相关兼容头是 arch_compat.h。
更新 Zephyr 时应复查是否仍需要这项兼容处理。

## 资源配置

| 配置 | 默认值 | 含义 |
| --- | ---: | --- |
| CPP_RUNTIME_THREAD_MAX | 8 | 仍占用槽的线程，包括未 join 的已结束线程 |
| CPP_RUNTIME_THREAD_STACK_SIZE | 4096 | 请求栈大小，实际还有架构对齐和权限栈开销 |
| CPP_RUNTIME_THREAD_PRIORITY | 5 | 子线程默认优先级，用户子线程不高于调用者 |
| CPP_RUNTIME_TLS_KEYS | 8 | 库 TSS key 容量 |
| CPP_RUNTIME_MUTEX_MAX | 64 | 用户态真实 mutex 池，包含运行库内部锁 |
| CPP_RUNTIME_CONDVAR_MAX | 32 | 用户态 CV 池，包含 timed/shared mutex 内部需求 |
| TIMEOUT_64BIT | y | 避免长等待被转换为 32 位 tick 后截断 |

Zephyr 的 MAX_THREAD_BYTES 还要容纳全系统线程对象权限位图，不能只考虑标准线程。
静态对象扫描会在不足时要求扩大该值。MPU/PMP 的分区预算也必须覆盖
libc、malloc、运行库及应用数据，cpp_runtime_userspace_init 的错误不能忽略。

句柄大小不等于总 RAM，真实对象池、栈和权限数据也占内存。
supervisor-only 与 userspace 对象布局不同；整个应用必须使用一致配置和匹配头文件重建。

## 自己跑实验

samples/thread_userspace 是完整应用：
特权主线程注册运行库域、加入 malloc 分区并启动 K_USER 根线程；
根线程创建标准线程，用 mutex/CV 传递结果，join 后清理自身 TLS。

~~~sh
source /home/linux/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=/home/linux/zephyrproject/zephyr-cpp-userspace-kernel
export ZEPHYR_SDK_INSTALL_DIR=/home/linux/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
export MODULE=/home/linux/zephyr-cpp-runtime

west build -b mps2/an385 -d /home/linux/cpp-user-example \
  "$MODULE/samples/thread_userspace" -- \
  -DEXTRA_ZEPHYR_MODULES="$MODULE" \
  -DLIBCXX_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxx-18.1.8.src \
  -DLIBCXXABI_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxxabi-18.1.8.src
west build -d /home/linux/cpp-user-example -t run
~~~

预期输出 C++ user caller=1 worker result=42。QEMU 继续空闲时按 Ctrl-A X 退出。
SDK Clang 用 ZEPHYR_TOOLCHAIN_VARIANT=zephyr/llvm，并选新的构建目录。
已安装编译器为 GCC 14.3.0、Clang 19.1.7；内部 LLVM 16 尚需实际验证。

运行库分区由 cpp_runtime_userspace_init(&k_mem_domain_default) 映射，
应用自己加入共享数据/堆分区并检查返回值。
不要把整个 app 归档开放给用户，其中可能含 TCB、栈或特权服务状态。
仅属于用户的源文件可独立归档，再用 zephyr_library_app_memory() 映射；
测试里的静态局部变量辅助归档演示了这一点。

## 测试与上线验收

~~~sh
cd /home/linux/zephyr-cpp-runtime
python3 scripts/validate_native.py                # 只打印命令
python3 scripts/validate_native.py --run          # 完整原生矩阵
python3 scripts/validate_native.py --suite thread_userspace \
  --toolchain gcc --platform mps2/an385 --run
python3 scripts/validate_native.py --suite thread_userspace \
  --toolchain llvm --platform qemu_riscv32_smp2 --run
~~~

矩阵覆盖 ARM、RV32 单核/双核和 GCC/Clang。
脚本核对每个用例、编译器、userspace/SMP 配置及输入哈希；
过滤、跳过、只构建不运行都不算通过。
ARM Ztest 额外占用内存分区；若需要关闭测试镜像的 MPU_STACK_GUARD 以腾出 MPU 槽，
会在结果中标明。独立示例仍验证带栈保护的用户态配置。

资源失败采用单独镜像进行故障注入，验证实际用户态 abort 后整镜像停止，
不将错误后的原地恢复当作通过：

~~~sh
export ZEPHYR_BASE=/home/linux/zephyrproject/zephyr-cpp-userspace-kernel
export ZEPHYR_SDK_INSTALL_DIR=/home/linux/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
python "$ZEPHYR_BASE/scripts/twister" \
  -T tests/thread_userspace_failfast -p mps2/an385 -j 2 \
  -O /home/linux/cpp-user-failfast --inline-logs \
  -x EXTRA_ZEPHYR_MODULES=/home/linux/zephyr-cpp-runtime \
  -x LIBCXX_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxx-18.1.8.src \
  -x LIBCXXABI_SOURCE_DIR=/home/linux/libcxx-poc-deps/libcxxabi-18.1.8.src
~~~

最终结果和构建基线见 docs/results/userspace/README.md。
历史 native/POSIX 性能数字来自旧 supervisor-only 提交，不代表当前用户态性能。

上线前仍需实际修改版 LLVM 16/GCC、目标硬件 MPU/PMP 配置和应用峰值验证：
真实抢占/SMP、长时间运行、栈余量、资源容量以及整个应用故障的处理策略。
这需要真实工具链和硬件，不能用 QEMU 代替。
