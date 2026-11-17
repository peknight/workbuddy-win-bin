#!/usr/bin/env bash
# 重建 winex11-xim-spot.so。
#
# 包里这份 winex11.so 来自 wine 11.22 源码，改了 dlls/winex11.drv/xim.c 两处：
#
#   wine-xim-query-caret.patch   组字时向应用发 WM_IME_REQUEST/IMR_QUERYCHARPOSITION
#                                问光标矩形，再交给 X11DRV_SetIMECompositionRect()。
#                                Chromium 138 只走这条路报位置，不再调
#                                ImmSetCompositionWindow。
#   wine-xim-spot-callbacks.patch 让 XIMPreeditCallbacks 风格的 XIC 也接受
#                                XNSpotLocation，而不再直接 return FALSE。
#
# 合起来 ibus 的候选窗才会跟着光标走，不再贴在焦点窗口左下角。原因和验证过程
# 见 PITFALLS.md 第 17 条。
#
# unix 库的 ABI 跟 wine 版本绑定，发行版 wine 升版后必须用对应版本重编，否则
# 启动器会按 wine --version 判定不匹配、退回原版库（输入法仍可用，只是候选框
# 位置不对）。
#
# 依赖：gcc、flex、bison，以及 libx11 / libxext 的头文件（configure 能认出来的
# X 扩展会一并编进去，缺哪个就自动关掉哪个，和发行版打包时的处理一致）。
# 用法：./build-winex11.sh [输出路径]，默认输出到当前目录。国内访问
# dl.winehq.org 慢时，可用 WINE_TARBALL / WINE_TARBALL_URL 指向镜像。
set -euo pipefail

readonly WINE_VERSION='11.22'
readonly WINE_TARBALL="${WINE_TARBALL:-wine-${WINE_VERSION}.tar.xz}"
readonly WINE_URL="${WINE_TARBALL_URL:-https://dl.winehq.org/wine/source/11.x/${WINE_TARBALL}}"
readonly HERE=$(cd "$(dirname "$0")" && pwd)
readonly OUTPUT=${1:-$PWD/winex11-xim-spot.so}

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
cd "$workdir"

[[ -f ${WINE_TARBALL} ]] || curl -fL -o "${WINE_TARBALL}" "${WINE_URL}"
tar xf "${WINE_TARBALL}"

cd "wine-${WINE_VERSION}"
patch -p1 <"${HERE}/wine-xim-spot-callbacks.patch"
patch -p1 <"${HERE}/wine-xim-query-caret.patch"

mkdir -p build
cd build
../configure --enable-archs=x86_64 --disable-tests

make -j"$(nproc)" dlls/winex11.drv/all

install -Dm755 dlls/winex11.drv/winex11.so "$OUTPUT"
strip -s "$OUTPUT"
printf 'built %s (wine-version %s)\n' "$OUTPUT" "$(./wine --version)"
