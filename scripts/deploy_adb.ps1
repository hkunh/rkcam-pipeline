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
adb shell "chmod +x /userdata/rkcam/bin/rkcamd /userdata/rkcam/bin/camctl /userdata/rkcam/bin/v4l2_capture_test"

Write-Host ""
Write-Host "deploy done"