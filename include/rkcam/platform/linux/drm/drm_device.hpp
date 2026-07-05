#pragma once

#include "rkcam/platform/linux/drm/drm_types.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <string>
#include <unordered_map>


namespace rkcam{

struct DrmDeviceConfig{
    std::string device = "/dev/dri/card0";
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    uint32_t plane_id = 0;

    PixelFormat format = PixelFormat::NV12;

    bool clear_plane_on_close = true;
};

class DrmDevice{
public:
    explicit DrmDevice(const DrmDeviceConfig& config);
    ~DrmDevice();

    DrmDevice(const DrmDevice&) = delete;
    DrmDevice& operator=(const DrmDevice&) = delete;

    bool open();
    void close();

    bool isOpen() const;

    int fd() const;
    uint32_t connectorId() const;
    uint32_t crtcId() const;
    uint32_t planeId() const;

    /*
     * 输入通用 dma_fd 描述，内部 import/cache 成 fb_id。
     */
    bool importFrameToFb(const DrmFrameDesc& frame, uint32_t& fb_id);


    /*
     * 只负责把 fb_id 提交到 plane。
     */
    bool displayFb(
        uint32_t fb_id,
        const DrmDisplayRect& dst_rect,
        int src_width,
        int src_height);

private:
    struct DmaBufPlaneKey{
        uint64_t dev = 0;
        uint64_t ino = 0;
        uint64_t size = 0;

        uint32_t offset = 0;
        uint32_t pitch = 0;

        bool operator==(const DmaBufPlaneKey& other) const;
    };

    struct DmaBufKey{
        int width = 0;
        int height = 0;
        PixelFormat format = PixelFormat::Unknown;

        DmaBufPlaneKey y;
        DmaBufPlaneKey uv;

        bool operator==(const DmaBufKey& other) const;
    };

    struct DmaBufKeyHash{
        size_t operator()(const DmaBufKey& key) const;
    };

    struct ImportedFb{
        uint32_t fb_id = 0;
        uint32_t handles[4] {};
        int handle_count = 0;
    };

private:
    bool validateTarget();
    bool planeSupportsFormat(uint32_t format) const;
    bool crtcAllowedForPlane();

    bool makeDmaBufKey(const DrmFrameDesc& frame, DmaBufKey& key) const;
    bool importFrameToFbSlow(const DrmFrameDesc& frame, ImportedFb& imported);

    void releaseImportedFb(ImportedFb& imported);
    void clearImportedFbCache();

    static uint32_t drmFormatFromPixelFormat(PixelFormat format);

private:
    DrmDeviceConfig config_;
    int fd_ = -1;

    uint32_t connector_id_ = 0;
    uint32_t crtc_id_ = 0;
    uint32_t plane_id_ = 0;

    std::unordered_map<DmaBufKey, ImportedFb, DmaBufKeyHash> fb_cache_;

    bool opened_ = false;
};


}