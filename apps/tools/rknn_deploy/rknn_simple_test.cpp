#include <rknn_api.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filessystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <string>
#include <vector>

namespace fs = std::filesystem

namespace {
/*
 * 当前 baseline:
 *
 * input:
 *   [1, 6, 640, 640]
 *
 * outputs:
 *   pred_logits [1, 300, 3]
 *   pred_boxes  [1, 300, 4]
 */
constexpr size_t kExpectedInnputElements = 1ULL * 6ULL * 640ULL * 640ULL;

constexpr size_t kExpectedInputBytes = kExpectedInputElements * sizeof(float);

constexpr size_t kExpextedLogitsElements = 1ULL * 300ULL * 3ULL;

constexpr size_t kExpectedBoxesElements = 1ULL * 300ULL * 4ULL;
/*
 * ============================================================
 * Binary file helpers
 * ============================================================
 */
bool readBinaryFile(const std::string& path, std::vector<uint8_t>& data)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        std::cerr
            << "[ERROR] failed to open file: "
            << path
            << std::endl;

        return false;
    }

    const std::streamsize size = ifs.tellg();

    if (size <= 0) {
        std::cerr
            << "[ERROR] invalid file size: "
            << path
            << " size="
            << size
            << std::endl;

        return false;
    }
    ifs.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));

    if(!ifs.read(reinterpret_cast<char*>(data.data()), size))
    {
        std::cerr
            << "[ERROR] failed to read file: "
            << path
            << std::endl;

        return false;
    }
    return true;

}
bool writeBinaryFile(const std::string& path, const void* data, size_t size)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        std::cerr
            << "[ERROR] failed to open output file: "
            << path
            << std::endl;

        return false;
    }

    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if(!ofs){
        std::cerr
            << "[ERROR] failed to write output file: "
            << path
            << std::endl;

        return false;
    }
    return true;
}
/*
 * ============================================================
 * Tensor information
 * ============================================================
 */
void dumpTensorAttr(const char* prefix, const rknn_tensor_attr& attr)
{
    std::cout 
        << prefix 
        << "index=" 
        << attr.index
        << "name="
        << attr.name
        << " dims=[";
    for(uint32_t i = 0; i < attr.n_dims; ++i)
    {
        std::count << attr.dims[i];
        if(i + 1 < attr.n_dims)
        {
            std::count << ", ";
        }
    }
    std::count
        <<"]"
        <<" n_elems="
        <<attr.n_elems
        <<" size="
        << attr.size
        << " fmt="
        << get_format_string(attr.fmt)
        << " type="
        << get_type_string(attr.type)
        << " qnt="
        << get_qnt_type_string(attr.qnt_type)
        << " zp="
        << attr.zp
        << " scale="
        << attr.scale
        <<std::endl;
}

void printFirstFloats(
    const std::string& name,
    const float* data,
    size_t count,
    size_t print_count = 10
)
{
    const size_t n = std::min(count, print_count);

    std::count 
        << "[INFO] "
        << name
        << " first "
        << n
        << " value:";
    for(size_t i = 0; i < n; ++i)
    {
        std::cout
            << " "
            << std::fixed //格式操纵符（Format Manipulator），强制将浮点数以 定点小数格式 输出。
            //std::setprecision 精度设置操纵符，用来指定浮点数输出时的位数
            //单独使用 setprecision(6) 时，代表“有效数字共 6 位”
            //但当它和 std::fixed 组合使用时，代表 “强制保留小数点后 6 位”（不足 6 位的会在末尾自动补 0）
            << std::setprecision(6)
            << data[i];
    }
    std::cout
        << std::endl;
}

/*
 * ============================================================
 * Timing statistics
 * ============================================================
 */

void printLatencyStats(
    const std::string& name,
    std::vector<double> values_ms
)
{
    if(values_ms.empty())
    {
        return;
    }
    std::sort(values_msvalues_ms.begin(), values_ms.end());

    const double sum = std::accumulate(values_ms.begin(), values_ms.end(), 0.0);

    const double avg = sum / static_cast<double>(values_ms.size());

    const double min_v = values_ms.front();
    const double max_v = values_ms.back();

    const size_t p50_index = static_cast<size_t>(0.50 * static_cast<double>(values_ms.size() - 1));
    const size_t p90_index = static_cast<size_t>(0.90 * static_cast<double>(values_ms.size() - 1));
    const size_t p99_index = static_cast<size_t>(0.99 * static_Cast<double>(values_ms.size() - 1));

    std::cout
        << "[PERF] "
        << name
        << " runs="
        << values_ms.size()
        << " min="
        << min_v
        << " ms"
        << " avg="
        << avg
        << " ms"
        << " p50="
        << values_ms[p50_index]
        << " ms"
        << " p90="
        << values_ms[p90_index]
        << " ms"
        << " p99="
        << values_ms[p99_index]
        << " ms"
        << " max="
        << max_v
        << " ms"
        << std::endl;
}




