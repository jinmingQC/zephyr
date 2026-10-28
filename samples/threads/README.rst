.. SPDX-License-Identifier: Apache-2.0
.. SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors

C++17 线程与同步示例
##################

本示例使用外部模块提供的 libc++，演示四个 ``std::thread``、一个
``std::mutex`` 和一个 ``std::condition_variable`` 的配合方式。代码和配置在
原生 Zephyr 后端与 POSIX 后端上保持相同；后端选择由模块所在分支决定。

四个工作线程先通过条件变量等待主线程发出开始通知，随后各自给共享计数器
加一共 1000 次。互斥锁保证同一时刻只有一个线程修改计数器，主线程通过
``join()`` 等待它们全部结束。成功时串口打印::

   libc++ threads: 4000 increments

``src/main.cpp`` 是可直接修改的入口。例如可以调整循环次数，或在
``std::lock_guard`` 的保护范围内加入自己的共享数据操作。新增同时运行的线程时，
需要同时检查 ``prj.conf`` 中的线程数量、线程栈和 TLS 资源限制。

构建前先按模块根目录的 `README.md <../../README.md>`_ 准备 libc++ 源码、
Zephyr 工作区和工具链；该文档包含 GCC、Clang 的构建和运行命令，以及两种后端的边界。
