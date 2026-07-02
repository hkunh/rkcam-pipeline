#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>


std::string fourccToString(uint32_t format)
{
    char s[5];
    s[0] = static_cast<char>(format & 0xff);
    s[1] = static_cast<char>((format >> 8) & 0xff);
    s[2] = static_cast<char>((format >> 16) & 0xff);
    s[3] = static_cast<char>((format >> 24) & 0xff);
    s[4] = '\0';

    for(int i = 0; i < 4; i++)
    {
        if(!std::isprint(static_cast<unsigned char>(s[i])))
        {
             s[i] = '?';
        }
    }
    return std::string(s);
}
const char* connectionToString(uint32_t connection)
{
    switch(connection){
        case DRM_MODE_CONNECTED:
            return "connected";
        case DRM_MODE_DISCONNECTED:
            return "disconnected";
        case DRM_MODE_UNKNOWNCONNECTION:
            return "unknown";
        default:
            return "invalid";
    }
}
const char* encoderTypeToString(uint32_t type) //信号转换器类型
{
    switch (type) {
    case DRM_MODE_ENCODER_NONE:
        return "NONE";
    case DRM_MODE_ENCODER_DAC:
        return "DAC";
    case DRM_MODE_ENCODER_TMDS: //用于HDMI/DVI
        return "TMDS";
    case DRM_MODE_ENCODER_LVDS:
        return "LVDS";
    case DRM_MODE_ENCODER_TVDAC:
        return "TVDAC";
    case DRM_MODE_ENCODER_VIRTUAL:
        return "VIRTUAL";
    case DRM_MODE_ENCODER_DSI: //MIPI DSI编码器
        return "DSI";
    case DRM_MODE_ENCODER_DPMST:
        return "DPMST";
    case DRM_MODE_ENCODER_DPI:
        return "DPI";
    default:
        return "UNKNOWN";
    }
}
const char* connectorTypeToString(uint32_t type) //物理接口类型
{
    switch (type) {
    case DRM_MODE_CONNECTOR_Unknown:
        return "Unknown";
    case DRM_MODE_CONNECTOR_VGA:
        return "VGA";
    case DRM_MODE_CONNECTOR_DVII:
        return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:
        return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:
        return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:
        return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:
        return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS:
        return "LVDS";
    case DRM_MODE_CONNECTOR_Component:
        return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:
        return "9PinDIN";
    case DRM_MODE_CONNECTOR_DisplayPort:
        return "DisplayPort";
    case DRM_MODE_CONNECTOR_HDMIA:
        return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
        return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:
        return "TV";
    case DRM_MODE_CONNECTOR_eDP:
        return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:
        return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:
        return "DSI";
    case DRM_MODE_CONNECTOR_DPI:
        return "DPI";
    default:
        return "UNKNOWN";
    }
}

const char* planeTypeToString(uint64_t value)
{
    switch(value){
        case DRM_PLANE_TYPE_OVERLAY: //叠加/视频图层
            return "Overlay";
        case DRM_PLANE_TYPE_PRIMARY: //主图层
            return "Primary";
        case DRM_PLANE_TYPE_CURSOR: //光标图层
            return "Cursor";
        default:
            return "Unknown";
    }
}

void printPropertyEnums(drmModePropertyRes* prop, uint64_t value)
{
    if(!prop){
        return;
    }
    for(int i =0; i < prop->count_enums; i++)
    {
        if(prop->enums[i].value == value)
        {
            std::printf(" (%s)", prop->enums[i].name);
            return;
        }
    }

}
void printObjectProperties(int fd, uint32_t object_id, uint32_t object_type, const char* indent)
{
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, object_id, object_type);
    if(!props)
    {
        std::printf("%sproperties: <none>\n", indent);
        return;
    }

    std::printf("%sproperties count=%u\n", indent, props->count_props);
    for(uint32_t i = 0; i < props->count_props; i++)
    {
        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
        if(!prop)
        {
            continue;
        }
        const uint64_t value = props->prop_values[i];
        std::printf("%s  - %s = %llu",
            indent,
            prop->name,
            static_cast<unsigned long long>(value));

        printPropertyEnums(prop, value);

        if (std::strcmp(prop->name, "type") == 0) {
            std::printf(" [%s]", planeTypeToString(value));
        }
        std::printf("\n");

        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);

}

