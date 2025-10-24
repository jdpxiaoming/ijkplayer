#!/bin/bash

# 🔧 ARM64增强修复验证脚本
# 用于验证ARM64架构上的流解析问题是否已解决

set -e

echo "🔍 ARM64增强修复验证脚本"
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
echo "📝 检查增强修复状态..."
echo "------------------------"

# 检查是否添加了增强的浮点精度控制
if grep -q "fesetenv(FE_DFL_ENV)" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加增强的浮点环境控制"
else
    echo "❌ 未找到增强的浮点环境控制"
fi

# 检查是否添加了浮点异常清除
if grep -q "feclearexcept(FE_ALL_EXCEPT)" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加浮点异常清除"
else
    echo "❌ 未找到浮点异常清除"
fi

# 检查是否添加了更激进的流解析优化
if grep -q "probesize.*10000000" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加10MB探测大小配置"
else
    echo "❌ 未找到10MB探测大小配置"
fi

# 检查是否添加了增强的重试机制
if grep -q "retry_count.*max_retries" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加增强的重试机制"
else
    echo "❌ 未找到增强的重试机制"
fi

# 检查是否添加了流重新解析逻辑
if grep -q "ARM64架构：尝试强制重新解析流信息" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加流重新解析逻辑"
else
    echo "❌ 未找到流重新解析逻辑"
fi

# 检查是否添加了详细的错误日志
if grep -q "流验证失败" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ 已添加详细的错误日志"
else
    echo "❌ 未找到详细的错误日志"
fi

# 检查编译配置
echo ""
echo "🔧 检查编译配置..."
echo "------------------------"

# 检查ARM64的Application.mk
if [ -f "android/ijkplayer/ijkplayer-arm64/src/main/jni/Application.mk" ]; then
    echo "✅ 找到ARM64 Application.mk"
    
    # 检查是否包含必要的编译标志
    if grep -q "D_FILE_OFFSET_BITS=64" android/ijkplayer/ijkplayer-arm64/src/main/jni/Application.mk; then
        echo "✅ 已配置大文件支持标志"
    else
        echo "❌ 未配置大文件支持标志"
    fi
else
    echo "❌ 未找到ARM64 Application.mk"
fi

# 检查FFmpeg编译配置
if [ -f "android/contrib/tools/do-compile-ffmpeg.sh" ]; then
    echo "✅ 找到FFmpeg编译脚本"
    
    # 检查ARM64 FFmpeg配置
    if grep -q "arm64.*--arch=aarch64" android/contrib/tools/do-compile-ffmpeg.sh; then
        echo "✅ 已配置ARM64 FFmpeg编译"
    else
        echo "❌ 未配置ARM64 FFmpeg编译"
    fi
else
    echo "❌ 未找到FFmpeg编译脚本"
fi

# 功能测试建议
echo ""
echo "🧪 功能测试建议..."
echo "------------------------"

echo "1. 编译并安装修复后的APK到ARM64设备"
echo "2. 测试之前失败的H264+AAC流URL"
echo "3. 对比ARMv7a和ARM64的播放效果"
echo "4. 检查日志中的ARM64优化信息"

# 日志检查建议
echo ""
echo "📋 日志检查要点..."
echo "------------------------"

echo "✅ 应该看到的日志："
echo "  - '🔧 ARM64架构：启用高精度浮点运算和流解析优化'"
echo "  - '🌐 检测到网络流，应用优化配置'"
echo "  - '✅ 网络流优化配置已应用'"
echo "  - '🔍 尝试获取流信息 (第1次尝试)'"
echo "  - '🔄 网络流重试中... (1/3)'"
echo "  - '🔄 ARM64架构：尝试强制重新解析流信息...'"
echo "  - '✅ ARM64架构：流重新解析成功'"

echo ""
echo "❌ 不应该看到的日志："
echo "  - 'Unknown error 1004'"
echo "  - 'could not find codec parameters'"
echo "  - 'Failed to open file'"
echo "  - '❌ ARM64架构：流重新解析失败'"

# 性能对比建议
echo ""
echo "📊 性能对比建议..."
echo "------------------------"

echo "1. 记录ARM64和ARMv7a的流解析时间"
echo "2. 对比两种架构的内存使用情况"
echo "3. 测试不同网络条件下的稳定性"
echo "4. 验证浮点运算的一致性"
echo "5. 监控重试次数和成功率"

# 总结
echo ""
echo "🎯 增强修复总结..."
echo "================================"

if [ "$ARM64_SPECIFIC" = true ]; then
    echo "当前在ARM64架构上运行，建议："
    echo "1. 重新编译整个项目"
    echo "2. 测试网络流播放功能"
    echo "3. 对比修复前后的行为差异"
    echo "4. 监控详细的调试日志"
else
    echo "当前在非ARM64架构上运行，建议："
    echo "1. 在ARM64设备上测试"
    echo "2. 验证编译配置的正确性"
    echo "3. 确保所有架构相关的修复都已应用"
fi

echo ""
echo "🔍 验证完成！"
echo "如果仍有问题，请检查编译日志和运行时日志。"
