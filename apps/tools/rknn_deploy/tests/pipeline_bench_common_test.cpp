// Host-only tests: no RKNN library and no claim about NPU speed/numerics.
#include "pipeline_bench_common.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace bench=rkcam::bench;
namespace fs=std::filesystem;

int main() {
    const auto root=fs::temp_directory_path()/("rkcam_bench_test_"+bench::uniqueTimestamp());
    try {
        fs::create_directories(root);
        const auto hashFile=root/"hash.txt";
        {std::ofstream file(hashFile);}
        bench::require(bench::sha256File(hashFile)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Empty SHA256 vector failed");
        {std::ofstream file(hashFile,std::ios::binary);file<<"abc";}
        bench::require(bench::sha256File(hashFile)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc vector failed");
        {std::ofstream file(hashFile,std::ios::binary);file<<std::string(1000000,'a');}
        bench::require(bench::sha256File(hashFile)=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","Streaming SHA256 vector failed");
        rkcam::ai::FloatTensor tensor;
        tensor.shape.dims={1,2,2,2}; tensor.layout=rkcam::ai::TensorLayout::NCHW;
        tensor.data={0,1,2,3,4,5,6,7};
        bench::writeFloatNpy(root/"tensor.npy",tensor);
        const auto parsed=bench::readFloatNpy(root/"tensor.npy");
        bench::require(parsed.shape.dims==tensor.shape.dims && parsed.data==tensor.data,"NPY roundtrip failed");
        const bench::TensorSpec spec{"image",tensor.shape.dims,"float32","NCHW"};
        const bench::Json record={{"path","tensor.npy"},{"sha256",bench::sha256File(root/"tensor.npy")}};
        const auto loaded=bench::loadTensor(root,record,spec);
        rkcam::ai::TensorAttr attr;
        attr.name="image"; attr.shape.dims={1,2,2,2}; attr.layout=rkcam::ai::TensorLayout::NHWC; attr.element_count=8;
        const auto nhwc=bench::prepareInput(loaded,attr);
        bench::require(nhwc.data==std::vector<float>({0,4,1,5,2,6,3,7}),"Image transpose failed");
        bench::require(bench::canonicalOutput(nhwc,spec).data==tensor.data,"Canonical transpose failed");
        const auto metrics=bench::compareTensor(parsed,parsed,false,.995,.05,.002);
        bench::require(metrics.at("status")=="PASS" && metrics.at("max_abs")==0,"Exact tensor comparison failed");
        auto bad=parsed; bad.shape.dims={1,1,2,4};
        bench::require(bench::compareTensor(bad,parsed,false,.995,.05,.002).at("status")=="FAIL","Shape mismatch accepted");
        bad=parsed; bad.data[0]=std::numeric_limits<float>::quiet_NaN();
        bench::require(bench::compareTensor(bad,parsed,false,.995,.05,.002).at("nonfinite")==1,"Nonfinite output lost");
        const auto stats=bench::timingStatistics({1,2,3,4,5});
        bench::require(stats.at("mean_ms")==3 && stats.at("p50_ms")==3 && std::abs(stats.at("p95_ms").get<double>()-4.8)<1e-12,"Latency statistics failed");
        bool escaped=false;
        try {bench::artifactPath(root,"../escape.npy");} catch(const std::runtime_error&) {escaped=true;}
        bench::require(escaped,"Run path escape accepted");
        {std::ofstream file(root/"tensor.npy",std::ios::binary|std::ios::app);file<<'x';}
        bool changed=false;
        try {bench::loadTensor(root,record,spec);} catch(const std::runtime_error&) {changed=true;}
        bench::require(changed,"Modified tensor accepted");
        // Temporary directory remains available for inspecting a failing test.
        std::cout<<"PASS: SHA256, NPY, layouts, metrics, statistics, paths and file integrity\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<"\nTEST DIRECTORY: "<<root<<'\n';
        return 1;
    }
}
