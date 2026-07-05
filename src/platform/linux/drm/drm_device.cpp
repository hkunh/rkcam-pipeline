#include "rkcam/platform/linux/drm/drm_device.hpp"

#include "rkcam/core/log.hpp"

#include <drm_fourcc.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
// 【外部输入】
//  DrmFrameDesc (原始帧描述)
//  ├── width / height / format (图像宽高和格式)
//  └── planes[0], planes[1] ... (DrmPlaneDesc 数组)
//        └── dma_fd (Linux 内存文件句柄)
//        └── offset / pitch (偏移和步长)

//                                │
//                                ▼ [通过 makeDmaBufKey 转换]
// 【哈希查找键】
//  DmaBufKey (缓存的唯一指纹 Key)
//  ├── width / height / format
//  ├── y  (DmaBufPlaneKey) ───► 包含通过 fstat(dma_fd) 拿到的:
//  └── uv (DmaBufPlaneKey)      dev (设备号), ino (i节点), size (大小)
//                               以及 offset, pitch
//                                │
//                                ▼ [在 fb_cache_ 中做哈希映射 Map ]
// 【哈希值/输出结果】
//  ImportedFb (DRM 登记结果)
//  ├── fb_id (由 drmModeAddFB2 生成的、硬件直接认识的缓冲ID)
//  ├── handles[4] (通过 Gem 转换后的显存句柄)
//  └── handle_count (句柄数量)
namespace rkcam {
namespace {

void hashCombine(size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

bool fillPlaneKeyFromFd(int fd, uint32_t offset, uint32_t pitch, uint64_t& dev, uint64_t& ino, uint64_t& size)
{
    if(fd < 0)
    {
        return false;
    }

    struct stat st {};
    if(fstat(fd, &st) != 0){
        return false;
    }

    dev = static_cast<uint64_t>(st.st_dev);
    ino = static_cast<uint64_t>(st.st_ino);
    size = static_cast<uint64_t>(st.st_size);

    (void) offset;
    (void) pitch;

    return true;
}
} //namespace


bool DrmDevice::DmaBufPlaneKey::operator==(const DmaBufPlaneKey& other) const
{
    return dev == other.dev &&
            ino == other.ino &&
            size == other.size &&
            offset == other.offset;
            pitch == other.pitch;
}
bool DrmDevice::DmaBufKey::operator==(
    const DmaBufKey& other) const
{
    return width == other.width &&
           height == other.height &&
           format == other.format &&
           y == other.y &&
           uv == other.uv;
}

size_t DrmDevice::DmaBufKeyHash::operator()(const DmaBufKey& key) const
{
    size_t seed = 0;

    hashCombine(seed, static_cast<size_t>(key.width));
    hashCombine(seed, static_cast<size_t>(key.height));
    hashCombine(seed, static_cast<size_t>(key.format));

    hashCombine(seed, static_cast<size_t>(key.y.dev));
    hashCombine(seed, static_cast<size_t>(key.y.ino));
    hashCombine(seed, static_cast<size_t>(key.y.size));
    hashCombine(seed, static_cast<size_t>(key.y.offset));
    hashCombine(seed, static_cast<size_t>(key.y.pitch));

    hashCombine(seed, static_cast<size_t>(key.uv.dev));
    hashCombine(seed, static_cast<size_t>(key.uv.ino));
    hashCombine(seed, static_cast<size_t>(key.uv.size));
    hashCombine(seed, static_cast<size_t>(key.uv.offset));
    hashCombine(seed, static_cast<size_t>(key.uv.pitch));

    return seed;

}

DrmDevice::DrmDevice(const DrmDeviceConfig& config)
    : config_(config)
{
}

DrmDevice::~DrmDevice()
{
    close();
}

bool DrmDevice::open()
{
    if(opened_)
    {
        return true;
    }

    fd_ = ::open(config_.device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        RKCAM_LOGE("DrmDevice open failed: device=%s errno=%d(%s)",
                   config_.device.c_str(),
                   errno,
                   std::strerror(errno));
        return false;
    }

    /*
    * 启用 universal planes，否则 overlay / cursor plane 可能看不到。
    */
    if(drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
    {
    RKCAM_LOGE("DrmDevice warning: set UNIVERSAL_PLANES failed errno=%d(%s)",
                errno,
                std::strerror(errno));
    }

    connector_id_ = config_.connector_id;
    crtc_id_ = config_.crtc_id;
    plane_id_ = config_.plane_id;

    if (!validateTarget()) {
        RKCAM_LOGE("DrmDevice validateTarget failed: connector=%u crtc=%u plane=%u",
                   connector_id_,
                   crtc_id_,
                   plane_id_);
        close();
        return false;
    }

    opened_ = true;
    RKCAM_LOGI("DrmDevice opened: device=%s connector=%u crtc=%u plane=%u format=%d",
               config_.device.c_str(),
               connector_id_,
               crtc_id_,
               plane_id_,
               static_cast<int>(config_.format));

    return true;
}

void DrmDevice::close()
{
    if(fd_ >= 0 && config_.clear_plane_on_close)
    {
        /*
        * @brief 使用硬件 Plane 将帧缓冲显示到屏幕上
        * 
        * @param fd           DRM 设备的文件描述符 (fd_)
        * @param plane_id     要操作的硬件图层 ID (plane_id_)
        * @param crtc_id      目标 CRTC (显示控制器) 的 ID (若为 0 表示禁用此 Plane)
        * @param fb_id        要显示的帧缓冲 ID (若为 0 表示禁用此 Plane)
        * @param flags        操作标志位 (通常为 0)
        * 
        * -- 目标显示区域 (Destination) --
        * @param crtc_x       在屏幕上的 X 坐标
        * @param crtc_y       在屏幕上的 Y 坐标
        * @param crtc_w       在屏幕上显示的宽度
        * @param crtc_h       在屏幕上显示的高度
        * 
        * -- 源内存裁剪区域 (Source, 固定点数格式，16.16定点数) --
        * @param src_x        源图像的起始 X 偏移 (src_x << 16)
        * @param src_y        源图像的起始 Y 偏移 (src_y << 16)
        * @param src_w        源图像显示的宽度 (src_w << 16)
        * @param src_h        源图像显示的高度 (src_h << 16)
        */
        drmModeSetPlane(fd_, plane_id_, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    clearImportedFbCache();
    if(fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }

    connector_id_ = 0;
    crtc_id_ = 0;
    plane_id_ = 0;
    opened_ = false;

    RKCAM_LOGI("DrmDevice closed");
}
bool DrmDevice::isOpen() const
{
    return opened_;
}

int DrmDevice::fd() const
{
    return fd_;
}

uint32_t DrmDevice::connectorId() const
{
    return connector_id_;
}

uint32_t DrmDevice::crtcId() const
{
    return crtc_id_;
}

uint32_t DrmDevice::planeId() const
{
    return plane_id_;
}
uint32_t DrmDevice::drmFormatFromPixelFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
        return DRM_FORMAT_NV12;
    default:
        return 0;
    }
}
bool DrmDevice::validateTarget()
{
    if(fd_ < 0)
    {
        return false;
    }
    if (connector_id_ == 0 || crtc_id_ == 0 || plane_id_ == 0) {
        RKCAM_LOGE("DrmDevice invalid ids: connector=%u crtc=%u plane=%u",
                   connector_id_,
                   crtc_id_,
                   plane_id_);
        return false;
    }

    drmModeConnector* conn = drmModeGetConnector(fd_, connector_id_);
    if(!conn)
    {
        RKCAM_LOGE("DrmDevice invalid ids: connector=%u crtc=%u plane=%u",
                   connector_id_,
                   crtc_id_,
                   plane_id_);
        return false;
    }

    const bool connected = conn->connection == DRM_MODE_CONNECTED;
    drmModeFreeConnector(conn);

    if(!connected)
    {
        RKCAM_LOGE("DRM connector is not connected: connector=%u",
                   connector_id_);
        return false;
    }

    drmModeCrtc* crtc = drmModeGetCrtc(fd_, crtc_id_);
    if (!crtc) {
        RKCAM_LOGE("drmModeGetCrtc failed: crtc=%u errno=%d(%s)",
                   crtc_id_,
                   errno,
                   std::strerror(errno));
        return false;
    }
    drmModeFreeCrtc(crtc);

    drmModePlane* plane = drmModeGetPlane(fd_, plane_id_);
    if (!plane) {
        RKCAM_LOGE("drmModeGetPlane failed: plane=%u errno=%d(%s)",
                   plane_id_,
                   errno,
                   std::strerror(errno));
        return false;
    }
    drmModeFreePlane(plane);

    const uint32_t drm_format = drmFormatFromPixelFormat(config_.format);
    if (drm_format == 0) {
        RKCAM_LOGE("unsupported display format=%d",
                   static_cast<int>(config_.format));
        return false;
    }

    if(!planeSupportsFormat(drm_format))
    {
        RKCAM_LOGE("plane %u does not support DRM format=0x%08x",
                   plane_id_,
                   drm_format);
        return false;
    }

    if (!crtcAllowedForPlane()) {
        RKCAM_LOGE("plane %u is not allowed for crtc %u",
                   plane_id_,
                   crtc_id_);
        return false;
    }

    return true;

}

bool DrmDevice::planeSupportsFormat(uint32_t drm_format) const
{
    if(fd_ < 0 || plane_id_ == 0)
    {
        return false;
    }

    drmModePlane* plane = drmModeGetPlane(fd_, plane_id_);
    if(!plane)
    {
        RKCAM_LOGE("drmModeGetPlane failed: plane=%u errno=%d(%s)",
                   plane_id_,
                   errno,
                   std::strerror(errno));
        return false;
    }

    bool supported = false;
    for(uint32_t i = 0; i < plane->count_formats; i++)
    {
        if(plane->formats[i] == drm_format){
            supported = true;
            break;
        }

    }

    drmModeFreePlane(plane);
    return supported;

}

bool DrmDevice::crtcAllowedForPlane()
{
    if (fd_ < 0 || crtc_id_ == 0 || plane_id_ == 0) {
        return false;
    }
    drmModeRes* res = drmModeGetResources(fd_);
    if (!res) {
        RKCAM_LOGE("drmModeGetResources failed errno=%d(%s)",
                   errno,
                   std::strerror(errno));
        return false;
    }

    int crtc_index = -1;
    for(int i = 0; i < res->count_crtcs; i++)
    {
        if(res->crtcs[i] == crtc_id_)
        {
            crtc_index = i;
            break;
        }
    }

    if(crtc_index < 0)
    {
        RKCAM_LOGE("cannot find crtc index for crtc=%u",
                   crtc_id_);
        drmModeFreeResources(res);
        return false;
    }

    drmModePlane* plane = drmModeGetPlane(fd_, plane_id_);
    if (!plane) {
        RKCAM_LOGE("drmModeGetPlane failed: plane=%u errno=%d(%s)",
                   plane_id_,
                   errno,
                   std::strerror(errno));
        drmModeFreeResources(res);
        return false;
    }

    const bool allowed = (plane->possible_crtcs & (1u << crtc_index)) != 0;
    drmModeFreePlane(plane);
    drmModeFreeResources(res);
    return allowed;
}

bool DrmDevice::importFrameToFb(const DrmFrameDesc& frame, uint32_t& fb_id)
{
    fb_id = 0;
    if(!opened_ || fd_ < 0)
    {
        RKCAM_LOGE("importFrameToFb failed: DrmDevice is not opened");
        return false;
    }

    if(frame.width <= 0 || frame.height <= 0)
    {
        RKCAM_LOGE("importFrameToFb failed: invalid size=%dx%d",
                   frame.width,
                   frame.height);
        return false;
    }
    if (frame.format != config_.format) {
        RKCAM_LOGE("importFrameToFb failed: format mismatch frame=%d device=%d",
                   static_cast<int>(frame.format),
                   static_cast<int>(config_.format));
        return false;
    }

    if (frame.format != PixelFormat::NV12) {
        RKCAM_LOGE("importFrameToFb failed: only NV12 is supported now");
        return false;
    }

    DmaBufKey key;
    if(!makeDmaBufKey(frame, key))
    {
        RKCAM_LOGE("importFrameToFb failed: makeDmaBufKey failed");
        return false;
    }

    auto it = fb_cache_.find(key);
    if(it != fb_cache_.end())
    {
        fb_id = it->second.fb_id;
        return fb_id != 0;
    }

    ImportedFb imported;
    if(!importFrameToFbSlow(frame, imported))
    {
        RKCAM_LOGE("importFrameToFb failed: importFrameToFbSlow failed");
        return false;
    }

    fb_id = imported.fb_id;
    fb_cache_.emplace(key, imported);


    RKCAM_LOGI("DrmDevice imported new dma-buf to fb: fb_id=%u cache_size=%zu",
               fb_id,
               fb_cache_.size());

    return fb_id != 0;
}

bool DrmDevice::makeDmaBufKey(const DrmFrameDesc& frame, DmaBufKey& key) const
{
    key = DmaBufKey{};
    key.width = frame.width;
    key.height = frame.height;
    key.format = frame.format;

    if (frame.format != PixelFormat::NV12) {
        return false;
    }

    /*
     * NV12 推荐 DrmFrameDesc 填两层：
     *   planes[0] = Y
     *   planes[1] = UV
     *
     * 如果只填 1 层，则默认 single-fd layout:
     *   UV offset = pitch * height
     */
    if (frame.plane_count <= 0)
    {
        return false;
    }
    const DrmPlaneDesc& y = frame.planes[0];

    if(y.dma_fd < 0 || y.pitch == 0)
    {
        return false;
    }

    uint32_t uv_fd = y.dma_fd;
    uint32_t uv_offset = y.pitch * static_cast<uint32_t>(frame.height);
    uint32_t uv_pitch = y.pitch;

    if(frame.plane_count >= 2){
        const DrmPlaneDesc& uv = frame.planes[1];

        if(uv.dma_fd >= 0)
        {
            uv_fd = static_cast<uint32_t>(uv.dma_fd);
        }

        if(uv.pitch > 0)
        {
            uv_pitch = uv.pitch;
        }
        uv_offset = uv.offset;
    }

    if(!fillPlaneKeyFromFd(y.dma_fd, y.offset, y.pitch, key.y.dev, key.y.ino, key.y.size))
    {
        RKCAM_LOGE("makeDmaBufKey failed: fstat Y fd=%d errno=%d(%s)",
                   y.dma_fd,
                   errno,
                   std::strerror(errno));
        return false;
    }

    key.y.offset = y.offset;
    key.y.pitch = y.offset;

    if(!fillPlaneKeyFromFd(static_cast<int>(uv_fd), uv_offset, uv_pitch, key.uv.dev, key.uv.ino, key.uv.size))
    {
        RKCAM_LOGE("makeDmaBufKey failed: fstat UV fd=%d errno=%d(%s)",
                   static_cast<int>(uv_fd),
                   errno,
                   std::strerror(errno));
        return false;
    }

    key.uv.offset = uv_offset;
    key.uv.pitch = uv_pitch;

    return true;
}

bool DrmDevice::importFrameToFbSlow(const DrmFrameDesc& frame, ImportedFb& imported)
{
    imported = ImportedFb{};

    if(frame.format != PixelFormat::NV12){
        return false;
    }
    if(frame.plane_count <= 0)
    {
        return false;
    }

    const DrmPlaneDesc& y = frame.planes[0];
    if (y.dma_fd < 0 || y.pitch == 0) {
        return false;
    }

    int uv_fd = y.dma_fd;
    uint32_t uv_offset = y.pitch * static_cast<uint32_t>(frame.height);
    uint32_t uv_pitch = y.pitch;

    if(frame.plane_count >= 2){
        const DrmPlaneDesc& uv = frame.planes[1];

        if(uv.dma_fd >= 0)
        {
            uv_fd = uv.dma_fd;
        }
        if (uv.pitch > 0) {
            uv_pitch = uv.pitch;
        }

        uv_offset = uv.offset;
    }

    uint32_t handle_y = 0;
    uint32_t handle_uv = 0;

    if(drmPrimeFDToHandle(fd_, y.dma_fd, &handle_y) != 0){
        RKCAM_LOGE("drmPrimeFDToHandle Y failed fd=%d errno=%d(%s)",
                   y.dma_fd,
                   errno,
                   std::strerror(errno));
        return false;
    }

    if(uv_fd == y.dma_fd)
    {
        handle_uv = handle_y;
    }
    else{
        if(drmPrimeFDToHandle(fd_, uv_fd, &handle_uv) != 0)
        {
            RKCAM_LOGE("drmPrimeFDToHandle UV failed fd=%d errno=%d(%s)",
                       uv_fd,
                       errno,
                       std::strerror(errno));

            drm_gem_close close_y {};
            close_y.handle = handle_y;
            drmIoctl(fd_, DRM_IOCTL_GEM_CLOSE, &close_y);

            return false;
        }

    }

    uint32_t handles[4] {};
    uint32_t pitches[4] {};
    uint32_t offsets[4] {};

    handles[0] = handle_y;
    handles[1] = handle_uv;

    pitches[0] = y.pitch;
    pitches[1] = uv_pitch;

    offsets[0] = y.offset;
    offsets[1] = uv_offset;

    uint32_t fb_id = 0;

    if(drmModeAddFB2(
        fd_, 
        static_cast<uint32_t>(frame.width),
        static_cast<uint32_t>(frame.height),
        DRM_FORMAT_NV12,
        handles,
        pitches,
        offsets,
        &fb_id,
        0
    ) != 0)
    {
        RKCAM_LOGE("drmModeAddFB2 NV12 failed width=%d height=%d "
                        "pitch_y=%u pitch_uv=%u offset_y=%u offset_uv=%u errno=%d(%s)",
                        frame.width,
                        frame.height,
                        y.pitch,
                        uv_pitch,
                        y.offset,
                        uv_offset,
                        errno,
                        std::strerror(errno));

        drm_gem_close close_y {};
        close_y.handle = handle_y;
        drmIoctl(fd_, DRM_IOCTL_GEM_CLOSE, &close_y);

        if(handle_uv != handle_y)
        {
            drm_gem_close close_uv{};
            close_uv.handle = handle_uv;
            drmIoctl(fd_, DRM_IOCTL_GEM_CLOSE, &close_uv);
        }

        return false;
    }

    imported.fb_id = fb_id;
    imported.handles[0] = handle_y;
    imported.handle_count = 1;

    if(handle_uv != handle_y)
    {
        imported.handles[1] = handle_uv;
        imported.handle_count = 2;
    }
    return true;


}


void DrmDevice::releaseImportedFb(ImportedFb& imported)
{
    if (fd_ < 0) {
        return;
    }

    if(imported.fb_id != 0)
    {
        drmModeRmFB(fd_, imported.fb_id);
        imported.fb_id = 0;
    }

    for(int i = 0; i < imported.handle_count; i++)
    {
        if(imported.handles[i] != 0)
        {
            drm_gem_close close_arg{};
            close_arg.handle = imported.handles[i];

            if(drmIoctl(fd_, DRM_IOCTL_GEM_CLOSE, &close_arg) != 0)
            {
                RKCAM_LOGE("DRM_IOCTL_GEM_CLOSE failed handle=%u errno=%d(%s)",
                           imported.handles[i],
                           errno,
                           std::strerror(errno));
            }
            imported.handles[i] = 0;
        }
    }
}

void DrmDevice::clearImportedFbCache()
{
    for (auto& kv : fb_cache_) {
        releaseImportedFb(kv.second);
    }

    fb_cache_.clear();
}

bool DrmDevice::displayFb(uint32_t fb_id,
    const DrmDisplayRect& dst_rect,
    int src_width,
    int src_height)
{
    if (!opened_ || fd_ < 0) {
        RKCAM_LOGE("displayFb failed: DrmDevice is not opened");
        return false;
    }

    if (fb_id == 0) {
        RKCAM_LOGE("displayFb failed: fb_id is 0");
        return false;
    }

    if (src_width <= 0 || src_height <= 0) {
        RKCAM_LOGE("displayFb failed: invalid src size=%dx%d",
                   src_width,
                   src_height);
        return false;
    }

    if (dst_rect.width <= 0 || dst_rect.height <= 0) {
        RKCAM_LOGE("displayFb failed: invalid dst rect=(%d,%d,%d,%d)",
                   dst_rect.x,
                   dst_rect.y,
                   dst_rect.width,
                   dst_rect.height);
        return false;
    }


    const int ret = drmModeSetPlane(
        fd_,
        plane_id_,    // 1. 明确指定操作哪一个硬件图层（比如专门给视频硬解码用的 Overlay 图层）
        crtc_id_,     // 2. 明确这个图层属于哪一个显示控制器（送给哪个屏幕）
        fb_id,        // 3. 这一帧画面的 Framebuffer ID
        0,            // 4. Flags，通常为 0
        /* 目标区域 (Destination) -> 画面显示在屏幕的什么坐标、多大尺寸 */
        static_cast<int32_t>(dst_rect.x),
        static_cast<int32_t>(dst_rect.y),
        static_cast<uint32_t>(dst_rect.width),
        static_cast<uint32_t>(dst_rect.height),
        /* 源区域 (Source) -> 从原始图片里裁剪哪一部分，注意这里的 << 16 */
        0, 0,
        static_cast<uint32_t>(src_width) << 16,
        static_cast<uint32_t>(src_height) << 16
    );

    if (ret != 0) {
        RKCAM_LOGE("drmModeSetPlane failed plane=%u crtc=%u fb=%u "
                   "dst=(%d,%d,%d,%d) src=%dx%d errno=%d(%s)",
                   plane_id_,
                   crtc_id_,
                   fb_id,
                   dst_rect.x,
                   dst_rect.y,
                   dst_rect.width,
                   dst_rect.height,
                   src_width,
                   src_height,
                   errno,
                   std::strerror(errno));
        return false;
    }

    return true;
}

}