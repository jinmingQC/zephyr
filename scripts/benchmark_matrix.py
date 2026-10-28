#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Preview or run a paired libc++ thread/synchronization benchmark matrix.

Examples:
    python3 scripts/benchmark_matrix.py
    python3 scripts/benchmark_matrix.py --backend native --platform mps2/an385 --run
    python3 scripts/benchmark_matrix.py --build-only --run
    python3 scripts/benchmark_matrix.py --measure-only --run

Builds retain ELF, map, full configuration and logs outside the repository.
Reports contain commands, source hashes, selected configuration, sizes and raw
timing samples. Default measurements alternate native/POSIX, POSIX/native,
native/POSIX; run with no other QEMU instance or compilation active.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import time

from parse_benchmark import parse_log

MODULE = Path(__file__).resolve().parents[1]
HOME = MODULE.parent
PLATFORMS = {
    "mps2/an385": ("mps2/an385", "arm", False),
    "qemu_riscv32": ("qemu_riscv32", "rv32-up", False),
    "qemu_riscv32_smp2": ("qemu_riscv32", "rv32-smp2", True),
}
SHARED = (
    "CMakeLists.txt",
    "benchmarks/thread_sync/CMakeLists.txt", "benchmarks/thread_sync/prj.conf",
    "benchmarks/thread_sync/src/main.cpp", "benchmarks/thread_sync/testcase.yaml",
    "benchmarks/minimal/CMakeLists.txt", "benchmarks/minimal/prj.conf",
    "benchmarks/minimal/src/main.cpp", "benchmarks/minimal/testcase.yaml",
    "scripts/parse_benchmark.py", "scripts/benchmark_matrix.py",
)
CONFIG_KEYS = (
    "CPP", "STD_CPP17", "CPP_RUNTIME", "EXTERNAL_MODULE_LIBCPP", "PICOLIBC",
    "PICOLIBC_IO_LONG_LONG", "CPP_RUNTIME_NATIVE", "CPP_RUNTIME_POSIX",
    "CPP_EXCEPTIONS", "CPP_RTTI", "SPEED_OPTIMIZATIONS", "SIZE_OPTIMIZATIONS",
    "THREAD_LOCAL_STORAGE", "THREAD_MONITOR", "THREAD_STACK_INFO", "CPP_RUNTIME_THREAD_MAX", "CPP_RUNTIME_THREAD_STACK_SIZE",
    "CPP_RUNTIME_THREAD_PRIORITY", "CPP_RUNTIME_TLS_KEYS", "MAIN_THREAD_PRIORITY",
    "MAIN_STACK_SIZE", "SYSTEM_WORKQUEUE_STACK_SIZE", "COMMON_LIBC_MALLOC",
    "COMMON_LIBC_MALLOC_ARENA_SIZE", "HEAP_MEM_POOL_SIZE", "SMP", "MP_MAX_NUM_CPUS",
    "POSIX_THREADS", "POSIX_THREAD_THREADS_MAX", "POSIX_THREAD_KEYS_MAX",
    "MAX_PTHREAD_MUTEX_COUNT", "MAX_PTHREAD_COND_COUNT", "DYNAMIC_THREAD",
    "DYNAMIC_THREAD_POOL_SIZE", "DYNAMIC_THREAD_STACK_SIZE",
    "DYNAMIC_THREAD_ALLOC", "DYNAMIC_THREAD_PREFER_POOL", "DYNAMIC_THREAD_PREFER_ALLOC",
    "SYS_CLOCK_HW_CYCLES_PER_SEC", "SYS_CLOCK_TICKS_PER_SEC",
)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--backend", choices=("native", "posix", "all"), default="all")
    parser.add_argument("--toolchain", choices=("gcc", "llvm", "all"), default="all")
    parser.add_argument("--platform", choices=(*PLATFORMS, "all"), default="all")
    parser.add_argument("--run", action="store_true", help="execute instead of preview")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--build-only", action="store_true")
    actions.add_argument("--measure-only", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--zephyr-base", type=Path,
                        default=HOME / "zephyrproject/zephyr-cpp-module-kernel")
    parser.add_argument("--sdk", type=Path, default=HOME / "zephyr-sdk-1.0.1")
    parser.add_argument("--native-module", type=Path, default=HOME / "zephyr-cpp-runtime")
    parser.add_argument("--posix-module", type=Path, default=HOME / "zephyr-cpp-runtime-posix")
    parser.add_argument("--libcxx-source-dir", type=Path,
                        default=HOME / "libcxx-poc-deps/libcxx-18.1.8.src")
    parser.add_argument("--libcxxabi-source-dir", type=Path,
                        default=HOME / "libcxx-poc-deps/libcxxabi-18.1.8.src")
    parser.add_argument("--output", type=Path,
                        default=HOME / "cpp-runtime-comparison-build/benchmark")
    parser.add_argument("--report-dir", type=Path, default=MODULE / "docs/results/benchmark")
    options = parser.parse_args()
    if not 1 <= options.jobs <= 4 or options.rounds < 1 or options.timeout < 1:
        parser.error("jobs must be 1..4; rounds and timeout must be positive")
    for key, value in vars(options).items():
        if isinstance(value, Path):
            setattr(options, key, value.expanduser().absolute())
    return options


