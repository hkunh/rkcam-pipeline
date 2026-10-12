# 板端 RKNN 流水线测速

`rknn_pipeline_bench` 使用 `manifest.json` 和 `build_manifest.json` 读取模型、张量和链路。
当前 baseline 是 `backbone_patch → keep_all`；模型数量不写死。
在板端 C++ 中常驻加载每个 `RknnEngine`，每轮第二段接收本轮第一段实际输出。
不使用 ADB 远程推理，不加载 ONNX Runtime，不接摄像头。

## 编译

在虚拟机相机工程根目录运行，使用已有 SDK 交叉编译环境：

```bash
source scripts/env_rk3568.sh
cmake -S . -B build-rk3568 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/aarch64-linux-gnu.cmake" \
  -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build-rk3568 --target rknn_pipeline_bench -j4
```

可执行文件位于 `build-rk3568/apps/tools/rknn_deploy/rknn_pipeline_bench`。
只编译这个 target，不需要编译现有其他测试程序。
通过现有拷贝方式或 ADB 将它放到板子 `/userdata/rkcam/bin/`。
已有工程 RPATH 为 `$ORIGIN/../lib`，因此 runtime 库应在 `/userdata/rkcam/lib/`。
工程当前选择 RKNN 2.3.2；实际加载的 API/driver 版本会写入报告。

## 板端运行

你已复制模型目录到 `/userdata/rkcam/rknn_deploy/model/baseline_final_v2`：

```bash
cd /userdata/rkcam
./bin/rknn_pipeline_bench
```

默认参数就是该模型目录、`rk3568_fp16_opt3`、manifest 的 reference 案例、预热 20 次、统计 200 次。
也可显式指定：

```bash
./bin/rknn_pipeline_bench \
  --run-dir /userdata/rkcam/rknn_deploy/model/baseline_final_v2 \
  --build-key rk3568_fp16_opt3 --case reference --warmup 20 --loops 200
```

程序需要 manifest、build_manifest、两个 RKNN、reference 外部输入和两种参考输出 NPY。
直接复制整个 run 最方便；ONNX 文件和训练 checkpoint 不参与 C++ 推理。
NPY 按头信息读取，当前支持小端 float32、C 连续布局。所有文件先校验 SHA256。
图像初始 NCHW→NHWC 转换在计时外；BTC/TBC 等非图像张量保留原轴顺序。

## 测量范围

1. 单独记录文件加载、模型初始化和首次完整推理。
2. 首次结果的每个阶段输出同时与 ONNX/PyTorch 参考比较；不通过就退出，不发布 FPS。
3. 预热完成后重复推理同一帧。计时过程中不打印、不保存输出、不运行参考对比。
4. 最后再次校验各阶段输出。计时包含当前 `RknnEngine::infer()` 的 SDK 输入处理、
   float32 输出转换、全部输出复制及释放。
5. 给出各阶段 infer、段间 bridge，以及直接测量的完整链路平均/P50/P95/min/max。

**这些是板端应用调用时间，不是纯 NPU 时间，也不是包含摄像头/裁剪/预对齐/归一化的跟踪 FPS。**
`equivalent_model_fps = 1000 / mean_pipeline_ms`，表示这套模型调用流程的等效吞吐率。
两个模型使用现有同步接口，不启用异步或详细性能收集。SDK 输出获取完成后才停止该段计时。

`keep_all` 保留全部 8 个输出并在本次模型接口中查找 `pred_boxes`；baseline 应对应索引 7。
第一段仅一个动态 token 输入传给第二段时，不额外复制段间 tensor；RknnEngine 内部的输入复制仍会计入时间。
如果未来模型有多路动态输入，绑定/转换/复制费用计入 bridge。

## 结果

默认生成一个独立目录，避免覆盖已有测量：

```text
baseline_final_v2/reports/board_cpp_benchmark/<timestamp>/
├── benchmark_report.json
├── timings.csv
└── validation/
    ├── first/<stage>/*.npy
    └── last/<stage>/*.npy
```

报告包含模型哈希、输入输出定义、编译配置、runtime/driver、首末数值验证、各段与整帧统计，
以及可获取的 CPU/NPU 频率和温度。不会自动修改板子频率或运行策略。
`--out-dir PATH` 可以改变报告目录；已有 report/CSV 的目录会被拒绝。
报告 `status=PASS` 且 `benchmark_completed=true` 表示数值校验和本次测速都完成。
错误时退出码为 1，并尽可能保存原因；首/末验证失败时不输出可用 FPS。

可以先检查复制是否完整：

```bash
./bin/rknn_pipeline_bench --inspect-only
```

这只检查清单、文件和张量，状态为 `BUNDLE_CHECK_PASS`，不初始化 RKNN，也不进行测速。

## 公共代码和依赖

`pipeline_bench_common.hpp/.cpp` 包含清单、SHA256、NPY、布局、数值指标和统计，后续 ONNX 程序可复用。
`third_party/nlohmann/json.hpp` 是固定版本 3.11.3 的 MIT 单头文件，不要求虚拟机联网或安装 JSON 开发包。
未修改相机采集代码、`RknnEngine` 或 `HiTModelRunner`。

公共代码主机测试不需要 RKNN，示例：

```bash
g++ -std=c++17 -O2 -I include -I third_party -I apps/tools/rknn_deploy \
  apps/tools/rknn_deploy/pipeline_bench_common.cpp \
  apps/tools/rknn_deploy/tests/pipeline_bench_common_test.cpp \
  -o /tmp/pipeline_bench_common_test
/tmp/pipeline_bench_common_test
```
