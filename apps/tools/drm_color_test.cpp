#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;
void signalHandler(int)
{
    g_running = 0;
}
struct DrmDumbBuffer {
    uint32_t width = 0;
    uint32_t height = 0;

    uint32_t bpp = 32; //Bits Per Pixel，每像素位数

    uint32_t handle = 0;
    uint32_t fb_id = 0;

    uint32_t pitch = 0; //步长 / 字节一行 在物理内存中，这一行像素实际占用了多少个字节（Byte）
    uint64_t size = 0;

    void* map = nullptr;
};
struct DrmTarget{
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    drmModeModeInfo mode {};
};
const char* connectorTypeToString(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_DSI:
        return "DSI";
    case DRM_MODE_CONNECTOR_HDMIA:
        return "HDMI-A";
    case DRM_MODE_CONNECTOR_eDP:
        return "eDP";
    case DRM_MODE_CONNECTOR_LVDS:
        return "LVDS";
    case DRM_MODE_CONNECTOR_VIRTUAL:
        return "Virtual";
    default:
        return "Other";
    }
}
bool findConnectedTarget(int fd, DrmTarget& target)
{
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        std::perror("drmModeGetResources failed");
        return false;
    }

    bool found = false;

    for (int i = 0; i < res->count_connectors && !found; i++)
    {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) {
            continue;
        }

        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes <= 0) {
            drmModeFreeConnector(conn);
            continue;
        }

        std::printf("Found connected connector: id=%u type=%s-%u modes=%d encoder_id=%u\n",
                    conn->connector_id,
                    connectorTypeToString(conn->connector_type),
                    conn->connector_type_id,
                    conn->count_modes,
                    conn->encoder_id);

        uint32_t crtc_id = 0;
        /*
         * 优先使用当前 encoder 绑定的 CRTC。
         * 你的日志里 DSI encoder 149 当前 crtc_id=85。
         */
        if(conn->encoder_id != 0)
        {
            drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
            if (enc) {
                crtc_id = enc->crtc_id;
                std::printf("Current encoder id=%u crtc_id=%u possible_crtcs=0x%x\n",
                            enc->encoder_id,
                            enc->crtc_id,
                            enc->possible_crtcs);
                drmModeFreeEncoder(enc);
            }
        }
        /*
         * 如果当前没有绑定 CRTC，则从可用 CRTC 里选一个。
         */
        if(crtc_id == 0)
        {
            for(int e = 0; e < conn->count_encoders; ++e)
            {
                drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[e]);
                if (!enc) {
                    continue;
                }

                for(int c = 0; c < res->count_crtcs; ++c)
                {
                    if(enc->possible_crtcs & (1u << c))
                    {
                        crtc_id = res->crtcs[c];
                        break;
                    }
                }
                drmModeFreeEncoder(enc);
            }
        }
        if (crtc_id == 0) {
            std::printf("No usable CRTC found for connector %u\n",
                        conn->connector_id);
            drmModeFreeConnector(conn);
            continue;
        }

        target.connector_id = conn->connector_id;
        target.crtc_id = crtc_id;
        target.mode = conn->modes[0];

        std::printf("Use connector=%u crtc=%u mode=%s %ux%u vrefresh=%d\n",
                    target.connector_id,
                    target.crtc_id,
                    target.mode.name,
                    target.mode.hdisplay,
                    target.mode.vdisplay,
                    target.mode.vrefresh);

        found = true;
        drmModeFreeConnector(conn);

    }
    drmModeFreeResources(res);
    return found;
}
bool createDumbBuffer(int fd, uint32_t width, uint32_t height, DrmDumbBuffer& buffer)
{
    buffer = DrmDumbBuffer{};
    buffer.width = width;
    buffer.height = height;
    buffer.bpp = 32; //Bits Per Pixel，每像素位数

    drm_mode_create_dumb create {};
    create.width = width;
    create.height = height;
    create.bpp = buffer.bpp;
    if(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
    {
        std::perror("DRM_IOCTL_MODE_CREATE_DUMB failed");
        return false;
    }
    buffer.handle = create.handle;
    buffer.pitch = create.pitch;
    buffer.size = create.size; //整块显存最终的物理总字节数

    std::printf("Dumb buffer created: %ux%u bpp=%u handle=%u pitch=%u size=%llu\n",
                buffer.width,
                buffer.height,
                buffer.bpp,
                buffer.handle,
                buffer.pitch,
                static_cast<unsigned long long>(buffer.size));

    uint32_t handles[4] = {};
    uint32_t pitches[4] = {};
    uint32_t offsets[4] = {};

    handles[0] = buffer.handle;
    pitches[0] = buffer.pitch;
    offsets[0] = 0;

    /*
     * XR24 = DRM_FORMAT_XRGB8888。
     * 你的 Primary Plane 支持 XR24。
     */
    // int drmModeAddFB2(
    //     int fd,                  // 1. 显卡设备文件描述符
    //     uint32_t width,          // 2. 画布的像素宽度（如 1080）
    //     uint32_t height,         // 3. 画布的像素高度（如 1920）
    //     uint32_t pixel_format,   // 4. 像素颜色格式宏
    //     const uint32_t handles[4],// 5. 内存句柄数组（关键！）
    //     const uint32_t pitches[4], // 6. 字节步长数组
    //     const uint32_t offsets[4], // 7. 内存偏移量数组
    //     uint32_t *buf_id,        // 8. 输出：返回的唯一画布 ID（fb_id）
    //     uint32_t flags           // 9. 特殊标志位（通常填 0）
    // );
    int ret = drmModeAddFB2(fd,
                           buffer.width,
                           buffer.height,
                           DRM_FORMAT_XRGB8888,
                           handles,
                           pitches,
                           offsets,
                           &buffer.fb_id,
                           0);
    if (ret != 0) {
        std::perror("drmModeAddFB2 DRM_FORMAT_XRGB8888 failed");

        drm_mode_destroy_dumb destroy {};
        destroy.handle = buffer.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

        buffer = DrmDumbBuffer{};
        return false;
    }
    std::printf("Framebuffer added: fb_id=%u format=XR24/XRGB8888\n",
                buffer.fb_id);

    drm_mode_map_dumb map_req {};
    map_req.handle = buffer.handle;
    if(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0)
    {
        std::perror("DRM_IOCTL_MODE_MAP_DUMB failed");
        drmModeRmFB(fd, buffer.fb_id);

        drm_mode_destroy_dumb destory{};
        destory.handle = buffer.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destory);

        buffer = DrmDumbBuffer{};
        return false;
    }

    buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_req.offset);
    if (buffer.map == MAP_FAILED) {
        std::perror("mmap dumb buffer failed");
        buffer.map = nullptr;

        drmModeRmFB(fd, buffer.fb_id);

        drm_mode_destroy_dumb destroy {};
        destroy.handle = buffer.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

        buffer = DrmDumbBuffer{};
        return false;
    }

    std::printf("Dumb buffer mapped: map=%p\n", buffer.map);
    return true;
}

