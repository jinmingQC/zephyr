#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Preview/run the native threading regression matrix; skipped cases fail audit."""
import argparse
from datetime import datetime, timezone
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

from validate import PLATFORMS, COMPILERS, CLEAN_ENV, read_cache, read_config

MODULE = Path(__file__).resolve().parents[1]
HOME = MODULE.parent
SUITES = {
    "threads": "cpp.runtime.threads",
    "thread_userspace": "cpp.runtime.thread_userspace",
    "thread_stress": "cpp.runtime.thread_stress",
}

def selected(value, choices):
    return tuple(choices) if value == "all" else (value,)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", action="store_true")
    p.add_argument("--suite", choices=(*SUITES, "all"), default="all")
    p.add_argument("--toolchain", choices=(*COMPILERS, "all"), default="all")
    p.add_argument("--platform", choices=(*PLATFORMS, "all"), default="all")
    p.add_argument("--zephyr-base", type=Path, default=HOME/"zephyrproject/zephyr-cpp-userspace-kernel")
    p.add_argument("--sdk", type=Path, default=HOME/"zephyr-sdk-1.0.1")
    p.add_argument("--output", type=Path, default=HOME/"cpp-runtime-userspace-build/matrix")
    p.add_argument("--python", type=Path, default=HOME/"zephyrproject/.venv/bin/python3")
    p.add_argument("--libcxx-source-dir", type=Path, default=HOME/"libcxx-poc-deps/libcxx-18.1.8.src")
    p.add_argument("--libcxxabi-source-dir", type=Path, default=HOME/"libcxx-poc-deps/libcxxabi-18.1.8.src")
    args = p.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().absolute())

    records = []
    failed = False
    for suite, compiler, platform in itertools.product(
            selected(args.suite, SUITES), selected(args.toolchain, COMPILERS),
            selected(args.platform, PLATFORMS)):
        board, label, smp = PLATFORMS[platform]
        source = MODULE/"tests"/suite
        output = args.output/f"{suite}-{compiler}-{label}"
        variant = f"zephyr/{COMPILERS[compiler]}"
        command = [str(args.python), str(args.zephyr_base/"scripts/twister"),
                   "-T", str(source), "-s", SUITES[suite], "-p", board,
                   "-j", "2", "-O", str(output), "--clobber-output",
                   "--runtime-artifact-cleanup", "pass", "--inline-logs",
                   "-x", f"EXTRA_ZEPHYR_MODULES={MODULE}",
                   "-x", f"LIBCXX_SOURCE_DIR={args.libcxx_source_dir}",
                   "-x", f"LIBCXXABI_SOURCE_DIR={args.libcxxabi_source_dir}"]
        if smp:
            command += ["-x", f"EXTRA_CONF_FILE={source/'smp2.conf'}"]
        env = os.environ.copy()
        for name in CLEAN_ENV:
            env.pop(name, None)
        env.update(ZEPHYR_BASE=str(args.zephyr_base), ZEPHYR_SDK_INSTALL_DIR=str(args.sdk),
                   ZEPHYR_TOOLCHAIN_VARIANT=variant, PYTHONDONTWRITEBYTECODE="1",
                   PATH=str(args.python.parent)+os.pathsep+env.get("PATH", ""))
        print(f"\n# {suite} / {compiler} / {platform}\n{shlex.join(command)}", flush=True)
        if not args.run:
            continue

        # Snapshot only inputs, not generated reports. A changed file during a
        # build makes that cell unauditable rather than silently changing proof.
        paths = [MODULE/"CMakeLists.txt", MODULE/"Kconfig"]
        adapter_sources = (["userspace_kernel.c", "userspace_threading.cpp"]
                           if suite == "thread_userspace" else ["native_threading.cpp"])
        paths += [MODULE/"adapters"/name for name in adapter_sources]
        paths += [MODULE/"scripts/fix_lld_app_partitions.py"]
        for directory in ("include", "backend", "zephyr", "cmake"):
            paths += [f for f in (MODULE/directory).rglob("*") if f.is_file()]
        paths += [f for f in source.rglob("*") if f.is_file()]
        hashes = {str(f.relative_to(MODULE)): hashlib.sha256(f.read_bytes()).hexdigest()
                  for f in sorted(paths)}
        cases = set()
        for f in (source/"src").glob("*.cpp"):
            for group, name in re.findall(
                    r"ZTEST(?:_USER)?\s*\(\s*(\w+)\s*,\s*test_(\w+)\s*\)",
                    f.read_text()):
                cases.add(f"{SUITES[suite]}.{group}.{name}")
        if not cases:
            raise ValueError(f"No tests found in {source}")
        result = subprocess.run(command, env=env, cwd=args.zephyr_base, check=False)
        record = dict(suite=suite, compiler=compiler, platform=platform,
                      command=command, returncode=result.returncode, inputs=hashes,
                      recorded_utc=datetime.now(timezone.utc).isoformat())
        try:
            if result.returncode:
                raise ValueError(f"Twister exited {result.returncode}")
            for name, digest in hashes.items():
                if hashlib.sha256((MODULE/name).read_bytes()).hexdigest() != digest:
                    raise ValueError(f"Build input changed during execution: {name}")
            report = json.loads((output/"twister.json").read_text())
            suites = report.get("testsuites", [])
            if len(suites) != 1 or suites[0].get("name") != SUITES[suite]:
                raise ValueError("Expected exactly one selected suite")
            run = suites[0]
            actual = run.get("testcases", [])
            if (run.get("status") != "passed" or run.get("toolchain") != variant or
                {t.get("identifier") for t in actual} != cases or len(actual) != len(cases) or
                any(t.get("status") != "passed" for t in actual)):
                raise ValueError("Wrong compiler, missing/filtered/skipped/failed tests")
            configs = [f for f in output.rglob(".config") if f.parent.name == "zephyr"]
            if len(configs) != 1:
                raise ValueError("Expected one retained configuration")
            config = read_config(configs[0])
            expected = {
                "CONFIG_CPP_RUNTIME_NATIVE": "y",
                "CONFIG_USERSPACE": "y" if suite == "thread_userspace" else "n",
                "CONFIG_TIMEOUT_64BIT": "y",
                "CONFIG_SMP": "y" if smp else "n",
                "CONFIG_MP_MAX_NUM_CPUS": "2" if smp else "1",
                "CONFIG_CPP_EXCEPTIONS": "n", "CONFIG_CPP_RTTI": "n",
                "CONFIG_POSIX_THREADS": "n",
            }
            for name, value in expected.items():
                if config.get(name, "n") != value:
                    raise ValueError(f"Unexpected {name}={config.get(name)}")
            cache = read_cache(configs[0].parent.parent/"CMakeCache.txt")
            if cache.get("TOOLCHAIN_VARIANT_COMPILER") != COMPILERS[compiler]:
                raise ValueError("CMake compiler mismatch")
            if Path(cache.get("ZEPHYR_SDK_INSTALL_DIR", "")).resolve() != args.sdk.resolve():
                raise ValueError("Unexpected SDK path")
            record.update(status="passed", passed=len(cases),
                          report=str(output/"twister.json"), config=str(configs[0]),
                          selected_config=expected)
        except (ValueError, OSError, KeyError) as error:
            failed = True
            record.update(status="failed", reason=str(error))
            print(f"Audit failed: {error}", flush=True)
        records.append(record)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output/"native-matrix-summary.json").write_text(
            json.dumps(dict(results=records), indent=2)+"\n")
        if failed:
            break
    if not args.run:
        print("\nPreview only; add --run. No builds or cleanup performed.")
    return int(failed)

if __name__ == "__main__":
    sys.exit(main())
