以后你改了设备树、IMX415 驱动、MIPI 屏幕驱动，不要提交整个 SDK，而是生成 patch：

```bash
cd ~/rk3568/sdk/rk3568_linux_sdk/kernel

git diff > ~/rk3568/workspace/rkcam-pipeline/bsp/patches/kernel/0001-enable-imx415.patch
