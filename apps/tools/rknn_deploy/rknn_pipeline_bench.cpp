// Board-local, synchronous multi-model RKNN application latency benchmark.
// Models and bindings come from PMATrack manifests; no camera or ONNX runtime.
#include "pipeline_bench_common.hpp"
#include "rkcam/ai/rknn_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace bench = rkcam::bench;
namespace fs = std::filesystem;
using bench::Json;
using rkcam::ai::FloatTensor;
using rkcam::ai::RknnEngine;
using Clock = std::chrono::steady_clock;

namespace {
struct Options {
    fs::path runDir="/userdata/rkcam/rknn_deploy/model/baseline_final_v2";
    std::string buildKey="rk3568_fp16_opt3";
    std::string caseName;
    fs::path outDir;
    int warmup=20;
    int loops=200;
    double cosMin=.995, relMax=.05, boxAtol=.002;
    bool inspectOnly=false;
};

double elapsedMs(Clock::time_point begin,Clock::time_point end) {
    return std::chrono::duration<double,std::milli>(end-begin).count();
}

void usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
        "  --run-dir PATH     default: /userdata/rkcam/rknn_deploy/model/baseline_final_v2\n"
        "  --build-key KEY    default: rk3568_fp16_opt3\n"
        "  --case NAME        default: manifest reference_case\n"
        "  --warmup N         default: 20\n"
        "  --loops N          default: 200\n"
        "  --out-dir PATH     default: RUN/reports/board_cpp_benchmark/<unique run>\n"
        "  --cos-min VALUE    default: 0.995\n"
        "  --rel-l2-max VALUE default: 0.05\n"
        "  --box-atol VALUE   default: 0.002 (normalized cxcywh)\n"
        "  --inspect-only     verify files/tensors; do not initialize RKNN or time inference\n";
}

int integer(const std::string& text) {
    size_t consumed=0;
    const int value=std::stoi(text,&consumed);
    bench::require(consumed==text.size(),"Invalid integer: "+text);
    return value;
}

double number(const std::string& text) {
    size_t consumed=0;
    const double value=std::stod(text,&consumed);
    bench::require(consumed==text.size() && std::isfinite(value),"Invalid number: "+text);
    return value;
}

Options parse(int argc,char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string name=argv[i];
        if(name=="--inspect-only") {options.inspectOnly=true; continue;}
        bench::require(i+1<argc,"Missing argument for "+name);
        const std::string value=argv[++i];
        if(name=="--run-dir") options.runDir=value;
        else if(name=="--build-key") options.buildKey=value;
        else if(name=="--case") options.caseName=value;
        else if(name=="--out-dir") options.outDir=value;
        else if(name=="--warmup") options.warmup=integer(value);
        else if(name=="--loops") options.loops=integer(value);
        else if(name=="--cos-min") options.cosMin=number(value);
        else if(name=="--rel-l2-max") options.relMax=number(value);
        else if(name=="--box-atol") options.boxAtol=number(value);
        else throw std::runtime_error("Unknown option: "+name);
    }
    bench::require(options.warmup>=0 && options.loops>0,"warmup must be >=0 and loops must be >0");
    bench::require(options.cosMin>=-1 && options.cosMin<=1 && options.relMax>=0 && options.boxAtol>=0,"Invalid accuracy threshold");
    bench::require(bench::safeName(options.buildKey)==options.buildKey,"Invalid build key");
    return options;
}

std::string optionalText(const fs::path& path) {
    std::ifstream stream(path);
    if(!stream) return {};
    std::ostringstream result;
    result << stream.rdbuf();
    auto text=result.str();
    while(!text.empty() && (text.back()=='\n'||text.back()=='\r')) text.pop_back();
    return text;
}

