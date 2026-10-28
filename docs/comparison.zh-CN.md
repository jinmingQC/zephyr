# 原生与 POSIX 外部模块：线程同步 PoC 对比

在目前不需要复用 POSIX 应用的前提下，建议优先采用**原生外部模块**。
它在本次六个编译器/平台组合中都具有较小的镜像和静态可写 RAM，
直接使用 Zephyr 内核接口，也不要求修改 Zephyr 的 pthread 实现。
代价是需要自行维护 619 行后端代码；POSIX 方案新增胶水更少，标准接口更适合后续复用。

如果以后已有大量 pthread 应用、需要统一 POSIX 维护或希望尽量少维护私有
libc++ external 接口，再优先考虑 POSIX 分支。当前建议针对本次资源配置和负载，
生产项目调整线程/对象池容量后，应重跑同一比较。

这不是完整 C++17 标准库认证。本次只比较线程与同步，不启用异常、RTTI、
文件系统和流。性能数据来自 QEMU，硬件实时性能仍需在目标板上验证。

## 固定条件

- 核心：Zephyr ec98918869caeb2ddaef6e698f9ff36fb60a97ac。
- 标准库：两边均从源码构建 libc++ / libc++abi 18.1.8，ABI namespace __1。
- SDK：1.0.1；实际编译器 GCC 14.3.0、Clang 19.1.7。
- 应用 C++17，运行库 C++23；性能和空基线均使用 -O2。
- 8 个线程槽、每线程请求 4096 B 栈、8 个 TLS key。
- 主线程及实际工作线程优先级均核对为 5；main stack 8192 B，
  system workqueue stack 4096 B，C malloc arena 32768 B，kernel heap 8192 B。
- POSIX 分支另需 16 个 mutex、16 个 CV 的池，以及 THREAD_MONITOR /
  THREAD_STACK_INFO。这些是该路线的必要开销，纳入比较。
- 三个平台：ARM mps2/an385、RV32 UP、同一 RV32 板开启 SMP/2 CPU。

两边的测试、性能程序、应用配置、公共 CMake 和源码版本一致；
后端需要的系统配置差异没有人为抹平。
原生模块曾在未改动的基线上编译运行示例；最终比较使用同一专用核心工作树，
其中 POSIX 的两个补丁在未启用 POSIX_THREADS 的原生配置中不参与线程实现。

## 同一批功能测试

| 后端 | GCC ARM | GCC RVUP | GCC SMP2 | Clang ARM | Clang RVUP | Clang SMP2 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 原生 | 22/22 | 22/22 | 22/22 | 22/22 | 22/22 | 22/22 |
| POSIX | 22/22 | 22/22 | 22/22 | 22/22 | 22/22 | 22/22 |

共 12 个配置、264 项执行全部通过，没有将过滤或跳过计作通过。
覆盖普通/递归/定时/共享锁、条件变量、call_once、线程 ID、join/detach、
重复回收、future/promise/async、语言 TLS 析构和退出就绪顺序等。
[逐项报告、实际配置和来源哈希](results/functional/index.json)。

POSIX 核心修复另外运行 upstream 原版 posix.threads_base.tls：
ARM 与 RV32 SMP2 各 47 passed、2 skipped、0 failed/error。
原测试在动态模式下跳过静态属性边界测试，并因无法分配 2^23 B 大栈跳过大栈测试；
这 4 项未计入通过数。测试包含 join 返回值、失败后重新 join、
descriptor 回收、TLS key、mutex 和 CV。
[原始回归记录](results/upstream-posix/index.json)。

默认 FetchContent 也在全新构建目录真实下载、校验并完成配置/生成，
没有依赖本地绝对源码缓存。这个打包检查没有再次编译应用。
[下载与哈希证据](results/packaging/native-default-fetch-configure.json)。

## 镜像与静态内存

