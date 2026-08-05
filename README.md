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


当前情况
本地 DRM：50 ms左右 （50~60ms）
板端采集到 RTSP write 返回：30～50 ms。
推流到Mediamxt的WebRTC：150ms左右（实际150~200ms）
MP4首尾音画漂移：视频帧设置30fps情况下，音频固定快一帧视频帧，运行30分钟延迟不持续增长。可通过调整pts修复


当前版本：功能优先

先实现：

所有Stage线程常驻
RecordRga/MPP/AAC暂时持续工作
Mp4RecordStage用状态机控制是否写文件
RtspPushStage用状态机控制是否建立连接

先证明：

预览不中断；
动态录像可反复开始、停止；
每次MP4都能正常播放；
每次都从IDR开始；
无死锁；
音视频同步稳定。
第二版：减少无效分发

增加：

VideoFrameTee encode输出开关
EncodedPacketTee MP4输出开关
EncodedPacketTee RTSP输出开关
AudioTee输出开关
第三版：降低空闲功耗

当录像和推流均关闭：

停止向RecordRga送帧
MPP不再编码
AAC不再编码
可选停止ALSA采集

线程仍然存在，但阻塞等待。

第四版：资源释放策略

长时间空闲后：

释放MPP context
释放RGA/MppBufferPool
关闭ALSA

重新开始录像时再初始化