Json boardState() {
    Json result=Json::object();
    result["cpu0_frequency_khz"]=optionalText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    result["cpu0_governor"]=optionalText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    const fs::path devfreq="/sys/class/devfreq";
    if(fs::exists(devfreq)) {
        for(const auto& entry:fs::directory_iterator(devfreq)) {
            if(entry.path().filename().string().find("npu")!=std::string::npos)
                result["npu_devfreq"][entry.path().filename().string()]={{"current_hz",optionalText(entry.path()/"cur_freq")},
                    {"governor",optionalText(entry.path()/"governor")}};
        }
    }
    const fs::path thermal="/sys/class/thermal";
    if(fs::exists(thermal)) {
        for(const auto& entry:fs::directory_iterator(thermal)) {
            if(entry.path().filename().string().find("thermal_zone")==0)
                result["thermal"][entry.path().filename().string()]={{"type",optionalText(entry.path()/"type")},
                    {"temperature_millicelsius",optionalText(entry.path()/"temp")}};
        }
    }
    return result;
}

struct Binding {
    bool external=false;
    size_t stage=0,output=0;
};

struct Node {
    std::unique_ptr<RknnEngine> engine;
    std::vector<Binding> bindings;
    std::vector<FloatTensor> staticInputs;
    std::vector<FloatTensor> scratch;
    bool allExternal=true;
    double initMs=0;
};

struct FrameTime {
    std::vector<double> stageMs;
    std::vector<double> bridgeMs;
    double totalMs=0;
    explicit FrameTime(size_t count):stageMs(count),bridgeMs(count) {}
};

class Pipeline {
    const bench::Bundle& bundle_;
public:
    std::vector<Node> nodes;
    std::vector<std::vector<FloatTensor>> outputs;
    explicit Pipeline(const bench::Bundle& bundle):bundle_(bundle),outputs(bundle.chain.size()) {
        nodes.reserve(bundle.chain.size());
        std::map<std::string,std::pair<size_t,size_t>> available;
        for(size_t i=0;i<bundle.chain.size();++i) {
            const auto& plan=bundle.chain[i];
            Node node;
            // Clean baseline has no custom GridSample op. Do not register unused custom operators.
            node.engine=std::make_unique<RknnEngine>(plan.modelPath.string(),false);
            const auto initBegin=Clock::now();
            bench::require(node.engine->init(),"RKNN initialization failed: "+plan.name+": "+node.engine->lastError());
            node.initMs=elapsedMs(initBegin,Clock::now());
            bench::require(node.engine->inputCount()==plan.inputs.size() && node.engine->outputCount()==plan.outputs.size(),"Runtime IO count mismatch: "+plan.name);
            for(size_t j=0;j<plan.inputs.size();++j) bench::validateRuntimeAttr(node.engine->inputAttr(j),plan.inputs[j]);
            for(size_t j=0;j<plan.outputs.size();++j) bench::validateRuntimeAttr(node.engine->outputAttr(j),plan.outputs[j]);
            node.staticInputs.resize(plan.inputs.size());
            node.scratch.resize(plan.inputs.size());
            for(size_t j=0;j<plan.inputs.size();++j) {
                const auto source=plan.bindings.at(plan.inputs[j].name);
                if(source.rfind("external:",0)==0) {
                    node.bindings.push_back({true,0,0});
                    node.staticInputs[j]=bench::prepareInput(bundle.external.at(source.substr(9)),node.engine->inputAttr(j));
                } else {
                    bench::require(available.count(source)>0,"Missing pipeline source: "+source);
                    const auto location=available.at(source);
                    node.bindings.push_back({false,location.first,location.second});
                    node.allExternal=false;
                }
            }
            for(size_t j=0;j<plan.outputs.size();++j) available.emplace(plan.name+":"+plan.outputs[j].name,std::make_pair(i,j));
            std::cout << "MODEL " << plan.name << " inputs=" << node.engine->inputCount()
                      << " outputs=" << node.engine->outputCount() << " API=" << node.engine->apiVersion()
                      << " driver=" << node.engine->driverVersion() << '\n';
            nodes.push_back(std::move(node));
        }
    }