镜像列使用 llvm-size 的 text+data，可写 RAM 列使用 data+bss，单位为字节。
完整 section 表、ELF 哈希、实际配置和链接器区域报告在原始测量文件中。
这是已链接占用，不是所有源码文件的总大小，也不是磁盘上带调试信息的 ELF 文件大小。

RV32 QEMU 把代码也放在 RAM：该板实际 RAM 还要包括 text 和链接器对齐，
不能把可写 RAM 列当成总 RAM。ARM 则分别使用 Flash 与 RAM。
本次没有测量堆峰值、每个线程实际栈深或最坏执行时间。

### 六项操作组成的性能应用

| 平台 / 编译器 | 原生镜像 B | POSIX 镜像 B | POSIX 增量 B | 原生可写 RAM B | POSIX 可写 RAM B | POSIX 增量 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ARM Cortex-M3 / GCC | 32,196 | 38,684 | +6,488 | 88,744 | 94,172 | +5,428 |
| RV32 UP / GCC | 41,546 | 49,956 | +8,410 | 90,592 | 96,068 | +5,476 |
| RV32 SMP2 / GCC | 51,336 | 62,094 | +10,758 | 92,060 | 99,332 | +7,272 |
| ARM Cortex-M3 / Clang | 36,816 | 42,920 | +6,104 | 88,755 | 94,180 | +5,425 |
| RV32 UP / Clang | 45,152 | 53,532 | +8,380 | 86,912 | 92,448 | +5,536 |
| RV32 SMP2 / Clang | 53,352 | 61,734 | +8,382 | 90,000 | 95,584 | +5,584 |

### 相同配置、main 直接返回的空基线

| 平台 / 编译器 | 原生镜像 B | POSIX 镜像 B | POSIX 增量 B | 原生可写 RAM B | POSIX 可写 RAM B | POSIX 增量 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ARM Cortex-M3 / GCC | 20,724 | 22,152 | +1,428 | 54,483 | 61,043 | +6,560 |
| RV32 UP / GCC | 29,480 | 31,002 | +1,522 | 55,928 | 61,224 | +5,296 |
| RV32 SMP2 / GCC | 38,600 | 40,914 | +2,314 | 57,952 | 62,632 | +4,680 |
| ARM Cortex-M3 / Clang | 21,944 | 23,904 | +1,960 | 54,497 | 61,047 | +6,550 |
| RV32 UP / Clang | 32,854 | 32,878 | +24 | 52,524 | 59,312 | +6,788 |
| RV32 SMP2 / Clang | 36,956 | 41,078 | +4,122 | 55,476 | 62,448 | +6,972 |

空基线展示选择某种后端后、不使用实验操作时的链接占用。
它不是两边都不带标准库的“裸 Zephyr”，也不能代替上面的实际应用大小。
对象池与栈是否被链接保留，取决于实际引用；具体以生成镜像为准。

## 性能

共运行 36 次：每个编译器/平台组合按原生→POSIX、POSIX→原生、
原生→POSIX 交错三轮。每项每轮预热一批后测 9 批，总计 1944 批原始样本。
每批先计时、后校验计数和打印，不把 printf 放入计时区间。

表中每项先取该轮 9 批的中位数，再计算同轮 POSIX/原生比值，
最后取三轮比值的中位数。比值大于 1 表示 POSIX 在该项耗时更长；
它不是各项均匀加权的“整体性能分数”。

### ARM / GCC 的具体读数

这是 QEMU 平台计时器换算出的**虚拟微秒**，不是芯片实测微秒。

| 操作 | 原生 虚拟 μs/操作 | POSIX 虚拟 μs/操作 | 配对耗时比 POSIX/原生 |
| --- | ---: | ---: | ---: |
| 已初始化 mutex：lock/unlock | 14.780 | 24.295 | 1.64× |
| mutex：构造、首次使用、销毁 | 32.559 | 52.741 | 1.62× |
| 双 worker 保护递增 workload | 21.211 | 70.063 | 3.30× |
| 条件变量往返一次 | 220.856 | 221.572 | 1.00× |
| 创建并 join 一个线程 | 531.929 | 727.508 | 1.37× |
| 已初始化 call_once 快路径 | 1.024 | 1.024 | 1.00× |

