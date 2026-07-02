# File: scripts/deploy_adb.ps1
# Windows PowerShell version of deploy_adb.sh

$ErrorActionPreference = "Stop"

# 当前脚本所在目录: Z:\scripts
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# 项目根目录: Z:\
$RootDir = Split-Path -Parent $ScriptDir

# 编译输出目录
$BuildDir = Join-Path $RootDir "build-rk3568"

# 需要部署的文件
$Files = @(
    @{
        Name = "rkcamd"
        LocalPath = Join-Path $BuildDir "apps\rkcamd\rkcamd"
        RemotePath = "/userdata/rkcam/bin/rkcamd"
    },
    @{
        Name = "camctl"
        LocalPath = Join-Path $BuildDir "apps\camctl\camctl"
        RemotePath = "/userdata/rkcam/bin/camctl"
    },
    @{
        Name = "v4l2_capture_test"
        LocalPath = Join-Path $BuildDir "apps\tools\v4l2_capture_test"
        RemotePath = "/userdata/rkcam/bin/v4l2_capture_test"
    },
    @{
        Name = "v4l2_dma_export_test"
        LocalPath = Join-Path $BuildDir "apps\tools\v4l2_dma_export_test"
        RemotePath = "/userdata/rkcam/bin/v4l2_dma_export_test"
    },
    @{
        Name = "rga_basic_test"
        LocalPath = Join-Path $BuildDir "apps\tools\rga_basic_test"
        RemotePath = "/userdata/rkcam/bin/rga_basic_test"
    },
    @{
        Name = "rga_process_test"
        LocalPath = Join-Path $BuildDir "apps\tools\rga_process_test"
        RemotePath = "/userdata/rkcam/bin/rga_process_test"
    },
    @{
        Name = "mpp_encoder_test"
        LocalPath = Join-Path $BuildDir "apps\tools\mpp_encoder_test"
        RemotePath = "/userdata/rkcam/bin/mpp_encoder_test"
    },
    @{
        Name = "mp4_mux_h264_test"
        LocalPath = Join-Path $BuildDir "apps\tools\mp4_mux_h264_test"
        RemotePath = "/userdata/rkcam/bin/mp4_mux_h264_test"
    },
    @{
        Name = "drm_info_test"
        LocalPath = Join-Path $BuildDir "apps\tools\drm_info_test"
        RemotePath = "/userdata/rkcam/bin/drm_info_test"
    },
    @{
        Name = "drm_color_test"
        LocalPath = Join-Path $BuildDir "apps\tools\drm_color_test"
        RemotePath = "/userdata/rkcam/bin/drm_color_test"
    }
)

Write-Host "Project root: $RootDir"
Write-Host "Build dir:    $BuildDir"
Write-Host ""

# 检查 adb 是否可用
$adb = Get-Command adb -ErrorAction SilentlyContinue
if (-not $adb) {
    Write-Error "adb not found. Please make sure adb is in PATH."
    exit 1
}

Write-Host "ADB:"
adb version
Write-Host ""


# 检查本地文件是否存在
foreach ($f in $Files) {
    if (-not (Test-Path $f.LocalPath)) {
        Write-Error "File not found: $($f.LocalPath)"
        exit 1
    }
}

# 创建板端目录
Write-Host "Creating remote directories..."
adb shell "mkdir -p /userdata/rkcam/bin /userdata/rkcam/configs"

# 推送文件
foreach ($f in $Files) {
    Write-Host "Pushing $($f.Name)..."
    Write-Host "  $($f.LocalPath) -> $($f.RemotePath)"
    adb push "$($f.LocalPath)" "$($f.RemotePath)"
}

# 设置可执行权限
Write-Host "Setting executable permissions..."
adb shell "chmod +x /userdata/rkcam/bin/*"

Write-Host ""
Write-Host "deploy done"