    void run(FrameTime& time) {
        const auto begin=Clock::now();
        for(size_t i=0;i<nodes.size();++i) {
            auto& node=nodes[i];
            const auto bridgeBegin=Clock::now();
            const FloatTensor* single=nullptr;
            if(!node.allExternal && node.bindings.size()==1) {
                const auto& binding=node.bindings[0];
                single=&outputs.at(binding.stage).at(binding.output);
                const auto& attr=node.engine->inputAttr(0);
                if(single->shape.dims!=attr.shape.dims || single->layout!=attr.layout) {
                    node.scratch[0]=bench::prepareInput(*single,attr);
                    single=&node.scratch[0];
                }
            } else if(!node.allExternal) {
                for(size_t j=0;j<node.bindings.size();++j) {
                    const auto& binding=node.bindings[j];
                    node.scratch[j]=binding.external?node.staticInputs[j]:bench::prepareInput(outputs.at(binding.stage).at(binding.output),node.engine->inputAttr(j));
                }
            }
            const auto inferBegin=Clock::now();
            time.bridgeMs[i]=elapsedMs(bridgeBegin,inferBegin);
            const bool ok=node.allExternal?node.engine->infer(node.staticInputs,outputs[i]):
                          single?node.engine->infer(*single,outputs[i]):node.engine->infer(node.scratch,outputs[i]);
            const auto inferEnd=Clock::now();
            time.stageMs[i]=elapsedMs(inferBegin,inferEnd);
            if(!ok) throw std::runtime_error("RKNN inference failed: "+bundle_.chain[i].name+": "+node.engine->lastError());
        }
        // Complete output buffers (including the prediction) are now available.
        time.totalMs=elapsedMs(begin,Clock::now());
    }
};

Json validateOutputs(const bench::Bundle& bundle,const Pipeline& pipeline,const Options& options,const fs::path& directory) {
    Json result={{"status","PASS"},{"stages",Json::object()}};
    for(size_t i=0;i<bundle.chain.size();++i) {
        const auto& plan=bundle.chain[i];
        bench::require(pipeline.outputs[i].size()==plan.outputs.size(),"Unexpected runtime output count");
        Json stage={{"status","PASS"},{"outputs",Json::object()}};
        for(size_t j=0;j<plan.outputs.size();++j) {
            const auto& spec=plan.outputs[j];
            const auto actual=bench::canonicalOutput(pipeline.outputs[i][j],spec);
            const bool box=spec.name=="pred_boxes";
            auto onnx=bench::compareTensor(actual,bundle.goldenOnnx[i][j],box,options.cosMin,options.relMax,options.boxAtol);
            auto torch=bench::compareTensor(actual,bundle.goldenTorch[i][j],box,options.cosMin,options.relMax,options.boxAtol);
            stage["outputs"][spec.name]={{"onnx",onnx},{"torch",torch}};
            bench::writeFloatNpy(directory/bench::safeName(plan.name)/(bench::safeName(spec.name)+".npy"),actual);
            if(onnx.at("status")!="PASS" || torch.at("status")!="PASS") stage["status"]="FAIL";
        }
        if(stage.at("status")!="PASS") result["status"]="FAIL";
        result["stages"][plan.name]=std::move(stage);
    }
    return result;
}

Json frameJson(const bench::Bundle& bundle,const FrameTime& time) {
    Json result={{"pipeline_ms",time.totalMs},{"stages",Json::object()}};
    for(size_t i=0;i<bundle.chain.size();++i)
        result["stages"][bundle.chain[i].name]={{"infer_ms",time.stageMs[i]},{"bridge_ms",time.bridgeMs[i]}};
    return result;
}

