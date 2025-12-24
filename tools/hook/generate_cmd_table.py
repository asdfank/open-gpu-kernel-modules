#!/usr/bin/env python3
"""
从 NVIDIA 驱动 ctrl 头文件中提取命令码定义，生成 C 头文件对照表
支持的命令码格式：
  - #define NVxxxx_CTRL_CMD_xxx (0xHHHHHHHH)
  - #define NVxxxx_CTRL_CMD_xxx 0xHHHHHHHH
"""

import os
import re
import sys

# 定义 ctrl 头文件目录
CTRL_DIR = "/home/lyh/Desktop/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/ctrl"
OUTPUT_FILE = "/home/lyh/Desktop/open-gpu-kernel-modules/tools/hook/nv_ctrl_cmd_table.h"

# 应该跳过的后缀模式（它们不是真正的命令）
SKIP_SUFFIXES = [
    '_FLAGS_', '_MODE_', '_TYPE_', '_STATE_', '_STATUS_',
    '_OFFSET_', '_SIZE_', '_MAX_', '_MIN_', '_INDEX_',
    '_COUNT_', '_MASK_', '_VAL_', '_FMT_', '_PROTOCOL_',
    '_OWNER_', '_BW_', '_LEVEL_', '_ID_', '_CAPS_',
    '_TABLE_', '_NONE_', '_DEFAULT_', '_INVALID_',
    '_SHIFT_', '_WIDTH_', '_BASE_', '_LIMIT_', '_RANGE_',
    '_VERSION_', '_REV_', '_PARAM_', '_PARAMS_MESSAGE_ID'
]

def extract_commands(ctrl_dir):
    """从所有头文件中提取命令定义"""
    commands = []
    
    # 正则表达式匹配命令定义
    # 例如: #define NV2080_CTRL_CMD_GPU_GET_INFO (0x20800101)
    # 或者: #define NV2080_CTRL_CMD_GPU_GET_INFO 0x20800101U
    # 支持 finn 评估表达式: (0x20800101) /* finn: ... */
    pattern = re.compile(
        r'#define\s+(NV[A-Za-z0-9_]*_CTRL_CMD_[A-Z0-9_]+)\s+'
        r'\(?(0x[0-9a-fA-F]+)U?\)?(?:\s*/\*.*?\*/)?',
        re.DOTALL
    )
    
    # 预编译排除后缀检查
    skip_pattern = re.compile('|'.join(re.escape(s) for s in SKIP_SUFFIXES))
    
    for root, dirs, files in os.walk(ctrl_dir):
        for filename in files:
            if filename.endswith('.h'):
                filepath = os.path.join(root, filename)
                try:
                    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                        content = f.read()
                        matches = pattern.findall(content)
                        for name, value in matches:
                            # 跳过 MESSAGE_ID 定义
                            if 'MESSAGE_ID' in name:
                                continue
                            
                            # 解析值
                            try:
                                int_value = int(value, 16)
                            except ValueError:
                                continue
                            
                            # 跳过常量定义（值小于 0x1000 的通常不是真正的命令）
                            if int_value < 0x1000:
                                continue
                            
                            # 检查命令码格式：高16位应该是已知的类
                            class_id = (int_value >> 16) & 0xFFFF
                            # 如果 class_id 为0 且值较小，可能是 NV0000 类的命令
                            if class_id == 0 and int_value > 0x10000:
                                continue
                            
                            # 跳过包含特定后缀的常量
                            if skip_pattern.search(name):
                                continue
                            
                            commands.append((name, value))
                except Exception as e:
                    print(f"Error reading {filepath}: {e}", file=sys.stderr)
    
    # 按命令码排序
    commands.sort(key=lambda x: int(x[1], 16))
    return commands

def generate_header(commands, output_file):
    """生成 C 头文件 - 只更新命令表部分，保留已有的函数定义"""
    
    # 检查现有文件是否存在
    existing_content = None
    if os.path.exists(output_file):
        with open(output_file, 'r', encoding='utf-8') as f:
            existing_content = f.read()
    
    # 生成命令表条目
    entries = []
    seen = set()
    for name, value in commands:
        if value in seen:
            continue
        seen.add(value)
        entries.append(f'    {{ {value}, "{name}", NULL }},')
    
    # 如果已有文件存在，只更新命令表部分
    if existing_content:
        # 查找命令表的开始和结束位置
        start_marker = 'static const nv_ctrl_cmd_entry_t g_nvCtrlCmdTable[] = {'
        end_marker = '    { 0, NULL, NULL }  /* 结束标记 */'
        
        start_idx = existing_content.find(start_marker)
        end_idx = existing_content.find(end_marker)
        
        if start_idx != -1 and end_idx != -1:
            # 构建新的命令表
            new_table = start_marker + '\n' + '\n'.join(entries) + '\n' + end_marker
            
            # 替换旧的命令表
            new_content = existing_content[:start_idx] + new_table + existing_content[end_idx + len(end_marker):]
            
            with open(output_file, 'w', encoding='utf-8') as f:
                f.write(new_content)
            
            print(f"Updated {output_file} with {len(entries)} commands (preserved existing functions)")
            return
    
    # 如果没有现有文件，报错提示用户需要先有基础文件
    print(f"Error: No existing file found at {output_file}")
    print("Please ensure nv_ctrl_cmd_table.h exists with the proper structure.")
    sys.exit(1)

def main():
    print(f"Extracting commands from {CTRL_DIR}...")
    commands = extract_commands(CTRL_DIR)
    print(f"Found {len(commands)} command definitions")
    
    print(f"Generating {OUTPUT_FILE}...")
    generate_header(commands, OUTPUT_FILE)
    print("Done!")

if __name__ == "__main__":
    main()
