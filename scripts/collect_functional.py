#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Archive the completed 12-cell functional matrix without rebuilding.

Run after scripts/validate.py has produced all reports. This reads existing
Twister reports and retained .config/CMakeCache files, verifies all 264 cases,
and writes identical compact evidence into both module branches. Compiler
--version and read-only Git commands are the only external commands executed.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import itertools
import json
from pathlib import Path
import re
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HOME = ROOT.parent
HELPERS = runpy.run_path(str(Path(__file__).with_name("validate.py")))
SCENARIO = HELPERS["SCENARIO"]
COMPILERS = HELPERS["COMPILERS"]
read_cache = HELPERS["read_cache"]
read_config = HELPERS["read_config"]
PLATFORMS = {
    "arm": "mps2/an385",
    "riscv32-up": "qemu_riscv32/qemu_virt_riscv32",
    "riscv32-smp2": "qemu_riscv32/qemu_virt_riscv32",
}
SHARED_TESTS = ("src/main.cpp", "prj.conf", "smp2.conf", "CMakeLists.txt", "tests.yaml")
SELECTED = (
    "ARCH BOARD_TARGET CPP STD_CPP17 STD_CPP_VERSION CPP_RUNTIME EXTERNAL_MODULE_LIBCPP "
    "PICOLIBC CPP_EXCEPTIONS CPP_RTTI THREAD_LOCAL_STORAGE CPP_RUNTIME_NATIVE "
    "CPP_RUNTIME_POSIX CPP_RUNTIME_THREAD_MAX CPP_RUNTIME_THREAD_STACK_SIZE "
    "CPP_RUNTIME_THREAD_PRIORITY CPP_RUNTIME_TLS_KEYS MAIN_STACK_SIZE MAIN_THREAD_PRIORITY "
    "ZTEST ZTEST_STACK_SIZE ZTEST_THREAD_PRIORITY SYSTEM_WORKQUEUE_STACK_SIZE "
    "COMMON_LIBC_MALLOC COMMON_LIBC_MALLOC_ARENA_SIZE HEAP_MEM_POOL_SIZE "
    "POSIX_SYSTEM_INTERFACES POSIX_TIMERS POSIX_MONOTONIC_CLOCK POSIX_THREADS "
    "POSIX_THREAD_THREADS_MAX POSIX_THREAD_KEYS_MAX DYNAMIC_THREAD "
    "DYNAMIC_THREAD_POOL_SIZE DYNAMIC_THREAD_STACK_SIZE DYNAMIC_THREAD_ALLOC "
    "MAX_PTHREAD_MUTEX_COUNT MAX_PTHREAD_COND_COUNT THREAD_STACK_INFO "
    "SMP MP_MAX_NUM_CPUS QEMU_ICOUNT NUM_PREEMPT_PRIORITIES "
    "SIZE_OPTIMIZATIONS SPEED_OPTIMIZATIONS ASSERT"
).split()
CACHE_SELECTED = (
    "BOARD CMAKE_C_COMPILER CMAKE_CXX_COMPILER ZEPHYR_TOOLCHAIN_VARIANT "
    "TOOLCHAIN_VARIANT_COMPILER ZEPHYR_SDK_INSTALL_DIR "
    "LIBCXX_SOURCE_DIR LIBCXXABI_SOURCE_DIR CMAKE_C_FLAGS CMAKE_CXX_FLAGS"
).split()
KERNEL_BASE = "ec98918869caeb2ddaef6e698f9ff36fb60a97ac"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(data):
    return (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode()


def command(*args):
    return subprocess.run(
        args, check=True, text=True, capture_output=True
    ).stdout.strip()


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-root", type=Path, default=HOME / "cpp-runtime-comparison-build"
    )
    parser.add_argument(
        "--native-module", type=Path, default=HOME / "zephyr-cpp-runtime"
    )
    parser.add_argument(
        "--posix-module", type=Path, default=HOME / "zephyr-cpp-runtime-posix"
    )
    parser.add_argument(
        "--zephyr-base",
        type=Path,
        default=HOME / "zephyrproject/zephyr-cpp-module-kernel",
    )
    options = parser.parse_args()
    for name, value in vars(options).items():
        setattr(options, name, value.expanduser().absolute())
    return options


