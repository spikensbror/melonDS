/*
    Copyright 2016-2020 Arisotura

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include "NDS.h"
#include "GPU.h"
#include "Config.h"
#include "Platform.h"


namespace GPU3D
{
namespace SoftRenderer
{

// buffer dimensions are 258x194 to add a offscreen 1px border
// which simplifies edge marking tests
// buffer is duplicated to keep track of the two topmost pixels
// TODO: check if the hardware can accidentally plot pixels
// offscreen in that border

const int ScanlineWidth = 258;
const int NumScanlines = 194;
const int BufferSize = ScanlineWidth * NumScanlines;
const int FirstPixelOffset = ScanlineWidth + 1;

u32 ColorBuffer[BufferSize * 2];
u32 DepthBuffer[BufferSize * 2];
u32 AttrBuffer[BufferSize * 2];

// attribute buffer:
// bit0-3: edge flags (left/right/top/bottom)
// bit4: backfacing flag
// bit8-12: antialiasing alpha
// bit15: fog enable
// bit16-21: polygon ID for translucent pixels
// bit22: translucent flag
// bit24-29: polygon ID for opaque pixels

u8 StencilBuffer[256*2];
bool PrevIsShadowMask;

bool Enabled;

bool FrameIdentical;

// threading

bool Threaded;
Platform::Thread* RenderThread;
bool RenderThreadRunning;
bool RenderThreadRendering;
Platform::Semaphore* Sema_RenderStart;
Platform::Semaphore* Sema_RenderDone;
Platform::Semaphore* Sema_ScanlineCount;

void RenderThreadFunc();


void StopRenderThread()
{
    if (RenderThreadRunning)
    {
        RenderThreadRunning = false;
        Platform::Semaphore_Post(Sema_RenderStart);
        Platform::Thread_Wait(RenderThread);
        Platform::Thread_Free(RenderThread);
    }
}

void SetupRenderThread()
{
    if (Threaded)
    {
        if (!RenderThreadRunning)
        {
            RenderThreadRunning = true;
            RenderThread = Platform::Thread_Create(RenderThreadFunc);
        }

        if (RenderThreadRendering)
            Platform::Semaphore_Wait(Sema_RenderDone);

        Platform::Semaphore_Reset(Sema_RenderStart);
        Platform::Semaphore_Reset(Sema_ScanlineCount);

        Platform::Semaphore_Post(Sema_RenderStart);
    }
    else
    {
        StopRenderThread();
    }
}


bool Init()
{
    Sema_RenderStart = Platform::Semaphore_Create();
    Sema_RenderDone = Platform::Semaphore_Create();
    Sema_ScanlineCount = Platform::Semaphore_Create();

    Threaded = false;
    RenderThreadRunning = false;
    RenderThreadRendering = false;

    return true;
}

void DeInit()
{
    StopRenderThread();

    Platform::Semaphore_Free(Sema_RenderStart);
    Platform::Semaphore_Free(Sema_RenderDone);
    Platform::Semaphore_Free(Sema_ScanlineCount);
}

void Reset()
{
    memset(ColorBuffer, 0, BufferSize * 2 * 4);
    memset(DepthBuffer, 0, BufferSize * 2 * 4);
    memset(AttrBuffer, 0, BufferSize * 2 * 4);

    PrevIsShadowMask = false;

    SetupRenderThread();
}

void SetRenderSettings(GPU::RenderSettings& settings)
{
    Threaded = settings.Soft_Threaded;
    SetupRenderThread();
}



// Notes on the interpolator:
//
// This is a theory on how the DS hardware interpolates values. It matches hardware output
// in the tests I did, but the hardware may be doing it differently. You never know.
//
// Assuming you want to perspective-correctly interpolate a variable named A across two points
// in a typical rasterizer, you would calculate A/W and 1/W at each point, interpolate linearly,
// then divide A/W by 1/W to recover the correct A value.
//
// The DS GPU approximates interpolation by calculating a perspective-correct interpolation
// between 0 and 1, then using the result as a factor to linearly interpolate the actual
// vertex attributes. The factor has 9 bits of precision when interpolating along Y and
// 8 bits along X.
//
// There's a special path for when the two W values are equal: it directly does linear
// interpolation, avoiding precision loss from the aforementioned approximation.
// Which is desirable when using the GPU to draw 2D graphics.

template<int dir>
class Interpolator
{
public:
    Interpolator() {}
    Interpolator(s32 x0, s32 x1, s32 w0, s32 w1)
    {
        Setup(x0, x1, w0, w1);
    }

    void Setup(s32 x0, s32 x1, s32 w0, s32 w1)
    {
        this->x0 = x0;
        this->x1 = x1;
        this->xdiff = x1 - x0;

        // calculate reciprocals for linear mode and Z interpolation
        // TODO eventually: use a faster reciprocal function?
        if (this->xdiff != 0)
            this->xrecip = (1<<30) / this->xdiff;
        else
            this->xrecip = 0;
        this->xrecip_z = this->xrecip >> 8;

        // linear mode is used if both W values are equal and have
        // low-order bits cleared (0-6 along X, 1-6 along Y)
        u32 mask = dir ? 0x7E : 0x7F;
        if ((w0 == w1) && !(w0 & mask) && !(w1 & mask))
            this->linear = true;
        else
            this->linear = false;

        if (dir)
        {
            // along Y

            if ((w0 & 0x1) && !(w1 & 0x1))
            {
                this->w0n = w0 - 1;
                this->w0d = w0 + 1;
                this->w1d = w1;
            }
            else
            {
                this->w0n = w0 & 0xFFFE;
                this->w0d = w0 & 0xFFFE;
                this->w1d = w1 & 0xFFFE;
            }

            this->shift = 9;
        }
        else
        {
            // along X

            this->w0n = w0;
            this->w0d = w0;
            this->w1d = w1;

            this->shift = 8;
        }
    }

    void SetX(s32 x)
    {
        x -= x0;
        this->x = x;
        if (xdiff != 0 && !linear)
        {
            s64 num = ((s64)x * w0n) << shift;
            s32 den = (x * w0d) + ((xdiff-x) * w1d);

            // this seems to be a proper division on hardware :/
            // I haven't been able to find cases that produce imperfect output
            if (den == 0) yfactor = 0;
            else          yfactor = (s32)(num / den);
        }
    }

    s32 Interpolate(s32 y0, s32 y1)
    {
        if (xdiff == 0 || y0 == y1) return y0;

        if (!linear)
        {
            // perspective-correct approx. interpolation
            if (y0 < y1)
                return y0 + (((y1-y0) * yfactor) >> shift);
            else
                return y1 + (((y0-y1) * ((1<<shift)-yfactor)) >> shift);
        }
        else
        {
            // linear interpolation
            // checkme: the rounding bias there (3<<24) is a guess
            if (y0 < y1)
                return y0 + ((((s64)(y1-y0) * x * xrecip) + (3<<24)) >> 30);
            else
                return y1 + ((((s64)(y0-y1) * (xdiff-x) * xrecip) + (3<<24)) >> 30);
        }
    }

    s32 InterpolateZ(s32 z0, s32 z1, bool wbuffer)
    {
        if (xdiff == 0 || z0 == z1) return z0;

        if (wbuffer)
        {
            // W-buffering: perspective-correct approx. interpolation
            if (z0 < z1)
                return z0 + (((s64)(z1-z0) * yfactor) >> shift);
            else
                return z1 + (((s64)(z0-z1) * ((1<<shift)-yfactor)) >> shift);
        }
        else
        {
            // Z-buffering: linear interpolation
            // still doesn't quite match hardware...
            s32 base, disp, factor;

            if (z0 < z1)
            {
                base = z0;
                disp = z1 - z0;
                factor = x;
            }
            else
            {
                base = z1;
                disp = z0 - z1,
                factor = xdiff - x;
            }

            if (dir)
            {
                int shift = 0;
                while (disp > 0x3FF)
                {
                    disp >>= 1;
                    shift++;
                }

                return base + ((((s64)disp * factor * xrecip_z) >> 22) << shift);
            }
            else
            {
                disp >>= 9;
                return base + (((s64)disp * factor * xrecip_z) >> 13);
            }
        }
    }

private:
    s32 x0, x1, xdiff, x;

    int shift;
    bool linear;

    s32 xrecip, xrecip_z;
    s32 w0n, w0d, w1d;

    u32 yfactor;
};


template<int side>
class Slope
{
public:
    Slope() {}

    s32 SetupDummy(s32 x0)
    {
        if (side)
        {
            dx = -0x40000;
            x0--;
        }
        else
        {
            dx = 0;
        }

        this->x0 = x0;
        this->xmin = x0;
        this->xmax = x0;

        Increment = 0;
        XMajor = false;

        Interp.Setup(0, 0, 0, 0);
        Interp.SetX(0);

        xcov_incr = 0;

        return x0;
    }

    s32 Setup(s32 x0, s32 x1, s32 y0, s32 y1, s32 w0, s32 w1, s32 y)
    {
        this->x0 = x0;
        this->y = y;

        if (x1 > x0)
        {
            this->xmin = x0;
            this->xmax = x1-1;
            this->Negative = false;
        }
        else if (x1 < x0)
        {
            this->xmin = x1;
            this->xmax = x0-1;
            this->Negative = true;
        }
        else
        {
            this->xmin = x0;
            if (side) this->xmin--;
            this->xmax = this->xmin;
            this->Negative = false;
        }

        xlen = xmax+1 - xmin;
        ylen = y1 - y0;

        // slope increment has a 18-bit fractional part
        // note: for some reason, x/y isn't calculated directly,
        // instead, 1/y is calculated and then multiplied by x
        // TODO: this is still not perfect (see for example x=169 y=33)
        if (ylen == 0)
            Increment = 0;
        else if (ylen == xlen)
            Increment = 0x40000;
        else
        {
            s32 yrecip = (1<<18) / ylen;
            Increment = (x1-x0) * yrecip;
            if (Increment < 0) Increment = -Increment;
        }

        XMajor = (Increment > 0x40000);

        if (side)
        {
            // right

            if (XMajor)              dx = Negative ? (0x20000 + 0x40000) : (Increment - 0x20000);
            else if (Increment != 0) dx = Negative ? 0x40000 : 0;
            else                     dx = -0x40000;
        }
        else
        {
            // left

            if (XMajor)              dx = Negative ? ((Increment - 0x20000) + 0x40000) : 0x20000;
            else if (Increment != 0) dx = Negative ? 0x40000 : 0;
            else                     dx = 0;
        }

        dx += (y - y0) * Increment;

        s32 x = XVal();

        if (XMajor)
        {
            if (side) Interp.Setup(x0-1, x1-1, w0, w1); // checkme
            else      Interp.Setup(x0, x1, w0, w1);
            Interp.SetX(x);

            // used for calculating AA coverage
            xcov_incr = (ylen << 10) / xlen;
        }
        else
        {
            Interp.Setup(y0, y1, w0, w1);
            Interp.SetX(y);
        }

        return x;
    }

    s32 Step()
    {
        dx += Increment;
        y++;

        s32 x = XVal();
        if (XMajor)
        {
            Interp.SetX(x);
        }
        else
        {
            Interp.SetX(y);
        }
        return x;
    }

    s32 XVal()
    {
        s32 ret;
        if (Negative) ret = x0 - (dx >> 18);
        else          ret = x0 + (dx >> 18);

        if (ret < xmin) ret = xmin;
        else if (ret > xmax) ret = xmax;
        return ret;
    }

    void EdgeParams_XMajor(s32* length, s32* coverage)
    {
        if (side ^ Negative)
            *length = (dx >> 18) - ((dx-Increment) >> 18);
        else
            *length = ((dx+Increment) >> 18) - (dx >> 18);

        // for X-major edges, we return the coverage
        // for the first pixel, and the increment for
        // further pixels on the same scanline
        s32 startx = dx >> 18;
        if (Negative) startx = xlen - startx;
        if (side)     startx = startx - *length + 1;

        s32 startcov = (((startx << 10) + 0x1FF) * ylen) / xlen;
        *coverage = (1<<31) | ((startcov & 0x3FF) << 12) | (xcov_incr & 0x3FF);
    }

    void EdgeParams_YMajor(s32* length, s32* coverage)
    {
        *length = 1;

        if (Increment == 0)
        {
            *coverage = 31;
        }
        else
        {
            s32 cov = ((dx >> 9) + (Increment >> 10)) >> 4;
            if ((cov >> 5) != (dx >> 18)) cov = 31;
            cov &= 0x1F;
            if (!(side ^ Negative)) cov = 0x1F - cov;

            *coverage = cov;
        }
    }

    void EdgeParams(s32* length, s32* coverage)
    {
        if (XMajor)
            return EdgeParams_XMajor(length, coverage);
        else
            return EdgeParams_YMajor(length, coverage);
    }

    s32 Increment;
    bool Negative;
    bool XMajor;
    Interpolator<1> Interp;

private:
    s32 x0, xmin, xmax;
    s32 xlen, ylen;
    s32 dx;
    s32 y;

    s32 xcov_incr;
    s32 ycoverage, ycov_incr;
};

struct RendererPolygon
{
    Polygon* PolyData;

    Slope<0> SlopeL;
    Slope<1> SlopeR;
    s32 XL, XR;
    u32 CurVL, CurVR;
    u32 NextVL, NextVR;

};

RendererPolygon PolygonList[2048];

template <typename T>
inline T ReadVRAM_Texture(u32 addr)
{
    return *(T*)&GPU::VRAMFlat_Texture[addr & 0x7FFFF];
}
template <typename T>
inline T ReadVRAM_TexPal(u32 addr)
{
    return *(T*)&GPU::VRAMFlat_TexPal[addr & 0x1FFFF];
}

void TextureLookup(u32 texparam, u32 texpal, s16 s, s16 t, u16* color, u8* alpha)
{
    u32 vramaddr = (texparam & 0xFFFF) << 3;

    s32 width = 8 << ((texparam >> 20) & 0x7);
    s32 height = 8 << ((texparam >> 23) & 0x7);

    s >>= 4;
    t >>= 4;

    // texture wrapping
    // TODO: optimize this somehow
    // testing shows that it's hardly worth optimizing, actually

    if (texparam & (1<<16))
    {
        if (texparam & (1<<18))
        {
            if (s & width) s = (width-1) - (s & (width-1));
            else           s = (s & (width-1));
        }
        else
            s &= width-1;
    }
    else
    {
        if (s < 0) s = 0;
        else if (s >= width) s = width-1;
    }

    if (texparam & (1<<17))
    {
        if (texparam & (1<<19))
        {
            if (t & height) t = (height-1) - (t & (height-1));
            else            t = (t & (height-1));
        }
        else
            t &= height-1;
    }
    else
    {
        if (t < 0) t = 0;
        else if (t >= height) t = height-1;
    }

    u8 alpha0;
    if (texparam & (1<<29)) alpha0 = 0;
    else                    alpha0 = 31;

    switch ((texparam >> 26) & 0x7)
    {
    case 1: // A3I5
        {
            vramaddr += ((t * width) + s);
            u8 pixel = ReadVRAM_Texture<u8>(vramaddr);

            texpal <<= 4;
            *color = ReadVRAM_TexPal<u16>(texpal + ((pixel&0x1F)<<1));
            *alpha = ((pixel >> 3) & 0x1C) + (pixel >> 6);
        }
        break;

    case 2: // 4-color
        {
            vramaddr += (((t * width) + s) >> 2);
            u8 pixel = ReadVRAM_Texture<u8>(vramaddr);
            pixel >>= ((s & 0x3) << 1);
            pixel &= 0x3;

            texpal <<= 3;
            *color = ReadVRAM_TexPal<u16>(texpal + (pixel<<1));
            *alpha = (pixel==0) ? alpha0 : 31;
        }
        break;

    case 3: // 16-color
        {
            vramaddr += (((t * width) + s) >> 1);
            u8 pixel = ReadVRAM_Texture<u8>(vramaddr);
            if (s & 0x1) pixel >>= 4;
            else         pixel &= 0xF;

            texpal <<= 4;
            *color = ReadVRAM_TexPal<u16>(texpal + (pixel<<1));
            *alpha = (pixel==0) ? alpha0 : 31;
        }
        break;

    case 4: // 256-color
        {
            vramaddr += ((t * width) + s);
            u8 pixel = ReadVRAM_Texture<u8>(vramaddr);

            texpal <<= 4;
            *color = ReadVRAM_TexPal<u16>(texpal + (pixel<<1));
            *alpha = (pixel==0) ? alpha0 : 31;
        }
        break;

    case 5: // compressed
        {
            vramaddr += ((t & 0x3FC) * (width>>2)) + (s & 0x3FC);
            vramaddr += (t & 0x3);

            u32 slot1addr = 0x20000 + ((vramaddr & 0x1FFFC) >> 1);
            if (vramaddr >= 0x40000)
                slot1addr += 0x10000;

            u8 val = ReadVRAM_Texture<u8>(vramaddr);
            val >>= (2 * (s & 0x3));

            u16 palinfo = ReadVRAM_Texture<u16>(slot1addr);
            u32 paloffset = (palinfo & 0x3FFF) << 2;
            texpal <<= 4;

            switch (val & 0x3)
            {
            case 0:
                *color = ReadVRAM_TexPal<u16>(texpal + paloffset);
                *alpha = 31;
                break;

            case 1:
                *color = ReadVRAM_TexPal<u16>(texpal + paloffset + 2);
                *alpha = 31;
                break;

            case 2:
                if ((palinfo >> 14) == 1)
                {
                    u16 color0 = ReadVRAM_TexPal<u16>(texpal + paloffset);
                    u16 color1 = ReadVRAM_TexPal<u16>(texpal + paloffset + 2);

                    u32 r0 = color0 & 0x001F;
                    u32 g0 = color0 & 0x03E0;
                    u32 b0 = color0 & 0x7C00;
                    u32 r1 = color1 & 0x001F;
                    u32 g1 = color1 & 0x03E0;
                    u32 b1 = color1 & 0x7C00;

                    u32 r = (r0 + r1) >> 1;
                    u32 g = ((g0 + g1) >> 1) & 0x03E0;
                    u32 b = ((b0 + b1) >> 1) & 0x7C00;

                    *color = r | g | b;
                }
                else if ((palinfo >> 14) == 3)
                {
                    u16 color0 = ReadVRAM_TexPal<u16>(texpal + paloffset);
                    u16 color1 = ReadVRAM_TexPal<u16>(texpal + paloffset + 2);

                    u32 r0 = color0 & 0x001F;
                    u32 g0 = color0 & 0x03E0;
                    u32 b0 = color0 & 0x7C00;
                    u32 r1 = color1 & 0x001F;
                    u32 g1 = color1 & 0x03E0;
                    u32 b1 = color1 & 0x7C00;

                    u32 r = (r0*5 + r1*3) >> 3;
                    u32 g = ((g0*5 + g1*3) >> 3) & 0x03E0;
                    u32 b = ((b0*5 + b1*3) >> 3) & 0x7C00;

                    *color = r | g | b;
                }
                else
                    *color = ReadVRAM_TexPal<u16>(texpal + paloffset + 4);
                *alpha = 31;
                break;

            case 3:
                if ((palinfo >> 14) == 2)
                {
                    *color = ReadVRAM_TexPal<u16>(texpal + paloffset + 6);
                    *alpha = 31;
                }
                else if ((palinfo >> 14) == 3)
                {
                    u16 color0 = ReadVRAM_TexPal<u16>(texpal + paloffset);
                    u16 color1 = ReadVRAM_TexPal<u16>(texpal + paloffset + 2);

                    u32 r0 = color0 & 0x001F;
                    u32 g0 = color0 & 0x03E0;
                    u32 b0 = color0 & 0x7C00;
                    u32 r1 = color1 & 0x001F;
                    u32 g1 = color1 & 0x03E0;
                    u32 b1 = color1 & 0x7C00;

                    u32 r = (r0*3 + r1*5) >> 3;
                    u32 g = ((g0*3 + g1*5) >> 3) & 0x03E0;
                    u32 b = ((b0*3 + b1*5) >> 3) & 0x7C00;

                    *color = r | g | b;
                    *alpha = 31;
                }
                else
                {
                    *color = 0;
                    *alpha = 0;
                }
                break;
            }
        }
        break;

    case 6: // A5I3
        {
            vramaddr += ((t * width) + s);
            u8 pixel = ReadVRAM_Texture<u8>(vramaddr);

            texpal <<= 4;
            *color = ReadVRAM_TexPal<u16>(texpal + ((pixel&0x7)<<1));
            *alpha = (pixel >> 3);
        }
        break;

    case 7: // direct color
        {
            vramaddr += (((t * width) + s) << 1);
            *color = ReadVRAM_Texture<u16>(vramaddr);
            *alpha = (*color & 0x8000) ? 31 : 0;
        }
        break;
    }
}

// depth test is 'less or equal' instead of 'less than' under the following conditions:
// * when drawing a front-facing pixel over an opaque back-facing pixel
// * when drawing wireframe edges, under certain conditions (TODO)
//
// range is different based on depth-buffering mode
// Z-buffering: +-0x200
// W-buffering: +-0xFF

bool DepthTest_Equal_Z(s32 dstz, s32 z, u32 dstattr)
{
    s32 diff = dstz - z;
    if ((u32)(diff + 0x200) <= 0x400)
        return true;

    return false;
}

bool DepthTest_Equal_W(s32 dstz, s32 z, u32 dstattr)
{
    s32 diff = dstz - z;
    if ((u32)(diff + 0xFF) <= 0x1FE)
        return true;

    return false;
}

bool DepthTest_LessThan(s32 dstz, s32 z, u32 dstattr)
{
    if (z < dstz)
        return true;

    return false;
}

bool DepthTest_LessThan_FrontFacing(s32 dstz, s32 z, u32 dstattr)
{
    if ((dstattr & 0x00400010) == 0x00000010) // opaque, back facing
    {
        if (z <= dstz)
            return true;
    }
    else
    {
        if (z < dstz)
            return true;
    }

    return false;
}

u32 AlphaBlend(u32 srccolor, u32 dstcolor, u32 alpha)
{
    u32 dstalpha = dstcolor >> 24;

    if (dstalpha == 0)
        return srccolor;

    u32 srcR = srccolor & 0x3F;
    u32 srcG = (srccolor >> 8) & 0x3F;
    u32 srcB = (srccolor >> 16) & 0x3F;

    if (RenderDispCnt & (1<<3))
    {
        u32 dstR = dstcolor & 0x3F;
        u32 dstG = (dstcolor >> 8) & 0x3F;
        u32 dstB = (dstcolor >> 16) & 0x3F;

        alpha++;
        srcR = ((srcR * alpha) + (dstR * (32-alpha))) >> 5;
        srcG = ((srcG * alpha) + (dstG * (32-alpha))) >> 5;
        srcB = ((srcB * alpha) + (dstB * (32-alpha))) >> 5;
        alpha--;
    }

    if (alpha > dstalpha)
        dstalpha = alpha;

    return srcR | (srcG << 8) | (srcB << 16) | (dstalpha << 24);
}

u32 RenderPixel(Polygon* polygon, u8 vr, u8 vg, u8 vb, s16 s, s16 t)
{
    u8 r, g, b, a;

    u32 blendmode = (polygon->Attr >> 4) & 0x3;
    u32 polyalpha = (polygon->Attr >> 16) & 0x1F;
    bool wireframe = (polyalpha == 0);

    if (blendmode == 2)
    {
        if (RenderDispCnt & (1<<1))
        {
            // highlight mode: color is calculated normally
            // except all vertex color components are set
            // to the red component
            // the toon color is added to the final color

            vg = vr;
            vb = vr;
        }
        else
        {
            // toon mode: vertex color is replaced by toon color

            u16 tooncolor = RenderToonTable[vr >> 1];

            vr = (tooncolor << 1) & 0x3E; if (vr) vr++;
            vg = (tooncolor >> 4) & 0x3E; if (vg) vg++;
            vb = (tooncolor >> 9) & 0x3E; if (vb) vb++;
        }
    }

    if ((RenderDispCnt & (1<<0)) && (((polygon->TexParam >> 26) & 0x7) != 0))
    {
        u8 tr, tg, tb;

        u16 tcolor; u8 talpha;
        TextureLookup(polygon->TexParam, polygon->TexPalette, s, t, &tcolor, &talpha);

        tr = (tcolor << 1) & 0x3E; if (tr) tr++;
        tg = (tcolor >> 4) & 0x3E; if (tg) tg++;
        tb = (tcolor >> 9) & 0x3E; if (tb) tb++;

        if (blendmode & 0x1)
        {
            // decal

            if (talpha == 0)
            {
                r = vr;
                g = vg;
                b = vb;
            }
            else if (talpha == 31)
            {
                r = tr;
                g = tg;
                b = tb;
            }
            else
            {
                r = ((tr * talpha) + (vr * (31-talpha))) >> 5;
                g = ((tg * talpha) + (vg * (31-talpha))) >> 5;
                b = ((tb * talpha) + (vb * (31-talpha))) >> 5;
            }
            a = polyalpha;
        }
        else
        {
            // modulate

            r = ((tr+1) * (vr+1) - 1) >> 6;
            g = ((tg+1) * (vg+1) - 1) >> 6;
            b = ((tb+1) * (vb+1) - 1) >> 6;
            a = ((talpha+1) * (polyalpha+1) - 1) >> 5;
        }
    }
    else
    {
        r = vr;
        g = vg;
        b = vb;
        a = polyalpha;
    }

    if ((blendmode == 2) && (RenderDispCnt & (1<<1)))
    {
        u16 tooncolor = RenderToonTable[vr >> 1];

        vr = (tooncolor << 1) & 0x3E; if (vr) vr++;
        vg = (tooncolor >> 4) & 0x3E; if (vg) vg++;
        vb = (tooncolor >> 9) & 0x3E; if (vb) vb++;

        r += vr;
        g += vg;
        b += vb;

        if (r > 63) r = 63;
        if (g > 63) g = 63;
        if (b > 63) b = 63;
    }

    // checkme: can wireframe polygons use texture alpha?
    if (wireframe) a = 31;

    return r | (g << 8) | (b << 16) | (a << 24);
}

void PlotTranslucentPixel(u32 pixeladdr, u32 color, u32 z, u32 polyattr, u32 shadow)
{
    u32 dstattr = AttrBuffer[pixeladdr];
    u32 attr = (polyattr & 0xE0F0) | ((polyattr >> 8) & 0xFF0000) | (1<<22) | (dstattr & 0xFF001F0F);

    if (shadow)
    {
        // for shadows, opaque pixels are also checked
        if (dstattr & (1<<22))
        {
            if ((dstattr & 0x007F0000) == (attr & 0x007F0000))
                return;
        }
        else
        {
            if ((dstattr & 0x3F000000) == (polyattr & 0x3F000000))
                return;
        }
    }
    else
    {
        // skip if translucent polygon IDs are equal
        if ((dstattr & 0x007F0000) == (attr & 0x007F0000))
            return;
    }

    // fog flag
    if (!(dstattr & (1<<15)))
        attr &= ~(1<<15);

    color = AlphaBlend(color, ColorBuffer[pixeladdr], color>>24);

    if (z != -1)
        DepthBuffer[pixeladdr] = z;

    ColorBuffer[pixeladdr] = color;
    AttrBuffer[pixeladdr] = attr;
}

void SetupPolygonLeftEdge(RendererPolygon* rp, s32 y)
{
    Polygon* polygon = rp->PolyData;

    while (y >= polygon->Vertices[rp->NextVL]->FinalPosition[1] && rp->CurVL != polygon->VBottom)
    {
        rp->CurVL = rp->NextVL;

        if (polygon->FacingView)
        {
            rp->NextVL = rp->CurVL + 1;
            if (rp->NextVL >= polygon->NumVertices)
                rp->NextVL = 0;
        }
        else
        {
            rp->NextVL = rp->CurVL - 1;
            if ((s32)rp->NextVL < 0)
                rp->NextVL = polygon->NumVertices - 1;
        }
    }

    rp->XL = rp->SlopeL.Setup(polygon->Vertices[rp->CurVL]->FinalPosition[0], polygon->Vertices[rp->NextVL]->FinalPosition[0],
                              polygon->Vertices[rp->CurVL]->FinalPosition[1], polygon->Vertices[rp->NextVL]->FinalPosition[1],
                              polygon->FinalW[rp->CurVL], polygon->FinalW[rp->NextVL], y);
}

void SetupPolygonRightEdge(RendererPolygon* rp, s32 y)
{
    Polygon* polygon = rp->PolyData;

    while (y >= polygon->Vertices[rp->NextVR]->FinalPosition[1] && rp->CurVR != polygon->VBottom)
    {
        rp->CurVR = rp->NextVR;

        if (polygon->FacingView)
        {
            rp->NextVR = rp->CurVR - 1;
            if ((s32)rp->NextVR < 0)
                rp->NextVR = polygon->NumVertices - 1;
        }
        else
        {
            rp->NextVR = rp->CurVR + 1;
            if (rp->NextVR >= polygon->NumVertices)
                rp->NextVR = 0;
        }
    }

    rp->XR = rp->SlopeR.Setup(polygon->Vertices[rp->CurVR]->FinalPosition[0], polygon->Vertices[rp->NextVR]->FinalPosition[0],
                              polygon->Vertices[rp->CurVR]->FinalPosition[1], polygon->Vertices[rp->NextVR]->FinalPosition[1],
                              polygon->FinalW[rp->CurVR], polygon->FinalW[rp->NextVR], y);
}

void SetupPolygon(RendererPolygon* rp, Polygon* polygon)
{
    u32 nverts = polygon->NumVertices;

    u32 vtop = polygon->VTop, vbot = polygon->VBottom;
    s32 ytop = polygon->YTop, ybot = polygon->YBottom;

    rp->PolyData = polygon;

    rp->CurVL = vtop;
    rp->CurVR = vtop;

    if (polygon->FacingView)
    {
        rp->NextVL = rp->CurVL + 1;
        if (rp->NextVL >= nverts) rp->NextVL = 0;
        rp->NextVR = rp->CurVR - 1;
        if ((s32)rp->NextVR < 0) rp->NextVR = nverts - 1;
    }
    else
    {
        rp->NextVL = rp->CurVL - 1;
        if ((s32)rp->NextVL < 0) rp->NextVL = nverts - 1;
        rp->NextVR = rp->CurVR + 1;
        if (rp->NextVR >= nverts) rp->NextVR = 0;
    }

    if (ybot == ytop)
    {
        vtop = 0; vbot = 0;
        int i;

        i = 1;
        if (polygon->Vertices[i]->FinalPosition[0] < polygon->Vertices[vtop]->FinalPosition[0]) vtop = i;
        if (polygon->Vertices[i]->FinalPosition[0] > polygon->Vertices[vbot]->FinalPosition[0]) vbot = i;

        i = nverts - 1;
        if (polygon->Vertices[i]->FinalPosition[0] < polygon->Vertices[vtop]->FinalPosition[0]) vtop = i;
        if (polygon->Vertices[i]->FinalPosition[0] > polygon->Vertices[vbot]->FinalPosition[0]) vbot = i;

        rp->CurVL = vtop; rp->NextVL = vtop;
        rp->CurVR = vbot; rp->NextVR = vbot;

        rp->XL = rp->SlopeL.SetupDummy(polygon->Vertices[rp->CurVL]->FinalPosition[0]);
        rp->XR = rp->SlopeR.SetupDummy(polygon->Vertices[rp->CurVR]->FinalPosition[0]);
    }
    else
    {
        SetupPolygonLeftEdge(rp, ytop);
        SetupPolygonRightEdge(rp, ytop);
    }
}

void RenderShadowMaskScanline(RendererPolygon* rp, s32 y)
{
    Polygon* polygon = rp->PolyData;

    u32 polyattr = (polygon->Attr & 0x3F008000);
    if (!polygon->FacingView) polyattr |= (1<<4);

    u32 polyalpha = (polygon->Attr >> 16) & 0x1F;
    bool wireframe = (polyalpha == 0);

    bool (*fnDepthTest)(s32 dstz, s32 z, u32 dstattr);
    if (polygon->Attr & (1<<14))
        fnDepthTest = polygon->WBuffer ? DepthTest_Equal_W : DepthTest_Equal_Z;
    else if (polygon->FacingView)
        fnDepthTest = DepthTest_LessThan_FrontFacing;
    else
        fnDepthTest = DepthTest_LessThan;

    if (!PrevIsShadowMask)
        memset(&StencilBuffer[256 * (y&0x1)], 0, 256);

    PrevIsShadowMask = true;

    if (polygon->YTop != polygon->YBottom)
    {
        if (y >= polygon->Vertices[rp->NextVL]->FinalPosition[1] && rp->CurVL != polygon->VBottom)
        {
            SetupPolygonLeftEdge(rp, y);
        }

        if (y >= polygon->Vertices[rp->NextVR]->FinalPosition[1] && rp->CurVR != polygon->VBottom)
        {
            SetupPolygonRightEdge(rp, y);
        }
    }

    Vertex *vlcur, *vlnext, *vrcur, *vrnext;
    s32 xstart, xend;
    bool l_filledge, r_filledge;
    s32 l_edgelen, r_edgelen;
    s32 l_edgecov, r_edgecov;
    Interpolator<1>* interp_start;
    Interpolator<1>* interp_end;

    xstart = rp->XL;
    xend = rp->XR;

    // CHECKME: edge fill rules for opaque shadow mask polygons

    if ((polyalpha < 31) || (RenderDispCnt & (3<<4)))
    {
        l_filledge = true;
        r_filledge = true;
    }
    else
    {
        l_filledge = (rp->SlopeL.Negative || !rp->SlopeL.XMajor);
        r_filledge = (!rp->SlopeR.Negative && rp->SlopeR.XMajor) || (rp->SlopeR.Increment==0);
    }

    s32 wl = rp->SlopeL.Interp.Interpolate(polygon->FinalW[rp->CurVL], polygon->FinalW[rp->NextVL]);
    s32 wr = rp->SlopeR.Interp.Interpolate(polygon->FinalW[rp->CurVR], polygon->FinalW[rp->NextVR]);

    s32 zl = rp->SlopeL.Interp.InterpolateZ(polygon->FinalZ[rp->CurVL], polygon->FinalZ[rp->NextVL], polygon->WBuffer);
    s32 zr = rp->SlopeR.Interp.InterpolateZ(polygon->FinalZ[rp->CurVR], polygon->FinalZ[rp->NextVR], polygon->WBuffer);

    // if the left and right edges are swapped, render backwards.
    if (xstart > xend)
    {
        vlcur = polygon->Vertices[rp->CurVR];
        vlnext = polygon->Vertices[rp->NextVR];
        vrcur = polygon->Vertices[rp->CurVL];
        vrnext = polygon->Vertices[rp->NextVL];

        interp_start = &rp->SlopeR.Interp;
        interp_end = &rp->SlopeL.Interp;

        rp->SlopeR.EdgeParams_YMajor(&l_edgelen, &l_edgecov);
        rp->SlopeL.EdgeParams_YMajor(&r_edgelen, &r_edgecov);

        s32 tmp;
        tmp = xstart; xstart = xend; xend = tmp;
        tmp = wl; wl = wr; wr = tmp;
        tmp = zl; zl = zr; zr = tmp;
        tmp = (s32)l_filledge; l_filledge = r_filledge; r_filledge = (bool)tmp;
    }
    else
    {
        vlcur = polygon->Vertices[rp->CurVL];
        vlnext = polygon->Vertices[rp->NextVL];
        vrcur = polygon->Vertices[rp->CurVR];
        vrnext = polygon->Vertices[rp->NextVR];

        interp_start = &rp->SlopeL.Interp;
        interp_end = &rp->SlopeR.Interp;

        rp->SlopeL.EdgeParams(&l_edgelen, &l_edgecov);
        rp->SlopeR.EdgeParams(&r_edgelen, &r_edgecov);
    }

    // color/texcoord attributes aren't needed for shadow masks
    // all the pixels are guaranteed to have the same alpha
    // even if a texture is used (decal blending is used for shadows)
    // similarly, we can perform alpha test early (checkme)

    if (wireframe) polyalpha = 31;
    if (polyalpha <= RenderAlphaRef) return;

    // in wireframe mode, there are special rules for equal Z (TODO)

    int yedge = 0;
    if (y == polygon->YTop)           yedge = 0x4;
    else if (y == polygon->YBottom-1) yedge = 0x8;
    int edge;

    s32 x = xstart;
    Interpolator<0> interpX(xstart, xend+1, wl, wr);

    if (x < 0) x = 0;
    s32 xlimit;

    // for shadow masks: set stencil bits where the depth test fails.
    // draw nothing.

    // part 1: left edge
    edge = yedge | 0x1;
    xlimit = xstart+l_edgelen;
    if (xlimit > xend+1) xlimit = xend+1;
    if (xlimit > 256) xlimit = 256;

    for (; x < xlimit; x++)
    {
        u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;

        interpX.SetX(x);

        s32 z = interpX.InterpolateZ(zl, zr, polygon->WBuffer);
        u32 dstattr = AttrBuffer[pixeladdr];

        // checkme
        if (!l_filledge)
            continue;

        if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
            StencilBuffer[256*(y&0x1) + x] |= 0x1;

        if (dstattr & 0x3)
        {
            pixeladdr += BufferSize;
            if (!fnDepthTest(DepthBuffer[pixeladdr], z, AttrBuffer[pixeladdr]))
                StencilBuffer[256*(y&0x1) + x] |= 0x2;
        }
    }

    // part 2: polygon inside
    edge = yedge;
    xlimit = xend-r_edgelen+1;
    if (xlimit > xend+1) xlimit = xend+1;
    if (xlimit > 256) xlimit = 256;
    if (wireframe && !edge) x = xlimit;
    else for (; x < xlimit; x++)
    {
        u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;

        interpX.SetX(x);

        s32 z = interpX.InterpolateZ(zl, zr, polygon->WBuffer);
        u32 dstattr = AttrBuffer[pixeladdr];

        if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
            StencilBuffer[256*(y&0x1) + x] = 1;

        if (dstattr & 0x3)
        {
            pixeladdr += BufferSize;
            if (!fnDepthTest(DepthBuffer[pixeladdr], z, AttrBuffer[pixeladdr]))
                StencilBuffer[256*(y&0x1) + x] |= 0x2;
        }
    }

    // part 3: right edge
    edge = yedge | 0x2;
    xlimit = xend+1;
    if (xlimit > 256) xlimit = 256;

    for (; x < xlimit; x++)
    {
        u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;

        interpX.SetX(x);

        s32 z = interpX.InterpolateZ(zl, zr, polygon->WBuffer);
        u32 dstattr = AttrBuffer[pixeladdr];

        // checkme
        if (!r_filledge)
            continue;

        if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
            StencilBuffer[256*(y&0x1) + x] = 1;

        if (dstattr & 0x3)
        {
            pixeladdr += BufferSize;
            if (!fnDepthTest(DepthBuffer[pixeladdr], z, AttrBuffer[pixeladdr]))
                StencilBuffer[256*(y&0x1) + x] |= 0x2;
        }
    }

    rp->XL = rp->SlopeL.Step();
    rp->XR = rp->SlopeR.Step();
}

void RenderPolygonScanline(RendererPolygon* rp, s32 y)
{
    Polygon* polygon = rp->PolyData;

    u32 polyattr = (polygon->Attr & 0x3F008000);
    if (!polygon->FacingView) polyattr |= (1<<4);

    u32 polyalpha = (polygon->Attr >> 16) & 0x1F;
    bool wireframe = (polyalpha == 0);

    bool (*fnDepthTest)(s32 dstz, s32 z, u32 dstattr);
    if (polygon->Attr & (1<<14))
        fnDepthTest = polygon->WBuffer ? DepthTest_Equal_W : DepthTest_Equal_Z;
    else if (polygon->FacingView)
        fnDepthTest = DepthTest_LessThan_FrontFacing;
    else
        fnDepthTest = DepthTest_LessThan;

    PrevIsShadowMask = false;

    if (polygon->YTop != polygon->YBottom)
    {
        if (y >= polygon->Vertices[rp->NextVL]->FinalPosition[1] && rp->CurVL != polygon->VBottom)
        {
            SetupPolygonLeftEdge(rp, y);
        }

        if (y >= polygon->Vertices[rp->NextVR]->FinalPosition[1] && rp->CurVR != polygon->VBottom)
        {
            SetupPolygonRightEdge(rp, y);
        }
    }

    Vertex *vlcur, *vlnext, *vrcur, *vrnext;
    s32 xstart, xend;
    bool l_filledge, r_filledge;
    s32 l_edgelen, r_edgelen;
    s32 l_edgecov, r_edgecov;
    Interpolator<1>* interp_start;
    Interpolator<1>* interp_end;

    xstart = rp->XL;
    xend = rp->XR;

    // edge fill rules for opaque pixels:
    // * right edge is filled if slope > 1
    // * left edge is filled if slope <= 1
    // * edges with slope = 0 are always filled
    // right vertical edges are pushed 1px to the left
    // edges are always filled if antialiasing/edgemarking are enabled or if the pixels are translucent

    if (wireframe || (RenderDispCnt & ((1<<4)|(1<<5))))
    {
        l_filledge = true;
        r_filledge = true;
    }
    else
    {
        l_filledge = (rp->SlopeL.Negative || !rp->SlopeL.XMajor);
        r_filledge = (!rp->SlopeR.Negative && rp->SlopeR.XMajor) || (rp->SlopeR.Increment==0);
    }

    s32 wl = rp->SlopeL.Interp.Interpolate(polygon->FinalW[rp->CurVL], polygon->FinalW[rp->NextVL]);
    s32 wr = rp->SlopeR.Interp.Interpolate(polygon->FinalW[rp->CurVR], polygon->FinalW[rp->NextVR]);

    s32 zl = rp->SlopeL.Interp.InterpolateZ(polygon->FinalZ[rp->CurVL], polygon->FinalZ[rp->NextVL], polygon->WBuffer);
    s32 zr = rp->SlopeR.Interp.InterpolateZ(polygon->FinalZ[rp->CurVR], polygon->FinalZ[rp->NextVR], polygon->WBuffer);

    // if the left and right edges are swapped, render backwards.
    // on hardware, swapped edges seem to break edge length calculation,
    // causing X-major edges to be rendered wrong when
    // wireframe/edgemarking/antialiasing are used
    // it also causes bad antialiasing, but not sure what's going on (TODO)
    // most probable explanation is that such slopes are considered to be Y-major

    if (xstart > xend)
    {
        vlcur = polygon->Vertices[rp->CurVR];
        vlnext = polygon->Vertices[rp->NextVR];
        vrcur = polygon->Vertices[rp->CurVL];
        vrnext = polygon->Vertices[rp->NextVL];

        interp_start = &rp->SlopeR.Interp;
        interp_end = &rp->SlopeL.Interp;

        rp->SlopeR.EdgeParams_YMajor(&l_edgelen, &l_edgecov);
        rp->SlopeL.EdgeParams_YMajor(&r_edgelen, &r_edgecov);

        s32 tmp;
        tmp = xstart; xstart = xend; xend = tmp;
        tmp = wl; wl = wr; wr = tmp;
        tmp = zl; zl = zr; zr = tmp;
        tmp = (s32)l_filledge; l_filledge = r_filledge; r_filledge = (bool)tmp;
    }
    else
    {
        vlcur = polygon->Vertices[rp->CurVL];
        vlnext = polygon->Vertices[rp->NextVL];
        vrcur = polygon->Vertices[rp->CurVR];
        vrnext = polygon->Vertices[rp->NextVR];

        interp_start = &rp->SlopeL.Interp;
        interp_end = &rp->SlopeR.Interp;

        rp->SlopeL.EdgeParams(&l_edgelen, &l_edgecov);
        rp->SlopeR.EdgeParams(&r_edgelen, &r_edgecov);
    }

    // interpolate attributes along Y

    s32 rl = interp_start->Interpolate(vlcur->FinalColor[0], vlnext->FinalColor[0]);
    s32 gl = interp_start->Interpolate(vlcur->FinalColor[1], vlnext->FinalColor[1]);
    s32 bl = interp_start->Interpolate(vlcur->FinalColor[2], vlnext->FinalColor[2]);

    s32 sl = interp_start->Interpolate(vlcur->TexCoords[0], vlnext->TexCoords[0]);
    s32 tl = interp_start->Interpolate(vlcur->TexCoords[1], vlnext->TexCoords[1]);

    s32 rr = interp_end->Interpolate(vrcur->FinalColor[0], vrnext->FinalColor[0]);
    s32 gr = interp_end->Interpolate(vrcur->FinalColor[1], vrnext->FinalColor[1]);
    s32 br = interp_end->Interpolate(vrcur->FinalColor[2], vrnext->FinalColor[2]);

    s32 sr = interp_end->Interpolate(vrcur->TexCoords[0], vrnext->TexCoords[0]);
    s32 tr = interp_end->Interpolate(vrcur->TexCoords[1], vrnext->TexCoords[1]);

    // in wireframe mode, there are special rules for equal Z (TODO)

    int yedge = 0;
    if (y == polygon->YTop)           yedge = 0x4;
    else if (y == polygon->YBottom-1) yedge = 0x8;
    int edge;

    s32 x = xstart;
    Interpolator<0> interpX(xstart, xend+1, wl, wr);

    if (x < 0) x = 0;
    s32 xlimit;

    s32 xcov = 0;

    // part 1: left edge
    edge = yedge | 0x1;
    xlimit = xstart+l_edgelen;
    if (xlimit > xend+1) xlimit = xend+1;
    if (xlimit > 256) xlimit = 256;
    if (l_edgecov & (1<<31))
    {
        xcov = (l_edgecov >> 12) & 0x3FF;
        if (xcov == 0x3FF) xcov = 0;
    }

    if (!l_filledge) x = std::min(xlimit, xend-r_edgelen+1);
    else
    for (; x < xlimit; x++)
    {
        u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;
        u32 dstattr = AttrBuffer[pixeladdr];

        // check stencil buffer for shadows
        if (polygon->IsShadow)
        {
            u8 stencil = StencilBuffer[256*(y&0x1) + x];
            if (!stencil)
                continue;
            if (!(stencil & 0x1))
                pixeladdr += BufferSize;
            if (!(stencil & 0x2))
                dstattr &= ~0x3; // quick way to prevent drawing the shadow under antialiased edges
        }

        interpX.SetX(x);

        s32 z = interpX.InterpolateZ(zl, zr, polygon->WBuffer);

        // if depth test against the topmost pixel fails, test
        // against the pixel underneath
        if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
        {
            if (!(dstattr & 0x3)) continue;

            pixeladdr += BufferSize;
            dstattr = AttrBuffer[pixeladdr];
            if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
                continue;
        }

        u32 vr = interpX.Interpolate(rl, rr);
        u32 vg = interpX.Interpolate(gl, gr);
        u32 vb = interpX.Interpolate(bl, br);

        s16 s = interpX.Interpolate(sl, sr);
        s16 t = interpX.Interpolate(tl, tr);

        u32 color = RenderPixel(polygon, vr>>3, vg>>3, vb>>3, s, t);
        u8 alpha = color >> 24;

        // alpha test
        if (alpha <= RenderAlphaRef) continue;

        if (alpha == 31)
        {
            u32 attr = polyattr | edge;

            if (RenderDispCnt & (1<<4))
            {
                // anti-aliasing: all edges are rendered

                // calculate coverage
                s32 cov = l_edgecov;
                if (cov & (1<<31))
                {
                    cov = xcov >> 5;
                    if (cov > 31) cov = 31;
                    xcov += (l_edgecov & 0x3FF);
                }
                attr |= (cov << 8);

                // push old pixel down if needed
                if (pixeladdr < BufferSize)
                {
                    ColorBuffer[pixeladdr+BufferSize] = ColorBuffer[pixeladdr];
                    DepthBuffer[pixeladdr+BufferSize] = DepthBuffer[pixeladdr];
                    AttrBuffer[pixeladdr+BufferSize] = AttrBuffer[pixeladdr];
                }
            }

            DepthBuffer[pixeladdr] = z;
            ColorBuffer[pixeladdr] = color;
            AttrBuffer[pixeladdr] = attr;
        }
        else
        {
            if (!(polygon->Attr & (1<<11))) z = -1;
            PlotTranslucentPixel(pixeladdr, color, z, polyattr, polygon->IsShadow);

            // blend with bottom pixel too, if needed
            if ((dstattr & 0x3) && (pixeladdr < BufferSize))
                PlotTranslucentPixel(pixeladdr+BufferSize, color, z, polyattr, polygon->IsShadow);
        }
    }

    // part 2: polygon inside
    edge = yedge;
    xlimit = xend-r_edgelen+1;
    if (xlimit > xend+1) xlimit = xend+1;
    if (xlimit > 256) xlimit = 256;

    if (wireframe && !edge) x = xlimit;
    else
    for (; x < xlimit; x++)
    {
        u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;
        u32 dstattr = AttrBuffer[pixeladdr];

        // check stencil buffer for shadows
        if (polygon->IsShadow)
        {
            u8 stencil = StencilBuffer[256*(y&0x1) + x];
            if (!stencil)
                continue;
            if (!(stencil & 0x1))
                pixeladdr += BufferSize;
            if (!(stencil & 0x2))
                dstattr &= ~0x3; // quick way to prevent drawing the shadow under antialiased edges
        }

        interpX.SetX(x);

        s32 z = interpX.InterpolateZ(zl, zr, polygon->WBuffer);

        // if depth test against the topmost pixel fails, test
        // against the pixel underneath
        if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
        {
            if (!(dstattr & 0x3)) continue;

            pixeladdr += BufferSize;
            dstattr = AttrBuffer[pixeladdr];
            if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
                continue;
        }

        u32 vr = interpX.Interpolate(rl, rr);
        u32 vg = interpX.Interpolate(gl, gr);
        u32 vb = interpX.Interpolate(bl, br);

        s16 s = interpX.Interpolate(sl, sr);
        s16 t = interpX.Interpolate(tl, tr);

        u32 color = RenderPixel(polygon, vr>>3, vg>>3, vb>>3, s, t);
        u8 alpha = color >> 24;

        // alpha test
        if (alpha <= RenderAlphaRef) continue;

        if (alpha == 31)
        {
            u32 attr = polyattr | edge;
            DepthBuffer[pixeladdr] = z;
            ColorBuffer[pixeladdr] = color;
            AttrBuffer[pixeladdr] = attr;
        }
        else
        {
            if (!(polygon->Attr & (1<<11))) z = -1;
            PlotTranslucentPixel(pixeladdr, color, z, polyattr, polygon->IsShadow);

            // blend with bottom pixel too, if needed
            if ((dstattr & 0x3) && (pixeladdr < BufferSize))
                PlotTranslucentPixel(pixeladdr+BufferSize, color, z, polyattr, polygon->IsShadow);
        }
    }

    // part 3: right edge
    edge = yedge | 0x2;
    xlimit = xend+1;
    if (xlimit > 256) xlimit = 256;
    if (r_edgecov & (1<<31))
    {
        xcov = (r_edgecov >> 12) & 0x3FF;
        if (xcov == 0x3FF) xcov = 0;
    }

    if (r_filledge)
    for (; x < xlimit; x++)
    {
        u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;
        u32 dstattr = AttrBuffer[pixeladdr];

        // check stencil buffer for shadows
        if (polygon->IsShadow)
        {
            u8 stencil = StencilBuffer[256*(y&0x1) + x];
            if (!stencil)
                continue;
            if (!(stencil & 0x1))
                pixeladdr += BufferSize;
            if (!(stencil & 0x2))
                dstattr &= ~0x3; // quick way to prevent drawing the shadow under antialiased edges
        }

        interpX.SetX(x);

        s32 z = interpX.InterpolateZ(zl, zr, polygon->WBuffer);

        // if depth test against the topmost pixel fails, test
        // against the pixel underneath
        if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
        {
            if (!(dstattr & 0x3)) continue;

            pixeladdr += BufferSize;
            dstattr = AttrBuffer[pixeladdr];
            if (!fnDepthTest(DepthBuffer[pixeladdr], z, dstattr))
                continue;
        }

        u32 vr = interpX.Interpolate(rl, rr);
        u32 vg = interpX.Interpolate(gl, gr);
        u32 vb = interpX.Interpolate(bl, br);

        s16 s = interpX.Interpolate(sl, sr);
        s16 t = interpX.Interpolate(tl, tr);

        u32 color = RenderPixel(polygon, vr>>3, vg>>3, vb>>3, s, t);
        u8 alpha = color >> 24;

        // alpha test
        if (alpha <= RenderAlphaRef) continue;

        if (alpha == 31)
        {
            u32 attr = polyattr | edge;

            if (RenderDispCnt & (1<<4))
            {
                // anti-aliasing: all edges are rendered

                // calculate coverage
                s32 cov = r_edgecov;
                if (cov & (1<<31))
                {
                    cov = 0x1F - (xcov >> 5);
                    if (cov < 0) cov = 0;
                    xcov += (r_edgecov & 0x3FF);
                }
                attr |= (cov << 8);

                // push old pixel down if needed
                if (pixeladdr < BufferSize)
                {
                    ColorBuffer[pixeladdr+BufferSize] = ColorBuffer[pixeladdr];
                    DepthBuffer[pixeladdr+BufferSize] = DepthBuffer[pixeladdr];
                    AttrBuffer[pixeladdr+BufferSize] = AttrBuffer[pixeladdr];
                }
            }

            DepthBuffer[pixeladdr] = z;
            ColorBuffer[pixeladdr] = color;
            AttrBuffer[pixeladdr] = attr;
        }
        else
        {
            if (!(polygon->Attr & (1<<11))) z = -1;
            PlotTranslucentPixel(pixeladdr, color, z, polyattr, polygon->IsShadow);

            // blend with bottom pixel too, if needed
            if ((dstattr & 0x3) && (pixeladdr < BufferSize))
                PlotTranslucentPixel(pixeladdr+BufferSize, color, z, polyattr, polygon->IsShadow);
        }
    }

    rp->XL = rp->SlopeL.Step();
    rp->XR = rp->SlopeR.Step();
}

void RenderScanline(s32 y, int npolys)
{
    for (int i = 0; i < npolys; i++)
    {
        RendererPolygon* rp = &PolygonList[i];
        Polygon* polygon = rp->PolyData;

        if (y >= polygon->YTop && (y < polygon->YBottom || (y == polygon->YTop && polygon->YBottom == polygon->YTop)))
        {
            if (polygon->IsShadowMask)
                RenderShadowMaskScanline(rp, y);
            else
                RenderPolygonScanline(rp, y);
        }
    }
}


u32 CalculateFogDensity(u32 pixeladdr)
{
    u32 z = DepthBuffer[pixeladdr];
    u32 densityid, densityfrac;

    if (z < RenderFogOffset)
    {
        densityid = 0;
        densityfrac = 0;
    }
    else
    {
        // technically: Z difference is shifted right by two, then shifted left by fog shift
        // then bit 0-16 are the fractional part and bit 17-31 are the density index
        // on hardware, the final value can overflow the 32-bit range with a shift big enough,
        // causing fog to 'wrap around' and accidentally apply to larger Z ranges

        z -= RenderFogOffset;
        z = (z >> 2) << RenderFogShift;

        densityid = z >> 17;
        if (densityid >= 32)
        {
            densityid = 32;
            densityfrac = 0;
        }
        else
            densityfrac = z & 0x1FFFF;
    }

    // checkme (may be too precise?)
    u32 density =
        ((RenderFogDensityTable[densityid] * (0x20000-densityfrac)) +
         (RenderFogDensityTable[densityid+1] * densityfrac)) >> 17;
    if (density >= 127) density = 128;

    return density;
}

void ScanlineFinalPass(s32 y)
{
    // to consider:
    // clearing all polygon fog flags if the master flag isn't set?
    // merging all final pass loops into one?

    if (RenderDispCnt & (1<<5))
    {
        // edge marking
        // only applied to topmost pixels

        for (int x = 0; x < 256; x++)
        {
            u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;

            u32 attr = AttrBuffer[pixeladdr];
            if (!(attr & 0xF)) continue;

            u32 polyid = attr >> 24; // opaque polygon IDs are used for edgemarking
            u32 z = DepthBuffer[pixeladdr];

            if (((polyid != (AttrBuffer[pixeladdr-1] >> 24)) && (z < DepthBuffer[pixeladdr-1])) ||
                ((polyid != (AttrBuffer[pixeladdr+1] >> 24)) && (z < DepthBuffer[pixeladdr+1])) ||
                ((polyid != (AttrBuffer[pixeladdr-ScanlineWidth] >> 24)) && (z < DepthBuffer[pixeladdr-ScanlineWidth])) ||
                ((polyid != (AttrBuffer[pixeladdr+ScanlineWidth] >> 24)) && (z < DepthBuffer[pixeladdr+ScanlineWidth])))
            {
                u16 edgecolor = RenderEdgeTable[polyid >> 3];
                u32 edgeR = (edgecolor << 1) & 0x3E; if (edgeR) edgeR++;
                u32 edgeG = (edgecolor >> 4) & 0x3E; if (edgeG) edgeG++;
                u32 edgeB = (edgecolor >> 9) & 0x3E; if (edgeB) edgeB++;

                ColorBuffer[pixeladdr] = edgeR | (edgeG << 8) | (edgeB << 16) | (ColorBuffer[pixeladdr] & 0xFF000000);

                // break antialiasing coverage (checkme)
                AttrBuffer[pixeladdr] = (AttrBuffer[pixeladdr] & 0xFFFFE0FF) | 0x00001000;
            }
        }
    }

    if (RenderDispCnt & (1<<7))
    {
        // fog

        // hardware testing shows that the fog step is 0x80000>>SHIFT
        // basically, the depth values used in GBAtek need to be
        // multiplied by 0x200 to match Z-buffer values

        // fog is applied to the topmost two pixels, which is required for
        // proper antialiasing

        // TODO: check the 'fog alpha glitch with small Z' GBAtek talks about

        bool fogcolor = !(RenderDispCnt & (1<<6));

        u32 fogR = (RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (RenderFogColor >> 16) & 0x1F;

        for (int x = 0; x < 256; x++)
        {
            u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;
            u32 density, srccolor, srcR, srcG, srcB, srcA;

            u32 attr = AttrBuffer[pixeladdr];
            if (!(attr & (1<<15))) continue;

            density = CalculateFogDensity(pixeladdr);

            srccolor = ColorBuffer[pixeladdr];
            srcR = srccolor & 0x3F;
            srcG = (srccolor >> 8) & 0x3F;
            srcB = (srccolor >> 16) & 0x3F;
            srcA = (srccolor >> 24) & 0x1F;

            if (fogcolor)
            {
                srcR = ((fogR * density) + (srcR * (128-density))) >> 7;
                srcG = ((fogG * density) + (srcG * (128-density))) >> 7;
                srcB = ((fogB * density) + (srcB * (128-density))) >> 7;
            }

            srcA = ((fogA * density) + (srcA * (128-density))) >> 7;

            ColorBuffer[pixeladdr] = srcR | (srcG << 8) | (srcB << 16) | (srcA << 24);

            // fog for lower pixel
            // TODO: make this code nicer, but avoid using a loop

            if (!(attr & 0x3)) continue;
            pixeladdr += BufferSize;

            attr = AttrBuffer[pixeladdr];
            if (!(attr & (1<<15))) continue;

            density = CalculateFogDensity(pixeladdr);

            srccolor = ColorBuffer[pixeladdr];
            srcR = srccolor & 0x3F;
            srcG = (srccolor >> 8) & 0x3F;
            srcB = (srccolor >> 16) & 0x3F;
            srcA = (srccolor >> 24) & 0x1F;

            if (fogcolor)
            {
                srcR = ((fogR * density) + (srcR * (128-density))) >> 7;
                srcG = ((fogG * density) + (srcG * (128-density))) >> 7;
                srcB = ((fogB * density) + (srcB * (128-density))) >> 7;
            }

            srcA = ((fogA * density) + (srcA * (128-density))) >> 7;

            ColorBuffer[pixeladdr] = srcR | (srcG << 8) | (srcB << 16) | (srcA << 24);
        }
    }

    if (RenderDispCnt & (1<<4))
    {
        // anti-aliasing

        // edges were flagged and their coverages calculated during rendering
        // this is where such edge pixels are blended with the pixels underneath

        for (int x = 0; x < 256; x++)
        {
            u32 pixeladdr = FirstPixelOffset + (y*ScanlineWidth) + x;

            u32 attr = AttrBuffer[pixeladdr];
            if (!(attr & 0x3)) continue;

            u32 coverage = (attr >> 8) & 0x1F;
            if (coverage == 0x1F) continue;

            if (coverage == 0)
            {
                ColorBuffer[pixeladdr] = ColorBuffer[pixeladdr+BufferSize];
                continue;
            }

            u32 topcolor = ColorBuffer[pixeladdr];
            u32 topR = topcolor & 0x3F;
            u32 topG = (topcolor >> 8) & 0x3F;
            u32 topB = (topcolor >> 16) & 0x3F;
            u32 topA = (topcolor >> 24) & 0x1F;

            u32 botcolor = ColorBuffer[pixeladdr+BufferSize];
            u32 botR = botcolor & 0x3F;
            u32 botG = (botcolor >> 8) & 0x3F;
            u32 botB = (botcolor >> 16) & 0x3F;
            u32 botA = (botcolor >> 24) & 0x1F;

            coverage++;

            // only blend color if the bottom pixel isn't fully transparent
            if (botA > 0)
            {
                topR = ((topR * coverage) + (botR * (32-coverage))) >> 5;
                topG = ((topG * coverage) + (botG * (32-coverage))) >> 5;
                topB = ((topB * coverage) + (botB * (32-coverage))) >> 5;
            }

            // alpha is always blended
            topA = ((topA * coverage) + (botA * (32-coverage))) >> 5;

            ColorBuffer[pixeladdr] = topR | (topG << 8) | (topB << 16) | (topA << 24);
        }
    }
}

void ClearBuffers()
{
    u32 clearz = ((RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
    u32 polyid = RenderClearAttr1 & 0x3F000000; // this sets the opaque polygonID

    // fill screen borders for edge marking

    for (int x = 0; x < ScanlineWidth; x++)
    {
        ColorBuffer[x] = 0;
        DepthBuffer[x] = clearz;
        AttrBuffer[x] = polyid;
    }

    for (int x = ScanlineWidth; x < ScanlineWidth*193; x+=ScanlineWidth)
    {
        ColorBuffer[x] = 0;
        DepthBuffer[x] = clearz;
        AttrBuffer[x] = polyid;
        ColorBuffer[x+257] = 0;
        DepthBuffer[x+257] = clearz;
        AttrBuffer[x+257] = polyid;
    }

    for (int x = ScanlineWidth*193; x < ScanlineWidth*194; x++)
    {
        ColorBuffer[x] = 0;
        DepthBuffer[x] = clearz;
        AttrBuffer[x] = polyid;
    }

    // clear the screen

    if (RenderDispCnt & (1<<14))
    {
        u8 xoff = (RenderClearAttr2 >> 16) & 0xFF;
        u8 yoff = (RenderClearAttr2 >> 24) & 0xFF;

        for (int y = 0; y < ScanlineWidth*192; y+=ScanlineWidth)
        {
            for (int x = 0; x < 256; x++)
            {
                u16 val2 = ReadVRAM_Texture<u16>(0x40000 + (yoff << 9) + (xoff << 1));
                u16 val3 = ReadVRAM_Texture<u16>(0x60000 + (yoff << 9) + (xoff << 1));

                // TODO: confirm color conversion
                u32 r = (val2 << 1) & 0x3E; if (r) r++;
                u32 g = (val2 >> 4) & 0x3E; if (g) g++;
                u32 b = (val2 >> 9) & 0x3E; if (b) b++;
                u32 a = (val2 & 0x8000) ? 0x1F000000 : 0;
                u32 color = r | (g << 8) | (b << 16) | a;

                u32 z = ((val3 & 0x7FFF) * 0x200) + 0x1FF;

                u32 pixeladdr = FirstPixelOffset + y + x;
                ColorBuffer[pixeladdr] = color;
                DepthBuffer[pixeladdr] = z;
                AttrBuffer[pixeladdr] = polyid | (val3 & 0x8000);

                xoff++;
            }

            yoff++;
        }
    }
    else
    {
        // TODO: confirm color conversion
        u32 r = (RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (RenderClearAttr1 >> 16) & 0x1F;
        u32 color = r | (g << 8) | (b << 16) | (a << 24);

		polyid |= (RenderClearAttr1 & 0x8000);

        for (int y = 0; y < ScanlineWidth*192; y+=ScanlineWidth)
        {
            for (int x = 0; x < 256; x++)
            {
                u32 pixeladdr = FirstPixelOffset + y + x;
                ColorBuffer[pixeladdr] = color;
                DepthBuffer[pixeladdr] = clearz;
                AttrBuffer[pixeladdr] = polyid;
            }
        }
    }
}

void RenderPolygons(bool threaded, Polygon** polygons, int npolys)
{
    int j = 0;
    for (int i = 0; i < npolys; i++)
    {
        if (polygons[i]->Degenerate) continue;
        SetupPolygon(&PolygonList[j++], polygons[i]);
    }

    RenderScanline(0, j);

    for (s32 y = 1; y < 192; y++)
    {
        RenderScanline(y, j);
        ScanlineFinalPass(y-1);

        if (threaded)
            Platform::Semaphore_Post(Sema_ScanlineCount);
    }

    ScanlineFinalPass(191);

    if (threaded)
        Platform::Semaphore_Post(Sema_ScanlineCount);
}

void VCount144()
{
    if (RenderThreadRunning)
        Platform::Semaphore_Wait(Sema_RenderDone);
}

void RenderFrame()
{
    auto textureDirty = GPU::VRAMDirty_Texture.DeriveState(GPU::VRAMMap_Texture);
    auto texPalDirty = GPU::VRAMDirty_TexPal.DeriveState(GPU::VRAMMap_TexPal);

    bool textureChanged = GPU::MakeVRAMFlat_TextureCoherent(textureDirty);
    bool texPalChanged = GPU::MakeVRAMFlat_TexPalCoherent(texPalDirty);

    FrameIdentical = !(textureChanged || texPalChanged) && RenderFrameIdentical;

    if (RenderThreadRunning)
    {
        Platform::Semaphore_Post(Sema_RenderStart);
    }
    else if (!FrameIdentical)
    {
        ClearBuffers();
        RenderPolygons(false, &RenderPolygonRAM[0], RenderNumPolygons);
    }
}

void RenderThreadFunc()
{
    for (;;)
    {
        Platform::Semaphore_Wait(Sema_RenderStart);
        if (!RenderThreadRunning) return;

        RenderThreadRendering = true;
        if (FrameIdentical)
        {
            Platform::Semaphore_Post(Sema_ScanlineCount, 192);
        }
        else
        {
            ClearBuffers();
            RenderPolygons(true, &RenderPolygonRAM[0], RenderNumPolygons);
        }

        Platform::Semaphore_Post(Sema_RenderDone);
        RenderThreadRendering = false;
    }
}

u32* GetLine(int line)
{
    if (RenderThreadRunning)
    {
        if (line < 192)
            Platform::Semaphore_Wait(Sema_ScanlineCount);
    }

    return &ColorBuffer[(line * ScanlineWidth) + FirstPixelOffset];
}

}
}