void destroyDumbBuffer(int fd, DrmDumbBuffer& buffer)
{
    if (buffer.map) {
        munmap(buffer.map, buffer.size);
        buffer.map = nullptr;
    }

    if (buffer.fb_id != 0) {
        drmModeRmFB(fd, buffer.fb_id);
        buffer.fb_id = 0;
    }

    if (buffer.handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = buffer.handle;

        if (drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) != 0) {
            std::perror("DRM_IOCTL_MODE_DESTROY_DUMB failed");
        }

        buffer.handle = 0;
    }
}
void fillColorXrgb8888(DrmDumbBuffer& buffer, uint32_t color)
{
    if (!buffer.map) {
        return;
    }

    /*
     * DRM_FORMAT_XRGB8888:
     *   uint32_t value = 0x00RRGGBB
     *
     * 例如：
     *   red   = 0x00ff0000
     *   green = 0x0000ff00
     *   blue  = 0x000000ff
     */
    uint8_t* base = static_cast<uint8_t*>(buffer.map);

    for (uint32_t y = 0; y < buffer.height; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(base + y * buffer.pitch);

        for (uint32_t x = 0; x < buffer.width; ++x) {
            row[x] = color;
        }
    }
}

bool showBuffer(int fd, const DrmTarget& target, const DrmDumbBuffer& buffer)
{
    uint32_t connector_id = target.connector_id;
    // int drmModeSetCrtc(
    //     int fd,
    //     uint32_t crtc_id,       // 1. 点名让哪位“大厨”上班
    //     uint32_t fb_id,         // 2. 给大厨塞一张“默认画布”
    //     uint32_t x, uint32_t y, // 3. 画布的抓取起点
    //     uint32_t *connectors,   // 4. 强行指定这条链路去往哪个“物理插座”
    //     uint32_t count,         // 5. 插座的数量（通常是 1）
    //     drmModeModeInfoPtr mode // 6. 给屏幕注入硬核“物理显示时序（脉冲）”
    // );
    //一次性强行焊死一整条完整的显示输出链路
    int ret = drmModeSetCrtc(fd, target.crtc_id, buffer.fb_id, 0, 0, &connector_id, 1, const_cast<drmModeModeInfoPtr>(&target.mode));
    if (ret != 0) {
        std::perror("drmModeSetCrtc failed");
        return false;
    }

    return true;
}
} //namespace


