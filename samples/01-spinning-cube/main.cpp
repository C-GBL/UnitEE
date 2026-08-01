// samples/01-spinning-cube -- plan section 9, M2 acceptance.
//
// Drives ps2ur's GsDevice: video mode and double buffering, a full-screen
// clear, and a rotating cube through the immediate-mode triangle path
// (EE-side transform, PATH3). Per ADR-003 this is the deliberately slow
// bootstrap renderer; M4 replaces the draw path with a VU1 microprogram and
// diffs the result against this one.
//
// Unlike 00-hello-triangle, this sample DOES link ps2ur -- it is the first
// real exercise of the runtime's graphics layer.

#include <ps2ur/gs_device.h>
#include <ps2ur/log.h>
#include <ps2ur/math.h>
#include <ps2ur/platform.h>
#include <ps2ur/time.h>

#include <kernel.h>
#include <math.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

constexpr uint32_t kScreenWidth = 512;
constexpr uint32_t kScreenHeight = 448;
constexpr uint32_t kFrameCount = 300; // enough to prove the loop is stable

// The framebuffer is 512x448 but the console stretches it to a 4:3 display, so
// pixels are not square: each is 1.167x wider than tall. Geometry must be
// squeezed horizontally by that factor or the cube renders visibly oblong.
constexpr float kPixelAspect =
    (4.0f / 3.0f) / (static_cast<float>(kScreenWidth) / static_cast<float>(kScreenHeight));

// Unit cube, 8 corners.
const Vec3 kCubeCorners[8] = {
    {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
    {-1.0f, -1.0f, 1.0f},  {1.0f, -1.0f, 1.0f},  {1.0f, 1.0f, 1.0f},  {-1.0f, 1.0f, 1.0f},
};

// Two triangles per face, six faces.
const uint8_t kCubeIndices[36] = {
    0, 1, 2, 0, 2, 3, // back
    5, 4, 7, 5, 7, 6, // front
    4, 0, 3, 4, 3, 7, // left
    1, 5, 6, 1, 6, 2, // right
    3, 2, 6, 3, 6, 7, // top
    4, 5, 1, 4, 1, 0, // bottom
};

// One colour per face, so rotation is obvious even without lighting.
const uint8_t kFaceColours[6][3] = {
    {220, 60, 60}, {60, 220, 60}, {60, 60, 220},
    {220, 220, 60}, {220, 60, 220}, {60, 220, 220},
};

} // namespace

int main(void)
{
    platform::init();
    time::init();

    gfx::VideoConfig config;
    config.width = kScreenWidth;
    config.height = kScreenHeight;
    config.standard = gfx::VideoStandard::NTSC;
    config.colour_format = gfx::PixelFormat::PSMCT32;
    config.depth_format = gfx::PixelFormat::PSMZ24;

    gfx::GsDevice device;
    if (!device.init(config)) {
        printf("PS2UR_TOKEN_SPINNING_CUBE_FAIL device init\n");
        SleepThread();
        return 1;
    }

    // Projection constants. All single-precision: `sinf`/`cosf`, never
    // `sin`/`cos`, because double on the EE is a soft-float library call
    // (plan section 3.1 and 11.5).
    const float half_h = static_cast<float>(kScreenHeight) * 0.5f;
    const float half_w = static_cast<float>(kScreenWidth) * 0.5f;
    const float y_scale = half_h;
    const float x_scale = half_h / kPixelAspect;
    const float focal = 2.2f;
    const float camera_z = 5.0f;
    const float z_near = 3.0f;
    const float z_far = 9.0f;

    uint32_t drawn_frames = 0;
    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        time::update();

        const float angle = static_cast<float>(frame) * 0.04f;
        const float cy = cosf(angle);
        const float sy = sinf(angle);
        const float cx = cosf(angle * 0.7f);
        const float sx = sinf(angle * 0.7f);

        gfx::GsDevice::Vertex verts[36];
        for (uint32_t i = 0; i < 36; ++i) {
            const Vec3 p = kCubeCorners[kCubeIndices[i]];

            // Yaw about Y, then pitch about X.
            const float x1 = p.x * cy + p.z * sy;
            const float z1 = p.z * cy - p.x * sy;
            const float y2 = p.y * cx - z1 * sx;
            const float z2 = z1 * cx + p.y * sx;

            const float zc = z2 + camera_z; // push in front of the camera
            const float inv = focal / zc;

            verts[i].x = static_cast<int32_t>(half_w + x1 * inv * x_scale);
            verts[i].y = static_cast<int32_t>(half_h - y2 * inv * y_scale);

            // Map view depth into the 24-bit Z range. The GS depth test is
            // configured GEQUAL, meaning larger Z wins, so near geometry must
            // produce the LARGER value -- the inverse of the usual convention.
            const float depth01 = 1.0f - (zc - z_near) / (z_far - z_near);
            const float clamped = depth01 < 0.0f ? 0.0f : (depth01 > 1.0f ? 1.0f : depth01);
            verts[i].z = static_cast<uint32_t>(clamped * 16777215.0f);

            const uint32_t face = i / 6u;
            verts[i].r = kFaceColours[face][0];
            verts[i].g = kFaceColours[face][1];
            verts[i].b = kFaceColours[face][2];
            verts[i].a = 0x80; // 0x80 is fully opaque on this hardware
        }

        device.begin_frame();
        device.clear(24, 32, 56);
        device.draw_triangles_immediate(verts, 36);
        device.end_frame();

        if (device.packet().overflowed()) {
            printf("PS2UR_TOKEN_SPINNING_CUBE_FAIL packet overflow\n");
            SleepThread();
            return 1;
        }
        drawn_frames++;
    }

    printf("[01-spinning-cube] %u frames, %ux%u PSMCT32 + PSMZ24, 12 tris/frame\n",
           static_cast<unsigned>(drawn_frames), static_cast<unsigned>(kScreenWidth),
           static_cast<unsigned>(kScreenHeight));
    printf("PS2UR_TOKEN_SPINNING_CUBE_OK\n");

    device.shutdown();

    // Park rather than return: returning from main() drops to the BIOS browser.
    SleepThread();
    return 0;
}
