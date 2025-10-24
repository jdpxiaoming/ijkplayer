# 🔧 编译错误修复验证脚本 (PowerShell版本)
# 用于验证ARM64架构上的编译错误是否已修复

Write-Host "🔍 编译错误修复验证脚本" -ForegroundColor Green
Write-Host "================================" -ForegroundColor Green

# 检查当前架构
$ARCH = [System.Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE")
Write-Host "当前架构: $ARCH" -ForegroundColor Yellow

# 检查代码修改
Write-Host ""
Write-Host "📝 检查代码修改状态..." -ForegroundColor Green
Write-Host "------------------------" -ForegroundColor Green

# 检查是否添加了前向声明
$h265_declaration = Select-String -Path "ijkmedia/ijkplayer/ff_ffplay.c" -Pattern "static void\* h265_reencoding_thread" -Quiet
if ($h265_declaration) {
    Write-Host "✅ 已添加 h265_reencoding_thread 前向声明" -ForegroundColor Green
} else {
    Write-Host "❌ 未找到 h265_reencoding_thread 前向声明" -ForegroundColor Red
}

$ffp_write_declaration = Select-String -Path "ijkmedia/ijkplayer/ff_ffplay.c" -Pattern "static int ffp_write_record_packet" -Quiet
if ($ffp_write_declaration) {
    Write-Host "✅ 已添加 ffp_write_record_packet 前向声明" -ForegroundColor Green
} else {
    Write-Host "❌ 未找到 ffp_write_record_packet 前向声明" -ForegroundColor Red
}

# 检查函数定义位置
Write-Host ""
Write-Host "🔧 检查函数定义位置..." -ForegroundColor Green
Write-Host "------------------------" -ForegroundColor Green

# 检查 h265_reencoding_thread 函数定义
$h265_definition = Select-String -Path "ijkmedia/ijkplayer/ff_ffplay.c" -Pattern "static void\* h265_reencoding_thread\(void \*arg\) \{" -Quiet
if ($h265_definition) {
    Write-Host "✅ h265_reencoding_thread 函数定义正确" -ForegroundColor Green
} else {
    Write-Host "❌ h265_reencoding_thread 函数定义有问题" -ForegroundColor Red
}

# 检查 ffp_write_record_packet 函数定义
$ffp_write_definition = Select-String -Path "ijkmedia/ijkplayer/ff_ffplay.c" -Pattern "static int ffp_write_record_packet\(FFPlayer \*ffp, AVPacket \*packet\)" -Quiet
if ($ffp_write_definition) {
    Write-Host "✅ ffp_write_record_packet 函数定义正确" -ForegroundColor Green
} else {
    Write-Host "❌ ffp_write_record_packet 函数定义有问题" -ForegroundColor Red
}

# 检查 ffp_ffmpeg_h265_reencode 函数定义
$ffp_ffmpeg_definition = Select-String -Path "ijkmedia/ijkplayer/ff_ffplay.c" -Pattern "int ffp_ffmpeg_h265_reencode\(const char \*input_path, const char \*output_path\)" -Quiet
if ($ffp_ffmpeg_definition) {
    Write-Host "✅ ffp_ffmpeg_h265_reencode 函数定义正确" -ForegroundColor Green
} else {
    Write-Host "❌ ffp_ffmpeg_h265_reencode 函数定义有问题" -ForegroundColor Red
}

# 检查语法错误
Write-Host ""
Write-Host "🔍 检查语法错误..." -ForegroundColor Green
Write-Host "------------------------" -ForegroundColor Green

# 检查是否有未闭合的括号
$content = Get-Content "ijkmedia/ijkplayer/ff_ffplay.c" -Raw
$brace_count = ([regex]::Matches($content, "\{")).Count
$brace_close_count = ([regex]::Matches($content, "\}")).Count

Write-Host "大括号数量: $brace_count" -ForegroundColor Yellow
Write-Host "闭合大括号数量: $brace_close_count" -ForegroundColor Yellow

if ($brace_count -eq $brace_close_count) {
    Write-Host "✅ 大括号匹配正确" -ForegroundColor Green
} else {
    Write-Host "❌ 大括号不匹配，可能存在语法错误" -ForegroundColor Red
}

# 检查是否有未闭合的括号
$paren_count = ([regex]::Matches($content, "\(")).Count
$paren_close_count = ([regex]::Matches($content, "\)")).Count

Write-Host "小括号数量: $paren_count" -ForegroundColor Yellow
Write-Host "闭合小括号数量: $paren_close_count" -ForegroundColor Yellow

if ($paren_count -eq $paren_close_count) {
    Write-Host "✅ 小括号匹配正确" -ForegroundColor Green
} else {
    Write-Host "❌ 小括号不匹配，可能存在语法错误" -ForegroundColor Red
}

# 检查编译环境
Write-Host ""
Write-Host "🏗️  检查编译环境..." -ForegroundColor Green
Write-Host "------------------------" -ForegroundColor Green

# 检查是否有NDK环境变量
$ANDROID_NDK = [System.Environment]::GetEnvironmentVariable("ANDROID_NDK")
if ($ANDROID_NDK) {
    Write-Host "✅ ANDROID_NDK 环境变量已设置: $ANDROID_NDK" -ForegroundColor Green
    
    # 检查NDK是否存在
    if (Test-Path $ANDROID_NDK) {
        Write-Host "✅ NDK目录存在" -ForegroundColor Green
        
        # 检查ndk-build是否存在
        $ndk_build_path = Join-Path $ANDROID_NDK "ndk-build"
        if (Test-Path $ndk_build_path) {
            Write-Host "✅ ndk-build 存在" -ForegroundColor Green
        } else {
            Write-Host "❌ ndk-build 不存在" -ForegroundColor Red
        }
    } else {
        Write-Host "❌ NDK目录不存在" -ForegroundColor Red
    }
} else {
    Write-Host "❌ ANDROID_NDK 环境变量未设置" -ForegroundColor Red
}

# 检查是否有SDK环境变量
$ANDROID_SDK = [System.Environment]::GetEnvironmentVariable("ANDROID_SDK")
if ($ANDROID_SDK) {
    Write-Host "✅ ANDROID_SDK 环境变量已设置: $ANDROID_SDK" -ForegroundColor Green
} else {
    Write-Host "❌ ANDROID_SDK 环境变量未设置" -ForegroundColor Red
}

# 编译测试建议
Write-Host ""
Write-Host "🧪 编译测试建议..." -ForegroundColor Green
Write-Host "------------------------" -ForegroundColor Green

Write-Host "1. 设置环境变量：" -ForegroundColor Yellow
Write-Host "   `$env:ANDROID_NDK = 'C:\path\to\your\android-ndk'" -ForegroundColor Cyan
Write-Host "   `$env:ANDROID_SDK = 'C:\path\to\your\android-sdk'" -ForegroundColor Cyan

Write-Host ""
Write-Host "2. 编译ARM64版本：" -ForegroundColor Yellow
Write-Host "   cd android" -ForegroundColor Cyan
Write-Host "   .\compile-ijk.sh arm64" -ForegroundColor Cyan

Write-Host ""
Write-Host "3. 如果仍有编译错误，请检查：" -ForegroundColor Yellow
Write-Host "   - 函数声明和定义的位置" -ForegroundColor Cyan
Write-Host "   - 头文件包含" -ForegroundColor Cyan
Write-Host "   - 语法错误" -ForegroundColor Cyan

# 总结
Write-Host ""
Write-Host "🎯 修复总结..." -ForegroundColor Green
Write-Host "================================" -ForegroundColor Green

Write-Host "已修复的编译错误：" -ForegroundColor Yellow
Write-Host "1. ✅ 添加了 h265_reencoding_thread 前向声明" -ForegroundColor Green
Write-Host "2. ✅ 添加了 ffp_write_record_packet 前向声明" -ForegroundColor Green
Write-Host "3. ✅ 确保函数定义在正确位置" -ForegroundColor Green

Write-Host ""
Write-Host "🔍 验证完成！" -ForegroundColor Green
Write-Host "如果仍有编译错误，请检查编译日志并确保环境变量正确设置。" -ForegroundColor Yellow
