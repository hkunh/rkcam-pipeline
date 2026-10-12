#include "pipeline_bench_common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace rkcam::bench {
using rkcam::ai::TensorLayout;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Json readJson(const fs::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot read JSON: " + path.string());
    Json result;
    input >> result;
    return result;
}

void writeJson(const fs::path& path, const Json& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    require(bool(output), "Cannot write JSON: " + path.string());
    output << std::setw(2) << value << '\n';
    require(bool(output), "JSON write failed: " + path.string());
}

fs::path artifactPath(const fs::path& root, const std::string& relative) {
    const auto base = fs::canonical(root);
    const auto path = fs::weakly_canonical(base / fs::path(relative));
    auto a = base.begin(), b = path.begin();
    for (; a != base.end(); ++a, ++b)
        require(b != path.end() && *a == *b, "Artifact escapes run directory: " + relative);
    return path;
}

namespace {
// Streaming SHA-256, so model verification does not require another runtime library.
class Sha256 {
    std::array<uint32_t, 8> state_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<uint8_t, 64> buffer_{};
    uint64_t count_ = 0;
    size_t used_ = 0;
    static uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* bytes) {
        static constexpr uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i)
            w[i] = (uint32_t(bytes[i*4])<<24) | (uint32_t(bytes[i*4+1])<<16) |
                   (uint32_t(bytes[i*4+2])<<8) | uint32_t(bytes[i*4+3]);
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3);
            const uint32_t s1 = rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        auto e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (size_t i=0; i<64; ++i) {
            const uint32_t s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
            const uint32_t t1=h+s1+((e&f)^((~e)&g))+k[i]+w[i];
            const uint32_t s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
            const uint32_t t2=s0+((a&b)^(a&c)^(b&c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }
public:
    void update(const uint8_t* data, size_t size) {
        count_ += size;
        while (size) {
            const size_t take=std::min(size, buffer_.size()-used_);
            std::memcpy(buffer_.data()+used_, data, take);
            used_+=take; data+=take; size-=take;
            if (used_==64) { block(buffer_.data()); used_=0; }
        }
    }
    std::string finish() {
        const uint64_t bits=count_*8;
        const uint8_t marker=0x80, zero=0;
        update(&marker,1);
        while (used_!=56) update(&zero,1);
        uint8_t length[8];
        for (unsigned i=0;i<8;++i) length[7-i]=uint8_t(bits>>(i*8));
        update(length,8);
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (auto v:state_) output << std::setw(8) << v;
        return output.str();
    }
};

size_t elementCount(const std::vector<int64_t>& shape) {
    require(!shape.empty(), "Scalar/empty tensors are not supported by this benchmark");
    size_t result=1;
    for (auto dim:shape) {
        require(dim>0 && uint64_t(dim)<=std::numeric_limits<size_t>::max()/result, "Invalid/overflowing tensor shape");
        result*=size_t(dim);
    }
    require(result<=std::numeric_limits<size_t>::max()/sizeof(float), "Tensor byte size overflows");
    return result;
}

FloatTensor transposeImage(const FloatTensor& input, bool toNhwc) {
    require(input.shape.rank()==4, "Image transpose requires rank four");
    const auto& s=input.shape.dims;
    const int64_t n=s[0], c=toNhwc?s[1]:s[3], h=toNhwc?s[2]:s[1], w=toNhwc?s[3]:s[2];
    FloatTensor result;
    result.shape.dims=toNhwc?std::vector<int64_t>{n,h,w,c}:std::vector<int64_t>{n,c,h,w};
    result.layout=toNhwc?TensorLayout::NHWC:TensorLayout::NCHW;
    result.data.resize(input.data.size());
    for (int64_t ni=0;ni<n;++ni) for (int64_t ci=0;ci<c;++ci)
        for (int64_t yi=0;yi<h;++yi) for (int64_t xi=0;xi<w;++xi) {
            const size_t nchw=size_t(((ni*c+ci)*h+yi)*w+xi);
            const size_t nhwc=size_t(((ni*h+yi)*w+xi)*c+ci);
            result.data[toNhwc?nhwc:nchw]=input.data[toNhwc?nchw:nhwc];
        }
    return result;
}
} // namespace

