#!/usr/bin/env python3
from pathlib import Path
from analyze_control_commands import parse_export_table  # 直接复用

BASE = Path("/home/lwq/Desktop/open-gpu-kernel-modules")
generated_dir = BASE / "src/nvidia/generated"

for f in sorted(generated_dir.glob("g_*_nvoc.c")):
    cls = f.stem.replace("g_", "").replace("_nvoc", "")
    cmds = parse_export_table(f)
    if cmds:  # 只打印有 methodId 的类
        print(f"{cls:40s} {len(cmds):4d} commands")