std::string outputFileName(
    const rknn_tensor_attr& attr,
    uint32_t index
)
{
    /*
     * 当前模型:
     *
     * logits:
     *   1 * 300 * 3 = 900
     *
     * boxes:
     *   1 * 300 * 4 = 1200
     *
     * 第一版按元素数量辨认。
     *
     * 同时仍然打印 attr.name，
     * 后面确认 RKNN 是否保留 ONNX 输出名。
     */
    if(attr.n_elems == kExpectedLogitsElements)
    {
        return "actual_pred_logits_f32.bin"
    }
    if(attr.n_elems == kExpectedBoxesElements)
    {
        return "actual_pred_boxes_f32.bin";
    }
    return "actual_output_" + std::to_string(index) + "_f32.bin";
}

void printUsage(const char* program)
{
    std::cout
        << "Usage:\n"
        << "   "
        << program
        << " <model.rknn>"
        << " <input_rgbt_f32.bin>"    
        << " <output_dir>"
        << " [warmup]"
        << " [loops]\n\n"

        << "Example:\n"
        << "   "
        << program
        << " "
        << "/userdata/rkcam/models/rtdetr_rgbt_baseline_640x640_fp16.rknn"
        << " "
        << "/userdata/rkcam/test/input_rgbt_f32.bin"
        << " "
        << "/userdata/rkcam/test/output"
        << " "
        << "5"
        << " "
        << "50"
        <<std::endl;
}

} //namespace

