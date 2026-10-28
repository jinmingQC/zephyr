# 公共线程/同步基准的实测比较

单位：体积为字节；时延为平台计时器换算的 ns/操作。它们是 QEMU 观测，不是实机 CPU 周期或产品性能保证。每轮有一次预热、9 批样本，共三轮交错 AB/BA/AB。

表中比值为每轮 POSIX 中位数/native 中位数，再取三轮比值的中位数；范围为三轮比值的最小至最大，不是置信区间。
两列时延分别是各自三轮批次中位数的中位数；比值列按同一轮配对计算，
因此在有波动的SMP数据中，不能用两列时延直接相除得到比值列。

## mps2/an385 / gcc

|指标|native ns/操作|POSIX ns/操作|POSIX/native|三轮比值范围|
|---|---:|---:|---:|---:|
|mutex_uncontended|14780.3|24295.1|1.644|1.644–1.644|
|mutex_first_use|32559.4|52741.0|1.620|1.620–1.620|
|mutex_contended|21210.5|70062.6|3.303|3.303–3.303|
|condition_round_trip|220855.8|221572.2|1.003|1.003–1.003|
|thread_create_join|531928.8|727507.5|1.368|1.368–1.368|
|call_once_ready|1024.4|1024.4|1.000|1.000–1.000|

## qemu_riscv32 / gcc

|指标|native ns/操作|POSIX ns/操作|POSIX/native|三轮比值范围|
|---|---:|---:|---:|---:|
|mutex_uncontended|8850.3|13472.0|1.522|1.522–1.522|
|mutex_first_use|17646.5|33628.2|1.906|1.906–1.906|
|mutex_contended|9267.2|13969.9|1.507|1.507–1.507|
|condition_round_trip|177329.0|187578.5|1.058|1.058–1.058|
|thread_create_join|374684.4|522012.5|1.393|1.393–1.393|
|call_once_ready|640.1|640.1|1.000|1.000–1.000|

## qemu_riscv32_smp2 / gcc

|指标|native ns/操作|POSIX ns/操作|POSIX/native|三轮比值范围|
|---|---:|---:|---:|---:|
|mutex_uncontended|1317.0|1734.3|1.349|1.224–1.430|
|mutex_first_use|2781.3|3751.5|1.358|1.241–1.445|
|mutex_contended|58317.3|52194.8|0.893|0.819–0.927|
|condition_round_trip|125765.5|110540.5|0.963|0.879–0.989|
|thread_create_join|161303.1|164609.4|0.947|0.936–1.537|
|call_once_ready|9.4|9.2|0.972|0.970–1.001|

## mps2/an385 / llvm

|指标|native ns/操作|POSIX ns/操作|POSIX/native|三轮比值范围|
|---|---:|---:|---:|---:|
|mutex_uncontended|15305.6|26743.4|1.747|1.747–1.747|
|mutex_first_use|35515.9|59415.3|1.673|1.673–1.673|
|mutex_contended|29462.0|48431.0|1.644|1.644–1.644|
|condition_round_trip|244745.2|274853.2|1.123|1.123–1.123|
|thread_create_join|610883.8|822723.8|1.347|1.347–1.347|
|call_once_ready|896.6|896.6|1.000|1.000–1.000|

## qemu_riscv32 / llvm

|指标|native ns/操作|POSIX ns/操作|POSIX/native|三轮比值范围|
|---|---:|---:|---:|---:|
|mutex_uncontended|8981.8|14630.0|1.629|1.629–1.629|
|mutex_first_use|18870.8|33453.1|1.773|1.773–1.773|
|mutex_contended|9448.6|15150.0|1.603|1.603–1.603|
|condition_round_trip|313457.5|238813.0|0.762|0.762–0.762|
|thread_create_join|507675.0|601306.2|1.184|1.184–1.184|
|call_once_ready|384.1|384.1|1.000|1.000–1.000|

## qemu_riscv32_smp2 / llvm

