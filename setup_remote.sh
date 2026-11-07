#!/bin/bash
# Remote build setup for seekdb 2026-competition on ModelScope DSW-CPU.
# Everything runs detached from the IDE session, so disconnects are harmless.
set -x
cd /mnt/workspace

# 1) source tarball (release asset, via ghfast mirror)
rm -f seekdb-src.tar.gz
U=https://ghfast.top/https://github.com/Amonginin/seekdb/releases/download/comp-src-20261107/seekdb-src.tar.gz
curl -sS -L -o seekdb-src.tar.gz "$U" || exit 1
ls -la seekdb-src.tar.gz

# 2) rust toolchain via TUNA mirror
R2=https://mirrors.tuna.tsinghua.edu.cn/rustup
curl -sSf "$R2/rustup/dist/x86_64-unknown-linux-gnu/rustup-init" -o /tmp/rustup-init || exit 1
chmod +x /tmp/rustup-init
export RUSTUP_DIST_SERVER=$R2 RUSTUP_UPDATE_ROOT=$R2/rustup
/tmp/rustup-init -y --profile minimal --default-toolchain 1.98.1 || exit 1

# 3) unpack source
tar xzf seekdb-src.tar.gz && ls seekdb | head -5

echo ALL_DONE
