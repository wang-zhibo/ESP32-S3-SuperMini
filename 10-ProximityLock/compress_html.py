#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HTML 压缩与 C++ PROGMEM Gzip 头文件生成工具
用法: python3 compress_html.py
"""

import gzip
import os
import re
import sys

def minify_html(content: str) -> str:
    """精简 HTML / CSS / JS，保留 JS 字符串与核心语法完整性"""
    # 移除 HTML 注释
    content = re.sub(r'<!--(?!\s*\[if[\s\S]+?<!\[endif\])[\s\S]*?-->', '', content)
    
    lines = content.splitlines()
    processed_lines = []
    
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        processed_lines.append(stripped)
    
    minified = '\n'.join(processed_lines)
    minified = re.sub(r'>\s+<', '><', minified)
    return minified

def process_directory(dir_path):
    source_path = os.path.join(dir_path, "web_html_source.html")
    if not os.path.exists(source_path):
        return False
        
    target_cpp = os.path.join(dir_path, "web_html.cpp")
    target_h = os.path.join(dir_path, "web_html.h")
    
    print(f"📖 正在读取源文件: {source_path}")
    with open(source_path, "r", encoding="utf-8") as f:
        raw_html = f.read()
    
    raw_size = len(raw_html.encode("utf-8"))
    minified_html = minify_html(raw_html)
    minified_bytes = minified_html.encode("utf-8")
    minified_size = len(minified_bytes)
    
    # Gzip level 9
    gz_data = gzip.compress(minified_bytes, compresslevel=9)
    gz_size = len(gz_data)
    ratio = (1.0 - (gz_size / raw_size)) * 100.0
    
    print(f"📊 压缩统计 ({dir_path}):")
    print(f"   • 原始体积: {raw_size / 1024:.2f} KB ({raw_size} 字节)")
    print(f"   • 精简体积: {minified_size / 1024:.2f} KB ({minified_size} 字节)")
    print(f"   • Gzip体积: {gz_size / 1024:.2f} KB ({gz_size} 字节)")
    print(f"   • 压缩比例: 节省 {ratio:.1f}% 空间")
    
    # 写入 web_html.h
    h_content = '''#pragma once
#include <Arduino.h>

extern const uint8_t htmlPageGz[] PROGMEM;
extern const size_t htmlPageGz_len;
'''
    with open(target_h, "w", encoding="utf-8") as f:
        f.write(h_content)
        
    # 写入 web_html.cpp
    out_lines = []
    out_lines.append('#include "web_html.h"\n')
    out_lines.append(f'// 预编译 Gzip 静态网页资源 (原始: {raw_size/1024:.1f}KB -> 精简Gzip: {gz_size/1024:.1f}KB, 节省 {ratio:.1f}%)')
    out_lines.append('const uint8_t htmlPageGz[] PROGMEM = {')
    
    for i in range(0, gz_size, 16):
        chunk = gz_data[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        if i + 16 < gz_size:
            out_lines.append(f'  {hex_str},')
        else:
            out_lines.append(f'  {hex_str}')
            
    out_lines.append('};')
    out_lines.append('const size_t htmlPageGz_len = sizeof(htmlPageGz);\n')
    
    with open(target_cpp, "w", encoding="utf-8") as f:
        f.write('\n'.join(out_lines))
        
    print(f"✅ 生成成功: {target_h} & {target_cpp}")
    return True

def main():
    cur_dir = os.path.dirname(os.path.abspath(__file__))
    dirs = [
        os.path.join(cur_dir, "ProximityLock"),
        os.path.join(cur_dir, "src"),
        cur_dir
    ]
    count = 0
    for d in dirs:
        if process_directory(d):
            count += 1
    if count == 0:
        print("未找到 web_html_source.html")
        sys.exit(1)

if __name__ == "__main__":
    main()