std::string sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot hash file: "+path.string());
    Sha256 hash;
    std::array<uint8_t, 65536> data{};
    while (input) {
        input.read(reinterpret_cast<char*>(data.data()), data.size());
        hash.update(data.data(), size_t(input.gcount()));
    }
    require(input.eof(), "File read failed while hashing: "+path.string());
    return hash.finish();
}

void verifyHash(const fs::path& path, const std::string& expected) {
    require(expected.size()==64 && sha256File(path)==expected, "SHA256 mismatch: "+path.string());
}

std::string safeName(const std::string& name) {
    require(!name.empty() && name!="." && name!="..", "Invalid name");
    std::string result;
    for (unsigned char c:name) result+=std::isalnum(c)||c=='_'||c=='-'||c=='.'?char(c):'_';
    return result;
}

std::string utcTimestamp() {
    const auto now=std::time(nullptr);
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value,&now);
#else
    gmtime_r(&now,&value);
#endif
    std::ostringstream output;
    output << std::put_time(&value,"%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string uniqueTimestamp() {
    return safeName(utcTimestamp())+"_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}

TensorSpec TensorSpec::fromJson(const Json& value) {
    TensorSpec result{value.at("name").get<std::string>(), value.at("shape").get<std::vector<int64_t>>(),
                      value.at("dtype").get<std::string>(),value.at("layout").get<std::string>()};
    require(result.dtype=="float32", "Benchmark currently accepts float32 logical tensors only");
    require(result.layout=="NCHW" || result.layout=="NHWC" || result.layout=="BTC" ||
            result.layout=="TBC" || result.layout=="native", "Unsupported logical tensor layout: "+result.layout);
    elementCount(result.shape);
    return result;
}

FloatTensor readFloatNpy(const fs::path& path) {
    const uint16_t endian=1;
    require(*reinterpret_cast<const uint8_t*>(&endian)==1, "Little-endian host required");
    std::ifstream input(path,std::ios::binary);
    require(bool(input), "Cannot read NPY: "+path.string());
    uint8_t prefix[8]{};
    input.read(reinterpret_cast<char*>(prefix),8);
    require(bool(input) && std::memcmp(prefix,"\x93NUMPY",6)==0, "Invalid NPY magic: "+path.string());
    require(prefix[6]>=1 && prefix[6]<=3 && prefix[7]==0, "Unsupported NPY version");
    const size_t lengthBytes=prefix[6]==1?2:4;
    uint8_t bytes[4]{};
    input.read(reinterpret_cast<char*>(bytes),lengthBytes);
    uint32_t headerLength=0;
    for(size_t i=0;i<lengthBytes;++i) headerLength|=uint32_t(bytes[i])<<(8*i);
    require(headerLength>0 && headerLength<=1024*1024, "Invalid NPY header length");
    std::string header(headerLength,'\0');
    input.read(header.data(),header.size());
    require(bool(input),"Truncated NPY header");
    std::smatch match;
    require(std::regex_search(header,match,std::regex("['\"]descr['\"]\\s*:\\s*['\"]([^'\"]+)['\"]")), "Missing NPY dtype");
    require(match[1]=="<f4" || match[1]=="=f4", "NPY must contain little-endian float32: "+path.string());
    require(std::regex_search(header,std::regex("['\"]fortran_order['\"]\\s*:\\s*False")), "Fortran NPY layout not supported");
    require(std::regex_search(header,match,std::regex("['\"]shape['\"]\\s*:\\s*\\(([^)]*)\\)")), "Missing NPY shape");
    std::string dimensions=match[1];
    for(char& c:dimensions) if(c==',') c=' ';
    std::istringstream dims(dimensions);
    FloatTensor result;
    int64_t dimension;
    while(dims>>dimension) result.shape.dims.push_back(dimension);
    require(dims.eof(),"Invalid NPY dimensions");
    const auto count=elementCount(result.shape.dims);
    const auto offset=input.tellg();
    input.seekg(0,std::ios::end);
    require(input.tellg()-offset==std::streamoff(count*sizeof(float)), "NPY payload length mismatch: "+path.string());
    input.seekg(offset);
    result.layout=TensorLayout::Unknown;
    result.data.resize(count);
    input.read(reinterpret_cast<char*>(result.data.data()),std::streamsize(count*sizeof(float)));
    require(bool(input), "Truncated NPY payload");
    return result;
}

void writeFloatNpy(const fs::path& path,const FloatTensor& tensor) {
    require(tensor.valid(),"Cannot write invalid tensor");
    fs::create_directories(path.parent_path());
    std::ostringstream shape;
    for(auto dim:tensor.shape.dims) shape<<dim<<", ";
    std::string header="{'descr': '<f4', 'fortran_order': False, 'shape': ("+shape.str()+"), }";
    header.append((64-((10+header.size()+1)%64))%64,' ');
    header+='\n';
    require(header.size()<=65535,"NPY header too large");
    std::ofstream output(path,std::ios::binary);
    require(bool(output),"Cannot write NPY: "+path.string());
    output.write("\x93NUMPY\x01\x00",8);
    const char length[2]={char(header.size()&255),char(header.size()>>8)};
    output.write(length,2);
    output.write(header.data(),header.size());
    output.write(reinterpret_cast<const char*>(tensor.data.data()),std::streamsize(tensor.data.size()*sizeof(float)));
    require(bool(output),"NPY write failed");
}

void validateTensor(const FloatTensor& tensor,const TensorSpec& spec,bool finite) {
    require(tensor.valid() && tensor.shape.dims==spec.shape,"Tensor shape mismatch: "+spec.name);
    if(finite) require(std::all_of(tensor.data.begin(),tensor.data.end(),[](float v){return std::isfinite(v);}),"Nonfinite tensor: "+spec.name);
}

FloatTensor loadTensor(const fs::path& root,const Json& record,const TensorSpec& spec) {
    const auto path=artifactPath(root,record.at("path"));
    verifyHash(path,record.at("sha256"));
    auto tensor=readFloatNpy(path);
    validateTensor(tensor,spec);
    tensor.layout=spec.layout=="NCHW"?TensorLayout::NCHW:spec.layout=="NHWC"?TensorLayout::NHWC:TensorLayout::Unknown;
    return tensor;
}

FloatTensor canonicalOutput(const FloatTensor& tensor,const TensorSpec& spec) {
    if(spec.layout=="NCHW" && tensor.layout==TensorLayout::NHWC) return transposeImage(tensor,false);
    if(spec.layout=="NHWC" && tensor.layout==TensorLayout::NCHW) return transposeImage(tensor,true);
    return tensor;
}

FloatTensor prepareInput(const FloatTensor& tensor,const TensorAttr& attr) {
    auto result=(tensor.layout==TensorLayout::NCHW && attr.layout==TensorLayout::NHWC)?transposeImage(tensor,true):
                (tensor.layout==TensorLayout::NHWC && attr.layout==TensorLayout::NCHW)?transposeImage(tensor,false):tensor;
    require(result.valid() && result.shape.dims==attr.shape.dims && result.layout==attr.layout,"Input shape/layout differs from runtime: "+attr.name);
    return result;
}

void validateRuntimeAttr(const TensorAttr& attr,const TensorSpec& spec) {
    require(attr.name==spec.name,"Runtime tensor name/order mismatch: expected "+spec.name+", got "+attr.name);
    auto logical=spec.shape;
    if(spec.layout=="NCHW" && attr.layout==TensorLayout::NHWC) {
        require(logical.size()==4,"NCHW rank mismatch");
        logical={logical[0],logical[2],logical[3],logical[1]};
    } else if(spec.layout=="NHWC" && attr.layout==TensorLayout::NCHW) {
        require(logical.size()==4,"NHWC rank mismatch");
        logical={logical[0],logical[3],logical[1],logical[2]};
    } else if(spec.layout!="NCHW" && spec.layout!="NHWC") {
        require(attr.layout==TensorLayout::Unknown,"Non-image runtime tensor has unexpected layout: "+spec.name);
    }
    require(logical==attr.shape.dims && elementCount(spec.shape)==attr.element_count,"Runtime shape mismatch: "+spec.name);
}

Bundle loadBundle(const fs::path& path,const std::string& buildKey,const std::string& selectedCase) {
    Bundle bundle;
    bundle.root=fs::canonical(path);
    bundle.manifest=readJson(bundle.root/"manifest.json");
    require(bundle.manifest.at("format")=="pmatrack.deployment.v1","Unsupported run manifest");
    bundle.build=readJson(artifactPath(bundle.root,"rknn/"+buildKey+"/build_manifest.json"));
    require(bundle.build.at("format")=="pmatrack.build.v1","Unsupported build manifest");
    verifyHash(bundle.root/"manifest.json",bundle.build.at("run_manifest_sha256"));
    bundle.caseName=selectedCase.empty()?bundle.manifest.at("reference_case").get<std::string>():selectedCase;
    bundle.sample=bundle.manifest.at("cases").at(bundle.caseName);
    require(bundle.sample.at("kind")=="reference","Select an image reference case for frame benchmarking");
    for(auto it=bundle.sample.at("external_inputs").begin();it!=bundle.sample.at("external_inputs").end();++it) {
        auto spec=TensorSpec::fromJson(bundle.manifest.at("external_inputs").at(it.key()));
        bundle.external.emplace(it.key(),loadTensor(bundle.root,it.value(),spec));
    }
    require(!bundle.external.empty(),"No external inputs");
    std::map<std::string,TensorSpec> available;
    for(auto it=bundle.manifest.at("external_inputs").begin();it!=bundle.manifest.at("external_inputs").end();++it)
        available.emplace("external:"+it.key(),TensorSpec::fromJson(it.value()));
    std::map<std::string,bool> stageNames,filenames;
    bool predictionFound=false;
    for(const auto& link:bundle.manifest.at("chain")) {
        StagePlan stage;
        stage.name=link.at("stage");
        require(stageNames.emplace(stage.name,true).second && filenames.emplace(safeName(stage.name),true).second,"Duplicate stage name");
        stage.bindings=link.at("bindings").get<std::map<std::string,std::string>>();
        const auto& definition=bundle.manifest.at("stages").at(stage.name);
        const auto& build=bundle.build.at("stages").at(stage.name);
        require(build.at("status")=="PASS","Unsuccessful model build: "+stage.name);
        require(build.at("signature").at("onnx_sha256")==definition.at("onnx").at("sha256"),"Compiled ONNX hash mismatch");
        require(build.at("signature").at("config")==bundle.build.at("config"),"Model compiler configuration mismatch");
        stage.modelPath=artifactPath(bundle.root,build.at("rknn").at("path"));
        stage.modelHash=build.at("rknn").at("sha256");
        verifyHash(stage.modelPath,stage.modelHash);
        for(const auto& v:definition.at("inputs")) stage.inputs.push_back(TensorSpec::fromJson(v));
        for(const auto& v:definition.at("outputs")) stage.outputs.push_back(TensorSpec::fromJson(v));
        require(stage.inputs.size()==stage.bindings.size(),"Incomplete chain bindings");
        for(const auto& spec:stage.inputs) {
            const auto& source=stage.bindings.at(spec.name);
            require(available.count(source)>0,"Unavailable or future chain binding: "+source);
            const auto& previous=available.at(source);
            require(spec.shape==previous.shape && spec.layout==previous.layout && spec.dtype==previous.dtype,"Incompatible chain binding: "+source);
        }
        std::vector<FloatTensor> onnx,torch;
        const auto& reference=bundle.sample.at("stages").at(stage.name);
        for(size_t i=0;i<stage.outputs.size();++i) {
            const auto& spec=stage.outputs[i];
            onnx.push_back(loadTensor(bundle.root,reference.at("golden_onnx").at(spec.name),spec));
            torch.push_back(loadTensor(bundle.root,reference.at("golden_torch").at(spec.name),spec));
            available.emplace(stage.name+":"+spec.name,spec);
            if(spec.name=="pred_boxes") {
                bundle.predictionStage=bundle.chain.size();
                bundle.predictionOutput=i;
                predictionFound=true;
            }
        }
        bundle.goldenOnnx.push_back(std::move(onnx));
        bundle.goldenTorch.push_back(std::move(torch));
        bundle.chain.push_back(std::move(stage));
    }
    require(!bundle.chain.empty() && predictionFound,"Chain must produce pred_boxes");
    require(bundle.predictionStage==bundle.chain.size()-1,"pred_boxes must be produced by the last stage");
    return bundle;
}

Json compareTensor(const FloatTensor& actual,const FloatTensor& golden,bool boxes,double cosMin,double relMax,double boxAtol) {
    Json result={{"status","FAIL"},{"actual_shape",actual.shape.dims},{"golden_shape",golden.shape.dims}};
    if(!actual.valid() || actual.shape.dims!=golden.shape.dims) return result;
    size_t nonfinite=0,saturated=0;
    for(float v:actual.data) {if(!std::isfinite(v)) ++nonfinite; if(std::abs(v)==65536.f) ++saturated;}
    result["nonfinite"]=nonfinite; result["abs65536"]=saturated;
    if(nonfinite) return result;
    double aa=0,gg=0,dot=0,dd=0,peak=0,mean=0;
    for(size_t i=0;i<actual.data.size();++i) {
        const double a=actual.data[i],g=golden.data[i],delta=a-g;
        aa+=a*a; gg+=g*g; dot+=a*g; dd+=delta*delta;
        peak=std::max(peak,std::abs(delta)); mean+=std::abs(delta);
    }
    const double cosine=aa>0 && gg>0?dot/std::sqrt(aa*gg):aa==0 && gg==0?1.:0.;
    const double relative=gg>0?std::sqrt(dd/gg):aa==0?0.:std::numeric_limits<double>::infinity();
    result["cosine"]=cosine;
    result["relative_l2"]=std::isfinite(relative)?Json(relative):Json(nullptr);
    result["max_abs"]=peak; result["mean_abs"]=mean/actual.data.size();
    bool passed=gg==0?peak<=1e-5:cosine>=cosMin && relative<=relMax;
    if(boxes) {
        require(actual.data.size()%4==0 && actual.shape.dims.back()==4,"Invalid cxcywh output shape");
        double minIou=1.;
        for(size_t i=0;i<actual.data.size();i+=4) {
            const double ax=actual.data[i],ay=actual.data[i+1],aw=actual.data[i+2],ah=actual.data[i+3];
            const double gx=golden.data[i],gy=golden.data[i+1],gw=golden.data[i+2],gh=golden.data[i+3];
            const double iw=std::max(0.,std::min(ax+aw/2,gx+gw/2)-std::max(ax-aw/2,gx-gw/2));
            const double ih=std::max(0.,std::min(ay+ah/2,gy+gh/2)-std::max(ay-ah/2,gy-gh/2));
            const double intersection=iw*ih;
            const double uni=std::max(0.,aw)*std::max(0.,ah)+std::max(0.,gw)*std::max(0.,gh)-intersection;
            minIou=std::min(minIou,intersection/std::max(uni,1e-12));
            passed=passed && aw>=0 && ah>=0;
        }
        result["box_atol"]=boxAtol; result["min_box_iou"]=minIou;
        passed=passed && peak<=boxAtol;
    }
    result["status"]=passed?"PASS":"FAIL";
    return result;
}

Json timingStatistics(const std::vector<double>& values) {
    require(!values.empty(),"No timing samples");
    auto sorted=values;
    require(std::all_of(sorted.begin(),sorted.end(),[](double v){return std::isfinite(v)&&v>=0;}),"Invalid timing sample");
    std::sort(sorted.begin(),sorted.end());
    const auto percentile=[&](double p) {
        const double position=p*(sorted.size()-1);
        const auto low=size_t(std::floor(position)),high=size_t(std::ceil(position));
        return sorted[low]+(sorted[high]-sorted[low])*(position-low);
    };
    const double mean=std::accumulate(sorted.begin(),sorted.end(),0.)/sorted.size();
    return {{"count",values.size()},{"mean_ms",mean},{"p50_ms",percentile(.5)},{"p95_ms",percentile(.95)},
            {"min_ms",sorted.front()},{"max_ms",sorted.back()}};
}
} // namespace rkcam::bench
