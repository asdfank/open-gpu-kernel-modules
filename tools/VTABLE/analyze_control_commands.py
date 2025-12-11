#!/usr/bin/env python3
"""
分析 NVIDIA 驱动中的控制命令导出表

用法:
    python3 analyze_control_commands.py Subdevice
    python3 analyze_control_commands.py Subdevice --pattern GSP
    python3 analyze_control_commands.py Subdevice --stats
    python3 analyze_control_commands.py Subdevice --method-id 0x20803601
"""

import re
import sys
import argparse
from pathlib import Path

def parse_export_table(nvoc_file):
    """解析 NVOC 导出表文件，提取所有命令信息"""
    commands = []
    
    with open(nvoc_file, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    # 匹配导出表条目
    # 格式: { /* [index] */ ... methodId=*/ 0x...u, ... func=*/ "function_name" }
    pattern = r'\{[^}]*?methodId[^}]*?0x([0-9a-fA-F]+)u[^}]*?(?:func[^}]*?"([^"]+)")?[^}]*?\}'
    
    matches = re.finditer(pattern, content, re.DOTALL)
    
    for match in matches:
        method_id = match.group(1)
        func_name = match.group(2) if match.group(2) else "unknown"
        
        # 提取 flags
        flags_match = re.search(r'flags[^}]*?0x([0-9a-fA-F]+)u', match.group(0))
        flags = flags_match.group(1) if flags_match else "0"
        
        # 提取 paramSize
        param_size_match = re.search(r'paramSize[^}]*?sizeof\(([^)]+)\)', match.group(0))
        param_size = param_size_match.group(1) if param_size_match else "unknown"
        
        commands.append({
            'methodId': method_id,
            'func': func_name,
            'flags': flags,
            'paramSize': param_size
        })
    
    return commands

def find_command_by_id(commands, method_id):
    """根据命令 ID 查找命令"""
    method_id_clean = method_id.lower().replace('0x', '')
    for cmd in commands:
        if cmd['methodId'].lower() == method_id_clean:
            return cmd
    return None

def print_commands(commands, pattern=None, method_id=None, stats=False):
    """打印命令列表"""
    if stats:
        print(f"\n统计信息:")
        print(f"  总命令数: {len(commands)}")
        
        # 按前缀分组统计
        prefixes = {}
        for cmd in commands:
            prefix = cmd['methodId'][:6]  # 前6位，例如 0x2080
            prefixes[prefix] = prefixes.get(prefix, 0) + 1
        
        print(f"\n按命令前缀分组:")
        for prefix, count in sorted(prefixes.items()):
            print(f"  {prefix}xxxx: {count} 个命令")
        
        return
    
    if method_id:
        cmd = find_command_by_id(commands, method_id)
        if cmd:
            print(f"\n找到命令:")
            print(f"  命令 ID: 0x{cmd['methodId']}")
            print(f"  函数名: {cmd['func']}")
            print(f"  标志位: 0x{cmd['flags']}")
            print(f"  参数类型: {cmd['paramSize']}")
        else:
            print(f"\n未找到命令 ID: {method_id}")
        return
    
    # 过滤命令
    filtered = commands
    if pattern:
        pl = pattern.lower()
        def match_cmd(cmd):
            if pl in cmd['func'].lower():
                return True
            if pl in cmd['methodId'].lower():
                return True
            if pl in cmd['paramSize'].lower():
                return True
            return False
        filtered = [cmd for cmd in commands if match_cmd(cmd)]
        
    # 打印
    print(f"\n找到 {len(filtered)} 个命令:\n")
    print(f"{'命令 ID':<12} {'函数名':<50} {'标志':<10} {'参数类型'}")
    print("-" * 100)
    
    for cmd in sorted(filtered, key=lambda x: int(x['methodId'], 16)):
        print(f"0x{cmd['methodId']:<10} {cmd['func']:<50} 0x{cmd['flags']:<8} {cmd['paramSize']}")

def main():
    parser = argparse.ArgumentParser(
        description='分析 NVIDIA 驱动中的控制命令导出表',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s Subdevice
  %(prog)s Subdevice --pattern GSP
  %(prog)s Subdevice --stats
  %(prog)s Subdevice --method-id 0x20803601
        """
    )
    
    parser.add_argument('class_name', help='类名 (如 Subdevice, Device)')
    parser.add_argument('--pattern', '-p', help='过滤函数名模式 (不区分大小写)')
    parser.add_argument('--method-id', '-m', help='查找特定命令 ID (如 0x20803601)')
    parser.add_argument('--stats', '-s', action='store_true', help='显示统计信息')
    
    args = parser.parse_args()
    
    # 构建文件路径
    class_name_lower = args.class_name.lower()
    nvoc_file = Path(f"/home/lwq/Desktop/open-gpu-kernel-modules/src/nvidia/generated/g_{class_name_lower}_nvoc.c")
    
    if not nvoc_file.exists():
        print(f"错误: 找不到文件 {nvoc_file}")
        print("\n可用的类名:")
        
        # 列出所有可用的类
        generated_dir = Path("/home/lwq/Desktop/open-gpu-kernel-modules/src/nvidia/generated")
        if generated_dir.exists():
            for f in sorted(generated_dir.glob("g_*_nvoc.c")):
                class_name = f.stem.replace("g_", "").replace("_nvoc", "")
                print(f"  {class_name}")
        sys.exit(1)
    
    print(f"解析文件: {nvoc_file}")
    commands = parse_export_table(nvoc_file)
    
    if not commands:
        print("警告: 未找到任何命令")
        sys.exit(1)
    
    print_commands(commands, args.pattern, args.method_id, args.stats)

if __name__ == '__main__':
    main()

