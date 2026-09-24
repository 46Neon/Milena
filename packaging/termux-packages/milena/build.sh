#!/data/data/com.termux/files/usr/bin/bash
# Candidate recipe for termux/termux-packages.  The source archive and digest
# are the real GitHub v0.2.0 tag values; this is not an assertion of acceptance
# into the official repositories.

TERMUX_PKG_HOMEPAGE=https://github.com/46Neon/Milena
TERMUX_PKG_DESCRIPTION="Milena programming language for data analysis"
TERMUX_PKG_LICENSE="MIT, Apache-2.0, Public-Domain"
TERMUX_PKG_MAINTAINER="Milena contributors"
TERMUX_PKG_VERSION=0.2.0
TERMUX_PKG_SRCURL="https://github.com/46Neon/Milena/archive/refs/tags/v${TERMUX_PKG_VERSION}.tar.gz"
TERMUX_PKG_SHA256=56e189bbd1e89aa25a7e8588e0606f0ea42d3bf5f1086fcfa3442d632d571153
# Milena links only against the Android/Bionic system libc and libm.
TERMUX_PKG_DEPENDS=""
TERMUX_PKG_BUILD_IN_SRC=true

termux_step_make() {
    # build-package.sh supplies the Termux Clang/Bionic flags.  Keep the
    # canonical lexer -> parser -> AST -> semantic -> runtime -> MilenaTable
    # source list from Makefile; tests and experimental compiler/IR/VM sources
    # are not part of this package build.
    make TERMUX=1 \
        CC="${CC:-clang}" \
        CFLAGS="${CFLAGS:--std=c17 -Oz -ffunction-sections -fdata-sections -Iinclude}" \
        LDFLAGS="${LDFLAGS:--lm -Wl,--gc-sections}" \
        all
}

termux_step_make_install() {
    install -Dm755 milena "$TERMUX_PREFIX/bin/milena"
    install -Dm644 README.md "$TERMUX_PREFIX/share/doc/milena/README.md"
    install -Dm644 LICENSE "$TERMUX_PREFIX/share/licenses/milena/LICENSE"
    install -Dm644 third_party/nanoarrow/LICENSE.txt "$TERMUX_PREFIX/share/licenses/milena/nanoarrow-LICENSE.txt"
    install -Dm644 third_party/nanoarrow/NOTICE.txt "$TERMUX_PREFIX/share/licenses/milena/nanoarrow-NOTICE.txt"
    install -Dm644 third_party/nanoarrow/FLATCC-LICENSE.txt "$TERMUX_PREFIX/share/licenses/milena/flatcc-LICENSE.txt"
    install -Dm644 third_party/sqlite/README.md "$TERMUX_PREFIX/share/licenses/milena/sqlite-PROVENANCE-LICENSE.md"
}
