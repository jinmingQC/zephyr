#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Print or run the shared C++17 threading functional matrix.

The default is a preview: no CMake, compiler, QEMU, or output cleanup is run.
Add --run to execute. Each matrix cell runs Twister with two build jobs and
removes passing build artifacts while keeping reports, logs, and .config.

Examples:
    python3 scripts/validate.py
    python3 scripts/validate.py --backend native --toolchain gcc \
        --platform mps2/an385 --run
    python3 scripts/validate.py --run
"""

import argparse
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from datetime import datetime, timezone

MODULE = Path(__file__).resolve().parents[1]
HOME = MODULE.parent
PLATFORMS = {
    "mps2/an385": ("mps2/an385", "arm", False),
    "qemu_riscv32": ("qemu_riscv32", "riscv32-up", False),
    "qemu_riscv32_smp2": ("qemu_riscv32", "riscv32-smp2", True),
}
COMPILERS = {"gcc": "gnu", "llvm": "llvm"}
CLEAN_ENV = (
    "TOOLCHAIN_ROOT",
    "LLVM_TOOLCHAIN_PATH",
    "CROSS_COMPILE",
    "EXTRA_ZEPHYR_MODULES",
    "ZEPHYR_EXTRA_MODULES",
)
SCENARIO = "cpp.runtime.threads"


def arguments():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--backend", choices=("native", "posix", "all"), default="all")
    parser.add_argument("--toolchain", choices=("gcc", "llvm", "all"), default="all")
    parser.add_argument("--platform", choices=(*PLATFORMS, "all"), default="all")
    parser.add_argument(
        "--run", action="store_true", help="execute; otherwise only print commands"
    )
    parser.add_argument(
        "--fail-fast", action="store_true", help="stop after a failed matrix cell"
    )
    parser.add_argument(
        "--zephyr-base",
        type=Path,
        default=HOME / "zephyrproject/zephyr-cpp-module-kernel",
    )
    parser.add_argument("--sdk", type=Path, default=HOME / "zephyr-sdk-1.0.1")
    parser.add_argument(
        "--native-module", type=Path, default=HOME / "zephyr-cpp-runtime"
    )
    parser.add_argument(
        "--posix-module", type=Path, default=HOME / "zephyr-cpp-runtime-posix"
    )
    parser.add_argument(
        "--libcxx-source-dir",
        type=Path,
        default=HOME / "libcxx-poc-deps/libcxx-18.1.8.src",
    )
    parser.add_argument(
        "--libcxxabi-source-dir",
        type=Path,
        default=HOME / "libcxx-poc-deps/libcxxabi-18.1.8.src",
    )
    parser.add_argument(
        "--output", type=Path, default=HOME / "cpp-runtime-comparison-build/functional"
    )
    python = HOME / "zephyrproject/.venv/bin/python3"
    parser.add_argument(
        "--python",
        type=Path,
        default=python if python.exists() else Path(sys.executable),
    )
    options = parser.parse_args()
    for key, value in vars(options).items():
        if isinstance(value, Path):
            setattr(options, key, value.expanduser().absolute())
    return options


def select(value, choices):
    return tuple(choices) if value == "all" else (value,)


def read_cache(path):
    values = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def read_config(path):
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"
    return values


def preflight(options, backends):
    required = [
        options.zephyr_base / "scripts/twister",
        options.python,
        options.sdk / "cmake/zephyr/gnu/generic.cmake",
        options.sdk / "cmake/zephyr/llvm/generic.cmake",
        options.libcxx_source_dir / "include/__config",
        options.libcxxabi_source_dir / "src/cxa_thread_atexit.cpp",
    ]
    modules = {"native": options.native_module, "posix": options.posix_module}
    shared = ("src/main.cpp", "prj.conf", "CMakeLists.txt", "tests.yaml", "smp2.conf")
    for backend in backends:
        required.extend(modules[backend] / "tests/threads" / name for name in shared)
    for path in required:
        if not path.is_file():
            raise ValueError(f"Required file is missing: {path}")

    sources = {}
    for backend in backends:
        source = modules[backend] / "tests/threads/src/main.cpp"
        names = re.findall(
            r"ZTEST\s*\(\s*libcxx_threads\s*,\s*test_(\w+)\s*\)", source.read_text()
        )
        if len(names) != 22 or len(set(names)) != 22:
            raise ValueError(f"Expected the shared 22 functional cases: {source}")
        sources[backend] = names
    if len(backends) == 2:
        for name in shared:
            paths = [modules[backend] / "tests/threads" / name for backend in backends]
            if paths[0].read_bytes() != paths[1].read_bytes():
                raise ValueError(f"The backends do not use identical tests: {name}")
    return sources


def build_command(options, backend, toolchain, platform):
    board, label, smp = PLATFORMS[platform]
    module = getattr(options, f"{backend}_module")
    output = options.output / f"{backend}-{toolchain}-{label}"
    variant = f"zephyr/{COMPILERS[toolchain]}"
    env = {
        "ZEPHYR_BASE": str(options.zephyr_base),
        "ZEPHYR_SDK_INSTALL_DIR": str(options.sdk),
        "ZEPHYR_TOOLCHAIN_VARIANT": variant,
        "PYTHONDONTWRITEBYTECODE": "1",
        "PATH": str(options.python.parent) + os.pathsep + os.environ.get("PATH", ""),
    }
    # The slash variant is parsed by Zephyr into zephyr + compiler gnu/llvm.
    # In particular, bare ZEPHYR_TOOLCHAIN_VARIANT=llvm is not this SDK.
    command = [
        str(options.python),
        str(options.zephyr_base / "scripts/twister"),
        "-T",
        str(module / "tests/threads"),
        "-s",
        SCENARIO,
        "-p",
        board,
        "-j",
        "2",
        "-O",
        str(output),
        "--clobber-output",
        "--runtime-artifact-cleanup",
        "pass",
        "--inline-logs",
        "-x",
        f"EXTRA_ZEPHYR_MODULES={module}",
        "-x",
        f"LIBCXX_SOURCE_DIR={options.libcxx_source_dir}",
        "-x",
        f"LIBCXXABI_SOURCE_DIR={options.libcxxabi_source_dir}",
    ]
    if smp:
        command.extend(("-x", f"EXTRA_CONF_FILE={module / 'tests/threads/smp2.conf'}"))
    return env, command, output


def audit_result(output, options, backend, toolchain, platform, expected_cases):
    report = json.loads((output / "twister.json").read_text())
    suites = report.get("testsuites", [])
    if len(suites) != 1 or suites[0].get("name") != SCENARIO:
        raise ValueError("Expected exactly one shared functional suite")
    suite = suites[0]
    expected_variant = f"zephyr/{COMPILERS[toolchain]}"
    if suite.get("status") != "passed" or suite.get("toolchain") != expected_variant:
        raise ValueError("Suite failed, was filtered, or used the wrong compiler")
    expected = {f"{SCENARIO}.libcxx_threads.{case}" for case in expected_cases}
    cases = suite.get("testcases", [])
    if (
        {case.get("identifier") for case in cases} != expected
        or len(cases) != 22
        or any(case.get("status") != "passed" for case in cases)
    ):
        raise ValueError("All 22 shared cases must run and pass; skips do not count")

    configs = [path for path in output.rglob(".config") if path.parent.name == "zephyr"]
    if len(configs) != 1:
        raise ValueError("Expected one retained zephyr/.config for configuration audit")
    config = read_config(configs[0])
    expected_config = {
        "CONFIG_CPP_RUNTIME": "y",
        "CONFIG_EXTERNAL_MODULE_LIBCPP": "y",
        f"CONFIG_CPP_RUNTIME_{backend.upper()}": "y",
        "CONFIG_CPP_RUNTIME_THREAD_MAX": "8",
        "CONFIG_CPP_RUNTIME_THREAD_STACK_SIZE": "4096",
        "CONFIG_CPP_RUNTIME_THREAD_PRIORITY": "5",
        "CONFIG_CPP_RUNTIME_TLS_KEYS": "8",
        "CONFIG_MAIN_THREAD_PRIORITY": "5",
        "CONFIG_ZTEST_THREAD_PRIORITY": "5",
        "CONFIG_CPP_EXCEPTIONS": "n",
        "CONFIG_CPP_RTTI": "n",
        "CONFIG_SMP": "y" if PLATFORMS[platform][2] else "n",
        "CONFIG_MP_MAX_NUM_CPUS": "2" if PLATFORMS[platform][2] else "1",
    }
    for key, value in expected_config.items():
        if config.get(key, "n") != value:
            raise ValueError(f"Unexpected {key}: {config.get(key)!r}; expected {value}")
    cache = read_cache(configs[0].parent.parent / "CMakeCache.txt")
    if cache.get("TOOLCHAIN_VARIANT_COMPILER") != COMPILERS[toolchain]:
        raise ValueError("CMakeCache compiler differs from the matrix selection")
    if Path(cache.get("ZEPHYR_SDK_INSTALL_DIR", "")).resolve() != options.sdk.resolve():
        raise ValueError("CMakeCache used a different SDK")
    return {
        "passed_cases": 22,
        "report": str(output / "twister.json"),
        "config": str(configs[0]),
        "execution_time": suite.get("execution_time"),
        "build_time": suite.get("build_time"),
    }


def main():
    options = arguments()
    backends = select(options.backend, ("native", "posix"))
    toolchains = select(options.toolchain, COMPILERS)
    platforms = select(options.platform, PLATFORMS)
    expected = preflight(options, backends) if options.run else None
    records = []
    failed = False
    for backend, toolchain, platform in itertools.product(
        backends, toolchains, platforms
    ):
        env_updates, command, output = build_command(
            options, backend, toolchain, platform
        )
        unsets = [part for name in CLEAN_ENV for part in ("-u", name)]
        printable = [
            "env",
            *unsets,
            *(f"{key}={value}" for key, value in env_updates.items() if key != "PATH"),
        ]
        path_assignment = f'PATH={shlex.quote(str(options.python.parent))}:"$PATH"'
        shell_command = (
            f"{shlex.join(printable)} {path_assignment} {shlex.join(command)}"
        )
        print(f"\n# {backend} / {toolchain} / {platform}", flush=True)
        print(
            f"cd {shlex.quote(str(options.zephyr_base))} && {shell_command}", flush=True
        )
        if not options.run:
            continue
        env = os.environ.copy()
        for name in CLEAN_ENV:
            env.pop(name, None)
        env.update(env_updates)
        result = subprocess.run(command, env=env, cwd=options.zephyr_base, check=False)
        record = {
            "backend": backend,
            "toolchain": toolchain,
            "platform": platform,
            "command": command,
            "returncode": result.returncode,
            "recorded_utc": datetime.now(timezone.utc).isoformat(),
        }
        try:
            if result.returncode:
                raise ValueError(f"Twister exited with {result.returncode}")
            record.update(
                audit_result(
                    output, options, backend, toolchain, platform, expected[backend]
                )
            )
            record["status"] = "passed"
        except (ValueError, OSError, KeyError) as error:
            record.update(status="failed", reason=str(error))
            failed = True
            print(f"Validation failed: {error}", file=sys.stderr, flush=True)
        records.append(record)
        options.output.mkdir(parents=True, exist_ok=True)
        (options.output / "matrix-summary.json").write_text(
            json.dumps({"libcxx_release": "18.1.8", "results": records}, indent=2)
            + "\n"
        )
        if failed and options.fail_fast:
            break
    if not options.run:
        print(
            "\nPreview only. Add --run to build and execute; passing artifacts are cleaned."
        )
    return int(failed)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(f"Validation setup failed: {error}", file=sys.stderr)
        sys.exit(2)