int main(int argc, char** argv)
{
    const char* device = "/dev/dri/card0";
    int seconds_per_color = 5;
    if(argc >= 2)
    {
        device = argv[1];
    }

    if(argc >= 3)
    {
        seconds_per_color = std::atoi(argv[2]);
        if(seconds_per_color <= 0){
            seconds_per_color = 5;
        }
    }
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::printf("DRM color test\n");
    std::printf("device=%s seconds_per_color=%d\n",
                device,
                seconds_per_color);

    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open DRM device failed");
        return 1;
    }

    DrmTarget target;
    if (!findConnectedTarget(fd, target)) {
        std::printf("No connected DRM target found\n");
        close(fd);
        return 1;
    }

    /*
     * 保存原 CRTC，程序结束时恢复原显示。
     */
    drmModeCrtc* old_crtc = drmModeGetCrtc(fd, target.crtc_id);
    if (!old_crtc) {
        std::printf("warning: drmModeGetCrtc old_crtc failed\n");
    }
    DrmDumbBuffer buffer;
    if (!createDumbBuffer(
            fd,
            target.mode.hdisplay,
            target.mode.vdisplay,
            buffer)) {
        if (old_crtc) {
            drmModeFreeCrtc(old_crtc);
        }

        close(fd);
        return 1;
    }

    struct ColorItem{
        const char* name;
        uint32_t value;
    };
    const std::vector<ColorItem> colors = {
        {"red",     0x00ff0000},
        {"green",   0x0000ff00},
        {"blue",    0x000000ff},
        {"white",   0x00ffffff},
        {"black",   0x00000000},
        {"gray",    0x00808080},
        {"magenta", 0x00ff00ff},
        {"cyan",    0x0000ffff},
        {"yellow",  0x00ffff00},
    };
    std::printf("Start color loop. Press Ctrl+C to stop.\n");

    size_t index = 0;

    while (g_running) {
        const ColorItem& color = colors[index % colors.size()];

        std::printf("show color: %s value=0x%08x\n",
                    color.name,
                    color.value);

        fillColorXrgb8888(buffer, color.value);

        if (!showBuffer(fd, target, buffer)) {
            break;
        }

        for (int i = 0; i < seconds_per_color && g_running; ++i) {
            sleep(1);
        }

        ++index;
    }

    std::printf("Stopping, restore old CRTC...\n");

    if(old_crtc->mode_valid)
    {
        uint32_t connector_id = target.connector_id;

        if(old_crtc->mode_valid)
        {
            drmModeSetCrtc(fd, old_crtc->crtc_id, old_crtc->buffer_id, old_crtc->x, old_crtc->y, &connector_id, 1, &old_crtc->mode);
        }else {
            drmModeSetCrtc(
                fd,
                old_crtc->crtc_id,
                0,
                0,
                0,
                nullptr,
                0,
                nullptr);
        }

        drmModeFreeCrtc(old_crtc);
    }
    destroyDumbBuffer(fd, buffer);
    close(fd);

    std::printf("Done.\n");
    return 0;
}