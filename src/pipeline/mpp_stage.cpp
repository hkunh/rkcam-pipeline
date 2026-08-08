#include "rkcam/pipeline/mpp_stage.hpp"
#include "rkcam/core/log.hpp"

#include <utility>

namespace rkcam {

MppStage::MppStage(
    const MppStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue,
    BlockingQueue<EncodedPacket>& output_queue)
    : config_(config),
      input_queue_(input_queue),
      output_queue_(output_queue),
      encoder_(config.encoder)
{
}

MppStage::~MppStage()
{
    stop();
}

bool MppStage::start()
{
    if(running_)
    {
        return true;
    }
    if(!encoder_.init())
    {
        RKCAM_LOGE("[%s] MppEncoder init failed",
                   config_.stage_name.c_str());
        return false;
    }
    encoded_packets_ = 0;
    failed_frames_ = 0;
    
    requested_idr_count_ = 0;
    applied_idr_count_ = 0;
    failed_idr_count_ = 0;

    pending_idr_min_pts_us_.store(-1, std::memory_order_release);//std::memory_order_release 在我把标志位改成 false 之前，我在此之前准备好的所有配置和数据，必须全都合规写入内存！绝对不许把顺序搞乱

    running_ = true;
    thread_ = std::thread(&MppStage::threadLoop, this);

    
    RKCAM_LOGI("[%s] MppStage started",
               config_.stage_name.c_str());

    return true;
}


void MppStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }
    /*
    * drain stop:
    *
    * input_queue_.stop() 不会丢弃队列中已有 frame。
    * BlockingQueue::pop() 会继续弹出残留 frame。
    * MppStage 会把 input_queue_ 中剩余帧编码完，再退出。
    */
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }

    output_queue_.stop();

    encoder_.close();

    running_ = false;

    RKCAM_LOGI("[%s] MppStage stopped, encoded_packets=%d failed_frames=%d",
               config_.stage_name.c_str(),
               encoded_packets_,
               failed_frames_);  
}
bool MppStage::requestIdr(int64_t min_pts_us)
{
    if (!running_) {
        RKCAM_LOGE(
            "[%s] request IDR failed: stage is not running",
            config_.stage_name.c_str());
        return false;
    }
    if (min_pts_us < 0) {
        RKCAM_LOGE(
            "[%s] requestIdrAfter invalid min_pts=%lld",
            config_.stage_name.c_str(),
            static_cast<long long>(min_pts_us));
        return false;
    }
    int64_t current =
        pending_idr_min_pts_us_.load(
            std::memory_order_acquire);
    /*
     * 合并尚未执行的请求：
     *
     * pending = max(old_pending, new_request)
     */
    while (current < min_pts_us) {
        //compare_exchange(CAS):只有当内存里的值依然是我刚才读到的旧值时，我才写入新值；否则拒绝写入，并告诉我最新值是多少
        if (pending_idr_min_pts_us_
                .compare_exchange_weak(
                    current,
                    min_pts_us,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            break;
        }

        /*
         * CAS失败后，current会自动更新为
         * 最新的pending值，然后继续判断。
         */
    }
    ++requested_idr_count_;
    RKCAM_LOGI(
        "[%s] IDR requested: min_pts=%lld pending=%lld count=%llu",
        config_.stage_name.c_str(),
        static_cast<long long>(min_pts_us),
        static_cast<long long>(
            pending_idr_min_pts_us_.load()),
        static_cast<unsigned long long>(
            requested_idr_count_));

    return true;
}
void MppStage::threadLoop(){
    while(true){
        PipelineVideoFrame frame;
        if(!input_queue_.pop(frame)){
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }

        /*
         * 所有 MPP encoder 操作都由当前编码线程完成，
         * 避免 control() 与 encode() 并发访问 MPP context。
         */
        const int64_t pending_pts = pending_idr_min_pts_us_.load(std::memory_order_acquire);

        if(pending_pts >= 0 && frame.pts_us >= pending_pts)
        {
            if(!encoder_.requestIdr())
            {
                ++failed_idr_count_;
                RKCAM_LOGE(
                    "[%s] apply IDR request failed",
                    config_.stage_name.c_str());
            }
            else{
                ++applied_idr_count_;
                /*
                 * 只清除“刚刚处理的这个请求”。
                 *
                 * 如果在 requestIdr() 执行期间，
                 * 又来了一个更晚的请求，
                 * pending值会已经发生变化，
                 * CAS失败，新请求会被保留下来。
                 */
                int64_t expected = pending_pts;
                const bool cleared = pending_idr_min_pts_us_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel, std::memory_order_acquire);
                RKCAM_LOGI(
                    "[%s] IDR request applied before "
                    "frame=%lld frame_pts=%lld "
                    "request_pts=%lld "
                    "cleared=%d pending_now=%lld",
                    config_.stage_name.c_str(),
                    static_cast<long long>(
                        frame.frame_id),
                    static_cast<long long>(
                        frame.pts_us),
                    static_cast<long long>(
                        pending_pts),
                    cleared ? 1 : 0,
                    static_cast<long long>(
                        pending_idr_min_pts_us_.load(
                            std::memory_order_acquire)));
            }            
        }


        EncodedPacket packet;
        if(!encoder_.encode(frame, packet)){
            ++failed_frames_;
            RKCAM_LOGE("[%s] encode failed, stream=%s frame_id=%lld",
                       config_.stage_name.c_str(),
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id));
            continue;
        }
        /*
        * 有些编码器调用可能成功但没有输出数据。
        * 你的当前 encode() 是阻塞等待 packet 的版本，
        * 正常情况下这里不会为空，但保留这个判断更稳。
        */
        if (packet.empty()) {
            RKCAM_LOGE("[%s] encode output empty packet, stream=%s frame_id=%lld",
                       config_.stage_name.c_str(),
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id));
            continue;
        }
        ++encoded_packets_;
        if(!output_queue_.push(std::move(packet)))
        {
            RKCAM_LOGE("[%s] output_queue push failed",
                       config_.stage_name.c_str());

                        /*
            * 下游已经停止，当前 stage 也停止继续消费上游。
            * 清掉 input_queue 中残留 frame，避免上游 DMA buffer 卡住。
            */
            if (output_queue_.stopped()) {
                input_queue_.stop();
                input_queue_.clear();
                break;
            }
            continue;
        }

    }
    output_queue_.stop();
    running_ = false;

    RKCAM_LOGI("[%s] MppStage thread exit, encoded_packets=%d failed_frames=%d",
               config_.stage_name.c_str(),
               encoded_packets_,
               failed_frames_);
}
bool MppStage::getCodecHeader(
    std::vector<uint8_t>& header)
{
    if (!running_) {
        RKCAM_LOGE(
            "[%s] get codec header failed: stage not running",
            config_.stage_name.c_str());

        return false;
    }

    if (!encoder_.getCodecHeader(header)) {
        RKCAM_LOGE(
            "[%s] get codec header failed",
            config_.stage_name.c_str());

        return false;
    }

    RKCAM_LOGI(
        "[%s] got codec header: size=%zu",
        config_.stage_name.c_str(),
        header.size());

    return true;
}
}