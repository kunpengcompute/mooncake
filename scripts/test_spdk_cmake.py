#!/usr/bin/env python3
"""Exercise SPDK discovery and real static linking without SPDK hardware.

The tiny fixture archives model both RDMA library layouts. They are not an
SPDK implementation and are never used for a Mooncake production build.
Run: python3 scripts/test_spdk_cmake.py
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(
        args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs
    )


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("cmake", "pkg-config", "cc", "ar")),
    "requires cmake, pkg-config, cc and ar",
)
class SpdkCmakeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mooncake-spdk-cmake-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project"
        self.project.mkdir()
        (self.project / "CMakeLists.txt").write_text(
            f"""cmake_minimum_required(VERSION 3.16)
project(spdk_fixture C CXX)
include(\"{REPO.as_posix()}/cmake/Spdk.cmake\")
mooncake_configure_spdk()
add_executable(probe main.c)
target_link_libraries(probe PRIVATE Mooncake::spdk)
"""
        )
        (self.project / "main.c").write_text(
            "int fixture_counter = 0;\n"
            "int main(void) { return fixture_counter == 1 ? 0 : 1; }\n"
        )

    def fixture(self, name, modern=True, require_rdma_symbols=False):
        root = self.root / name
        pc = root / "build/lib/pkgconfig"
        pc.mkdir(parents=True)
        (root / "mk").mkdir()
        (root / "mk/config.mk").write_text("CONFIG_RDMA?=y\n")
        include = root / "include/spdk"
        include.mkdir(parents=True)
        # Header-only declarations for the configure-time API shape check.
        (include / "env.h").write_text("#pragma once\n")
        (include / "dma.h").write_text(
            "#pragma once\nstruct spdk_memory_domain_rdma_ctx { void *ibv_pd; };\n"
            "void spdk_memory_domain_set_translation();\n"
        )
        (include / "nvme.h").write_text(
            "#pragma once\nstruct spdk_nvme_ctrlr_opts { char src_addr[256]; };\n"
            "struct spdk_nvme_ns_cmd_ext_io_opts { unsigned size; void *memory_domain; };\n"
            "void spdk_nvme_ns_cmd_readv_ext();\n"
            "void spdk_nvme_ns_cmd_writev_ext();\n"
            "void spdk_nvme_ctrlr_get_memory_domains();\n"
        )
        rdma = "spdk_rdma_provider" if modern else "spdk_rdma"
        packages = {
            "spdk_nvme": [rdma, "spdk_keyring"] if modern else [rdma],
            "spdk_env_dpdk": ["spdk_dpdklibs"],
            "spdk_sock_posix": [],
            "spdk_syslibs": [],
            "spdk_dpdklibs": [],
            rdma: ["spdk_rdma_utils"] if modern else [],
        }
        if modern:
            packages.update({"spdk_keyring": [], "spdk_rdma_utils": []})
        for package, deps in packages.items():
            library = "rte_eal" if package == "spdk_dpdklibs" else package
            libs = "" if package == "spdk_syslibs" else f"-l{library}"
            (pc / f"{package}.pc").write_text(
                f"Name: {package}\nDescription: test fixture\nVersion: 1.0\n"
                f"Requires: {' '.join(deps)}\n"
                f"Libs: -L{root}/build/lib {libs}\n"
                f"Cflags: -I{root}/include\n"
            )
            if libs:
                source = root / f"{library}.c"
                code = (
                    "extern int fixture_counter;\n"
                    "__attribute__((constructor)) static void register_transport(void) "
                    "{ ++fixture_counter; }\n"
                    if package == rdma
                    else f"void fixture_{library}(void) {{}}\n"
                )
                if package == rdma and require_rdma_symbols:
                    # Refer to actual RDMA symbols without calling them. This
                    # catches the installed .pc omission without RDMA hardware.
                    code += (
                        "#include <rdma/rdma_cma.h>\n"
                        "struct rdma_event_channel *(*fixture_cm)(void) = "
                        "rdma_create_event_channel;\n"
                        "struct ibv_device **(*fixture_verbs)(int *) = "
                        "ibv_get_device_list;\n"
                    )
                source.write_text(code)
                obj = source.with_suffix(".o")
                result = run("cc", "-fPIC", "-c", str(source), "-o", str(obj))
                self.assertEqual(result.returncode, 0, result.stdout)
                result = run(
                    "ar", "rcs", str(root / f"build/lib/lib{library}.a"), str(obj)
                )
                self.assertEqual(result.returncode, 0, result.stdout)
        return root

    def configure(self, root, build="build", *extra, env=None):
        result = run(
            "cmake",
            "-S",
            str(self.project),
            "-B",
            str(self.root / build),
            f"-DSPDK_ROOT_DIR={root}",
            *extra,
            env=env,
        )
        return result

    def build_and_run(self, build="build"):
        directory = self.root / build
        result = run("cmake", "--build", str(directory), "-j2")
        self.assertEqual(result.returncode, 0, result.stdout)
        result = run(str(directory / "probe"))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_legacy_and_modern_layout_link_transport_constructors(self):
        for modern in (False, True):
            with self.subTest(modern=modern):
                name = "modern" if modern else "legacy"
                root = self.fixture(name, modern)
                result = self.configure(root, name + "-build")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.build_and_run(name + "-build")

    def test_reconfigure_changes_selected_archives(self):
        first = self.fixture("first", False)
        second = self.fixture("second", True)
        for root in (first, second):
            result = self.configure(root)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.build_and_run()
        link = (self.root / "build/CMakeFiles/probe.dir/link.txt").read_text()
        self.assertIn(str(second / "build/lib/libspdk_rdma_provider.a"), link)
        self.assertNotIn(str(first / "build/lib"), link)

    def test_missing_rdma_system_flags_still_link(self):
        root = self.fixture("missing-rdma-flags", require_rdma_symbols=True)
        env = dict(os.environ, PKG_CONFIG_PATH=str(root / "build/lib/pkgconfig"))
        flags = run("pkg-config", "--libs", "--static", "spdk_syslibs", env=env)
        self.assertEqual(flags.returncode, 0, flags.stdout)
        self.assertNotIn("-lrdmacm", flags.stdout)
        self.assertNotIn("-libverbs", flags.stdout)
        result = self.configure(root)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.build_and_run()
        # Check shared linking too: otherwise undefined symbols can be hidden
        # until a consuming executable (such as mooncake_master) is linked.
        with (self.project / "CMakeLists.txt").open("a") as cmake:
            cmake.write(
                "add_library(probe_shared SHARED main.c)\n"
                "target_link_libraries(probe_shared PRIVATE Mooncake::spdk)\n"
                "target_link_options(probe_shared PRIVATE -Wl,--no-undefined)\n"
            )
        result = self.configure(root)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.build_and_run()

    def test_rejects_disabled_rdma(self):
        root = self.fixture("no-rdma")
        (root / "mk/config.mk").write_text("CONFIG_RDMA?=n\n")
        result = self.configure(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("configured without RDMA", " ".join(result.stdout.split()))

    def test_rejects_missing_archive_at_configure(self):
        root = self.fixture("missing-archive")
        (root / "build/lib/libspdk_rdma_utils.a").unlink()
        result = self.configure(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing libspdk_rdma_utils.a", result.stdout)

    def test_rejects_dependency_from_other_installation(self):
        root = self.fixture("selected")
        other = self.fixture("other")
        (root / "build/lib/pkgconfig/spdk_keyring.pc").unlink()
        env = dict(os.environ, PKG_CONFIG_PATH=str(other / "build/lib/pkgconfig"))
        result = self.configure(root, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mixed SPDK versions", result.stdout)

    def test_installed_prefix_selection(self):
        root = self.fixture("installed")
        result = self.configure(
            "", "installed-build", f"-DSPDK_LIB_DIR={root}/build/lib"
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.build_and_run("installed-build")


if __name__ == "__main__":
    unittest.main()
