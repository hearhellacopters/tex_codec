/*
 * gx2_addrlib.h - Wii U GX2 surface layout (AMD R600 addrlib subset).
 *
 * Ported from ref/G1TFormatConvert.h, which in turn is a C port of the
 * Switch-Toolbox GX2 decoder (KillzXGaming/Switch-Toolbox, itself derived
 * from AboodXD's addrlib and AMD's addrlib).
 *
 * Only the paths used by G1T textures are exposed: mip level 0, 2D
 * surfaces, no AA, tile modes 0-15 (default 4 = ADDR_TM_2D_TILED_THIN1).
 */
#ifndef TEXC_GX2_ADDRLIB_H
#define TEXC_GX2_ADDRLIB_H

#include <stdint.h>

namespace texc {
namespace gx2 {

struct SurfaceInfo {
    uint32_t pitch;    /* padded pitch in elements (blocks for BCn)   */
    uint32_t height;   /* padded height in elements                   */
    uint32_t surf_size;/* total tiled surface size in bytes           */
    uint32_t tile_mode;/* resolved addr tile mode (16 == linear)      */
    uint32_t bpp;      /* bits per element (64 BC1-class, 128 BC3-class, 32 raw) */
};

/* hw_format: GX2 hardware format code, e.g. 0x31 (BC1-class 8B/4x4 block),
 * 0x33 (BC3-class 16B/4x4 block), 0x1A (RGBA8). width/height in pixels.
 * tile_mode: GX2 tile mode (4 = 2D_TILED_THIN1, the G1T default).
 * Returns false on failure. */
bool compute_surface_info_mip0(uint32_t hw_format, uint32_t width,
                               uint32_t height, uint32_t tile_mode,
                               SurfaceInfo *out);

/* Byte address of element (x, y) inside the tiled surface (slice 0,
 * sample 0). bpp/pitch/height/tile_mode come from SurfaceInfo;
 * pipe_swizzle/bank_swizzle are decoded from the GX2 swizzle value as
 * pipe = (swz >> 8) & 1, bank = (swz >> 9) & 3. */
int64_t addr_from_coord(uint32_t x, uint32_t y, uint32_t bpp,
                        uint32_t pitch, uint32_t height, uint32_t tile_mode,
                        uint32_t pipe_swizzle, uint32_t bank_swizzle);

} /* namespace gx2 */
} /* namespace texc */

#endif /* TEXC_GX2_ADDRLIB_H */