def selected(value, choices):
    return tuple(choices) if value == "all" else (value,)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def read_assignments(path, config=False):
    values = {}
    for line in path.read_text().splitlines():
        if config and line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"
        elif "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def module(options, backend):
    return getattr(options, f"{backend}_module")


def environment(options, compiler):
    env = dict(os.environ)
    for name in ("TOOLCHAIN_ROOT", "LLVM_TOOLCHAIN_PATH", "CROSS_COMPILE",
                 "EXTRA_ZEPHYR_MODULES", "ZEPHYR_EXTRA_MODULES"):
        env.pop(name, None)
    env.update(ZEPHYR_BASE=str(options.zephyr_base),
               ZEPHYR_SDK_INSTALL_DIR=str(options.sdk),
               ZEPHYR_TOOLCHAIN_VARIANT="zephyr/" + {"gcc": "gnu", "llvm": "llvm"}[compiler],
               PYTHONDONTWRITEBYTECODE="1")
    python_bin = options.zephyr_base.parent / ".venv/bin"
    if python_bin.is_dir():
        env["PATH"] = str(python_bin) + os.pathsep + env.get("PATH", "")
    return env


def build_directory(options, backend, compiler, platform, application):
    return options.output / f"{backend}-{compiler}-{PLATFORMS[platform][1]}-{application}"


def configure_command(options, backend, platform, application, output):
    board, _, smp = PLATFORMS[platform]
    source = module(options, backend)
    definitions = {
        "LIBCXX_SOURCE_DIR": options.libcxx_source_dir,
        "LIBCXXABI_SOURCE_DIR": options.libcxxabi_source_dir,
        "BUILD_VERSION": "cpp-runtime-comparison",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "BOARD": board, "EXTRA_ZEPHYR_MODULES": source,
    }
    if smp:
        definitions["EXTRA_CONF_FILE"] = source / "tests/threads/smp2.conf"
    return ["cmake", "-S", str(source / "benchmarks" / application), "-B", str(output),
            "-GNinja", *(f"-D{key}={value}" for key, value in definitions.items())]


def source_manifest(options, backends):
    hashes = {}
    for backend in backends:
        base = module(options, backend)
        hashes[backend] = {name: digest(base / name) for name in SHARED}
        if "native" in hashes and hashes[backend] != hashes["native"]:
            raise ValueError(f"{backend}: benchmark source/configuration differs from native")
    return hashes


def command_log(command, path, env, cwd):
    with path.open("a") as output:
        output.write("\nCOMMAND: " + shlex.join(command) + "\n")
        output.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {path}")


