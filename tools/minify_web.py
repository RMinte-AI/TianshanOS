#!/usr/bin/env python3
"""
TianshanOS Web 资源优化脚本

对 web 目录下的 JS/CSS 文件进行就地压缩（minify）+ gzip 预压缩。
纯 Python 实现，不依赖 Node.js / terser / csso。

用法:
    python3 tools/minify_web.py <web_dir> [--gzip]
    
例如:
    python3 tools/minify_web.py components/ts_webui/web --gzip
"""

import os
import re
import sys
import glob
import gzip as gzip_module


def minify_js(source: str) -> str:
    """简易 JS 压缩：移除注释和多余空白，保留字符串内容"""
    result = []
    i = 0
    n = len(source)
    
    while i < n:
        # 字符串字面量（单引号/双引号/模板字符串）
        if source[i] in ('"', "'", '`'):
            quote = source[i]
            result.append(source[i])
            i += 1
            while i < n:
                if source[i] == '\\' and i + 1 < n:
                    result.append(source[i])
                    result.append(source[i + 1])
                    i += 2
                elif source[i] == quote:
                    result.append(source[i])
                    i += 1
                    break
                else:
                    result.append(source[i])
                    i += 1
        # 单行注释
        elif source[i] == '/' and i + 1 < n and source[i + 1] == '/':
            # 跳到行尾
            while i < n and source[i] != '\n':
                i += 1
        # 多行注释
        elif source[i] == '/' and i + 1 < n and source[i + 1] == '*':
            i += 2
            while i < n - 1:
                if source[i] == '*' and source[i + 1] == '/':
                    i += 2
                    break
                i += 1
            else:
                i = n
        # 正则表达式字面量（简化处理）
        elif source[i] == '/' and i > 0 and result and result[-1] in ('=', '(', ',', '!', '&', '|', '?', ':', ';', '{', '}', '[', '\n'):
            result.append(source[i])
            i += 1
            while i < n:
                if source[i] == '\\' and i + 1 < n:
                    result.append(source[i])
                    result.append(source[i + 1])
                    i += 2
                elif source[i] == '/':
                    result.append(source[i])
                    i += 1
                    # Regex flags
                    while i < n and source[i].isalpha():
                        result.append(source[i])
                        i += 1
                    break
                else:
                    result.append(source[i])
                    i += 1
        else:
            result.append(source[i])
            i += 1
    
    text = ''.join(result)
    
    # 合并多余空行为单个换行
    text = re.sub(r'\n\s*\n+', '\n', text)
    # 移除行首空白（保留至少一个空格在关键字之间）
    lines = []
    for line in text.split('\n'):
        stripped = line.strip()
        if stripped:
            lines.append(stripped)
    text = '\n'.join(lines)
    
    return text


def minify_css(source: str) -> str:
    """CSS 压缩：移除注释和多余空白"""
    # 移除注释
    result = re.sub(r'/\*.*?\*/', '', source, flags=re.DOTALL)
    # 移除多余空白
    result = re.sub(r'\s+', ' ', result)
    # 移除 { } ; : , 前后多余空格
    result = re.sub(r'\s*([{}:;,>~+])\s*', r'\1', result)
    # 恢复某些必要的空格（如 "and (" in media queries）
    result = re.sub(r'\band\(', 'and (', result)
    result = re.sub(r'\bnot\(', 'not (', result)
    # 移除末尾分号（在 } 前）
    result = result.replace(';}', '}')
    return result.strip()


def process_directory(web_dir: str) -> None:
    """处理 web 目录下所有 JS/CSS 文件"""
    if not os.path.isdir(web_dir):
        print(f"Error: {web_dir} is not a directory")
        sys.exit(1)
    
    total_saved = 0
    
    # 处理 JS 文件
    for filepath in glob.glob(os.path.join(web_dir, '**', '*.js'), recursive=True):
        # Preserve third-party release bytes; gzip still runs below.
        if os.path.relpath(filepath, web_dir).split(os.sep)[0] == 'vendor':
            continue
        original_size = os.path.getsize(filepath)
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
        
        minified = minify_js(content)
        
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(minified)
        
        new_size = os.path.getsize(filepath)
        saved = original_size - new_size
        total_saved += saved
        pct = (saved / original_size * 100) if original_size > 0 else 0
        rel_path = os.path.relpath(filepath, web_dir)
        print(f"  JS  {rel_path}: {original_size:,} -> {new_size:,} ({pct:.1f}% saved)")
    
    # 处理 CSS 文件
    for filepath in glob.glob(os.path.join(web_dir, '**', '*.css'), recursive=True):
        # Preserve third-party release bytes; gzip still runs below.
        if os.path.relpath(filepath, web_dir).split(os.sep)[0] == 'vendor':
            continue
        original_size = os.path.getsize(filepath)
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
        
        minified = minify_css(content)
        
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(minified)
        
        new_size = os.path.getsize(filepath)
        saved = original_size - new_size
        total_saved += saved
        pct = (saved / original_size * 100) if original_size > 0 else 0
        rel_path = os.path.relpath(filepath, web_dir)
        print(f"  CSS {rel_path}: {original_size:,} -> {new_size:,} ({pct:.1f}% saved)")
    
    print(f"\n  Total saved: {total_saved:,} bytes ({total_saved / 1024:.1f} KB)")


def gzip_directory(web_dir: str) -> None:
    """对 web 目录下所有 JS/CSS/HTML 文件生成 .gz 副本"""
    if not os.path.isdir(web_dir):
        print(f"Error: {web_dir} is not a directory")
        sys.exit(1)
    
    extensions = ('*.js', '*.css', '*.html')
    total_original = 0
    total_compressed = 0
    count = 0
    
    for ext in extensions:
        for filepath in glob.glob(os.path.join(web_dir, '**', ext), recursive=True):
            original_size = os.path.getsize(filepath)
            gz_path = filepath + '.gz'
            
            with open(filepath, 'rb') as f_in:
                with gzip_module.open(gz_path, 'wb', compresslevel=9) as f_out:
                    f_out.write(f_in.read())
            
            gz_size = os.path.getsize(gz_path)
            total_original += original_size
            total_compressed += gz_size
            count += 1
            
            pct = (1 - gz_size / original_size) * 100 if original_size > 0 else 0
            rel_path = os.path.relpath(filepath, web_dir)
            print(f"  GZ  {rel_path}: {original_size:,} -> {gz_size:,} ({pct:.1f}% smaller)")
    
    if total_original > 0:
        overall_pct = (1 - total_compressed / total_original) * 100
        print(f"\n  Gzipped {count} files: {total_original:,} -> {total_compressed:,} ({overall_pct:.1f}% overall)")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <web_dir> [--gzip]")
        sys.exit(1)
    
    web_dir = sys.argv[1]
    do_gzip = '--gzip' in sys.argv
    
    print(f"Minifying web resources in {web_dir}...")
    process_directory(web_dir)
    
    if do_gzip:
        print(f"\nGzipping web resources in {web_dir}...")
        gzip_directory(web_dir)
    
    print("Done.")