### 全部组合的配对耗时比

| 平台 / 编译器 | 已初始化锁 | 首次锁使用 | 双 worker 更新 | CV 往返 | 创建/join | call_once |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ARM UP / GCC | 1.64 | 1.62 | 3.30 | 1.00 | 1.37 | 1.00 |
| RV32 UP / GCC | 1.52 | 1.91 | 1.51 | 1.06 | 1.39 | 1.00 |
| RV32 SMP2 / GCC | 1.35 | 1.36 | 0.89 | 0.96 | 0.95 | 0.97 |
| ARM UP / Clang | 1.75 | 1.67 | 1.64 | 1.12 | 1.35 | 1.00 |
| RV32 UP / Clang | 1.63 | 1.77 | 1.60 | 0.76 | 1.18 | 1.00 |
| RV32 SMP2 / Clang | 1.60 | 1.36 | 0.99 | 0.94 | 0.93 | 1.00 |

单核四组中，原生已初始化 mutex 耗时较低，POSIX/原生约为 1.52–1.75；
首次使用也较低，线程创建/join 的比值约为 1.18–1.39。
call_once 快路径基本一致。
条件变量则依赖编译器/平台：RV32/Clang 单核的 POSIX 往返比原生更快，
比值约 0.76，不能据此得出“原生所有操作都更快”。

双 worker 更新项包含 gate release、工作负载和 join；UP 同优先级线程
不保证每次 lock 都发生阻塞，不能把这一项当作纯锁争用延迟。
条件变量项不含线程创建和最后 join；创建/join 项则包括正常退出清理及资源回收。

### 计时限制

ARM UP 使用 QEMU icount shift=7，RV32 UP 为 shift=6；
SMP 无 icount，计时明显受主机调度影响。
例如 GCC/SMP 创建/join 的三轮比值范围为约 0.94–1.54，
这说明其数值不能用于硬件性能保证，也不能与 UP 的绝对时间相比较。
SMP 的 CV、竞争和创建结果具有不同方向，当前数据不足以宣布某后端在 SMP 上全面更快。

QEMU 的 k_cycle_get_32 是平台计时器读数，**不是 CPU retired cycles**。
三轮交错减少顺序偏差，但没有把虚拟计时变成目标板时延，也不构成 WCET 测试。
选择产品方案时，应在目标硬件、内部工具链和真实任务优先级下再运行同一负载。

### C++ 对象本身的大小

以下是 ARM/GCC 的 sizeof；所有平台的运行元数据也保存在原始记录中。

| 对象 | 原生 B | POSIX B |
| --- | ---: | ---: |
| std::mutex | 24 | 4 |
| std::condition_variable | 12 | 4 |
| std::once_flag | 4 | 4 |
| std::thread | 4 | 4 |
| std::shared_mutex | 52 | 16 |

POSIX 小的是 C++ 对象中的句柄，内核对象仍在全局池里。
因此不能只看 sizeof 就判断整个系统更省 RAM。

[最终镜像、元数据与全部原始样本](results/benchmark/benchmark-report.json)；
[构建命令与原始构建快照](results/benchmark/benchmark-build-report.json)。
后者的全矩阵起始源码快照早于 POSIX 胶水的纯注释/冗余声明风格调整；
实际 POSIX 镜像均已吸收最终源码，最终测量报告中的源码哈希与交付源码一致。


## 实现复杂度与维护代价