def audit_configuration(path, backend, smp):
    config = read_assignments(path, config=True)
    expected = {
        "CPP": "y", "STD_CPP17": "y", "CPP_RUNTIME": "y",
        "EXTERNAL_MODULE_LIBCPP": "y", "PICOLIBC": "y", "PICOLIBC_IO_LONG_LONG": "y",
        f"CPP_RUNTIME_{backend.upper()}": "y", "CPP_EXCEPTIONS": "n", "CPP_RTTI": "n",
        "SPEED_OPTIMIZATIONS": "y", "SIZE_OPTIMIZATIONS": "n",
        "CPP_RUNTIME_THREAD_MAX": "8", "CPP_RUNTIME_THREAD_STACK_SIZE": "4096",
        "CPP_RUNTIME_THREAD_PRIORITY": "5", "CPP_RUNTIME_TLS_KEYS": "8",
        "MAIN_THREAD_PRIORITY": "5", "MAIN_STACK_SIZE": "8192",
        "SYSTEM_WORKQUEUE_STACK_SIZE": "4096", "COMMON_LIBC_MALLOC": "y",
        "COMMON_LIBC_MALLOC_ARENA_SIZE": "32768", "HEAP_MEM_POOL_SIZE": "8192",
        "SMP": "y" if smp else "n", "MP_MAX_NUM_CPUS": "2" if smp else "1",
    }
    if backend == "posix":
        expected.update(POSIX_THREADS="y", POSIX_THREAD_THREADS_MAX="8",
                        POSIX_THREAD_KEYS_MAX="8", DYNAMIC_THREAD_POOL_SIZE="8",
                        DYNAMIC_THREAD_STACK_SIZE="4096", DYNAMIC_THREAD_PREFER_POOL="y",
                        DYNAMIC_THREAD_ALLOC="n")
    for key, value in expected.items():
        if config.get("CONFIG_" + key, "n") != value:
            raise ValueError(f"{path}: expected CONFIG_{key}={value}")
    return {"CONFIG_" + key: config.get("CONFIG_" + key, "n") for key in CONFIG_KEYS}


def image_record(options, backend, compiler, platform, application):
    folder = build_directory(options, backend, compiler, platform, application)
    config = folder / "zephyr/.config"
    elf = folder / "zephyr/zephyr.elf"
    size_tool = options.sdk / "llvm/bin/llvm-size"
    size = subprocess.check_output([str(size_tool), str(elf)], text=True)
    sections = subprocess.check_output([str(size_tool), "-A", str(elf)], text=True)
    (options.output / f"{folder.name}-sections.txt").write_text(sections)
    text, data, bss = map(int, size.splitlines()[1].split()[:3])
    cache = read_assignments(folder / "CMakeCache.txt")
    compile_commands = json.loads((folder / "compile_commands.json").read_text())
    source = str(module(options, backend) / "benchmarks" / application / "src/main.cpp")
    application_command = next(row["command"] for row in compile_commands if row["file"] == source)
    flags = shlex.split(application_command)
    optimization = [flag for flag in flags if flag.startswith("-O")]
    if not optimization or optimization[-1] != "-O2" or "-std=c++17" not in flags:
        raise ValueError(f"{folder}: application does not use O2/C++17")
    return {
        "backend": backend, "compiler": compiler, "platform": platform, "application": application,
        "build_directory": str(folder), "elf_sha256": digest(elf), "config_sha256": digest(config),
        "selected_config": audit_configuration(config, backend, PLATFORMS[platform][2]),
        "text_bytes": text, "data_bytes": data, "bss_bytes": bss,
        "rom_text_plus_data_bytes": text + data, "static_writable_ram_data_plus_bss_bytes": data + bss,
        "text_data_bss_total_bytes": text + data + bss,
        "sections": sections,
        "memory_region_build_report": [
            line for line in (options.output / f"{folder.name}-build.log").read_text().splitlines()
            if line.strip().startswith(("FLASH:", "RAM:", "IDT_LIST:"))
        ],
        "compiler_path": cache.get("CMAKE_CXX_COMPILER"),
        "application_compile_command": application_command,
        "compiler_version_output": subprocess.check_output(
            [cache["CMAKE_CXX_COMPILER"], "--version"], text=True),
    }


def build_image(options, backend, compiler, platform, application):
    folder = build_directory(options, backend, compiler, platform, application)
    configure = configure_command(options, backend, platform, application, folder)
    compile_command = ["cmake", "--build", str(folder), "--parallel", str(options.jobs)]
    env = environment(options, compiler)
    print("BUILD", folder.name, flush=True)
    command_log(configure, options.output / f"{folder.name}-configure.log", env, options.zephyr_base)
    command_log(compile_command, options.output / f"{folder.name}-build.log", env, options.zephyr_base)
    record = image_record(options, backend, compiler, platform, application)
    record.update(configure_command=configure, build_command=compile_command,
                  environment={name: env[name] for name in
                               ("ZEPHYR_BASE", "ZEPHYR_SDK_INSTALL_DIR", "ZEPHYR_TOOLCHAIN_VARIANT")})
    return record


