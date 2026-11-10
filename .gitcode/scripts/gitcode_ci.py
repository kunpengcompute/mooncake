#!/usr/bin/env python3
"""Run the repository checks used by the GitCode PR gate."""

from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TASKS = ("static", "python", "docs", "nof")
COMMON_TESTS = (
    "allocation_strategy_test",
    "segment_test",
    "master_admin_server_test",
)
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".cu", ".cuh"}
SHA = re.compile(r"[0-9a-fA-F]{40,64}\Z")


def run(command: list[str], *, capture: bool = False, env=None):
    print("+ " + shlex.join(command), flush=True)
    return subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        check=True,
        capture_output=capture,
        text=True,
        encoding="utf-8",
    )


def git(*args: str) -> str:
    return run(["git", *args], capture=True).stdout.strip()


def resolve_base(environment: dict[str, str], explicit: str | None) -> str:
    """Resolve to a merge-base commit so all tools check the same change set."""
    event = environment.get("CI_EVENT", "")
    if explicit:
        candidate = explicit
    elif event == "pull_request":
        candidate = environment.get("CI_PR_BASE_SHA", "")
        if not SHA.fullmatch(candidate):
            raise ValueError(
                "PR base SHA is missing or invalid; refusing an empty diff"
            )
    elif event == "push":
        candidate = environment.get("CI_BEFORE_SHA", "")
        if not SHA.fullmatch(candidate):
            raise ValueError("Push before SHA is missing or invalid")
        if not candidate.strip("0"):
            candidate = "origin/" + environment.get("CI_DEFAULT_BRANCH", "dev_poc1230")
    else:
        candidate = environment.get("CI_BASE_REF", "origin/dev_poc1230")

    # checkout(fetch-depth=0) normally provides these objects. Fetch an exact
    # missing PR/push SHA without constructing shell code from event contents.
    try:
        commit = git(
            "rev-parse", "--verify", "--end-of-options", candidate + "^{commit}"
        )
    except subprocess.CalledProcessError:
        if not SHA.fullmatch(candidate):
            raise ValueError(f"Base ref is unavailable: {candidate}") from None
        git("fetch", "--no-tags", "origin", candidate)
        commit = git(
            "rev-parse", "--verify", "--end-of-options", candidate + "^{commit}"
        )
    return git("merge-base", commit, "HEAD")


def changed_paths(base: str) -> list[str]:
    output = run(
        ["git", "diff", "--name-only", "--no-renames", "-z", f"{base}...HEAD"],
        capture=True,
    ).stdout
    return [path for path in output.split("\0") if path]


def selected(task: str, paths: list[str], *, manual: bool = False) -> bool:
    if manual:
        return True
    ci_changed = any(p.startswith((".gitcode/", "scripts/ci/")) for p in paths)
    native = ci_changed or any(
        p.startswith(("mooncake-", "extern/", "cmake/", "python/"))
        or p in {"CMakeLists.txt", "dependencies.sh"}
        for p in paths
    )
    if task == "nof":
        return native
    if task == "python":
        return native or any(p.startswith("scripts/") for p in paths)
    if task == "docs":
        return ci_changed or any(p.startswith("docs/") for p in paths)
    if task == "static":
        return bool(paths)
    raise ValueError(f"Unknown task: {task}")


def plan_path(task: str) -> Path:
    return ROOT / "build" / "gitcode-ci" / f"{task}.json"


def select_task(task: str, base_ref: str | None) -> None:
    base = resolve_base(dict(os.environ), base_ref)
    paths = changed_paths(base)
    enabled = selected(
        task, paths, manual=os.environ.get("CI_EVENT") == "workflow_dispatch"
    )
    plan = {
        "task": task,
        "base": base,
        "head": git("rev-parse", "HEAD"),
        "paths": paths,
        "run": enabled,
    }
    target = plan_path(task)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(plan, indent=2), flush=True)
    if output := os.environ.get("ATOMGIT_OUTPUT"):
        with open(output, "a", encoding="utf-8") as stream:
            stream.write(f"run={str(enabled).lower()}\n")


def is_owned(path: str) -> bool:
    return not path.startswith(("extern/", "FAST25-release/", "build/"))


def static_checks(plan: dict) -> None:
    paths = [p for p in plan["paths"] if is_owned(p) and (ROOT / p).is_file()]
    run(["git", "diff", "--check", f"{plan['base']}...HEAD"])
    if not paths:
        return
    for hook in (
        "trailing-whitespace",
        "end-of-file-fixer",
        "check-yaml",
        "check-merge-conflict",
        "check-added-large-files",
    ):
        run(["pre-commit", "run", hook, "--files", *paths])
    run(["typos", "--config", ".typos.toml", "--", *paths])
    cpp = [
        p
        for p in paths
        if Path(p).suffix in CPP_SUFFIXES
        and "cachelib_memory_allocator/" not in p
        and "/thirdparty/" not in p
    ]
    if cpp:
        run(
            [
                "bash",
                "scripts/code_format.sh",
                "--check",
                "--changed-lines",
                "--base",
                plan["base"],
                "--",
                *cpp,
            ]
        )
    python = [p for p in paths if p.endswith(".py")]
    if python:
        run(["ruff", "check", "--", *python])
        run(
            [
                sys.executable,
                ".gitcode/scripts/check_changed_python_format.py",
                plan["base"],
                *python,
            ]
        )
    cmake = [
        p for p in paths if Path(p).name == "CMakeLists.txt" or p.endswith(".cmake")
    ]
    if cmake:
        run(["cmake-format", "--check", *cmake])
    # Hooks may rewrite files in the ephemeral checkout, but CI never commits
    # those edits. A successful gate requires the checked-out files unchanged.
    run(["git", "diff", "--exit-code"])