| 项目 | 原生模块 | POSIX 模块 |
| --- | --- | --- |
| libc++ 接口 | external threading API | 上游原版 pthread backend |
| 自有适配 | 538 行 C++ + 81 行头文件 = 619 行 | 127 行 C 胶水 |
| 自有线程池/TSS/同步适配 | 由模块维护 | 复用 Zephyr POSIX |
| Zephyr 核心修改 | 0 | 3 个文件，31 行新增、6 行删除 |
| 线程策略 | Kconfig 固定优先级、静态槽和栈 | 继承创建者优先级、静态 dynamic stack pool |
| mutex/CV 资源 | 对象内含内核结构，无单独句柄池上限 | 句柄较小，但受全局对象池容量约束 |
| 升级标准库 | 需核对 libc++ 私有 external 接口与对象布局 | pthread 接口稳定，需核对 Zephyr 行为及补丁 |
| 使用其他 POSIX 软件 | 不提供这项收益 | 可复用现有线程基础设施 |

行数是含注释空行的物理行，只是维护面积的代理，不能直接换算成质量或工作量。
POSIX 复用了当前核心 pthread/mutex/cond/key 共 2670 行实现，另有 142 行私有头；
不能把这些系统代码当成“不存在”，也不应把它们全部算作本模块新增代码。
[统计方法与哈希](results/benchmark/source-complexity.json)。

POSIX 两个必要补丁分别是：

1. **线程生命周期**：两份私有文件，28 行新增、3 行删除。
   DONE 标记早于内核线程真正停止，回收前需等待终止；
   joiner 也要保留对 retval 的所有权，避免回收线程提前复用控制块。
2. **静态初始化宏的类型**：一份公开头文件，3 行新增、3 行删除。
   句柄为无符号整数，原始 -1 宏在 C++ 列表初始化中会产生 narrowing 错误；
   改成对应 pthread 类型转换，数值与 ABI 不变。

127 行胶水并不是又实现一套 pthread。它只补 C++ language TLS 析构先于
libc++ 退出通知的顺序，以及主线程等原生调用者的 C++ thread::id。
后者不表示完整 pthread 接管。两边的自动退出清理保证针对适配器创建的标准线程；
外部强制 abort、取消、用户态和动态卸载未包含在本次保证内。

## LLVM 16 是否可用

libc++ 18.1.8 的[官方支持表](https://releases.llvm.org/18.1.8/projects/libcxx/docs/)
列出 Clang 16 与 GCC 13。CMake 对 Clang 16 使用 -std=c++2b 构建运行库，
应用仍是 -std=c++17；该参数与
[Clang 16 的语言标准定义](https://github.com/llvm/llvm-project/blob/llvmorg-16.0.6/clang/include/clang/Basic/LangStandards.def)
一致。

本次 WSL 没有你们修改版 LLVM 16 的路径，实际通过的是 SDK Clang 19.1.7。
因此结论是“所选版本和构建方式兼顾 LLVM 16”，不是“内部 LLVM 16 已实测通过”。
拿到该工具链后应重新跑同一功能和尺寸矩阵，并核对目标 ABI、TLS、Picolibc 和链接参数。
本次没有修改 SDK 或编译器安装。

## 分支与复现

两分支的根目录都是外部模块，当前沿用 haha 远端，后续可以整体迁移到独立仓库。

- libcxx-native-module-threads：原生实现。
- libcxx-posix-module-threads：POSIX 实现及两个核心前置补丁。

现有目录：

- /home/linux/zephyr-cpp-runtime
- /home/linux/zephyr-cpp-runtime-posix
- /home/linux/zephyrproject/zephyr-cpp-module-kernel（专用核心，已经应用 POSIX 两补丁）

[中文原理、架构图、克隆与应用实验步骤](../README.md)。

```sh
# 先打印并检查命令；默认不执行
python3 scripts/validate.py
python3 scripts/benchmark_matrix.py

# 共同功能矩阵
python3 scripts/validate.py --run

# 先构建全部镜像，之后停止其他编译/QEMU再计时
python3 scripts/benchmark_matrix.py --build-only --run
python3 scripts/benchmark_matrix.py --measure-only --run
```

性能脚本默认 3 轮，每项每轮 1 次预热和 9 批样本。
完整构建/运行命令、原始样本、配置、工具版本、ELF 与源码哈希均保留，
便于在目标板或内部编译器上重做比较。