void printMode(const drmModeModeInfo& mode, const char* indent)
{
    std::printf(
        "%s%s: %dx%d clock=%d htotal=%d vtotal=%d vrefresh=%d flags=0x%x type=0x%x\n",
        indent,
        mode.name,
        mode.hdisplay,
        mode.vdisplay,
        mode.clock,
        mode.htotal,
        mode.vtotal,
        mode.vrefresh,
        mode.flags,
        mode.type
    );
}
void printConnector(int fd, drmModeRes* res, uint32_t connector_id)
{
    drmModeConnector* conn = drmModeGetConnector(fd, connector_id);
    std::printf("\n========== Connector ==========\n");
    std::printf("id=%u type=%s-%u status=%s\n",
                conn->connector_id,
                connectorTypeToString(conn->connector_type),
                conn->connector_type_id,
                connectionToString(conn->connection));

    std::printf("mm_width=%u mm_height=%u\n",
                conn->mmWidth,
                conn->mmHeight);

    std::printf("encoder_id=%u count_encoders=%d count_modes=%d\n",
                conn->encoder_id,
                conn->count_encoders,
                conn->count_modes);
    if (conn->count_encoders > 0) {
        std::printf("encoders:");
        for (int i = 0; i < conn->count_encoders; ++i) {
            std::printf(" %u", conn->encoders[i]);
        }
        std::printf("\n");
    }
    if (conn->count_modes > 0) {
        std::printf("modes:\n");
        for (int i = 0; i < conn->count_modes; ++i) {
            printMode(conn->modes[i], "  ");
        }
    }
    printObjectProperties(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, " ");
    drmModeFreeConnector(conn);
}

void printEncoder(int fd, uint32_t encoder_id)
{
    drmModeEncoder* enc = drmModeGetEncoder(fd, encoder_id);
    std::printf("\n========== Encoder ==========\n");
    std::printf("id=%u type=%s crtc_id=%u possible_crtcs=0x%x possible_clones=0x%x\n",
                enc->encoder_id,
                encoderTypeToString(enc->encoder_type),
                enc->crtc_id,
                enc->possible_crtcs,
                enc->possible_clones);

    drmModeFreeEncoder(enc);
}
void printCrtc(int fd, uint32_t crtc_id){
    drmModeCrtc* crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc) {
        std::printf("CRTC %u: get failed\n", crtc_id);
        return;
    }

    std::printf("\n========== CRTC ==========\n");
    std::printf("id=%u buffer_id=%u x=%d y=%d width=%u height=%u mode_valid=%d\n",
                crtc->crtc_id,
                crtc->buffer_id,
                crtc->x,
                crtc->y,
                crtc->width,
                crtc->height,
                crtc->mode_valid);

    if (crtc->mode_valid) {
        printMode(crtc->mode, "  current mode ");
    }
    printObjectProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC, " ");
    drmModeFreeCrtc(crtc);
}

void printPossibleCrtcs(drmModeRes* res, uint32_t possible_crtcs)
{
    std::printf("possible_crtcs mask=0x%x ids=[", possible_crtcs);

    bool first = true;
    for(int i = 0; i < res->count_crtcs; ++i)
    {
        if(possible_crtcs & (1u << i))
        {
            if(!first)
            {
                std::printf(", ");
            }
            std::printf("%u", res->crtcs[i]);
            first = false;
        }
    }
    std::printf("]\n");
}
void printPlane(int fd, drmModeRes* res, uint32_t plane_id){
    drmModePlane* plane = drmModeGetPlane(fd, plane_id);
    if (!plane) {
        std::printf("Plane %u: get failed\n", plane_id);
        return;
    }

    std::printf("\n========== Plane ==========\n");
    std::printf("id=%u crtc_id=%u fb_id=%u x=%u y=%u gamma_size=%u\n",
                plane->plane_id,
                plane->crtc_id,
                plane->fb_id,
                plane->x, //图层在屏幕上的绝对物理坐标（起点位置）
                plane->y,
                plane->gamma_size);
    
    printPossibleCrtcs(res, plane->possible_crtcs);



    for(uint32_t i = 0; i < plane->count_formats; i++)
    {
        const uint32_t fmt = plane->formats[i];
        const std::string name = fourccToString(fmt);

        std::printf("  %s (%u) \n", name.c_str(), fmt);
    }

    printObjectProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, " ");
    drmModeFreePlane(plane);

}

