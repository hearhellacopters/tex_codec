/*
 * gx2_addrlib.cpp - Wii U GX2 surface layout (AMD R600 addrlib subset).
 *
 * Ported from ref/G1TFormatConvert.h ("WII U SWIZZLE CODE" section), a C
 * port of KillzXGaming/Switch-Toolbox GX2.cs (derived from AboodXD's
 * addrlib / AMD addrlib). Function bodies are kept structurally identical
 * to the reference so behaviour matches byte for byte.
 */

#include "gx2_addrlib.h"

namespace texc {
namespace gx2 {
namespace {

inline uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

struct Flags { uint32_t value; };

struct SurfaceIn {
    uint32_t size;
    uint32_t tileMode;
    uint32_t format;
    uint32_t bpp;
    uint32_t numSamples;
    uint32_t width;
    uint32_t height;
    uint32_t numSlices;
    uint32_t slice;
    uint32_t mipLevel;
    Flags    flags;
    uint32_t numFrags;
    uint32_t tileType;
};

struct SurfaceOut {
    uint32_t size;
    uint32_t pitch;
    uint32_t height;
    uint32_t depth;
    uint32_t surfSize;
    uint32_t tileMode;
    uint32_t baseAlign;
    uint32_t pitchAlign;
    uint32_t heightAlign;
    uint32_t depthAlign;
    uint32_t bpp;
    uint32_t pixelPitch;
    uint32_t pixelHeight;
    uint32_t pixelBits;
    uint32_t sliceSize;
    uint32_t pitchTileMax;
    uint32_t heightTileMax;
    uint32_t sliceTileMax;
};

uint32_t nextPow2(uint32_t dim)
{
    uint32_t newDim = 1;
    if (dim < 0x7FFFFFFF) {
        while (newDim < dim)
            newDim *= 2;
    } else {
        newDim = 0x80000000u;
    }
    return newDim;
}

uint32_t powTwoAlign(uint32_t x, uint32_t align)
{
    return ~(align - 1) & (x + align - 1);
}

uint32_t hwlComputeMipLevel(SurfaceIn *pIn)
{
    uint32_t handled = 0;
    if (49 <= pIn->format && pIn->format <= 55) {
        if (pIn->mipLevel != 0) {
            uint32_t width = pIn->width;
            uint32_t height = pIn->height;
            uint32_t slices = pIn->numSlices;

            if (((pIn->flags.value >> 12) & 1) != 0) {
                uint32_t widtha = width >> (int)pIn->mipLevel;
                uint32_t heighta = height >> (int)pIn->mipLevel;

                if (((pIn->flags.value >> 4) & 1) == 0)
                    slices >>= (int)pIn->mipLevel;

                width = umax(1, widtha);
                height = umax(1, heighta);
                slices = umax(1, slices);
            }

            pIn->width = nextPow2(width);
            pIn->height = nextPow2(height);
            pIn->numSlices = slices;
        }
        handled = 1;
    }
    return handled;
}

void computeMipLevel(SurfaceIn *pIn)
{
    uint32_t slices = 0;
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t hwlHandled = 0;

    if (49 <= pIn->format && pIn->format <= 55 &&
        (pIn->mipLevel == 0 || ((pIn->flags.value >> 12) & 1) != 0)) {
        pIn->width = powTwoAlign(pIn->width, 4);
        pIn->height = powTwoAlign(pIn->height, 4);
    }

    hwlHandled = hwlComputeMipLevel(pIn);

    if (hwlHandled == 0 && pIn->mipLevel != 0 &&
        ((pIn->flags.value >> 12) & 1) != 0) {
        width = umax(1, pIn->width >> (int)pIn->mipLevel);
        height = umax(1, pIn->height >> (int)pIn->mipLevel);
        slices = umax(1, pIn->numSlices);

        if (((pIn->flags.value >> 4) & 1) == 0)
            slices = umax(1, slices >> (int)pIn->mipLevel);

        if (pIn->format != 47 && pIn->format != 48) {
            width = nextPow2(width);
            height = nextPow2(height);
            slices = nextPow2(slices);
        }
        pIn->width = width;
        pIn->height = height;
        pIn->numSlices = slices;
    }
}

uint32_t adjustSurfaceInfo(uint32_t elemMode, uint32_t expandX,
                           uint32_t expandY, uint32_t bpp, uint32_t width,
                           uint32_t height, SurfaceIn *pIn)
{
    uint32_t bBCnFormat = 0;
    uint32_t widtha, heighta;

    switch (elemMode) {
    case 9: case 10: case 11: case 12: case 13:
        if (bpp != 0)
            bBCnFormat = 1;
        break;
    }

    if (width != 0 && height != 0) {
        if (expandX > 1 || expandY > 1) {
            if (elemMode == 4) {
                widtha = expandX * width;
                heighta = expandY * height;
            } else if (bBCnFormat != 0) {
                widtha = width / expandX;
                heighta = height / expandY;
            } else {
                widtha = (width + expandX - 1) / expandX;
                heighta = (height + expandY - 1) / expandY;
            }
            pIn->width = umax(1, widtha);
            pIn->height = umax(1, heighta);
        }
    }

    if (bpp != 0) {
        switch (elemMode) {
        case 4:
            pIn->bpp = bpp / expandX / expandY;
            break;
        case 5: case 6:
            pIn->bpp = expandY * expandX * bpp;
            break;
        case 9: case 12:
            pIn->bpp = 64;
            break;
        case 10: case 11: case 13:
            pIn->bpp = 128;
            break;
        default:
            pIn->bpp = bpp;
            break;
        }
        return pIn->bpp;
    }
    return 0;
}

uint32_t convertToNonBankSwappedMode(uint32_t tileMode)
{
    switch (tileMode) {
    case 8:  return 4;
    case 9:  return 5;
    case 10: return 6;
    case 11: return 7;
    case 14: return 12;
    case 15: return 13;
    }
    return tileMode;
}

uint32_t computeSurfaceThickness(uint32_t tileMode)
{
    switch (tileMode) {
    case 0x03: case 0x07: case 0x0B: case 0x0D: case 0x0F:
        return 4;
    case 0x10: case 0x11:
        return 8;
    default:
        return 1;
    }
}

uint32_t computeSurfaceTileSlices(uint32_t tileMode, uint32_t bpp,
                                  uint32_t numSamples)
{
    uint32_t bytePerSample = ((bpp << 6) + 7) >> 3;
    uint32_t tileSlices = 1;
    uint32_t samplePerTile;

    if (computeSurfaceThickness(tileMode) > 1)
        numSamples = 4;

    if (bytePerSample != 0) {
        samplePerTile = 2048 / bytePerSample;
        if (samplePerTile < numSamples)
            tileSlices = umax(1, numSamples / samplePerTile);
    }
    return tileSlices;
}

uint32_t computeSurfaceMipLevelTileMode(uint32_t baseTileMode, uint32_t bpp,
                                        uint32_t level, uint32_t width,
                                        uint32_t height, uint32_t numSlices,
                                        uint32_t numSamples, uint32_t isDepth,
                                        uint32_t noRecursive)
{
    uint32_t widthAlignFactor = 1;
    uint32_t macroTileWidth = 32;
    uint32_t macroTileHeight = 16;
    uint32_t tileSlices = computeSurfaceTileSlices(baseTileMode, bpp, numSamples);
    uint32_t expTileMode = baseTileMode;

    uint32_t widtha, heighta, numSlicesa, thickness, microTileBytes;

    if (numSamples > 1 || tileSlices > 1 || isDepth != 0) {
        if (baseTileMode == 7)
            expTileMode = 4;
        else if (baseTileMode == 13)
            expTileMode = 12;
        else if (baseTileMode == 11)
            expTileMode = 8;
        else if (baseTileMode == 15)
            expTileMode = 14;
    }

    if (baseTileMode == 2 && numSamples > 1) {
        expTileMode = 4;
    } else if (baseTileMode == 3) {
        if (numSamples > 1 || isDepth != 0)
            expTileMode = 2;
        if (numSamples == 2 || numSamples == 4)
            expTileMode = 7;
    } else {
        expTileMode = baseTileMode;
    }

    if (noRecursive != 0 || level == 0)
        return expTileMode;

    switch (bpp) {
    case 24: case 48: case 96:
        bpp /= 3;
        break;
    }

    widtha = nextPow2(width);
    heighta = nextPow2(height);
    numSlicesa = nextPow2(numSlices);

    expTileMode = convertToNonBankSwappedMode(expTileMode);
    thickness = computeSurfaceThickness(expTileMode);
    microTileBytes = (numSamples * bpp * (thickness << 6) + 7) >> 3;

    if (microTileBytes < 256)
        widthAlignFactor = umax(1, 256 / microTileBytes);

    if (expTileMode == 4 || expTileMode == 12) {
        if ((widtha < widthAlignFactor * macroTileWidth) || heighta < macroTileHeight)
            expTileMode = 2;
    } else if (expTileMode == 5) {
        macroTileWidth = 16;
        macroTileHeight = 32;
        if ((widtha < widthAlignFactor * macroTileWidth) || heighta < macroTileHeight)
            expTileMode = 2;
    } else if (expTileMode == 6) {
        macroTileWidth = 8;
        macroTileHeight = 64;
        if ((widtha < widthAlignFactor * macroTileWidth) || heighta < macroTileHeight)
            expTileMode = 2;
    } else if (expTileMode == 7 || expTileMode == 13) {
        if ((widtha < widthAlignFactor * macroTileWidth) || heighta < macroTileHeight)
            expTileMode = 3;
    }

    if (numSlicesa < 4) {
        if (expTileMode == 3)
            expTileMode = 2;
        else if (expTileMode == 7)
            expTileMode = 4;
        else if (expTileMode == 13)
            expTileMode = 12;
    }

    return computeSurfaceMipLevelTileMode(expTileMode, bpp, level, widtha,
                                          heighta, numSlicesa, numSamples,
                                          isDepth, 1);
}

void padDimensions(uint32_t tileMode, uint32_t padDims, uint32_t isCube,
                   uint32_t pitchAlign, uint32_t heightAlign,
                   uint32_t sliceAlign, uint32_t expPitch[1],
                   uint32_t expHeight[1], uint32_t expNumSlices[1])
{
    uint32_t thickness = computeSurfaceThickness(tileMode);
    if (padDims == 0)
        padDims = 3;

    if ((pitchAlign & (pitchAlign - 1)) == 0) {
        expPitch[0] = powTwoAlign(expPitch[0], pitchAlign);
    } else {
        expPitch[0] += pitchAlign - 1;
        expPitch[0] /= pitchAlign;
        expPitch[0] *= pitchAlign;
    }

    if (padDims > 1)
        expHeight[0] = powTwoAlign(expHeight[0], heightAlign);

    if (padDims > 2 || thickness > 1) {
        if (isCube != 0)
            expNumSlices[0] = nextPow2(expNumSlices[0]);
        if (thickness > 1)
            expNumSlices[0] = powTwoAlign(expNumSlices[0], sliceAlign);
    }
}

uint32_t adjustPitchAlignment(Flags flags, uint32_t pitchAlign[1])
{
    if (((flags.value >> 13) & 1) != 0)
        pitchAlign[0] = powTwoAlign(pitchAlign[0], 0x20);
    return pitchAlign[0];
}

void computeSurfaceAlignmentsLinear(uint32_t tileMode, uint32_t bpp,
                                    Flags flags, uint32_t out[3])
{
    uint32_t pixelsPerPipeInterleave;
    uint32_t baseAlign, pitchAlign, heightAlign;

    if (tileMode == 0) {
        baseAlign = 1;
        pitchAlign = (bpp != 1 ? (uint32_t)1 : 8);
        heightAlign = 1;
    } else if (tileMode == 1) {
        pixelsPerPipeInterleave = 2048 / bpp;
        baseAlign = 256;
        pitchAlign = umax(0x40, pixelsPerPipeInterleave);
        heightAlign = 1;
    } else {
        baseAlign = 1;
        pitchAlign = 1;
        heightAlign = 1;
    }
    pitchAlign = adjustPitchAlignment(flags, &pitchAlign);

    out[0] = baseAlign;
    out[1] = pitchAlign;
    out[2] = heightAlign;
}

void computeSurfaceInfoLinear(uint32_t tileMode, uint32_t bpp,
                              uint32_t numSamples, uint32_t pitch,
                              uint32_t height, uint32_t numSlices,
                              uint32_t mipLevel, uint32_t padDims, Flags flags,
                              uint32_t expPitch[1], uint32_t expHeight[1],
                              uint32_t expNumSlices[1], uint32_t out[9])
{
    expPitch[0] = pitch;
    expHeight[0] = height;
    expNumSlices[0] = numSlices;

    uint32_t valid = 1;
    uint32_t microTileThickness = computeSurfaceThickness(tileMode);
    uint32_t slices;
    uint32_t align3[3];

    computeSurfaceAlignmentsLinear(tileMode, bpp, flags, align3);
    uint32_t baseAlign = align3[0];
    uint32_t pitchAlign = align3[1];
    uint32_t heightAlign = align3[2];

    if ((((flags.value >> 9) & 1) != 0) && (mipLevel == 0)) {
        expPitch[0] /= 3;
        expPitch[0] = nextPow2(expPitch[0]);
    }
    if (mipLevel != 0) {
        expPitch[0] = nextPow2(expPitch[0]);
        expHeight[0] = nextPow2(expHeight[0]);

        if (((flags.value >> 4) & 1) != 0) {
            expNumSlices[0] = numSlices;
            if (numSlices <= 1)
                padDims = 2;
            else
                padDims = 0;
        } else {
            expNumSlices[0] = nextPow2(numSlices);
        }
    }

    padDimensions(tileMode, padDims, (flags.value >> 4) & 1, pitchAlign,
                  heightAlign, microTileThickness, expPitch, expHeight,
                  expNumSlices);

    if ((((flags.value >> 9) & 1) != 0) && (mipLevel == 0))
        expPitch[0] *= 3;

    slices = expNumSlices[0] * numSamples / microTileThickness;

    out[0] = valid;
    out[1] = expPitch[0];
    out[2] = expHeight[0];
    out[3] = expNumSlices[0];
    out[4] = (expHeight[0] * expPitch[0] * slices * bpp * numSamples + 7) / 8;
    out[5] = baseAlign;
    out[6] = pitchAlign;
    out[7] = heightAlign;
    out[8] = microTileThickness;
}

void computeSurfaceAlignmentsMicroTiled(uint32_t tileMode, uint32_t bpp,
                                        Flags flags, uint32_t numSamples,
                                        uint32_t out[3])
{
    switch (bpp) {
    case 24: case 48: case 96:
        bpp /= 3;
        break;
    }
    uint32_t thickness = computeSurfaceThickness(tileMode);
    uint32_t baseAlign = 256;
    uint32_t pitchAlign = umax(8, 256 / bpp / numSamples / thickness);
    uint32_t heightAlign = 8;

    pitchAlign = adjustPitchAlignment(flags, &pitchAlign);

    out[0] = baseAlign;
    out[1] = pitchAlign;
    out[2] = heightAlign;
}

void computeSurfaceInfoMicroTiled(uint32_t tileMode, uint32_t bpp,
                                  uint32_t numSamples, uint32_t pitch,
                                  uint32_t height, uint32_t numSlices,
                                  uint32_t mipLevel, uint32_t padDims,
                                  Flags flags, uint32_t expPitch[1],
                                  uint32_t expHeight[1],
                                  uint32_t expNumSlices[1], uint32_t out[10])
{
    expPitch[0] = pitch;
    expHeight[0] = height;
    expNumSlices[0] = numSlices;

    uint32_t valid = 1;
    uint32_t expTileMode = tileMode;
    uint32_t microTileThickness = computeSurfaceThickness(tileMode);

    if (mipLevel != 0) {
        expPitch[0] = nextPow2(pitch);
        expHeight[0] = nextPow2(height);
        if (((flags.value >> 4) & 1) != 0) {
            expNumSlices[0] = numSlices;
            if (numSlices <= 1)
                padDims = 2;
            else
                padDims = 0;
        } else {
            expNumSlices[0] = nextPow2(numSlices);
        }
        if (expTileMode == 3 && expNumSlices[0] < 4) {
            expTileMode = 2;
            microTileThickness = 1;
        }
    }

    uint32_t align3[3];
    computeSurfaceAlignmentsMicroTiled(expTileMode, bpp, flags, numSamples, align3);
    uint32_t baseAlign = align3[0];
    uint32_t pitchAlign = align3[1];
    uint32_t heightAlign = align3[2];

    padDimensions(expTileMode, padDims, (flags.value >> 4) & 1, pitchAlign,
                  heightAlign, microTileThickness, expPitch, expHeight,
                  expNumSlices);

    out[0] = valid;
    out[1] = expPitch[0];
    out[2] = expHeight[0];
    out[3] = expNumSlices[0];
    out[4] = (expHeight[0] * expPitch[0] * expNumSlices[0] * bpp * numSamples + 7) / 8;
    out[5] = expTileMode;
    out[6] = baseAlign;
    out[7] = pitchAlign;
    out[8] = heightAlign;
    out[9] = microTileThickness;
}

uint32_t isThickMacroTiled(uint32_t tileMode)
{
    switch (tileMode) {
    case 0x7: case 0x8: case 0xD: case 0xF:
        return 1;
    default:
        return 0;
    }
}

uint32_t computeMacroTileAspectRatio(uint32_t tileMode)
{
    switch (tileMode) {
    case 0x5: case 0x9:
        return 2;
    case 0x6: case 0xA:
        return 4;
    default:
        return 1;
    }
}

void computeSurfaceAlignmentsMacroTiled(uint32_t tileMode, uint32_t bpp,
                                        Flags flags, uint32_t numSamples,
                                        uint32_t out[5])
{
    uint32_t aspectRatio = computeMacroTileAspectRatio(tileMode);
    uint32_t thickness = computeSurfaceThickness(tileMode);

    switch (bpp) {
    case 24: case 48: case 96:
        bpp /= 3;
        break;
    case 3:
        bpp = 1;
        break;
    }
    uint32_t macroTileWidth = 32 / aspectRatio;
    uint32_t macroTileHeight = aspectRatio * 16;

    uint32_t pitchAlign =
        umax(macroTileWidth, macroTileWidth * (256 / bpp / (8 * thickness) / numSamples));
    pitchAlign = adjustPitchAlignment(flags, &pitchAlign);

    uint32_t heightAlign = macroTileHeight;
    uint32_t macroTileBytes =
        numSamples * ((bpp * macroTileHeight * macroTileWidth + 7) >> 3);

    uint32_t baseAlign;
    if (thickness == 1)
        baseAlign = umax(macroTileBytes,
                         (numSamples * heightAlign * bpp * pitchAlign + 7) >> 3);
    else
        baseAlign = umax(256, (4 * heightAlign * bpp * pitchAlign + 7) >> 3);

    uint32_t microTileBytes = (thickness * numSamples * (bpp << 6) + 7) >> 3;
    uint32_t numSlicesPerMicroTile =
        (microTileBytes < 2048 ? (uint32_t)1 : microTileBytes / 2048);

    baseAlign /= numSlicesPerMicroTile;

    out[0] = baseAlign;
    out[1] = pitchAlign;
    out[2] = heightAlign;
    out[3] = macroTileWidth;
    out[4] = macroTileHeight;
}

uint32_t isBankSwappedTileMode(uint32_t tileMode)
{
    switch (tileMode) {
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: case 0xF:
        return 1;
    default:
        return 0;
    }
}

uint32_t computeSurfaceBankSwappedWidth(uint32_t tileMode, uint32_t bpp,
                                        uint32_t numSamples, uint32_t pitch)
{
    if (isBankSwappedTileMode(tileMode) == 0)
        return 0;

    uint32_t bytesPerSample = 8 * bpp;
    uint32_t samplesPerTile = 0, slicesPerTile;

    if (bytesPerSample != 0) {
        samplesPerTile = 2048 / bytesPerSample;
        slicesPerTile = umax(1, numSamples / samplesPerTile);
    } else {
        slicesPerTile = 1;
    }

    if (isThickMacroTiled(tileMode) != 0)
        numSamples = 4;

    uint32_t bytesPerTileSlice = numSamples * bytesPerSample / slicesPerTile;

    uint32_t factor = computeMacroTileAspectRatio(tileMode);
    uint32_t swapTiles = umax(1, 128 / bpp);

    uint32_t swapWidth = swapTiles * 32;
    uint32_t heightBytes = numSamples * factor * bpp * 2 / slicesPerTile;
    uint32_t swapMax = 0x4000 / heightBytes;
    uint32_t swapMin = 256 / bytesPerTileSlice;

    uint32_t bankSwapWidth = umin(swapMax, umax(swapMin, swapWidth));

    while (bankSwapWidth >= 2 * pitch)
        bankSwapWidth >>= 1;

    return bankSwapWidth;
}

void computeSurfaceInfoMacroTiled(uint32_t tileMode, uint32_t baseTileMode,
                                  uint32_t bpp, uint32_t numSamples,
                                  uint32_t pitch, uint32_t height,
                                  uint32_t numSlices, uint32_t mipLevel,
                                  uint32_t padDims, Flags flags,
                                  uint32_t expPitch[1], uint32_t expHeight[1],
                                  uint32_t expNumSlices[1], uint32_t out[10])
{
    expPitch[0] = pitch;
    expHeight[0] = height;
    expNumSlices[0] = numSlices;

    uint32_t valid = 1;
    uint32_t expTileMode = tileMode;
    uint32_t microTileThickness = computeSurfaceThickness(tileMode);

    uint32_t baseAlign = 0, pitchAlign = 0, heightAlign = 0;
    uint32_t bankSwappedWidth = 0, pitchAlignFactor = 0;
    uint32_t result = 0, pPitchOut = 0, pHeightOut = 0, pNumSlicesOut = 0;
    uint32_t pSurfSize = 0, pTileModeOut = 0, pBaseAlign = 0, pPitchAlign = 0;
    uint32_t pHeightAlign = 0, pDepthAlign = 0;

    if (mipLevel != 0) {
        expPitch[0] = nextPow2(pitch);
        expHeight[0] = nextPow2(height);

        if (((flags.value >> 4) & 1) != 0) {
            expNumSlices[0] = numSlices;
            if (numSlices <= 1)
                padDims = 2;
            else
                padDims = 0;
        } else {
            expNumSlices[0] = nextPow2(numSlices);
        }

        if (expTileMode == 7 && expNumSlices[0] < 4) {
            expTileMode = 4;
            microTileThickness = 1;
        }
    }

    if (tileMode == baseTileMode || mipLevel == 0 ||
        isThickMacroTiled(baseTileMode) == 0 || isThickMacroTiled(tileMode) != 0) {
        uint32_t tup[5];
        computeSurfaceAlignmentsMacroTiled(tileMode, bpp, flags, numSamples, tup);

        baseAlign = tup[0];
        pitchAlign = tup[1];
        heightAlign = tup[2];

        bankSwappedWidth = computeSurfaceBankSwappedWidth(tileMode, bpp, numSamples, pitch);
        if (bankSwappedWidth > pitchAlign)
            pitchAlign = bankSwappedWidth;

        padDimensions(tileMode, padDims, (flags.value >> 4) & 1, pitchAlign,
                      heightAlign, microTileThickness, expPitch, expHeight,
                      expNumSlices);

        pPitchOut = expPitch[0];
        pHeightOut = expHeight[0];
        pNumSlicesOut = expNumSlices[0];
        pSurfSize = (expHeight[0] * expPitch[0] * expNumSlices[0] * bpp * numSamples + 7) / 8;
        pTileModeOut = expTileMode;
        pBaseAlign = baseAlign;
        pPitchAlign = pitchAlign;
        pHeightAlign = heightAlign;
        pDepthAlign = microTileThickness;
        result = valid;
    } else {
        uint32_t tup[5];
        computeSurfaceAlignmentsMacroTiled(baseTileMode, bpp, flags, numSamples, tup);

        baseAlign = tup[0];
        pitchAlign = tup[1];
        heightAlign = tup[2];

        pitchAlignFactor = umax(1, 32 / bpp);

        if (expPitch[0] < pitchAlign * pitchAlignFactor || expHeight[0] < heightAlign) {
            expTileMode = 2;

            uint32_t micro[10];
            computeSurfaceInfoMicroTiled(2, bpp, numSamples, pitch, height,
                                         numSlices, mipLevel, padDims, flags,
                                         expPitch, expHeight, expNumSlices,
                                         micro);
            result = micro[0];
            pPitchOut = micro[1];
            pHeightOut = micro[2];
            pNumSlicesOut = micro[3];
            pSurfSize = micro[4];
            pTileModeOut = micro[5];
            pBaseAlign = micro[6];
            pPitchAlign = micro[7];
            pHeightAlign = micro[8];
            pDepthAlign = micro[9];
        } else {
            uint32_t tup2[5];
            computeSurfaceAlignmentsMacroTiled(tileMode, bpp, flags, numSamples, tup2);

            baseAlign = tup2[0];
            pitchAlign = tup2[1];
            heightAlign = tup2[2];

            bankSwappedWidth = computeSurfaceBankSwappedWidth(tileMode, bpp, numSamples, pitch);
            if (bankSwappedWidth > pitchAlign)
                pitchAlign = bankSwappedWidth;

            padDimensions(tileMode, padDims, (flags.value >> 4) & 1, pitchAlign,
                          heightAlign, microTileThickness, expPitch, expHeight,
                          expNumSlices);

            pPitchOut = expPitch[0];
            pHeightOut = expHeight[0];
            pNumSlicesOut = expNumSlices[0];
            pSurfSize = (expHeight[0] * expPitch[0] * expNumSlices[0] * bpp * numSamples + 7) / 8;
            pTileModeOut = expTileMode;
            pBaseAlign = baseAlign;
            pPitchAlign = pitchAlign;
            pHeightAlign = heightAlign;
            pDepthAlign = microTileThickness;
            result = valid;
        }
    }

    out[0] = result;
    out[1] = pPitchOut;
    out[2] = pHeightOut;
    out[3] = pNumSlicesOut;
    out[4] = pSurfSize;
    out[5] = pTileModeOut;
    out[6] = pBaseAlign;
    out[7] = pitchAlign;
    out[8] = heightAlign;
    out[9] = pDepthAlign;
}

uint32_t ComputeSurfaceInfoEx(SurfaceIn *pIn, SurfaceOut *pOut)
{
    uint32_t tileMode = pIn->tileMode;
    uint32_t bpp = pIn->bpp;
    uint32_t numSamples = umax(1, pIn->numSamples);
    uint32_t pitch = pIn->width;
    uint32_t height = pIn->height;
    uint32_t numSlices = pIn->numSlices;
    uint32_t mipLevel = pIn->mipLevel;
    Flags flags = {0};
    uint32_t pPitchOut = pOut->pitch;
    uint32_t pHeightOut = pOut->height;
    uint32_t pNumSlicesOut = pOut->depth;
    uint32_t pTileModeOut = pOut->tileMode;
    uint32_t pSurfSize = pOut->surfSize;
    uint32_t pBaseAlign = pOut->baseAlign;
    uint32_t pPitchAlign = pOut->pitchAlign;
    uint32_t pHeightAlign = pOut->heightAlign;
    uint32_t pDepthAlign = pOut->depthAlign;
    uint32_t padDims = 0;
    uint32_t valid = 0;
    uint32_t baseTileMode = tileMode;

    flags.value = pIn->flags.value;

    if ((((flags.value >> 4) & 1) != 0) && (mipLevel == 0))
        padDims = 2;

    if (((flags.value >> 6) & 1) != 0)
        tileMode = convertToNonBankSwappedMode(tileMode);
    else
        tileMode = computeSurfaceMipLevelTileMode(tileMode, bpp, mipLevel, pitch,
                                                  height, numSlices, numSamples,
                                                  (flags.value >> 1) & 1, 0);

    uint32_t expPitch[1] = {0};
    uint32_t expHeight[1] = {0};
    uint32_t expNumSlices[1] = {0};
    uint32_t infoLinear[9] = {0};
    uint32_t infoMicro[10] = {0};
    uint32_t infoMacro[10] = {0};

    switch (tileMode) {
    case 0:
    case 1:
        computeSurfaceInfoLinear(tileMode, bpp, numSamples, pitch, height,
                                 numSlices, mipLevel, padDims, flags, expPitch,
                                 expHeight, expNumSlices, infoLinear);
        valid = infoLinear[0];
        pPitchOut = infoLinear[1];
        pHeightOut = infoLinear[2];
        pNumSlicesOut = infoLinear[3];
        pSurfSize = infoLinear[4];
        pBaseAlign = infoLinear[5];
        pPitchAlign = infoLinear[6];
        pHeightAlign = infoLinear[7];
        pDepthAlign = infoLinear[8];
        pTileModeOut = tileMode;
        break;
    case 2:
    case 3:
        computeSurfaceInfoMicroTiled(tileMode, bpp, numSamples, pitch, height,
                                     numSlices, mipLevel, padDims, flags,
                                     expPitch, expHeight, expNumSlices,
                                     infoMicro);
        valid = infoMicro[0];
        pPitchOut = infoMicro[1];
        pHeightOut = infoMicro[2];
        pNumSlicesOut = infoMicro[3];
        pSurfSize = infoMicro[4];
        pTileModeOut = infoMicro[5];
        pBaseAlign = infoMicro[6];
        pPitchAlign = infoMicro[7];
        pHeightAlign = infoMicro[8];
        pDepthAlign = infoMicro[9];
        break;
    default:
        computeSurfaceInfoMacroTiled(tileMode, baseTileMode, bpp, numSamples,
                                     pitch, height, numSlices, mipLevel,
                                     padDims, flags, expPitch, expHeight,
                                     expNumSlices, infoMacro);
        valid = infoMacro[0];
        pPitchOut = infoMacro[1];
        pHeightOut = infoMacro[2];
        pNumSlicesOut = infoMacro[3];
        pSurfSize = infoMacro[4];
        pTileModeOut = infoMacro[5];
        pBaseAlign = infoMacro[6];
        pPitchAlign = infoMacro[7];
        pHeightAlign = infoMacro[8];
        pDepthAlign = infoMacro[9];
        break;
    }

    pOut->pitch = pPitchOut;
    pOut->height = pHeightOut;
    pOut->depth = pNumSlicesOut;
    pOut->tileMode = pTileModeOut;
    pOut->surfSize = pSurfSize;
    pOut->baseAlign = pBaseAlign;
    pOut->pitchAlign = pPitchAlign;
    pOut->heightAlign = pHeightAlign;
    pOut->depthAlign = pDepthAlign;

    if (valid == 0)
        return 3;
    return 0;
}

uint32_t restoreSurfaceInfo(uint32_t elemMode, uint32_t expandX,
                            uint32_t expandY, uint32_t bpp, SurfaceOut *pOut)
{
    uint32_t width, height;

    if (pOut->pixelPitch != 0 && pOut->pixelHeight != 0) {
        width = pOut->pixelPitch;
        height = pOut->pixelHeight;

        if (expandX > 1 || expandY > 1) {
            if (elemMode == 4) {
                width /= expandX;
                height /= expandY;
            } else {
                width *= expandX;
                height *= expandY;
            }
        }
        pOut->pixelPitch = umax(1, width);
        pOut->pixelHeight = umax(1, height);
    }
    if (bpp != 0) {
        switch (elemMode) {
        case 4:
            return expandY * expandX * bpp;
        case 5: case 6:
            return bpp / expandX / expandY;
        case 9: case 12:
            return 64;
        case 10: case 11: case 13:
            return 128;
        default:
            return bpp;
        }
    }
    return 0;
}

static const uint8_t formatExInfo[64 * 4] = {
    /* bpp   expX  expY  elemMode */
    0x00, 0x01, 0x01, 0x03,  0x08, 0x01, 0x01, 0x03,
    0x08, 0x01, 0x01, 0x03,  0x08, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x03,  0x10, 0x01, 0x01, 0x03,
    0x10, 0x01, 0x01, 0x03,  0x10, 0x01, 0x01, 0x03,
    0x10, 0x01, 0x01, 0x03,  0x10, 0x01, 0x01, 0x03,
    0x10, 0x01, 0x01, 0x03,  0x10, 0x01, 0x01, 0x03,
    0x10, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x40, 0x01, 0x01, 0x03,  0x40, 0x01, 0x01, 0x03,
    0x40, 0x01, 0x01, 0x03,  0x40, 0x01, 0x01, 0x03,
    0x40, 0x01, 0x01, 0x03,  0x00, 0x01, 0x01, 0x03,
    0x80, 0x01, 0x01, 0x03,  0x80, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x03,  0x01, 0x08, 0x01, 0x05,
    0x01, 0x08, 0x01, 0x06,  0x10, 0x01, 0x01, 0x07,
    0x10, 0x01, 0x01, 0x08,  0x20, 0x01, 0x01, 0x03,
    0x20, 0x01, 0x01, 0x03,  0x20, 0x01, 0x01, 0x03,
    0x18, 0x03, 0x01, 0x04,  0x30, 0x03, 0x01, 0x04,
    0x30, 0x03, 0x01, 0x04,  0x60, 0x03, 0x01, 0x04,
    0x60, 0x03, 0x01, 0x04,  0x40, 0x04, 0x04, 0x09,
    0x80, 0x04, 0x04, 0x0A,  0x80, 0x04, 0x04, 0x0B,
    0x40, 0x04, 0x04, 0x0C,  0x40, 0x04, 0x04, 0x0D,
    0x40, 0x04, 0x04, 0x0D,  0x40, 0x04, 0x04, 0x0D,
    0x00, 0x01, 0x01, 0x03,  0x00, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x03,  0x00, 0x01, 0x01, 0x03,
    0x00, 0x01, 0x01, 0x03,  0x00, 0x01, 0x01, 0x03,
    0x40, 0x01, 0x01, 0x03,  0x00, 0x01, 0x01, 0x03,
};

static const uint8_t formatHwInfo[64 * 4] = {
    /* bits  expX  expY  elemMode (hw table; only [0] is used here) */
    0x00, 0x00, 0x00, 0x01,  0x08, 0x03, 0x00, 0x01,
    0x08, 0x01, 0x00, 0x01,  0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,  0x10, 0x07, 0x00, 0x00,
    0x10, 0x03, 0x00, 0x01,  0x10, 0x03, 0x00, 0x01,
    0x10, 0x0B, 0x00, 0x01,  0x10, 0x01, 0x00, 0x01,
    0x10, 0x03, 0x00, 0x01,  0x10, 0x03, 0x00, 0x01,
    0x10, 0x03, 0x00, 0x01,  0x20, 0x03, 0x00, 0x00,
    0x20, 0x07, 0x00, 0x00,  0x20, 0x03, 0x00, 0x00,
    0x20, 0x03, 0x00, 0x01,  0x20, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x20, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x01,
    0x20, 0x03, 0x00, 0x01,  0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,  0x20, 0x0B, 0x00, 0x01,
    0x20, 0x0B, 0x00, 0x01,  0x20, 0x0B, 0x00, 0x01,
    0x40, 0x05, 0x00, 0x00,  0x40, 0x03, 0x00, 0x00,
    0x40, 0x03, 0x00, 0x00,  0x40, 0x03, 0x00, 0x00,
    0x40, 0x03, 0x00, 0x01,  0x00, 0x00, 0x00, 0x00,
    0x80, 0x03, 0x00, 0x00,  0x80, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01,  0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01,  0x10, 0x01, 0x00, 0x00,
    0x10, 0x01, 0x00, 0x00,  0x20, 0x01, 0x00, 0x00,
    0x20, 0x01, 0x00, 0x00,  0x20, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x01,  0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00,  0x60, 0x01, 0x00, 0x00,
    0x60, 0x01, 0x00, 0x00,  0x40, 0x01, 0x00, 0x01,
    0x80, 0x01, 0x00, 0x01,  0x80, 0x01, 0x00, 0x01,
    0x40, 0x01, 0x00, 0x01,  0x80, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

void computeSurfaceInfo(SurfaceIn *pIn, SurfaceOut *pOut)
{
    uint32_t returnCode = 0;
    uint32_t width, height, bpp, elemMode = 0;
    uint32_t expandY = 1, expandX = 1;

    if (pIn->bpp > 0x80)
        returnCode = 3;

    if (returnCode == 0) {
        computeMipLevel(pIn);

        width = pIn->width;
        height = pIn->height;
        bpp = pIn->bpp;
        expandX = 1;
        expandY = 1;

        pOut->pixelBits = pIn->bpp;

        if (pIn->format != 0) {
            bpp = formatExInfo[pIn->format * 4];
            expandX = formatExInfo[pIn->format * 4 + 1];
            expandY = formatExInfo[pIn->format * 4 + 2];
            elemMode = formatExInfo[pIn->format * 4 + 3];

            if (elemMode == 4 && expandX == 3 && pIn->tileMode == 1)
                pIn->flags.value |= 0x200;

            bpp = adjustSurfaceInfo(elemMode, expandX, expandY, bpp, width,
                                    height, pIn);
        } else if (pIn->bpp != 0) {
            pIn->width = umax(1, pIn->width);
            pIn->height = umax(1, pIn->height);
        } else {
            returnCode = 3;
        }

        if (returnCode == 0)
            returnCode = ComputeSurfaceInfoEx(pIn, pOut);

        if (returnCode == 0) {
            pOut->bpp = pIn->bpp;
            pOut->pixelPitch = pOut->pitch;
            pOut->pixelHeight = pOut->height;

            if (pIn->format != 0 &&
                (((pIn->flags.value >> 9) & 1) == 0 || pIn->mipLevel == 0))
                bpp = restoreSurfaceInfo(elemMode, expandX, expandY, bpp, pOut);

            if (((pIn->flags.value >> 5) & 1) != 0) {
                pOut->sliceSize = pOut->surfSize;
            } else {
                pOut->sliceSize = pOut->depth ? (pOut->surfSize / pOut->depth) : 0;
                if (pIn->slice == (pIn->numSlices - 1) && pIn->numSlices > 1)
                    pOut->sliceSize += pOut->sliceSize * (pOut->depth - pIn->numSlices);
            }

            pOut->pitchTileMax = (pOut->pitch >> 3) - 1;
            pOut->heightTileMax = (pOut->height >> 3) - 1;
            pOut->sliceTileMax = (pOut->height * pOut->pitch >> 6) - 1;
        }
    }
}

uint32_t computePixelIndexWithinMicroTile(uint32_t x, uint32_t y, uint32_t z,
                                          uint32_t bpp, uint32_t tileMode,
                                          bool isDepth)
{
    uint32_t pixelBit0 = 0, pixelBit1 = 0, pixelBit2 = 0, pixelBit3 = 0;
    uint32_t pixelBit4 = 0, pixelBit5 = 0, pixelBit6 = 0, pixelBit7 = 0;
    uint32_t pixelBit8 = 0;

    uint32_t thickness = computeSurfaceThickness(tileMode);

    if (isDepth) {
        pixelBit0 = x & 1;
        pixelBit1 = y & 1;
        pixelBit2 = (x & 2) >> 1;
        pixelBit3 = (y & 2) >> 1;
        pixelBit4 = (x & 4) >> 2;
        pixelBit5 = (y & 4) >> 2;
    } else {
        switch (bpp) {
        case 8:
            pixelBit0 = x & 1;
            pixelBit1 = (x & 2) >> 1;
            pixelBit2 = (x & 4) >> 2;
            pixelBit3 = (y & 2) >> 1;
            pixelBit4 = y & 1;
            pixelBit5 = (y & 4) >> 2;
            break;
        case 0x10:
            pixelBit0 = x & 1;
            pixelBit1 = (x & 2) >> 1;
            pixelBit2 = (x & 4) >> 2;
            pixelBit3 = y & 1;
            pixelBit4 = (y & 2) >> 1;
            pixelBit5 = (y & 4) >> 2;
            break;
        case 0x20:
        case 0x60:
            pixelBit0 = x & 1;
            pixelBit1 = (x & 2) >> 1;
            pixelBit2 = y & 1;
            pixelBit3 = (x & 4) >> 2;
            pixelBit4 = (y & 2) >> 1;
            pixelBit5 = (y & 4) >> 2;
            break;
        case 0x40:
            pixelBit0 = x & 1;
            pixelBit1 = y & 1;
            pixelBit2 = (x & 2) >> 1;
            pixelBit3 = (x & 4) >> 2;
            pixelBit4 = (y & 2) >> 1;
            pixelBit5 = (y & 4) >> 2;
            break;
        case 0x80:
            pixelBit0 = y & 1;
            pixelBit1 = x & 1;
            pixelBit2 = (x & 2) >> 1;
            pixelBit3 = (x & 4) >> 2;
            pixelBit4 = (y & 2) >> 1;
            pixelBit5 = (y & 4) >> 2;
            break;
        default:
            pixelBit0 = x & 1;
            pixelBit1 = (x & 2) >> 1;
            pixelBit2 = y & 1;
            pixelBit3 = (x & 4) >> 2;
            pixelBit4 = (y & 2) >> 1;
            pixelBit5 = (y & 4) >> 2;
            break;
        }
    }

    if (thickness > 1) {
        pixelBit6 = z & 1;
        pixelBit7 = (z & 2) >> 1;
    }
    if (thickness == 8)
        pixelBit8 = (z & 4) >> 2;

    return (pixelBit8 << 8) | (pixelBit7 << 7) | (pixelBit6 << 6) |
           32 * pixelBit5 | 16 * pixelBit4 | 8 * pixelBit3 | 4 * pixelBit2 |
           pixelBit0 | 2 * pixelBit1;
}

int64_t computeSurfaceAddrFromCoordMicroTiled(uint32_t x, uint32_t y,
                                              uint32_t slice, uint32_t bpp,
                                              uint32_t pitch, uint32_t height,
                                              uint32_t tileMode, bool isDepth)
{
    uint32_t microTileThickness = 1;
    if (tileMode == 0x3)
        microTileThickness = 4;

    uint32_t microTileBytes = (64 * microTileThickness * bpp + 7) / 8;
    uint32_t microTilesPerRow = pitch >> 3;
    uint32_t microTileIndexX = x >> 3;
    uint32_t microTileIndexY = y >> 3;
    uint32_t microTileIndexZ = slice / microTileThickness;

    int64_t microTileOffset =
        (int64_t)microTileBytes * (microTileIndexX + (uint64_t)microTileIndexY * microTilesPerRow);
    int64_t sliceBytes = ((int64_t)pitch * height * microTileThickness * bpp + 7) / 8;
    int64_t sliceOffset = (int64_t)microTileIndexZ * sliceBytes;

    uint32_t pixelIndex = computePixelIndexWithinMicroTile(x, y, slice, bpp, tileMode, isDepth);
    int64_t pixelOffset = ((int64_t)bpp * pixelIndex) >> 3;

    return pixelOffset + microTileOffset + sliceOffset;
}

uint32_t computePipeFromCoordWoRotation(uint32_t x, uint32_t y)
{
    return ((y >> 3) ^ (x >> 3)) & 1;
}

uint32_t computeBankFromCoordWoRotation(uint32_t x, uint32_t y)
{
    return (((y >> 5) ^ (x >> 3)) & 1) | (2 * (((y >> 4) ^ (x >> 4)) & 1));
}

uint32_t computeSurfaceRotationFromTileMode(uint32_t tileMode)
{
    switch (tileMode) {
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return 2;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        return 1;
    default:
        return 0;
    }
}

int64_t computeSurfaceAddrFromCoordMacroTiled(
    uint32_t x, uint32_t y, uint32_t slice, uint32_t sample, uint32_t bpp,
    uint32_t pitch, uint32_t height, uint32_t numSamples, uint32_t tileMode,
    bool isDepth, uint32_t pipeSwizzle, uint32_t bankSwizzle)
{
    uint32_t microTileThickness = computeSurfaceThickness(tileMode);

    uint32_t microTileBits = numSamples * bpp * (microTileThickness * 64);
    uint32_t microTileBytes = (microTileBits + 7) / 8;

    uint32_t pixelIndex = computePixelIndexWithinMicroTile(x, y, slice, bpp, tileMode, isDepth);
    uint32_t bytesPerSample = microTileBytes / numSamples;
    uint32_t sampleOffset = 0;
    uint32_t pixelOffset = 0;
    uint32_t samplesPerSlice = 0;
    uint32_t numSampleSplits = 0;
    uint32_t sampleSlice = 0;

    if (isDepth) {
        sampleOffset = bpp * sample;
        pixelOffset = numSamples * bpp * pixelIndex;
    } else {
        sampleOffset = sample * (microTileBits / numSamples);
        pixelOffset = bpp * pixelIndex;
    }

    uint32_t elemOffset = pixelOffset + sampleOffset;

    if (numSamples <= 1 || microTileBytes <= 2048) {
        samplesPerSlice = numSamples;
        numSampleSplits = 1;
        sampleSlice = 0;
    } else {
        samplesPerSlice = 2048 / bytesPerSample;
        numSampleSplits = numSamples / samplesPerSlice;
        numSamples = samplesPerSlice;

        uint32_t tileSliceBits = microTileBits / numSampleSplits;
        sampleSlice = elemOffset / tileSliceBits;
        elemOffset %= tileSliceBits;
    }

    elemOffset = (elemOffset + 7) / 8;

    uint32_t pipe = computePipeFromCoordWoRotation(x, y);
    uint32_t bank = computeBankFromCoordWoRotation(x, y);

    uint32_t swizzle_ = pipeSwizzle + 2 * bankSwizzle;
    uint32_t bankPipe = pipe + 2 * bank;
    uint32_t rotation = computeSurfaceRotationFromTileMode(tileMode);
    uint32_t sliceIn = slice;

    if (isThickMacroTiled(tileMode) != 0)
        sliceIn >>= 2;

    bankPipe ^= 2 * sampleSlice * 3 ^ (swizzle_ + sliceIn * rotation);
    bankPipe %= 8;

    pipe = bankPipe % 2;
    bank = bankPipe / 2;

    uint32_t sliceBytes =
        (height * pitch * microTileThickness * bpp * numSamples + 7) / 8;
    uint32_t sliceOffset =
        sliceBytes * (sampleSlice + numSampleSplits * slice) / microTileThickness;

    uint32_t macroTilePitch = 32;
    uint32_t macroTileHeight = 16;

    switch (tileMode) {
    case 0x5:
    case 0x9:
        macroTilePitch = 16;
        macroTileHeight = 32;
        break;
    case 0x6:
    case 0x1:
        macroTilePitch = 8;
        macroTileHeight = 64;
        break;
    }

    uint32_t macroTilesPerRow = pitch / macroTilePitch;
    uint32_t macroTileBytes =
        (numSamples * microTileThickness * bpp * macroTileHeight * macroTilePitch + 7) / 8;
    uint32_t macroTileIndexX = x / macroTilePitch;
    uint32_t macroTileIndexY = y / macroTileHeight;
    int64_t macroTileOffset =
        (int64_t)(macroTileIndexX + (uint64_t)macroTilesPerRow * macroTileIndexY) * macroTileBytes;

    static const uint8_t bankSwapOrder[10] = {0, 1, 3, 2, 6, 7, 5, 4, 0, 0};

    if (isBankSwappedTileMode(tileMode) != 0) {
        uint32_t bankSwapWidth = computeSurfaceBankSwappedWidth(tileMode, bpp, 1, pitch);
        uint32_t swapIndex = macroTilePitch * macroTileIndexX / bankSwapWidth;
        bank ^= bankSwapOrder[swapIndex & 3];
    }

    int64_t totalOffset = elemOffset + ((macroTileOffset + sliceOffset) >> 3);
    return ((int64_t)bank << 9) | ((int64_t)pipe << 8) | (totalOffset & 255) |
           ((totalOffset & ~(int64_t)255) << 3);
}

} /* anonymous namespace */

bool compute_surface_info_mip0(uint32_t hw_format, uint32_t width,
                               uint32_t height, uint32_t tile_mode,
                               SurfaceInfo *out)
{
    if (!out)
        return false;

    uint32_t hwFormat = hw_format & 0x3F;
    if (formatHwInfo[hwFormat * 4] == 0)
        return false;

    SurfaceIn in = {};
    SurfaceOut o = {};

    /* Mirrors getSurfaceInfo() from the reference for level 0, 2D (dim 1),
     * no AA, slice 0. */
    if (tile_mode == 16) {
        uint32_t blockSize = (hwFormat < 0x31 || hwFormat > 0x35) ? 1 : 4;
        uint32_t w = ~(blockSize - 1) & (width + blockSize - 1);

        o.bpp = formatHwInfo[hwFormat * 4];
        o.pitch = w / blockSize;
        o.height = (~(blockSize - 1) & (height + blockSize - 1)) / blockSize;
        o.surfSize = (uint32_t)(((uint64_t)o.bpp * o.height * o.pitch) >> 3);
        o.tileMode = 16;
    } else {
        in.size = 60;
        in.tileMode = tile_mode & 0x0F;
        in.format = hwFormat;
        in.bpp = formatHwInfo[hwFormat * 4];
        in.numSamples = 1;
        in.numFrags = 1;
        in.width = umax(1, width);
        in.height = umax(1, height);
        in.numSlices = 1;
        in.slice = 0;
        in.mipLevel = 0;
        in.flags.value = 1u << 12; /* level 0 */

        computeSurfaceInfo(&in, &o);

        if (o.tileMode == 0)
            o.tileMode = 16;
    }

    out->pitch = o.pitch;
    out->height = o.height;
    out->surf_size = o.surfSize;
    out->tile_mode = o.tileMode;
    out->bpp = o.bpp;
    return out->pitch != 0 && out->height != 0 && out->surf_size != 0;
}

int64_t addr_from_coord(uint32_t x, uint32_t y, uint32_t bpp, uint32_t pitch,
                        uint32_t height, uint32_t tile_mode,
                        uint32_t pipe_swizzle, uint32_t bank_swizzle)
{
    uint32_t tm = (tile_mode == 16) ? 0 : tile_mode;

    if (tm == 0 || tm == 1)
        return ((int64_t)y * pitch + x) * (bpp / 8);
    if (tm == 2 || tm == 3)
        return computeSurfaceAddrFromCoordMicroTiled(x, y, 0, bpp, pitch,
                                                     height, tm, false);
    return computeSurfaceAddrFromCoordMacroTiled(x, y, 0, 0, bpp, pitch,
                                                 height, 1, tm, false,
                                                 pipe_swizzle, bank_swizzle);
}

} /* namespace gx2 */
} /* namespace texc */
