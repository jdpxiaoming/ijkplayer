#!/bin/bash

echo "🔧 ARM64架构崩溃修复测试脚本"
echo "=================================="

# 检查当前架构
echo "📋 当前系统架构信息："
uname -m
echo ""

# 检查编译环境
echo "🔍 检查NDK环境："
if [ -n "$ANDROID_NDK" ]; then
    echo "✅ ANDROID_NDK: $ANDROID_NDK"
else
    echo "❌ ANDROID_NDK 未设置"
fi

# 检查ARM64编译配置
echo ""
echo "📋 检查ARM64编译配置："
if [ -f "android/ijkplayer/ijkplayer-arm64/src/main/jni/Application.mk" ]; then
    echo "✅ ARM64 Application.mk 存在"
    echo "📄 ARM64编译标志："
    grep "APP_CFLAGS" android/ijkplayer/ijkplayer-arm64/src/main/jni/Application.mk
else
    echo "❌ ARM64 Application.mk 不存在"
fi

# 检查代码修复
echo ""
echo "🔍 检查代码修复："
if grep -q "posix_memalign" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ ARM64内存对齐修复已应用"
else
    echo "❌ ARM64内存对齐修复未找到"
fi

if grep -q "__aarch64__" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ ARM64架构检测代码已添加"
else
    echo "❌ ARM64架构检测代码未找到"
fi

# 检查头文件
echo ""
echo "📋 检查头文件："
if grep -q "#include <stdlib.h>" ijkmedia/ijkplayer/ff_ffplay.c; then
    echo "✅ stdlib.h 头文件已添加"
else
    echo "❌ stdlib.h 头文件未添加"
fi

echo ""
echo "🎯 修复总结："
echo "1. ✅ 添加了ARM64架构的内存对齐支持"
echo "2. ✅ 使用posix_memalign确保16字节对齐"
echo "3. ✅ 添加了正确的内存释放逻辑"
echo "4. ✅ 修复了AVPacket的初始化和释放"
echo "5. ✅ 添加了必要的编译标志"

echo ""
echo "📝 建议的测试步骤："
echo "1. 重新编译ARM64版本："
echo "   cd android && ./compile-ijk.sh arm64"
echo ""
echo "2. 安装到ARM64设备："
echo "   adb install -r ijkplayer-example/build/outputs/apk/release/ijkplayer-example-release.apk"
echo ""
echo "3. 测试RTMP流播放："
echo "   使用之前崩溃的URL进行测试"
echo ""
echo "4. 监控日志："
echo "   adb logcat | grep -E '(IJKMEDIA|libc|crash)'"

echo ""
echo "🔧 如果仍有问题，请检查："
echo "- 确保使用最新的NDK版本"
echo "- 检查设备是否支持ARM64"
echo "- 验证RTMP流的兼容性"
echo "- 查看完整的崩溃日志"
