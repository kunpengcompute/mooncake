"""Shared compiler runner for CPU checks of production NoF code."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


def definition(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise AssertionError(f"Unclosed production definition: {marker}")
    return source[start : end + (source[end : end + 1] == ";")]


class CppTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = (
            os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
        )
        if not cls.compiler:
            raise unittest.SkipTest("C++20 compiler required; set CXX or pass --cxx")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = (
            str(Path(cls.compiler).resolve().parent)
            + os.pathsep
            + os.environ.get("PATH", "")
        )
        temporary = tempfile.TemporaryDirectory(prefix="mooncake-nof-test-")
        cls.addClassCleanup(temporary.cleanup)
        cls.directory = Path(temporary.name)

    @classmethod
    def build(cls, source, name="check", *, headers=None, sources=(), defines=()):
        for path, contents in (headers or {}).items():
            header = cls.directory / path
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(contents, encoding="utf-8")
        cpp = cls.directory / f"{name}.cpp"
        cpp.write_text(source, encoding="utf-8")
        executable = cls.directory / (name + (".exe" if os.name == "nt" else ""))
        result = subprocess.run(
            [
                cls.compiler,
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                "-O0",
                *defines,
                "-I",
                str(cls.directory),
                "-I",
                str(REPO / "mooncake-store/include"),
                *(str(REPO / path) for path in sources),
                str(cpp),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=cls.environment,
            timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return executable

    def run_case(self, executable, *args, expected=0):
        result = subprocess.run(
            [str(executable), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=self.environment,
            timeout=30,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", help="C++20 compiler executable")
    args, remaining = parser.parse_known_args()
    if args.cxx:
        os.environ["CXX"] = args.cxx
    unittest.main(argv=[__file__, *remaining])
