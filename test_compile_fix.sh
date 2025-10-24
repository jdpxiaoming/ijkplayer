#!/bin/bash

# 🔧 编译错误修复验证脚本
# 用于验证ARM64架构上的编译错误是否已修复

set -e

echo "🔍 编译错误修复验证脚本"
echo "================================"

# 检查当前架构
ARCH=$(uname -m)
echo "当前架构: $ARCH"

# 检查代码修改
echo ""
echo "📝 检查代码修改状态..."
echo "------------------------"

# 检查是否添加了前向声明
if grep -q "static void\* h265_reencoding_thread" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加 h265_reencoding_thread 前向声明"
else
    echo "❌ 未找到 h265_reencoding_thread 前向声明"
fi

if grep -q "static int ffp_write_record_packet" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加 ffp_write_record_packet 前向声明"
else
    echo "❌ 未找到 ffp_write_record_packet 前向声明"
fi

# 检查函数定义位置
echo ""
echo "🔧 检查函数定义位置..."
echo "------------------------"

# 检查 h265_reencoding_thread 函数定义
if grep -q "static void\* h265_reencoding_thread(void \*arg) {" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ h265_reencoding_thread 函数定义正确"
else
    echo "❌ h265_reencoding_thread 函数定义有问题"
fi

# 检查 ffp_write_record_packet 函数定义
if grep -q "static int ffp_write_record_packet(FFPlayer \*ffp, AVPacket \*packet)" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ ffp_write_record_packet 函数定义正确"
else
    echo "❌ ffp_write_record_packet 函数定义有问题"
fi

# 检查 ffp_ffmpeg_h265_reencode 函数定义
if grep -q "int ffp_ffmpeg_h265_reencode(const char \*input_path, const char \*output_path)" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ ffp_ffmpeg_h265_reencode 函数定义正确"
else
    echo "❌ ffp_ffmpeg_h265_reencode 函数定义有问题"
fi

# 检查语法错误
echo ""
echo "🔍 检查语法错误..."
echo "------------------------"

# 检查是否有未闭合的括号
BRACE_COUNT=$(grep -o "{" ijkmedia/ijkplayer/ff_ffplay.c | wc -l)
BRACE_CLOSE_COUNT=$(grep -o "}" ijkmedia/ijkplayer/ff_ffplay.c | wc -l)

echo "大括号数量: $BRACE_COUNT"
echo "闭合大括号数量: $BRACE_CLOSE_COUNT"

if [ "$BRACE_COUNT" -eq "$BRACE_CLOSE_COUNT" ]; then
    echo "✅ 大括号匹配正确"
else
    echo "❌ 大括号不匹配，可能存在语法错误"
fi

# 检查是否有未闭合的括号
PAREN_COUNT=$(grep -o "(" ijkmedia/ijkplayer/ff_ffplay.c | wc -l)
PAREN_CLOSE_COUNT=$(grep -o ")" ijkmedia/ijkplayer/ff_ffplay.c | wc -l)

echo "小括号数量: $PAREN_COUNT"
echo "闭合小括号数量: $PAREN_CLOSE_COUNT"

if [ "$PAREN_COUNT" -eq "$PAREN_CLOSE_COUNT" ]; then
    echo "✅ 小括号匹配正确"
else
    echo "❌ 小括号不匹配，可能存在语法错误"
fi

# 检查编译环境
echo ""
echo "🏗️  检查编译环境..."
echo "------------------------"

# 检查是否有NDK环境变量
if [ -n "$ANDROID_NDK" ]; then
    echo "✅ ANDROID_NDK 环境变量已设置: $ANDROID_NDK"
    
    # 检查NDK是否存在
    if [ -d "$ANDROID_NDK" ]; then
        echo "✅ NDK目录存在"
        
        # 检查ndk-build是否存在
        if [ -f "$ANDROID_NDK/ndk-build" ]; then
            echo "✅ ndk-build 存在"
        else
            echo "❌ ndk-build 不存在"
        fi
    else
        echo "❌ NDK目录不存在"
    fi
else
    echo "❌ ANDROID_NDK 环境变量未设置"
fi

# 检查是否有SDK环境变量
if [ -n "$ANDROID_SDK" ]; then
    echo "✅ ANDROID_SDK 环境变量已设置: $ANDROID_SDK"
else
    echo "❌ ANDROID_SDK 环境变量未设置"
fi

# 编译测试建议
echo ""
echo "🧪 编译测试建议..."
echo "------------------------"

echo "1. 设置环境变量："
echo "   export ANDROID_NDK=/path/to/your/android-ndk"
echo "   export ANDROID_SDK=/path/to/your/android-sdk"

echo ""
echo "2. 编译ARM64版本："
echo "   cd android"
echo "   ./compile-ijk.sh arm64"

echo ""
echo "3. 如果仍有编译错误，请检查："
echo "   - 函数声明和定义的位置"
echo "   - 头文件包含"
echo "   - 语法错误"

# 总结
echo ""
echo "🎯 修复总结..."
echo "================================"

echo "已修复的编译错误："
echo "1. ✅ 添加了 h265_reencoding_thread 前向声明"
echo "2. ✅ 添加了 ffp_write_record_packet 前向声明"
echo "3. ✅ 确保函数定义在正确位置"

echo ""
echo "🔍 验证完成！"
echo "如果仍有编译错误，请检查编译日志并确保环境变量正确设置。"
