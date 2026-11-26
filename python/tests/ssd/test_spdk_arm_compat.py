import os
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.fixture
def bash():
    git_bash = Path("C:/Program Files/Git/bin/bash.exe")
    executable = (
        str(git_bash) if os.name == "nt" and git_bash.exists() else shutil.which("bash")
    )
    if executable is None:
        pytest.skip("Bash is required to test the SPDK compatibility block")
    return executable


def run_compat_block(bash, arch, flags, cpu="0x48 0x0 8 0xd06 0x0", cpu_exit=0):
    script = (Path(__file__).parents[3] / "dependencies.sh").read_text(encoding="utf-8")
    start = script.index("    # DPDK bundled with SPDK")
    end = script.index("    # Configure SPDK with RDMA support", start)
    # Execute only the compatibility block, with host inspection stubbed out.
    # Never configure, build, install, bind, or reset any real devices.
    fixture = r"""
uname() { printf '%s\n' "$TEST_ARCH"; }
python3() { printf '%s\n' "$TEST_CPU"; return "$TEST_CPU_EXIT"; }
check_success() { local status=$?; if [ "$status" -ne 0 ]; then exit "$status"; fi; }
"""
    env = os.environ.copy()
    env.update(
        TEST_ARCH=arch, TEST_CPU=cpu, TEST_CPU_EXIT=str(cpu_exit), DPDKBUILD_FLAGS=flags
    )
    return subprocess.run(
        [
            bash,
            "--noprofile",
            "--norc",
            "-c",
            fixture + script[start:end] + '\nprintf "RESULT=%s\\n" "$DPDKBUILD_FLAGS"',
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.mark.parametrize("arch", ["aarch64", "arm64"])
def test_hisilicon_d06_uses_generic_platform(bash, arch):
    result = run_compat_block(bash, arch, "")
    assert result.returncode == 0
    assert "RESULT=-Dplatform=generic" in result.stdout


@pytest.mark.parametrize(
    "flags", ["-Dplatform=arm", "-Dplatform arm", "--cross-file cross.ini"]
)
def test_user_platform_or_cross_compilation_options_are_preserved(bash, flags):
    result = run_compat_block(bash, "aarch64", flags, cpu_exit=99)
    assert result.returncode == 0
    assert f"RESULT={flags}\n" in result.stdout


def test_unrelated_build_options_are_preserved_when_generic_is_added(bash):
    result = run_compat_block(bash, "aarch64", "-Dexamples=all")
    assert result.returncode == 0
    assert "RESULT=-Dexamples=all -Dplatform=generic" in result.stdout


def test_other_arm_cpu_keeps_existing_flags(bash):
    result = run_compat_block(
        bash, "aarch64", "-Dexamples=all", cpu="0x41 0x0 8 0xd0c 0x0"
    )
    assert result.returncode == 0
    assert "RESULT=-Dexamples=all\n" in result.stdout


def test_x86_does_not_run_arm_cpu_detection(bash):
    result = run_compat_block(bash, "x86_64", "", cpu_exit=99)
    assert result.returncode == 0
    assert "RESULT=\n" in result.stdout


def test_failed_cpu_detection_stops_configuration(bash):
    result = run_compat_block(bash, "aarch64", "", cpu_exit=7)
    assert result.returncode == 7
    assert "RESULT=" not in result.stdout