|指标|native ns/操作|POSIX ns/操作|POSIX/native|三轮比值范围|
|---|---:|---:|---:|---:|
|mutex_uncontended|1275.1|2031.0|1.603|1.499–1.623|
|mutex_first_use|2752.0|3715.0|1.365|1.250–1.370|
|mutex_contended|58008.3|56154.5|0.993|0.948–1.033|
|condition_round_trip|123043.5|115616.5|0.940|0.931–0.952|
|thread_create_join|177928.1|168143.8|0.934|0.887–0.992|
|call_once_ready|8.8|8.9|1.000|1.000–1.005|

## 镜像体积

image=text+data；静态可写=data+bss。RV32 链接为 RAM-only，text 也占 RAM；不能把静态可写一列当板上总 RAM。Flash/RAM 实际驻留和对齐见原始链接区报告。

|平台|编译器|程序|native image|POSIX image|差值|native静态可写|POSIX静态可写|差值|
|---|---|---|---:|---:|---:|---:|---:|---:|
|mps2/an385|gcc|minimal|20724|22152|+1428|54483|61043|+6560|
|mps2/an385|gcc|thread_sync|32196|38684|+6488|88744|94172|+5428|
|qemu_riscv32|gcc|minimal|29480|31002|+1522|55928|61224|+5296|
|qemu_riscv32|gcc|thread_sync|41546|49956|+8410|90592|96068|+5476|
|qemu_riscv32_smp2|gcc|minimal|38600|40914|+2314|57952|62632|+4680|
|qemu_riscv32_smp2|gcc|thread_sync|51336|62094|+10758|92060|99332|+7272|
|mps2/an385|llvm|minimal|21944|23904|+1960|54497|61047|+6550|
|mps2/an385|llvm|thread_sync|36816|42920|+6104|88755|94180|+5425|
|qemu_riscv32|llvm|minimal|32854|32878|+24|52524|59312|+6788|
|qemu_riscv32|llvm|thread_sync|45152|53532|+8380|86912|92448|+5536|
|qemu_riscv32_smp2|llvm|minimal|36956|41078|+4122|55476|62448|+6972|
|qemu_riscv32_smp2|llvm|thread_sync|53352|61734|+8382|90000|95584|+5584|

两条路线都保留线程8×4096、TLS key8、相同分配器容量和优先级5。POSIX 的 THREAD_MONITOR/STACK_INFO 和对象表属于该路线的必需开销。没有测量分配器或栈的动态峰值。

mutex_contended 是两 worker 保护递增 workload（含 gate release 和 join）；UP 未强制锁阻塞，不代表纯锁竞争延迟。ARM/RV32 UP 使用 QEMU icount，SMP 未启用 icount，三者绝对值不能跨平台对比。

## 标准库对象本体尺寸

单位字节。POSIX 对象本体更小，但还需要内核对象表；本表不能替代整张镜像的RAM比较。

|平台|编译器|后端|mutex|condition_variable|once_flag|thread|shared_mutex|
|---|---|---|---:|---:|---:|---:|---:|
|mps2/an385|gcc|native|24|12|4|4|52|
|qemu_riscv32|gcc|native|24|12|4|4|52|
|qemu_riscv32_smp2|gcc|native|24|12|4|4|52|
|mps2/an385|llvm|native|24|12|4|4|52|
|qemu_riscv32|llvm|native|24|12|4|4|52|
|qemu_riscv32_smp2|llvm|native|24|12|4|4|52|
|mps2/an385|gcc|posix|4|4|4|4|16|
|qemu_riscv32|gcc|posix|4|4|4|4|16|
|qemu_riscv32_smp2|gcc|posix|4|4|4|4|16|
|mps2/an385|llvm|posix|4|4|4|4|16|
|qemu_riscv32|llvm|posix|4|4|4|4|16|
|qemu_riscv32_smp2|llvm|posix|4|4|4|4|16|
