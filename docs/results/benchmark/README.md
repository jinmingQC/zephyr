# 基准结果索引

- comparison.md：三轮配对性能表、24镜像size表及标准库对象尺寸。
- comparison.json：表格对应数值；所有比值从原始计时器tick计算。
- benchmark-report.json：最终冻结源码hash、实际ELF hash、选中配置、section/内存区表及全部216个指标样本组。
- benchmark-build-report.json：完整24镜像配置/编译命令、环境和构建期snapshot。
- execution-environment.json：QEMU完整命令/版本、宿主环境和内核补丁文件hash。
- posix-object-source-audit.json：12个POSIX adapter.obj的hash/mtime，证明都晚于最终127行适配源码。
- source-complexity.json：自研适配代码、两个内核补丁及复用POSIX源码的行数和hash。
- raw-logs/：36次未裁剪console日志，可重新交给 scripts/parse_benchmark.py。

构建期报告开始采集后，POSIX glue发生一次只改SPDX/prototype写法的129→127行格式调整。
因此build-only里的起始POSIX glue hash保留原始值；最终measure报告的源码hash、ELF hash
和obj审计是最终结果依据。实际12份POSIX镜像均在这个调整之后编译。

原始ELF、map、完整.config、compile_commands与编译日志留在
/home/linux/cpp-runtime-comparison-build/benchmark，没有放进Git。
报告保留配置审计和完整.config hash。复现命令见 benchmarks/README.rst，
工具默认先预览；--build-only --run后在无其他编译/QEMU时--measure-only --run。

数据口径：image=text+data，静态可写=data+bss；RV32 text也在RAM，
板上实际驻留/对齐看链接区表。未测动态分配器/栈峰值。
UP使用icount（ARM shift7、RV32 shift6），SMP没有icount。
这些是QEMU实验观测，不是目标板CPU周期、绝对时延或总体性能保证。
三轮paired ratio范围只是实测范围，不是置信区间。
