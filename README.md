# rkcam-pipeline

基于 RK3568 + IMX415 的模块化嵌入式视频 Pipeline 项目。

## 项目目标

本项目用于构建一个可扩展的嵌入式相机系统，逐步实现：

- V4L2 摄像头采集
- RGA 图像处理
- MPP H.264/H.265 编码
- RTSP 推流
- 本地录像
- 拍照
- 本地回放
- MIPI/HDMI 显示
- LVGL/Qt UI
- RKNN AI 检测

## 项目边界

本仓库只保存应用层代码和必要的 BSP patch。

不保存：

- Rockchip SDK
- rootfs
- kernel 完整源码
- u-boot 完整源码
- 交叉工具链
- sysroot
- 编译产物
- 大型测试视频
- 大型模型文件

## 推荐外部目录

```text
~/rk3568/
├── sdk/rk3568_linux_sdk/
├── toolchains/
├── sysroot/
├── workspace/rkcam-pipeline/
└── output/