def source_report(options, backend, compiler, label):
    build = options.build_root
    if backend == "posix":
        return build / "posix-functional" / f"posix-{compiler}-{label}/twister.json"
    if compiler == "llvm":
        return build / "native-llvm-functional" / f"native-llvm-{label}/twister.json"
    if label == "riscv32-smp2":
        return build / "native-gcc-smp-functional/native-gcc-riscv32-smp2/twister.json"
    return build / "native-gnu-up/twister.json"


def find_build(report_path, platform, backend, compiler, smp):
    found = []
    for config_path in report_path.parent.rglob(".config"):
        if config_path.parent.name != "zephyr":
            continue
        config = read_config(config_path)
        cache_path = config_path.parent.parent / "CMakeCache.txt"
        cache = read_cache(cache_path)
        if (
            config.get("CONFIG_BOARD_TARGET", "").strip('"') == platform
            and config.get("CONFIG_SMP", "n") == ("y" if smp else "n")
            and config.get(f"CONFIG_CPP_RUNTIME_{backend.upper()}") == "y"
            and cache.get("TOOLCHAIN_VARIANT_COMPILER") == COMPILERS[compiler]
        ):
            found.append((config_path, config, cache_path, cache))
    if len(found) != 1:
        raise ValueError(
            f"Expected one matching retained build for {report_path}: {len(found)}"
        )
    return found[0]


def check_configuration(config, backend, smp):
    expected = {
        "CPP_RUNTIME": "y",
        "EXTERNAL_MODULE_LIBCPP": "y",
        "STD_CPP17": "y",
        "CPP_RUNTIME_THREAD_MAX": "8",
        "CPP_RUNTIME_THREAD_STACK_SIZE": "4096",
        "CPP_RUNTIME_THREAD_PRIORITY": "5",
        "CPP_RUNTIME_TLS_KEYS": "8",
        "MAIN_THREAD_PRIORITY": "5",
        "ZTEST_THREAD_PRIORITY": "5",
        "CPP_EXCEPTIONS": "n",
        "CPP_RTTI": "n",
        "SMP": "y" if smp else "n",
        "MP_MAX_NUM_CPUS": "2" if smp else "1",
        "POSIX_THREADS": "y" if backend == "posix" else "n",
    }
    if smp:
        expected["QEMU_ICOUNT"] = "n"
    if backend == "posix":
        expected.update(
            POSIX_THREAD_THREADS_MAX="8",
            POSIX_THREAD_KEYS_MAX="8",
            DYNAMIC_THREAD_POOL_SIZE="8",
            DYNAMIC_THREAD_STACK_SIZE="4096",
            MAX_PTHREAD_MUTEX_COUNT="16",
            MAX_PTHREAD_COND_COUNT="16",
        )
    for key, value in expected.items():
        if config.get("CONFIG_" + key, "n") != value:
            raise ValueError(
                f"{backend}: unexpected CONFIG_{key}={config.get('CONFIG_' + key)}"
            )


def build_metadata(cache, compiler, banners):
    if cache.get("ZEPHYR_TOOLCHAIN_VARIANT") != "zephyr":
        raise ValueError("The comparison requires the Zephyr SDK toolchain")
    sdk = Path(cache["ZEPHYR_SDK_INSTALL_DIR"])
    executable = cache["CMAKE_CXX_COMPILER"]
    if not Path(executable).resolve().is_relative_to(sdk.resolve()):
        raise ValueError(f"Compiler is outside the recorded SDK: {executable}")
    if executable not in banners:
        banners[executable] = command(executable, "--version").splitlines()[0]
    return {
        "variant": f"zephyr/{COMPILERS[compiler]}",
        "c_compiler": cache["CMAKE_C_COMPILER"],
        "cxx_compiler": executable,
        "version_banner": banners[executable],
        "sdk": {"path": str(sdk), "version": (sdk / "sdk_version").read_text().strip()},
        "libcxx_source_dir": cache["LIBCXX_SOURCE_DIR"],
        "libcxxabi_source_dir": cache["LIBCXXABI_SOURCE_DIR"],
    }