void writeCsv(const fs::path& path,const bench::Bundle& bundle,const std::vector<FrameTime>& times) {
    std::ofstream output(path);
    bench::require(bool(output),"Cannot write timing CSV");
    output << "iteration,pipeline_ms";
    for(const auto& plan:bundle.chain) output << ',' << bench::safeName(plan.name) << "_infer_ms," << bench::safeName(plan.name) << "_bridge_ms";
    output << '\n' << std::setprecision(10);
    for(size_t i=0;i<times.size();++i) {
        output << i << ',' << times[i].totalMs;
        for(size_t j=0;j<bundle.chain.size();++j) output << ',' << times[i].stageMs[j] << ',' << times[i].bridgeMs[j];
        output << '\n';
    }
    bench::require(bool(output),"Timing CSV write failed");
}
} // namespace

int main(int argc,char** argv) {
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--help" || std::string(argv[i])=="-h") {usage(argv[0]);return 0;}
    fs::path out;
    Json report={{"format","rkcam.pipeline_benchmark.v1"},{"status","ERROR"},{"benchmark_completed",false},
                 {"created_at",bench::utcTimestamp()},{"backend","board_local_rknn"}};
    try {
        const auto options=parse(argc,argv);
        const auto proposedOut=options.outDir.empty()?fs::absolute(options.runDir)/"reports/board_cpp_benchmark"/bench::uniqueTimestamp():fs::absolute(options.outDir);
        bench::require(!fs::exists(proposedOut/"benchmark_report.json") && !fs::exists(proposedOut/"timings.csv"),"Output already contains a report; choose a new --out-dir");
        out=proposedOut;
        fs::create_directories(out);
        report["options"]={{"warmup",options.warmup},{"loops",options.loops},{"build_key",options.buildKey},
            {"cos_min",options.cosMin},{"rel_l2_max",options.relMax},{"box_atol",options.boxAtol}};
        report["timing_scope"]="Board-local RknnEngine application chain, including SDK input handling, all output conversion/copies and output release";
        report["excluded"]={"file loading and hashes","model initialization","initial image layout conversion", "camera/crop/prealignment/normalization","validation and file/log output"};
        report["pure_npu_time_measured"]=false;
        report["sdk_detailed_perf_enabled"]=false;
        report["async_enabled"]=false;
        const auto loadBegin=Clock::now();
        const auto bundle=bench::loadBundle(options.runDir,options.buildKey,options.caseName);
        report["bundle_load_ms"]=elapsedMs(loadBegin,Clock::now());
        report["run_dir"]=bundle.root.string(); report["case"]=bundle.caseName;
        report["manifest_sha256"]=bench::sha256File(bundle.root/"manifest.json");
        report["compiler_config"]=bundle.build.at("config");
        report["model_steps"]=Json::array(); report["models"]=Json::object();
        for(const auto& stage:bundle.chain) {
            report["model_steps"].push_back(stage.name);
            report["models"][stage.name]={{"path",stage.modelPath.string()},{"sha256",stage.modelHash},
                {"inputs",bundle.manifest.at("stages").at(stage.name).at("inputs")},
                {"outputs",bundle.manifest.at("stages").at(stage.name).at("outputs")}};
        }
        report["prediction"]={{"stage",bundle.chain[bundle.predictionStage].name},{"output",bundle.predictionOutput},{"name","pred_boxes"}};
        if(options.inspectOnly) {
            report["status"]="BUNDLE_CHECK_PASS";
            bench::writeJson(out/"benchmark_report.json",report);
            std::cout << "BUNDLE_CHECK_PASS; RKNN was not initialized.\nREPORT: " << (out/"benchmark_report.json") << '\n';
            return 0;
        }
        report["board_state_before"]=boardState();
        const auto setupBegin=Clock::now();
        Pipeline pipeline(bundle);
        report["pipeline_setup_ms"]=elapsedMs(setupBegin,Clock::now());
        for(size_t i=0;i<bundle.chain.size();++i) {
            auto& record=report["models"][bundle.chain[i].name];
            record["init_ms"]=pipeline.nodes[i].initMs;
            record["api_version"]=pipeline.nodes[i].engine->apiVersion();
            record["driver_version"]=pipeline.nodes[i].engine->driverVersion();
        }
        FrameTime first(bundle.chain.size());
        pipeline.run(first);
        report["first_frame"]=frameJson(bundle,first);
        report["validation"]["first"]=validateOutputs(bundle,pipeline,options,out/"validation/first");
        bench::require(report["validation"]["first"]["status"]=="PASS","First-frame numerical validation failed; latency benchmark rejected");
        std::cout << "FIRST FRAME: " << first.totalMs << " ms; numerical validation PASS\n";
        std::cout << "WARMUP " << options.warmup << ", MEASURE " << options.loops << " frames (same fixed inputs)\n";
        FrameTime warm(bundle.chain.size());
        for(int i=0;i<options.warmup;++i) pipeline.run(warm);
        std::vector<FrameTime> times;
        times.reserve(size_t(options.loops));
        for(int i=0;i<options.loops;++i) times.emplace_back(bundle.chain.size());
        for(auto& time:times) pipeline.run(time);
        report["validation"]["last"]=validateOutputs(bundle,pipeline,options,out/"validation/last");
        bench::require(report["validation"]["last"]["status"]=="PASS","Last-frame numerical validation failed; latency benchmark rejected");
        report["board_state_after"]=boardState();
        std::vector<double> total;
        for(const auto& time:times) total.push_back(time.totalMs);
        report["statistics"]["pipeline"]=bench::timingStatistics(total);
        const double mean=report["statistics"]["pipeline"]["mean_ms"];
        bench::require(mean>0,"Invalid zero duration");
        report["equivalent_model_fps"]=1000./mean;
        for(size_t i=0;i<bundle.chain.size();++i) {
            std::vector<double> infer,bridge;
            for(const auto& time:times) {infer.push_back(time.stageMs[i]);bridge.push_back(time.bridgeMs[i]);}
            report["statistics"]["stages"][bundle.chain[i].name]={{"infer",bench::timingStatistics(infer)},{"bridge",bench::timingStatistics(bridge)}};
        }
        report["pred_boxes"]=pipeline.outputs[bundle.predictionStage][bundle.predictionOutput].data;
        writeCsv(out/"timings.csv",bundle,times);
        report["status"]="PASS"; report["benchmark_completed"]=true;
        bench::writeJson(out/"benchmark_report.json",report);
        std::cout << std::fixed << std::setprecision(3);
        for(size_t i=0;i<bundle.chain.size();++i) {
            const auto& stats=report["statistics"]["stages"][bundle.chain[i].name]["infer"];
            std::cout << bundle.chain[i].name << ": mean=" << stats["mean_ms"].get<double>()
                      << " ms p50=" << stats["p50_ms"].get<double>() << " ms p95=" << stats["p95_ms"].get<double>() << " ms\n";
        }
        const auto& stats=report["statistics"]["pipeline"];
        std::cout << "PIPELINE: mean=" << mean << " ms p50=" << stats["p50_ms"].get<double>()
                  << " ms p95=" << stats["p95_ms"].get<double>() << " ms min=" << stats["min_ms"].get<double>()
                  << " ms max=" << stats["max_ms"].get<double>() << " ms equivalent_model_fps=" << 1000./mean << '\n';
        std::cout << "PASS (application latency; camera/preprocessing excluded)\nREPORT: " << (out/"benchmark_report.json") << '\n';
        return 0;
    } catch(const std::exception& error) {
        report["error"]=error.what();
        if(report.contains("validation")) report["status"]="VALIDATION_OR_RUNTIME_FAILED";
        if(!out.empty()) {
            try {bench::writeJson(out/"benchmark_report.json",report);}
            catch(const std::exception& writeError) {std::cerr << "REPORT WRITE ERROR: " << writeError.what() << '\n';}
        }
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
