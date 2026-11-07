#!/bin/bash
# Remote build setup v2: USTC mirror (has 1.98.1), survives IDE disconnects.
set -x
cd /mnt/workspace

# 1) source tarball from the release asset
rm -f seekdb-src.tar.gz
U=https://ghfast.top/https://github.com/Amonginin/seekdb/releases/download/comp-src-20261107/seekdb-src.tar.gz
curl -sS -L -o seekdb-src.tar.gz "$U" || exit 1
ls -la seekdb-src.tar.gz
tar tzf seekdb-src.tar.gz | head -3 || exit 1

# 2) rust 1.98.1 via USTC
R2=https://mirrors.ustc.edu.cn/rust-static
curl -sSf "$R2/rustup/dist/x86_64-unknown-linux-gnu/rustup-init" -o /tmp/rustup-init || exit 1
chmod +x /tmp/rustup-init
export RUSTUP_DIST_SERVER=$R2 RUSTUP_UPDATE_ROOT=$R2/rustup
/tmp/rustup-init -y --profile minimal --default-toolchain 1.98.1 || exit 1
$HOME/.cargo/bin/rustc --version

# 3) unpack source
rm -rf seekdb
tar xzf seekdb-src.tar.gz && ls seekdb | head -5

echo ALL_DONE