def measure(options, backend, compiler, platform, round_number):
    folder = build_directory(options, backend, compiler, platform, "thread_sync")
    log = options.output / f"{backend}-{compiler}-{PLATFORMS[platform][1]}-round{round_number}.log"
    command = ["cmake", "--build", str(folder), "--target", "run"]
    print("MEASURE", log.name, flush=True)
    with log.open("w") as output:
        process = subprocess.Popen(command, env=environment(options, compiler),
                                   cwd=options.zephyr_base, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        deadline = time.monotonic() + options.timeout
        try:
            while time.monotonic() < deadline:
                time.sleep(0.1)
                contents = log.read_text(errors="replace")
                if ("CPP_BENCH_DONE" in contents or "BENCH_ERROR," in contents
                        or "CPP_BENCH_FAILED" in contents or process.poll() is not None):
                    break
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
    result = parse_log(log)
    expected = {
        "main_priority": 5, "worker_priority": 5, "cplusplus": 201703,
        "compiler": "gcc" if compiler == "gcc" else "clang",
        "smp": int(PLATFORMS[platform][2]), "thread_capacity": 8,
        "thread_stack_request": 4096,
    }
    for name, value in expected.items():
        if result["metadata"].get(name) != value:
            raise ValueError(f"{log}: unexpected runtime metadata {name}")
    result.update(backend=backend, compiler=compiler, platform=platform, round=round_number,
                  command=command, log_sha256=digest(log))
    return result


def main():
    options = arguments()
    backends = selected(options.backend, ("native", "posix"))
    compilers = selected(options.toolchain, ("gcc", "llvm"))
    platforms = selected(options.platform, PLATFORMS)
    if not options.run:
        print("Preview only; add --run to execute. Build concurrency:", options.jobs)
        for backend in backends:
            for compiler in compilers:
                for platform in platforms:
                    for application in ("thread_sync", "minimal"):
                        folder = build_directory(options, backend, compiler, platform, application)
                        print(f"{backend}/{compiler}/{platform}/{application}:",
                              shlex.join(configure_command(options, backend, platform,
                                                           application, folder)))
        print(f"Timing: {options.rounds} alternating backend rounds; "
              "each run has one warmup and nine raw batches per metric.")
        return
    options.output.mkdir(parents=True, exist_ok=True)
    initial_hashes = source_manifest(options, backends)
    runtime_hashes = {}
    for backend in backends:
        base = module(options, backend)
        files = [base / name for name in ("CMakeLists.txt", "Kconfig", "libcxx.ld",
                                         "zephyr/module.yml")]
        for directory in ("adapters", "backend", "include"):
            files.extend(path for path in (base / directory).rglob("*") if path.is_file())
        runtime_hashes[backend] = {
            str(path.relative_to(base)): digest(path) for path in sorted(files)
        }
    report = {
        "sdk_path": str(options.sdk), "zephyr_path": str(options.zephyr_base),
        "libcxx_source_path": str(options.libcxx_source_dir),
        "libcxxabi_source_path": str(options.libcxxabi_source_dir),
        "runtime_source_sha256": runtime_hashes,
        "libcxx_config_sha256": digest(options.libcxx_source_dir / "include/__config"),
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "source_sha256": initial_hashes, "rounds": options.rounds,
        "timer_interpretation": "QEMU platform-timer observations, not physical CPU cycles.",
        "ram_interpretation": "data+bss is static writable RAM. RV32 images also execute text from RAM; "
                              "use the linker memory-region report for residency/alignment. "
                              "Peak dynamic allocation/stack use is not measured.",
        "images": [], "runs": [],
    }
    for backend in backends:
        for compiler in compilers:
            for platform in platforms:
                for application in ("thread_sync", "minimal"):
                    if options.measure_only:
                        record = image_record(options, backend, compiler, platform, application)
                    else:
                        record = build_image(options, backend, compiler, platform, application)
                    report["images"].append(record)
                    write_json(options.output / "benchmark-report.json", report)
    if not options.build_only:
        for compiler in compilers:
            for platform in platforms:
                for round_number in range(1, options.rounds + 1):
                    order = backends if round_number % 2 else backends[::-1]
                    for backend in order:
                        report["runs"].append(measure(options, backend, compiler,
                                                      platform, round_number))
                        write_json(options.output / "benchmark-report.json", report)
    if source_manifest(options, backends) != initial_hashes:
        raise ValueError("Benchmark source/configuration changed while the matrix was running")
    write_json(options.report_dir / "benchmark-report.json", report)
    print("Report:", options.report_dir / "benchmark-report.json")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
