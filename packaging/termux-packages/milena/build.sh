#!/data/data/com.termux/files/usr/bin/bash
# Template for a future termux-packages pull request.
# This file is intentionally not an accepted package recipe: replace the
# checksum placeholder only after a real Milena Release has been built and
# independently verified on Termux/aarch64.

TERMUX_PKG_HOMEPAGE=https://github.com/46Neon/Milena
TERMUX_PKG_DESCRIPTION="Milena SST data analysis language"
TERMUX_PKG_LICENSE="MIT"
TERMUX_PKG_MAINTAINER="Milena contributors"
TERMUX_PKG_VERSION=0.1.1
TERMUX_PKG_SRCURL="https://github.com/46Neon/Milena/archive/refs/tags/v${TERMUX_PKG_VERSION}.tar.gz"
TERMUX_PKG_SHA256="@REPLACE_WITH_VERIFIED_RELEASE_SHA256@"
TERMUX_PKG_BUILD_IN_SRC=true

termux_step_make() {
    make CC=clang \
        CFLAGS="-std=c17 -Oz -ffunction-sections -fdata-sections -Iinclude" \
        LDFLAGS="-lm -Wl,--gc-sections"
}

termux_step_make_install() {
    install -Dm755 milena "$TERMUX_PREFIX/bin/milena"
    install -Dm644 README.md "$TERMUX_PREFIX/share/doc/milena/README.md"
}