int main(int argc, char** argv)
{
    const char* device = "/dev/dri/card0";
    if(argc >= 2)
    {
        device = argv[1];
    }
    std::printf("DRM info test\n");
    std::printf("device: %s\n", device);

    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0){
        std::perror("open DRM device failed");
        std::printf("try:\n");
        std::printf("  ls -l /dev/dri\n");
        std::printf("  %s /dev/dri/card1\n", argv[0]);
        return 1;
    }

    const char* name = drmGetDeviceNameFromFd(fd);
    if(name){
        std::printf("drm device name: %s\n", name);
        drmFree(const_cast<char*>(name));
    }

    int ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); //开启“通用图层（Universal Planes）”能力
    if (ret != 0) {
        std::printf("warning: DRM_CLIENT_CAP_UNIVERSAL_PLANES failed: %s\n",
                    std::strerror(errno));
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1); //开启原子更新（Atomic Commit）能力
    if (ret != 0) {
        std::printf("warning: DRM_CLIENT_CAP_ATOMIC failed: %s\n",
                    std::strerror(errno));
    }
    
    drmModeRes* res = drmModeGetResources(fd); //返回硬件资源数量和 ID 数组 有多少个屏幕插座（count_connectors，里面就有你的 MIPI DSI 接口 ID） 显示核心（count_crtcs，也就是 VOP 核心 ID）编码器（count_encoders） 
    if (!res) {
        std::perror("drmModeGetResources failed");
        close(fd);
        return 1;
    }
    std::printf("\n========== Resources ==========\n");
    std::printf("count_fbs=%d count_crtcs=%d count_connectors=%d count_encoders=%d\n",
                res->count_fbs, //当前正在使用的framebuffer数量
                res->count_crtcs,
                res->count_connectors,
                res->count_encoders);

    std::printf("crtcs:");
    for (int i = 0; i < res->count_crtcs; ++i) {
        std::printf(" %u", res->crtcs[i]);
    }
    std::printf("\n");

    std::printf("connectors:");
    for (int i = 0; i < res->count_connectors; ++i) {
        std::printf(" %u", res->connectors[i]);
    }
    std::printf("\n");

    std::printf("encoders:");
    for (int i = 0; i < res->count_encoders; ++i) {
        std::printf(" %u", res->encoders[i]);
    }
    std::printf("\n");

    for (int i = 0; i < res->count_connectors; ++i) {
        printConnector(fd, res, res->connectors[i]);
    }

    for (int i = 0; i < res->count_encoders; ++i) {
        printEncoder(fd, res->encoders[i]);
    }

    for (int i = 0; i < res->count_crtcs; ++i) {
        printCrtc(fd, res->crtcs[i]);
    }
    // 在瑞芯微的官方芯片白皮书里，他们不叫 Plane，他们把这些硬件通道叫做：

    // Cluster Window：高级硬件图层，支持 YUV 视频格式、硬件全动态缩放、旋转。

    // Esmart Window：中级硬件图层，支持 YUV/RGB，支持缩放。

    // Smart Window：普通图层，通常用来放简单的 UI
    drmModePlaneRes* plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        std::printf("\nwarning: drmModeGetPlaneResources failed: %s\n",
                    std::strerror(errno));
    } else {
        std::printf("\n========== Plane Resources ==========\n");
        std::printf("count_planes=%u\n", plane_res->count_planes);

        for(uint32_t i = 0; i < plane_res->count_planes; i++)
        {
            printPlane(fd, res, plane_res->planes[i]); 
        }
        drmModeFreePlaneResources(plane_res);
    }
    drmModeFreeResources(res);
    close(fd);

    std::printf("\nDone.\n");
    return 0;
}