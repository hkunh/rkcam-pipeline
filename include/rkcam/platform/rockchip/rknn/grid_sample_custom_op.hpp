#pragma once
#include  "rknn_api.h"

namespace rkcam::platform::rockchip::rknn {

/**
 * @brief 注册 GridSample CPU custom op。
 *
 * 当前为第一阶段 probe/debug 版本：
 *
 *   - op_type = "GridSample"
 *   - target  = CPU
 *   - compute callback 只打印输入/输出 tensor 信息
 *   - 不执行真正 GridSample
 *   - compute 最后主动返回 RKNN_ERR_FAIL
 *
 * 目的：
 *
 *   先验证 RKNN Runtime 是否能够把模型中的：
 *
 *       GridSample CPU fallback
 *
 *   正确派发到我们注册的 callback。
 *
 * 调用时机：
 *
 *       rknn_init()
 *           ↓
 *       registerGridSampleCustomOp()
 *           ↓
 *       rknn_query(...)
 *           ↓
 *       rknn_run()
 *
 * @param context 已成功创建的 RKNN context。
 *
 * @return
 *      RKNN_SUCC      注册成功
 *      其他 RKNN 错误码 注册失败
 */
[[nodiscard]] int registerGridSampleCustomOp(rknn_context context) noexcept;

} //namespace rkcam::platform::rockchip::rknn