def common_metadata(options):
    tests = {}
    for name in SHARED_TESTS:
        paths = [
            module / "tests/threads" / name
            for module in (options.native_module, options.posix_module)
        ]
        if paths[0].read_bytes() != paths[1].read_bytes():
            raise ValueError(
                f"Functional source/config differs between branches: {name}"
            )
        tests["tests/threads/" + name] = sha256(paths[0].read_bytes())
    cmakes = [
        module / "CMakeLists.txt"
        for module in (options.native_module, options.posix_module)
    ]
    if cmakes[0].read_bytes() != cmakes[1].read_bytes():
        raise ValueError("The final common CMake must be identical in both branches")
    validators = [
        module / "scripts/validate.py"
        for module in (options.native_module, options.posix_module)
    ]
    if validators[0].read_bytes() != validators[1].read_bytes():
        raise ValueError("Validation scripts differ between branches")
    return {
        "shared_test_sha256": tests,
        "validator_sha256": sha256(validators[0].read_bytes()),
        "final_common_cmake": {
            "path": "CMakeLists.txt",
            "sha256": sha256(cmakes[0].read_bytes()),
            "post_matrix_change": (
                "Final prerequisite fatal checks were added after validation. "
                "The checks verify one backend, C++17 or newer, and clock prerequisites; "
                "runtime sources and compiler flags did not change. No extra build/QEMU run "
                "was made for these checks-only changes."
            ),
        },
    }


def kernel_metadata(options):
    kernel = options.zephyr_base
    head = command("git", "-C", str(kernel), "rev-parse", "HEAD")
    if head != KERNEL_BASE:
        raise ValueError(
            f"The saved matrix belongs to kernel base {KERNEL_BASE}, not {head}"
        )
    patches = []
    for name in (
        "0001-posix-thread-lifetime.patch",
        "0002-posix-cxx-initializers.patch",
    ):
        path = options.posix_module / "patches/zephyr" / name
        command("git", "-C", str(kernel), "apply", "--reverse", "--check", str(path))
        patches.append(
            {
                "path_on_posix_branch": str(path.relative_to(options.posix_module)),
                "source_path": str(path),
                "sha256": sha256(path.read_bytes()),
                "required_by_backend": "posix",
                "reverse_apply_check_passed": True,
            }
        )
    modified = command("git", "-C", str(kernel), "diff", "--name-only").splitlines()
    return {
        "source_path": str(kernel),
        "base_commit": head,
        "prerequisite_patches": patches,
        "current_modified_file_sha256": {
            name: sha256((kernel / name).read_bytes()) for name in modified
        },
        "native_note": (
            "Native builds do not enable CONFIG_POSIX_THREADS. The two patches are POSIX "
            "prerequisites; the shared kernel worktree contains them, but they do not "
            "change the native backend's compiled pthread code."
        ),
    }


