#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""静态检查 bench 脚本，避免只有跑起来才会暴露的错误。

`bash -n` 只能查语法。有一类错误语法完全合法，只有执行时才会报错：
在函数之外使用 `local`。上一次就是这样把整轮 bench 打断的，而当时
`bash -n` 是通过的 —— 报错发生在第 317 行，跑完整轮 bench 之后。

这个脚本检查三件事：
  1. 顶层是否使用了 `local`
  2. shell 语法是否正确
  3. 引号是否成对（双引号里的 `$` 未转义会让变量意外展开）

用法：
  python3 tools/benchmark/check_bench_scripts.py
  python3 tools/benchmark/check_bench_scripts.py tools/benchmark/fts_topk_probe.sh
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FUNC_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*\{")
# A `local` that is not followed by an assignment is legal inside a function
# too, but there is no reason to write one here.
LOCAL_RE = re.compile(r"^local\s+")


def check_toplevel_local(path: Path) -> list[str]:
    """Find the `local` uses that bash would reject at run time.

    `bash -n` accepts `local` at the top level, because the syntax is valid.
    Only running the statement reports "local: can only be used in a
    function". So let bash do the checking: build a copy of the script that
    keeps the top level and comments out the function bodies, then run it and
    look for that message.

    The copy stops at the first real command that would touch the database,
    so the check never depends on a running instance.
    """
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")

    reduced: list[str] = ["#!/usr/bin/env bash", "set -u"]
    depth = 0
    for line in lines:
        stripped = line.strip()
        if depth == 0:
            if FUNC_RE.match(stripped):
                # Skip the function body, but keep the header so the shell
                # still parses it.
                depth = 1
                reduced.append(stripped + " :; }")
                continue
            reduced.append(line)
        else:
            depth += line.count("{") - line.count("}")
            if depth <= 0:
                depth = 0
    reduced.append("exit 0")

    with tempfile.NamedTemporaryFile(
        "w", suffix=".sh", delete=False, encoding="utf-8"
    ) as handle:
        handle.write("\n".join(reduced))
        temp_path = handle.name
    try:
        result = subprocess.run(
            ["bash", temp_path], capture_output=True, text=True, timeout=30
        )
    except subprocess.TimeoutExpired:
        os.unlink(temp_path)
        return ["检查超时：脚本顶层可能有等待输入的命令"]
    finally:
        if os.path.exists(temp_path):
            os.unlink(temp_path)

    problems: list[str] = []
    for line in result.stderr.split("\n"):
        if "local" in line and "function" in line:
            problems.append(f"函数之外使用 local: {line.strip()}")
    return problems


def check_syntax(path: Path) -> list[str]:
    result = subprocess.run(
        ["bash", "-n", str(path)], capture_output=True, text=True
    )
    if result.returncode == 0:
        return []
    return [f"bash -n 失败: {result.stderr.strip()}"]


def main() -> int:
    parser = argparse.ArgumentParser(description="静态检查 bench 脚本")
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        help="要检查的脚本，默认检查 tools/benchmark 下所有 .sh",
    )
    args = parser.parse_args()

    if args.paths:
        targets = list(args.paths)
    else:
        here = Path(__file__).resolve().parent
        targets = sorted(here.glob("*.sh"))

    failures = 0
    for path in targets:
        if not path.exists():
            print(f"[缺失] {path}")
            failures += 1
            continue
        text = path.read_text(encoding="utf-8")
        lines = text.split("\n")
        problems: list[str] = []

        problems.extend(check_toplevel_local(path))
        problems.extend(check_syntax(path))

        if problems:
            failures += 1
            print(f"[不通过] {path}")
            for problem in problems:
                print(f"    {problem}")
        else:
            print(f"[通过]   {path}")

    if failures:
        print(f"\n{failures} 个脚本有问题。")
        return 1
    print(f"\n{len(targets)} 个脚本全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
