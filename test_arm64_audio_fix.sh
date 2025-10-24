#!/bin/bash

# 🔧 ARM64音频解码修复验证脚本
# 用于验证ARM64架构上的音频解码崩溃问题是否已解决

set -e

echo "🔍 ARM64音频解码修复验证脚本"
echo "================================"

# 检查当前架构
ARCH=$(uname -m)
echo "当前架构: $ARCH"

# 检查是否为ARM64
if [[ "$ARCH" == "aarch64" || "$ARCH" == "arm64" ]]; then
    echo "✅ 检测到ARM64架构"
    ARM64_SPECIFIC=true
else
    echo "⚠️  非ARM64架构，部分测试可能不适用"
    ARM64_SPECIFIC=false
fi

# 检查代码修改
echo ""
echo "📝 检查音频解码修复状态..."
echo "------------------------"

# 检查是否添加了音频线程的ARM64修复
if grep -q "ARM64架构：音频解码线程启动" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加音频线程ARM64修复"
else
    echo "❌ 未找到音频线程ARM64修复"
fi

# 检查是否添加了AAC解码器配置
if grep -q "ARM64架构：检测到AAC编码器" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加AAC解码器兼容性修复"
else
    echo "❌ 未找到AAC解码器兼容性修复"
fi

# 检查是否添加了音频解码错误处理
if grep -q "ARM64架构：音频解码失败" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加音频解码错误处理"
else
    echo "❌ 未找到音频解码错误处理"
fi

# 检查是否添加了音频帧验证
if grep -q "ARM64架构：检测到无效音频帧数据" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加音频帧数据验证"
else
    echo "❌ 未找到音频帧数据验证"
fi

# 检查是否添加了音频解码器初始化优化
if grep -q "ARM64架构：初始化音频解码器" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加音频解码器初始化优化"
else
    echo "❌ 未找到音频解码器初始化优化"
fi

# 检查是否添加了内存对齐检查
if grep -q "音频帧数据未对齐" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加内存对齐检查"
else
    echo "❌ 未找到内存对齐检查"
fi

# 功能测试建议
echo ""
echo "🧪 功能测试建议..."
echo "------------------------"

echo "1. 编译并安装修复后的APK到ARM64设备"
echo "2. 测试之前崩溃的H264+AAC流URL"
echo "3. 监控音频解码相关的日志"
echo "4. 验证是否还有SIGABRT崩溃"

# 日志检查建议
echo ""
echo "📋 日志检查要点..."
echo "------------------------"

echo "✅ 应该看到的日志："
echo "  - '🔧 ARM64架构：音频解码线程启动，启用内存对齐和错误处理'"
echo "  - '🔧 ARM64架构：检测到AAC编码器，应用兼容性修复'"
echo "  - '🔧 ARM64架构：初始化音频解码器，应用兼容性配置'"
echo "  - '🔧 ARM64架构：AAC解码器配置完成'"

echo ""
echo "❌ 不应该看到的日志："
echo "  - 'Scudo ERROR: corrupted chunk header'"
echo "  - 'Prediction is not allowed in AAC-LC'"
echo "  - 'Fatal signal 6 (SIGABRT)'"
echo "  - '⚠️ ARM64架构：检测到无效音频帧数据'"
echo "  - '⚠️ ARM64架构：音频帧数据未对齐'"

# 性能对比建议
echo ""
echo "📊 性能对比建议..."
echo "------------------------"

echo "1. 记录音频解码的稳定性"
echo "2. 监控内存使用情况"
echo "3. 测试不同音频格式的兼容性"
echo "4. 验证音频质量是否正常"
echo "5. 检查音频同步是否准确"

# 调试建议
echo ""
echo "🔍 调试建议..."
echo "------------------------"

echo "1. 如果仍有崩溃，请提供完整的崩溃日志"
echo "2. 检查音频编码格式是否支持"
echo "3. 验证设备音频硬件兼容性"
echo "4. 测试不同的音频流源"

# 总结
echo ""
echo "🎯 音频解码修复总结..."
echo "================================"

if [ "$ARM64_SPECIFIC" = true ]; then
    echo "当前在ARM64架构上运行，建议："
    echo "1. 重新编译整个项目"
    echo "2. 测试音频解码功能"
    echo "3. 对比修复前后的稳定性"
    echo "4. 监控详细的调试日志"
else
    echo "当前在非ARM64架构上运行，建议："
    echo "1. 在ARM64设备上测试"
    echo "2. 验证编译配置的正确性"
    echo "3. 确保所有架构相关的修复都已应用"
fi

echo ""
echo "🔍 验证完成！"
echo "如果仍有音频解码问题，请检查崩溃日志并确保所有修复都已应用。"