def collect(options):
    common = common_metadata(options)
    source = (options.native_module / "tests/threads/src/main.cpp").read_text()
    names = re.findall(r"ZTEST\s*\(\s*libcxx_threads\s*,\s*test_(\w+)\s*\)", source)
    expected_cases = {f"{SCENARIO}.libcxx_threads.{name}" for name in names}
    if len(expected_cases) != 22:
        raise ValueError("The shared source must define exactly 22 distinct cases")
    files = {}
    cells = []
    banners = {}
    for backend, compiler, label in itertools.product(
        ("native", "posix"), COMPILERS, PLATFORMS
    ):
        path = source_report(options, backend, compiler, label)
        raw_report = path.read_bytes()
        report = json.loads(raw_report)
        suites = [
            suite
            for suite in report["testsuites"]
            if suite.get("name") == SCENARIO
            and suite.get("platform") == PLATFORMS[label]
        ]
        if len(suites) != 1:
            raise ValueError(f"Cannot isolate one functional suite in {path}")
        suite = suites[0]
        cases = suite.get("testcases", [])
        if (
            suite.get("status") != "passed"
            or len(cases) != 22
            or {case["identifier"] for case in cases} != expected_cases
            or any(case.get("status") != "passed" for case in cases)
            or suite.get("toolchain") != f"zephyr/{COMPILERS[compiler]}"
        ):
            raise ValueError(f"Not all expected cases ran and passed: {path}")
        config_path, config, cache_path, cache = find_build(
            path, PLATFORMS[label], backend, compiler, label.endswith("smp2")
        )
        check_configuration(config, backend, label.endswith("smp2"))
        metadata = build_metadata(cache, compiler, banners)
        key = f"{backend}-{compiler}-{label}"
        report_name = f"reports/{key}.twister.json"
        config_name = f"configs/{key}.config.json"
        split = len(report["testsuites"]) != 1
        if split:
            report["testsuites"] = [suite]
        archived_report = json_bytes(report) if split else raw_report
        files[report_name] = archived_report
        selected_config = {
            "source_config": str(config_path),
            "source_config_sha256": sha256(config_path.read_bytes()),
            "source_cmake_cache": str(cache_path),
            "source_cmake_cache_sha256": sha256(cache_path.read_bytes()),
            "selected": {
                "CONFIG_" + name: config.get("CONFIG_" + name, "n") for name in SELECTED
            },
            "cache_selected": {name: cache.get(name) for name in CACHE_SELECTED},
        }
        files[config_name] = json_bytes(selected_config)
        cells.append(
            {
                "backend": backend,
                "compiler": compiler,
                "platform": label,
                "twister_platform": suite["platform"],
                "status": "passed",
                "passed_cases": 22,
                "report": report_name,
                "report_sha256": sha256(archived_report),
                "configuration": config_name,
                "configuration_sha256": sha256(files[config_name]),
                "source_report": str(path),
                "source_report_sha256": sha256(raw_report),
                "source_report_suite_count": 2 if split else 1,
                "report_extraction": (
                    "One of two suites selected; original environment and suite fields retained"
                    if split
                    else "Byte-identical copy of original single-suite report"
                ),
                "twister_retries": suite.get("retries"),
                "report_run_date": report.get("environment", {}).get("run_date"),
                "build": metadata,
            }
        )
    if len(cells) != 12 or sum(cell["passed_cases"] for cell in cells) != 264:
        raise ValueError(
            "The archive must contain 12 passed cells and 264 passed cases"
        )
    manifest = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "summary": {
            "cells": 12,
            "cases_per_cell": 22,
            "passed_cases": 264,
            "failed_cases": 0,
            "skipped_cases": 0,
        },
        "scope": "C++17 threading/synchronization; exceptions and RTTI disabled",
        "libcxx_release": "18.1.8",
        "modules": {
            "native": str(options.native_module),
            "posix": str(options.posix_module),
        },
        "collector": {
            "path": "scripts/collect_functional.py",
            "sha256": sha256(Path(__file__).read_bytes()),
        },
        **common,
        "kernel": kernel_metadata(options),
        "cells": cells,
        "notes": [
            "The SDK compilers are GCC14.3.0 and Clang19.1.7; internal LLVM16 was not tested.",
            "Reports and selected configuration extracts are archived; full .config files "
            "and binary/build artifacts are not committed.",
            "Original absolute source paths are retained for local provenance. "
            "Archive paths are relative to this index.json.",
            "The native GCC UP report contains retries from corrected initial configuration; "
            "original retries and run metadata have been preserved.",
        ],
    }
    files["index.json"] = json_bytes(manifest)
    for module in (options.native_module, options.posix_module):
        destination = module / "docs/results/functional"
        for name, data in files.items():
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        print(f"Wrote {len(files)} compact evidence files: {destination}")
    print(
        "Verified 12/12 cells and 264/264 cases passed; no build or QEMU run performed."
    )


if __name__ == "__main__":
    try:
        collect(arguments())
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Collection failed: {error}") from error
