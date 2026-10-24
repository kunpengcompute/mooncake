# NoF SPDK build selection

Mooncake reads the selected SPDK build's `pkg-config` dependency graph. This
supports the older `spdk_rdma` layout and the v26.01
`spdk_rdma_provider` / `spdk_rdma_utils` layout without copying a library list.
SPDK and DPDK archives are linked with their registration constructors retained.
ISA-L is linked only when the selected build declares that dependency.
NoF explicitly links `librdmacm` and `libibverbs`, even if an installed SPDK
`spdk_syslibs.pc` omits them. Their development libraries must be installed.
Nonstandard installations can use `MOONCAKE_SPDK_RDMACM_LIBRARY` and
`MOONCAKE_SPDK_IBVERBS_LIBRARY` to select matching library files.

## Build from an SPDK source directory

First build SPDK with RDMA enabled. Keep any other options required by your
platform when rerunning configure. Do not run Mooncake's
`dependencies.sh --with-spdk` to select an existing external checkout: that script
builds its own pinned `extern/spdk` checkout.

```bash
cd /opt/spdk/spdk
# Inspect the current configuration before changing it.
grep -E '^CONFIG_(RDMA|RDMA_PROV|DPDK_DIR|ISAL)' mk/config.mk
./configure --with-rdma
make -j16

cd /opt/mooncake_github/Mooncake
cmake -S . -B build-staging -DSPDK_ROOT_DIR=/opt/spdk/spdk
cmake --build build-staging -j16
```

The Mooncake command above updates an **existing** staging build and preserves
its other cached feature flags. For a new build, also pass the desired feature
options, for example:

```bash
cmake -S . -B build-staging \
  -DUSE_CUDA=OFF -DUSE_ASCEND=OFF -DUSE_ASCEND_DIRECT=ON \
  -DUSE_ASCEND_NDR=OFF -DUSE_ASCEND_STAGING=ON \
  -DUSE_NOF=ON -DUSE_SPDK_NPU_UB=OFF -DUSE_TENT=OFF \
  -DSPDK_ROOT_DIR=/opt/spdk/spdk
```

For parameter direct NoF, use `USE_ASCEND_NDR=ON`,
`USE_SPDK_NPU_UB=ON` and the corresponding runtime configuration.
`USE_ASCEND_DIRECT` is needed only when also using the Ascend P/D transport.
These examples assume the other Mooncake/CANN prerequisites are already met.

`SPDK_ROOT_DIR` selects `build/lib/pkgconfig` and the compiler/linker paths in
those files. It takes precedence over old cached `SPDK_INCLUDE_DIR`,
`DPDK_INCLUDE_DIR` and library-directory settings, so an old `/usr/local` header
path does not accidentally override the chosen source build. Configure checks
that RDMA was enabled, that dependent SPDK packages come from the same build,
that static SPDK/DPDK archives exist, and that required NVMe/translation APIs
are declared. Complete the full SPDK build: building just `lib/nvme` may leave
the generated module and system-library `.pc` files missing.

## Installed SPDK

With `SPDK_ROOT_DIR` empty, use either `SPDK_PKG_CONFIG_DIR` or the existing
`SPDK_LIB_DIR` option:

```bash
cmake -S . -B build-staging -DSPDK_ROOT_DIR= \
  -DSPDK_PKG_CONFIG_DIR=/opt/spdk-install/lib/pkgconfig
```

If neither is supplied, the default is `/usr/local/lib/pkgconfig`.
`DPDK_LIB_DIR/pkgconfig` and the caller's `PKG_CONFIG_PATH` can supply external
DPDK or system dependencies. All `spdk_*.pc` dependencies must still come from
the selected SPDK directory. Headers and archives must match the same CPU
architecture and SPDK build. There is no mandatory `libisal.a` path when the
selected SPDK disables ISA-L.

## Validation

```bash
python3 scripts/test_spdk_cmake.py
```

The regression test uses small fixture archives to check both layouts,
constructor retention, reconfiguration, and early rejection of incomplete or
mixed builds. It does not emulate SPDK I/O. Real NoF validation still requires
RDMA hardware, hugepages, and a target namespace.