def python_checks() -> None:
    # 本仓库的 wheel 测试依赖编译后的原生扩展，未构建 wheel 时无法导入，
    # 因此这里只做纯 Python 源码的语法与字节码编译检查。
    run(
        [
            sys.executable,
            "-m",
            "compileall",
            "-q",
            ".gitcode/scripts",
            "mooncake-wheel/mooncake",
        ]
    )


def native_checks() -> None:
    build = "build/gitcode-nof"
    configure = [
        "cmake",
        "-S",
        ".",
        "-B",
        build,
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_UNIT_TESTS=ON",
        "-DWITH_STORE=ON",
        "-DUSE_HTTP=ON",
        "-DUSE_CUDA=OFF",
        "-DUSE_TENT=OFF",
        "-DWITH_STORE_RUST=OFF",
        "-DWITH_STORE_RS=OFF",
        "-DUSE_ETCD=OFF",
        "-DSTORE_USE_ETCD=OFF",
        "-DENABLE_SCCACHE=OFF",
        "-DUSE_NOF=ON",
    ]
    # runner 访问不了 github.com，改用 gitcode_bootstrap.sh 预取的镜像源码。
    fetchcontent = ROOT / "build" / "gitcode-ci" / "fetchcontent"
    for name, directory in (
        ("GOOGLETEST", fetchcontent / "googletest"),
        ("YALANTINGLIBS", fetchcontent / "yalantinglibs"),
    ):
        if directory.is_dir():
            configure.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={directory}")
    run(configure)
    jobs = os.environ.get("CI_BUILD_JOBS", "4")
    if not jobs.isdecimal() or int(jobs) < 1:
        raise ValueError("CI_BUILD_JOBS must be a positive integer")
    run(["cmake", "--build", build, "--parallel", jobs])
    tests = [*COMMON_TESTS, "nof_heartbeat_test"]
    catalog = json.loads(
        run(["ctest", "--test-dir", build, "--show-only=json-v1"], capture=True).stdout
    )
    require_tests(tests, catalog)
    report = ROOT / build / "test-results" / "ctest.xml"
    report.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["LD_LIBRARY_PATH"] = (
        "/usr/local/lib:/usr/local/lib64:" + environment.get("LD_LIBRARY_PATH", "")
    )
    environment["MC_METADATA_SERVER"] = "http://127.0.0.1:8080/metadata"
    environment["DEFAULT_KV_LEASE_TTL"] = "500"
    run(
        [
            "ctest",
            "--test-dir",
            build,
            "--output-on-failure",
            "--no-tests=error",
            "--parallel",
            "1",
            "--timeout",
            "180",
            "--output-junit",
            str(report),
            "-R",
            "^(" + "|".join(map(re.escape, tests)) + ")$",
        ],
        env=environment,
    )


def docs_checks() -> int:
    from sphinx.application import Sphinx

    # Use an API override: Sphinx 7 cannot replace a dictionary with -D.
    # Keep the source conf.py and normal documentation builds unchanged.
    build = ROOT / "build" / "gitcode-docs"
    warnings = io.StringIO()
    app = Sphinx(
        srcdir=str(ROOT / "docs" / "source"),
        confdir=str(ROOT / "docs" / "source"),
        outdir=str(build / "html"),
        doctreedir=str(build / "doctrees"),
        buildername="html",
        confoverrides={"intersphinx_mapping": {}},
        warning=warnings,
        freshenv=True,
        warningiserror=False,
        keep_going=True,
    )
    app.build()
    if app.statuscode:
        return app.statuscode
    # 未收录到 toctree 的设计文档只影响导航，不阻塞构建；其余告警仍然失败。
    allowed = "document isn't included in any toctree"
    blocked = [
        line
        for line in warnings.getvalue().splitlines()
        if "WARNING" in line and allowed not in line
    ]
    if blocked:
        print("\n".join(blocked), file=sys.stderr)
        return 1
    return 0


def require_tests(expected: list[str], catalog: dict) -> None:
    found = {test["name"] for test in catalog.get("tests", [])}
    if missing := set(expected) - found:
        raise ValueError(
            "Required CTest tests are missing: " + ", ".join(sorted(missing))
        )


def check_gate(results: dict) -> None:
    failed = [
        name for name in TASKS if results.get(name, {}).get("result") != "success"
    ]
    if failed:
        raise ValueError(
            "CI Gate failed (failed, cancelled, skipped or missing): "
            + ", ".join(failed)
        )
    print("CI Gate passed: every required job succeeded")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("select", "run", "gate"))
    parser.add_argument("task", choices=TASKS, nargs="?")
    parser.add_argument("--base")
    args = parser.parse_args()
    try:
        if args.action == "gate":
            check_gate(json.loads(os.environ["CI_RESULTS_JSON"]))
            return 0
        if args.task is None:
            parser.error("select/run requires a task")
        if args.action == "select":
            select_task(args.task, args.base)
            return 0
        plan = json.loads(plan_path(args.task).read_text(encoding="utf-8"))
        if plan["head"] != git("rev-parse", "HEAD") or plan["task"] != args.task:
            raise ValueError("Checkout changed after selection; regenerate the plan")
        if not plan["run"]:
            print(f"{args.task}: no relevant changes")
        elif args.task == "static":
            static_checks(plan)
        elif args.task == "python":
            python_checks()
        elif args.task == "docs":
            return docs_checks()
        else:
            native_checks()
        return 0
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"CI error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