int main(int argc, char** argv)
{
    if(argc < 4)
    {
        printUsage(argv[0]);
        return 1;
    }
    const std::string model_path = argv[1];
    const std::string input_path = argv[2];
    const std::string output_dir = argv[3];
    
    const int warmup = argc >= 5 ? std::max(0, std::atoi(argv[4])) : 5;
    const int loops = argc >= 6 ? std::max(0, std::atoi(argv[5])) : 50;

    std::cout
        << "============================================================"
        << std::endl;

    std::cout
        << "RKNN RGBT baseline test"
        << std::endl;

    std::cout
        << "model      : "
        << model_path
        << std::endl;

    std::cout
        << "input      : "
        << input_path
        << std::endl;

    std::cout
        << "output dir : "
        << output_dir
        << std::endl;

    std::cout
        << "warmup     : "
        << warmup
        << std::endl;

    std::cout
        << "loops      : "
        << loops
        << std::endl;

    std::cout
        << "============================================================"
        << std::endl;
    /*
     * ========================================================
     * 1. Check files
     * ========================================================
     */
    if(!fs::exist(model_path))
    {
        std::cerr
            << "[ERROR] RKNN model not found: "
            << model_path
            << std::endl;

        return 1;
    }
    if(!fs.exit(input_path))
    {
        std::cerr
            << "[ERROR] input bin not found: "
            << input_path
            << std::endl;

        return 1;
    }
    fs::create_directories(output_dir);
    /*
     * ========================================================
     * 2. Load input tensor
     * ========================================================
     */
    std::vector<uint8_t> input_data;
    if(!readBinaryFile(input_path, input_data))
    {
        return 1;
    }
    std::cout
    << "[INFO] input bin size="
    << input_data.size()
    << " bytes"
    << std::endl;
    
    /*
     * 当前输入:
     *
     * float32
     * [1, 6, 640, 640]
     *
     * 6 * 640 * 640 * 4
     * = 9,830,400 bytes
     */
    if(input_data.size() != kExpectedInputBytes)
    {
        std::cerr
            <<"[ERROR] input size mismatch."
            <<" expected="
            << kExpectedInputBytes
            << " actual="
            << input_data.size()
            << std::endl;
        return 1;
    }

    /*
     * ========================================================
     * 3. Initialize RKNN
     * ========================================================
     */
    rknn_context ctx = 0;
    /*
     * size == 0:
     * model parameter is treated as a model file path.
     */
    int ret = rknn_init(&ctx, const_cast<char*>(model_path.c_str()), 0, 0, nullptr);
    if (ret != RKNN_SUCC) {
        std::cerr
            << "[ERROR] rknn_init failed, ret="
            << ret
            << std::endl;

        return 1;
    }

    std::cout
        << "[PASS] rknn_init"
        << std::endl;
    /*
     * ========================================================
     * 4. Query SDK / driver
     * ========================================================
     */
    rknn_sdk_version sdk_version{};
    ret = rknn_query(
        ctx,
        RKNN_QUERY_SDK_VERSION,
        &sdk_version,
        sizeof(sdk_version)
    );
    if (ret == RKNN_SUCC) {
        std::cout
            << "[INFO] RKNN API version: "
            << sdk_version.api_version
            << std::endl;

        std::cout
            << "[INFO] RKNN driver version: "
            << sdk_version.drv_version
            << std::endl;
    }
    else {
        std::cerr
            << "[WARNING] query SDK version failed, ret="
            << ret
            << std::endl;
    }

    /*
     * ========================================================
     * 5. Query memory
     * ========================================================
     */
    rknn_mem_size mem_size{};
    ret = rknn_query(
        ctx,
        RKNN_QUERY_MEM_SIZE,
        &mem_size,
        sizeof(mem_size)
    );
    if(ret == RKNN_SUCC)
    {
        std::cout
            <<"[INFO] weight memory :"
            << static_cast<double>(mem_size.total_weight_size) / 1024.0 / 1024.0
            << "MB"
            << std::endl;
        std:: cout
            << "[INFO] internal memory : "
            << static_cast<double>(mem_size.total_internal_size) / 1024.0 /1024.0
            << " MB"
            << std::endl;
        
    }

    /*
     * ========================================================
     * 6. Query input / output count
     * ========================================================
     */
    rknn_input_output_num io_num{};

    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        std::cerr
            << "[ERROR] RKNN_QUERY_IN_OUT_NUM failed, ret="
            << ret
            << std::endl;

        rknn_destroy(ctx);
        return 1;
    }
    std::cout
        << "[INFO] input num="
        << io_num.n_input
        << io_num.n_output
        <<std::endl;
    /*
     * 当前导出模型应该严格是:
     *
     * 1 input
     * 2 outputs
     */
    if (io_num.n_input != 1 ||
        io_num.n_output != 2) {

        std::cerr
            << "[ERROR] unexpected model IO count."
            << std::endl;

        rknn_destroy(ctx);
        return 1;
    }
    /*
     * ========================================================
     * 7. Query tensor attributes
     * ========================================================
     */

    std::vector<rknn_tensor_attr> input_attrs(io_num.n_input);
    std::vector<rknn_tensor_attr> output_attrs(io_num.n_output);
    std::cout
        << "\nInput tensors:"
        << std::endl;
    for(uint32_t i = 0; i < io_num.n_input; ++i)
    {
        std::memset(&input_attrs[i], 0, sizeof(rknn_tensor_attr));
        input_attrs[i].index = i;
        ret = rknn_query(
            ctx,
            RKNN_QUERY_INPUT_ATTR,
            &input_attrs[i],
            sizeof(rknn_tensor_attr)
        );
        if (ret != RKNN_SUCC) {
            std::cerr
                << "[ERROR] query input attr failed index="
                << i
                << " ret="
                << ret
                << std::endl;

            rknn_destroy(ctx);
            return 1;
        }
        dumpTensorAttr("    input", input_attrs[i]);
    }

    std::cout
        <<"\nOutput tensors:"
        << std::endl;
    for(uint32_t i = 0; i < io_num.n_output; ++i)
    {
        std::memset(&output_attrs[i], 0, sizeof(rknn_tensor_attr));
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            std::cerr
                << "[ERROR] query output attr failed index="
                << i
                << " ret="
                << ret
                << std::endl;

            rknn_destroy(ctx);
            return 1;
        }
        dumpTensorAttr("    output", output_attrs[i]);

    }
    /*
     * 额外检查逻辑输入元素数量。
     */
    if (input_attrs[0].n_elems !=
        kExpectedInputElements) {

        std::cerr
            << "[ERROR] model input element count mismatch."
            << " expected="
            << kExpectedInputElements
            << " actual="
            << input_attrs[0].n_elems
            << std::endl;

        rknn_destroy(ctx);
        return 1;
    }
    /*
     * ========================================================
     * 8. Set input
     * ========================================================
     *
     * input_data 本身是:
     *
     * float32
     * NCHW
     * [1, 6, 640, 640]
     *
     * pass_through = 0:
     *
     * 告诉 RKNN Runtime 按这里声明的 type/fmt
     * 将用户输入转换成模型真正需要的内部格式。
     */
    rknn_input input{};
    input.index = 0;

    input.buf = input_data.data();

    input.size = static_cast<uint32_t>(input_data.size());

    input.pass_through = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.fmt = RKNN_TENSOR_NCHW;
    ret = rknn_inputs_set(ctx, 1, &input);
    if (ret != RKNN_SUCC) {
        std::cerr
            << "[ERROR] rknn_inputs_set failed, ret="
            << ret
            << std::endl;

        rknn_destroy(ctx);
        return 1;
    }
    std::cout
        << "\n[PASS] rknn_inputs_set"
        << std::endl;


    /*
     * ========================================================
     * Helper:
     * execute one synchronous inference
     * ========================================================
     */

}