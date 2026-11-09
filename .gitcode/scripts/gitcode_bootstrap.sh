#!/usr/bin/env bash
# Install dependencies on dedicated openEuler 24.03 ARM64 CI environments.
set -euo pipefail

task=${1:?Usage: gitcode_bootstrap.sh static|python|docs|nof}
case "$task" in
  static|python|docs|nof) ;;
  *) echo "Unknown task: $task" >&2; exit 2 ;;
esac

# Runner labels alone do not guarantee the OS of its execution container.
source "${OS_RELEASE_FILE:-/etc/os-release}"
if [[ "${ID,,}" != openeuler || "${VERSION_ID:-}" != 24.03* ]]; then
  echo "Expected openEuler 24.03; found ${ID:-unknown} ${VERSION_ID:-unknown}" >&2
  exit 1
fi
if [[ "$(uname -m)" != aarch64 ]]; then
  echo "Expected aarch64 (ARM64); found $(uname -m)" >&2
  exit 1
fi
if [[ "${2:-}" = --check ]]; then
  echo "Environment verified: openEuler $VERSION_ID aarch64"
  exit 0
fi

privilege=()
if [[ "$(id -u)" != 0 ]]; then privilege=(sudo); fi
root=$(git rev-parse --show-toplevel)
cd "$root"

# 由 gitcode_ci.py 通过 FETCHCONTENT_SOURCE_DIR_* 指向这些目录。
prefetch() {
  local dest=$1 url=$2 ref=$3
  if [[ -e "${dest}/CMakeLists.txt" ]]; then return 0; fi
  rm -rf "$dest"
  mkdir -p "$dest"
  if git -C "$dest" init -q \
    && git -C "$dest" fetch -q --depth 1 "$url" "$ref" \
    && git -C "$dest" checkout -q FETCH_HEAD; then
    echo "已预取 ${url}@${ref} -> ${dest}"
  else
    rm -rf "$dest"
    echo "WARNING: 预取 ${url}@${ref} 失败，将回退到 CMake 默认下载" >&2
  fi
}
"${privilege[@]}" dnf install -y python3 python3-pip python3-devel \
  git curl gcc gcc-c++ make ninja-build
python3 -m venv build/gitcode-ci/venv
source build/gitcode-ci/venv/bin/activate
# 受限网络下统一走华为云 PyPI 镜像（与构建镜像保持一致），避免直连 pypi.org 超时。
export PIP_INDEX_URL=https://repo.huaweicloud.com/repository/pypi/simple
python -m pip install --upgrade pip
python -m pip install --disable-pip-version-check \
  pytest==8.3.5 PyYAML==6.0.2 aiohttp==3.11.18 packaging==24.2 pybind11==2.13.6

case "$task" in
  static)
    "${privilege[@]}" dnf install -y nodejs npm cargo rust openssl-devel pkgconf-pkg-config
    python -m pip install pre-commit==3.7.1 ruff==0.6.9 cmake-format==0.6.13 \
      clang-format==20.1.8 -i https://repo.huaweicloud.com/repository/pypi/simple
    # Pin the binary used by code_format.sh even if the host has another LLVM.
    ln -sf clang-format "$VIRTUAL_ENV/bin/clang-format-20"
    clang-format-20 --version
    command -v git-clang-format
    # Use a workspace-local install; do not overwrite runner-global tools.
    cargo install --locked --version 1.30.2 --root "$root/build/gitcode-tools" typos-cli
    ;;
  docs)
    python -m pip install -r .gitcode/dockerfiles/requirements-docs.txt \
      -i https://repo.huaweicloud.com/repository/pypi/simple
    ;;
  nof)
    "${privilege[@]}" bash dependencies.sh -y --with-spdk
    "${privilege[@]}" ldconfig
    # 版本号从工程内的 CMake 文件中读取，避免与源码侧依赖升级脱节。
    fetchcontent=build/gitcode-ci/fetchcontent
    if [[ -f mooncake-common/FindGTest.cmake ]]; then
      gtest_version=$(awk '/^set\(GOOGLETEST_VERSION /{gsub(/[()]/, ""); print $2; exit}' \
        mooncake-common/FindGTest.cmake)
      if [[ -n "$gtest_version" ]]; then
        prefetch "${fetchcontent}/googletest" \
          https://gitcode.com/GitHub_Trending/go/googletest.git "v${gtest_version}"
      fi
    fi
    if [[ -f mooncake-common/FindYLT.cmake ]]; then
      ylt_commit=$(awk '/^set\(YLT_COMMIT /{gsub(/[()]/, ""); print $2; exit}' \
        mooncake-common/FindYLT.cmake)
      if [[ -n "$ylt_commit" ]]; then
        prefetch "${fetchcontent}/yalantinglibs" \
          https://gitcode.com/gh_mirrors/ya/yalantinglibs.git "$ylt_commit"
      fi
    fi
    ;;
esac
