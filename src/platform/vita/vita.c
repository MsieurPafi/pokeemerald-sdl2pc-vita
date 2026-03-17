/*
 * vita.c — PS Vita platform layer for pokeemerald
 *
 * Replaces src/platform/sdl2.c (Wii+SDL2 port) with VitaSDK equivalents.
 * The GBA hardware emulation (DMA, BIOS, scanline renderer) is copied verbatim
 * from sdl2.c — only the platform I/O is replaced.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <vita2d.h>

#define NO_UNDERSCORE_HACK

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"

/* VitaSDK/newlib: declare user heap size.
 * Without this the default heap is ~4 MB which is far too small for pokeemerald.
 * 192 MB is the practical ceiling for a 512 MB Vita; use 128 MB to be safe. */
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;

extern void (*const gIntrTable[])(void);
extern u16 gPlttBufferUnfaded[];
extern u16 gPlttBufferFaded[];

/* =========================================================================
 * GBA hardware state — emulated memory regions
 * ========================================================================= */

u16 INTR_CHECK;
void *INTR_VECTOR;
unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
static FILE *sDbgLog = NULL;  /* forward-declared so LZ77 debug can use it */
static int sFrameCount = 0;   /* forward-declared so LZ77 debug can log frame number */
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));
unsigned char FLASH_BASE[131072] __attribute__((aligned(4)));
struct SoundInfo *SOUND_INFO_PTR;

/* =========================================================================
 * DMA emulation (verbatim from sdl2.c)
 * ========================================================================= */

#define DMA_COUNT 4

struct DMATransfer {
    union {
        const void *src;
        const u16  *src16;
        const u32  *src32;
    };
    union {
        void  *dst;
        vu16  *dst16;
        vu32  *dst32;
    };
    u32 size;
    u16 control;
} DMAList[DMA_COUNT];

enum { DMA_NOW, DMA_VBLANK, DMA_HBLANK, DMA_SPECIAL };

static void RunDMAs(u32 type)
{
    for (int dmaNum = 0; dmaNum < DMA_COUNT; dmaNum++)
    {
        struct DMATransfer *dma = &DMAList[dmaNum];
        u32 dmaCntReg = (&REG_DMA0CNT)[dmaNum * 3];
        if (!((dmaCntReg >> 16) & DMA_ENABLE))
            dma->control &= ~DMA_ENABLE;

        if ((dma->control & DMA_ENABLE) &&
            (((dma->control & DMA_START_MASK) >> 12) == type))
        {
            for (int i = 0; i < (int)(dma->size); i++)
            {
                if ((dma->control) & DMA_32BIT)
                     *dma->dst32 = *dma->src32;
                else *dma->dst16 = *dma->src16;

                if (((dma->control) & DMA_DEST_MASK) == DMA_DEST_INC)
                {
                    if ((dma->control) & DMA_32BIT) dma->dst32++;
                    else                            dma->dst16++;
                }
                else if (((dma->control) & DMA_DEST_MASK) == DMA_DEST_DEC)
                {
                    if ((dma->control) & DMA_32BIT) dma->dst32--;
                    else                            dma->dst16--;
                }
                else if (((dma->control) & DMA_DEST_MASK) == DMA_DEST_RELOAD)
                {
                    if ((dma->control) & DMA_32BIT) dma->dst32++;
                    else                            dma->dst16++;
                }

                if (((dma->control) & DMA_SRC_MASK) == DMA_SRC_INC)
                {
                    if ((dma->control) & DMA_32BIT) dma->src32++;
                    else                            dma->src16++;
                }
                else if (((dma->control) & DMA_SRC_MASK) == DMA_SRC_DEC)
                {
                    if ((dma->control) & DMA_32BIT) dma->src32--;
                    else                            dma->src16--;
                }
            }

            if (dma->control & DMA_REPEAT)
            {
                dma->size = ((&REG_DMA0CNT)[dmaNum * 3] & 0x1FFFF);
                if (((dma->control) & DMA_DEST_MASK) == DMA_DEST_RELOAD)
                    dma->dst = ((&REG_DMA0DAD)[dmaNum * 3]);
            }
            else
            {
                dma->control &= ~DMA_ENABLE;
            }
        }
    }
}

void DmaSet(int dmaNum, const void *src, void *dest, u32 control)
{
    if (dmaNum >= DMA_COUNT)
    {
        fprintf(stderr, "DmaSet: invalid DMA number %d\n", dmaNum);
        return;
    }
    (&REG_DMA0SAD)[dmaNum * 3] = src;
    (&REG_DMA0DAD)[dmaNum * 3] = dest;
    (&REG_DMA0CNT)[dmaNum * 3] = control;

    struct DMATransfer *dma = &DMAList[dmaNum];
    dma->src     = src;
    dma->dst     = dest;
    dma->size    = control & 0x1ffff;
    dma->control = control >> 16;
    RunDMAs(DMA_NOW);
}

/* =========================================================================
 * BIOS functions (verbatim from sdl2.c)
 * ========================================================================= */

static uint32_t CPUReadMemory(const void *src)   { return *(uint32_t *)src; }
static void CPUWriteMemory(void *dest, uint32_t val) { *(uint32_t *)dest = val; }
static uint16_t CPUReadHalfWord(const void *src) { return *(uint16_t *)src; }
static void CPUWriteHalfWord(void *dest, uint16_t val) { *(uint16_t *)dest = val; }
static uint8_t CPUReadByte(const void *src) { return *(uint8_t *)src; }
static void CPUWriteByte(void *dest, uint8_t val) { *(uint8_t *)dest = val; }

void CpuSet(const void *src, void *dst, u32 cnt)
{
    if (dst == NULL) { puts("CpuSet to NULL\n"); return; }
    int count = cnt & 0x1FFFFF;
    const u8 *source = src;
    u8 *dest = dst;
    if ((cnt >> 26) & 1) {
        if ((cnt >> 24) & 1) {
            uint32_t value = CPUReadMemory(source);
            while (count) { CPUWriteMemory(dest, value); dest += 4; count--; }
        } else {
            while (count) { CPUWriteMemory(dest, CPUReadMemory(source)); source += 4; dest += 4; count--; }
        }
    } else {
        if ((cnt >> 24) & 1) {
            uint16_t value = CPUReadHalfWord(source);
            while (count) { CPUWriteHalfWord(dest, value); dest += 2; count--; }
        } else {
            while (count) { CPUWriteHalfWord(dest, CPUReadHalfWord(source)); source += 2; dest += 2; count--; }
        }
    }
}

void CpuFastSet(const void *src, void *dst, u32 cnt)
{
    if (dst == NULL) { puts("CpuFastSet to NULL\n"); return; }
    int count = cnt & 0x1FFFFF;
    const u8 *source = src;
    u8 *dest = dst;
    if ((cnt >> 24) & 1) {
        uint32_t value = CPUReadMemory(source);
        while (count > 0) {
            for (int i = 0; i < 8; i++) { CPUWriteMemory(dest, value); dest += 4; }
            count -= 8;
        }
    } else {
        while (count > 0) {
            for (int i = 0; i < 8; i++) {
                uint32_t value = CPUReadMemory(source);
                CPUWriteMemory(dest, value);
                source += 4; dest += 4;
            }
            count -= 8;
        }
    }
}

/* Standard GBA LZ77 decompression.
   Header: [0x10, size_lo, size_mid, size_hi] (4 bytes)
   Flags byte (MSB first): 0=literal, 1=back-reference (2 bytes: len/offset). */
static void lz77_decomp(const u8 *src, u8 *dest)
{
    static int sLz77Count = 0;
    static const char sHex[]="0123456789ABCDEF";
    int destSize = src[1] | (src[2] << 8) | (src[3] << 16);
    int srcPos   = 4;
    int destPos  = 0;

    if (sLz77Count < 30 && sDbgLog) {
        sLz77Count++;
        /* log header: type byte, destSize, frame count */
        char buf[80];
        buf[0]='L';buf[1]='Z';buf[2]='7';buf[3]='7';buf[4]='#';
        /* two-digit count */
        buf[5]='0'+(sLz77Count/10); buf[6]='0'+(sLz77Count%10); buf[7]=' ';
        buf[8]='t';buf[9]='y';buf[10]='p';buf[11]='e';buf[12]='=';
        buf[13]='0';buf[14]='x';buf[15]=sHex[(src[0]>>4)&0xF];buf[16]=sHex[src[0]&0xF];
        buf[17]=' ';buf[18]='s';buf[19]='z';buf[20]='=';
        unsigned int ds = (unsigned int)destSize;
        buf[21]=sHex[(ds>>28)&0xF];buf[22]=sHex[(ds>>24)&0xF];
        buf[23]=sHex[(ds>>20)&0xF];buf[24]=sHex[(ds>>16)&0xF];
        buf[25]=sHex[(ds>>12)&0xF];buf[26]=sHex[(ds>>8)&0xF];
        buf[27]=sHex[(ds>>4)&0xF];buf[28]=sHex[ds&0xF];
        /* append frame number */
        buf[29]=' ';buf[30]='f';buf[31]='r';buf[32]='m';buf[33]='=';
        unsigned int fc = (unsigned int)sFrameCount;
        buf[34]=sHex[(fc>>12)&0xF];buf[35]=sHex[(fc>>8)&0xF];
        buf[36]=sHex[(fc>>4)&0xF];buf[37]=sHex[fc&0xF];
        buf[38]='\0';
        fputs(buf, sDbgLog); fputs("\n", sDbgLog); fflush(sDbgLog);
    }

    while (destPos < destSize) {
        u8 flags = src[srcPos++];
        for (int i = 0; i < 8 && destPos < destSize; i++, flags <<= 1) {
            if (flags & 0x80) {
                int ref      = (src[srcPos] << 8) | src[srcPos + 1];
                srcPos      += 2;
                int length   = (ref >> 12) + 3;
                int offset   = (ref & 0xFFF) + 1;
                int copyPos  = destPos - offset;
                for (int j = 0; j < length && destPos < destSize; j++)
                    dest[destPos++] = dest[copyPos + j];
            } else {
                dest[destPos++] = src[srcPos++];
            }
        }
    }
}

void LZ77UnCompVram(const u32 *src_, void *dest_)
{
    lz77_decomp((const u8 *)src_, (u8 *)dest_);
}

void LZ77UnCompWram(const u32 *src_, void *dest_)
{
    lz77_decomp((const u8 *)src_, (u8 *)dest_);
}

void RLUnCompWram(const void *src, void *dest)
{
    int remaining = CPUReadMemory(src) >> 8;
    int padding = (4 - remaining) & 0x3;
    src += 4;
    while (remaining > 0) {
        int blockHeader = CPUReadByte(src++);
        if (blockHeader & 0x80) {
            blockHeader = (blockHeader & 0x7F) + 3;
            int block = CPUReadByte(src++);
            while (blockHeader-- && remaining) { remaining--; CPUWriteByte(dest, block); dest++; }
        } else {
            blockHeader++;
            while (blockHeader-- && remaining) {
                remaining--;
                CPUWriteByte(dest, CPUReadByte(src++));
                dest++;
            }
        }
    }
    while (padding--) { CPUWriteByte(dest, 0); dest++; }
}

void RLUnCompVram(const void *src, void *dest)
{
    int remaining = CPUReadMemory(src) >> 8;
    int padding = (4 - remaining) & 0x3;
    int halfWord = 0;
    src += 4;
    while (remaining > 0) {
        int blockHeader = CPUReadByte(src++);
        if (blockHeader & 0x80) {
            blockHeader = (blockHeader & 0x7F) + 3;
            int block = CPUReadByte(src++);
            while (blockHeader-- && remaining) {
                remaining--;
                if ((u32)(uintptr_t)dest & 1) { halfWord |= block << 8; CPUWriteHalfWord((void *)((uintptr_t)dest ^ 1), halfWord); }
                else halfWord = block;
                dest++;
            }
        } else {
            blockHeader++;
            while (blockHeader-- && remaining) {
                remaining--;
                u8 byte = CPUReadByte(src++);
                if ((u32)(uintptr_t)dest & 1) { halfWord |= byte << 8; CPUWriteHalfWord((void *)((uintptr_t)dest ^ 1), halfWord); }
                else halfWord = byte;
                dest++;
            }
        }
    }
    if ((u32)(uintptr_t)dest & 1) { padding--; dest++; }
    for (; padding > 0; padding -= 2, dest += 2) CPUWriteHalfWord(dest, 0);
}

static const s16 sineTable[256] = {
    (s16)0x0000, (s16)0x0192, (s16)0x0323, (s16)0x04B5, (s16)0x0645, (s16)0x07D5, (s16)0x0964, (s16)0x0AF1,
    (s16)0x0C7C, (s16)0x0E05, (s16)0x0F8C, (s16)0x1111, (s16)0x1294, (s16)0x1413, (s16)0x158F, (s16)0x1708,
    (s16)0x187D, (s16)0x19EF, (s16)0x1B5D, (s16)0x1CC6, (s16)0x1E2B, (s16)0x1F8B, (s16)0x20E7, (s16)0x223D,
    (s16)0x238E, (s16)0x24DA, (s16)0x261F, (s16)0x275F, (s16)0x2899, (s16)0x29CD, (s16)0x2AFA, (s16)0x2C21,
    (s16)0x2D41, (s16)0x2E5A, (s16)0x2F6B, (s16)0x3076, (s16)0x3179, (s16)0x3274, (s16)0x3367, (s16)0x3453,
    (s16)0x3536, (s16)0x3612, (s16)0x36E5, (s16)0x37AF, (s16)0x3871, (s16)0x392A, (s16)0x39DA, (s16)0x3A82,
    (s16)0x3B20, (s16)0x3BB6, (s16)0x3C42, (s16)0x3CC5, (s16)0x3D3E, (s16)0x3DAE, (s16)0x3E14, (s16)0x3E71,
    (s16)0x3EC5, (s16)0x3F0E, (s16)0x3F4E, (s16)0x3F84, (s16)0x3FB1, (s16)0x3FD3, (s16)0x3FEC, (s16)0x3FFB,
    (s16)0x4000, (s16)0x3FFB, (s16)0x3FEC, (s16)0x3FD3, (s16)0x3FB1, (s16)0x3F84, (s16)0x3F4E, (s16)0x3F0E,
    (s16)0x3EC5, (s16)0x3E71, (s16)0x3E14, (s16)0x3DAE, (s16)0x3D3E, (s16)0x3CC5, (s16)0x3C42, (s16)0x3BB6,
    (s16)0x3B20, (s16)0x3A82, (s16)0x39DA, (s16)0x392A, (s16)0x3871, (s16)0x37AF, (s16)0x36E5, (s16)0x3612,
    (s16)0x3536, (s16)0x3453, (s16)0x3367, (s16)0x3274, (s16)0x3179, (s16)0x3076, (s16)0x2F6B, (s16)0x2E5A,
    (s16)0x2D41, (s16)0x2C21, (s16)0x2AFA, (s16)0x29CD, (s16)0x2899, (s16)0x275F, (s16)0x261F, (s16)0x24DA,
    (s16)0x238E, (s16)0x223D, (s16)0x20E7, (s16)0x1F8B, (s16)0x1E2B, (s16)0x1CC6, (s16)0x1B5D, (s16)0x19EF,
    (s16)0x187D, (s16)0x1708, (s16)0x158F, (s16)0x1413, (s16)0x1294, (s16)0x1111, (s16)0x0F8C, (s16)0x0E05,
    (s16)0x0C7C, (s16)0x0AF1, (s16)0x0964, (s16)0x07D5, (s16)0x0645, (s16)0x04B5, (s16)0x0323, (s16)0x0192,
    (s16)0x0000, (s16)0xFE6E, (s16)0xFCDD, (s16)0xFB4B, (s16)0xF9BB, (s16)0xF82B, (s16)0xF69C, (s16)0xF50F,
    (s16)0xF384, (s16)0xF1FB, (s16)0xF074, (s16)0xEEEF, (s16)0xED6C, (s16)0xEBED, (s16)0xEA71, (s16)0xE8F8,
    (s16)0xE783, (s16)0xE611, (s16)0xE4A3, (s16)0xE33A, (s16)0xE1D5, (s16)0xE075, (s16)0xDF19, (s16)0xDDC3,
    (s16)0xDC72, (s16)0xDB26, (s16)0xD9E1, (s16)0xD8A1, (s16)0xD767, (s16)0xD633, (s16)0xD506, (s16)0xD3DF,
    (s16)0xD2BF, (s16)0xD1A6, (s16)0xD095, (s16)0xCF8A, (s16)0xCE87, (s16)0xCD8C, (s16)0xCC99, (s16)0xCBAD,
    (s16)0xCACA, (s16)0xC9EE, (s16)0xC91B, (s16)0xC851, (s16)0xC78F, (s16)0xC6D6, (s16)0xC626, (s16)0xC57E,
    (s16)0xC4E0, (s16)0xC44A, (s16)0xC3BE, (s16)0xC33B, (s16)0xC2C2, (s16)0xC252, (s16)0xC1EC, (s16)0xC18F,
    (s16)0xC13B, (s16)0xC0F2, (s16)0xC0B2, (s16)0xC07C, (s16)0xC04F, (s16)0xC02D, (s16)0xC014, (s16)0xC005,
    (s16)0xC000, (s16)0xC005, (s16)0xC014, (s16)0xC02D, (s16)0xC04F, (s16)0xC07C, (s16)0xC0B2, (s16)0xC0F2,
    (s16)0xC13B, (s16)0xC18F, (s16)0xC1EC, (s16)0xC252, (s16)0xC2C2, (s16)0xC33B, (s16)0xC3BE, (s16)0xC44A,
    (s16)0xC4E0, (s16)0xC57E, (s16)0xC626, (s16)0xC6D6, (s16)0xC78F, (s16)0xC851, (s16)0xC91B, (s16)0xC9EE,
    (s16)0xCACA, (s16)0xCBAD, (s16)0xCC99, (s16)0xCD8C, (s16)0xCE87, (s16)0xCF8A, (s16)0xD095, (s16)0xD1A6,
    (s16)0xD2BF, (s16)0xD3DF, (s16)0xD506, (s16)0xD633, (s16)0xD767, (s16)0xD8A1, (s16)0xD9E1, (s16)0xDB26,
    (s16)0xDC72, (s16)0xDDC3, (s16)0xDF19, (s16)0xE075, (s16)0xE1D5, (s16)0xE33A, (s16)0xE4A3, (s16)0xE611,
    (s16)0xE783, (s16)0xE8F8, (s16)0xEA71, (s16)0xEBED, (s16)0xED6C, (s16)0xEEEF, (s16)0xF074, (s16)0xF1FB,
    (s16)0xF384, (s16)0xF50F, (s16)0xF69C, (s16)0xF82B, (s16)0xF9BB, (s16)0xFB4B, (s16)0xFCDD, (s16)0xFE6E
};

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    for (s32 i = 0; i < count; i++) {
        s32 cx = src[i].texX, cy = src[i].texY;
        s16 dispx = src[i].scrX, dispy = src[i].scrY;
        s16 rx = src[i].sx, ry = src[i].sy;
        u16 theta = src[i].alpha >> 8;
        s32 a = sineTable[(theta + 0x40) & 255];
        s32 b = sineTable[theta];
        s16 dx = (rx * a) >> 14, dmx = (rx * b) >> 14;
        s16 dy = (ry * b) >> 14, dmy = (ry * a) >> 14;
        dest[i].pa = dx; dest[i].pb = -dmx;
        dest[i].pc = dy; dest[i].pd = dmy;
        dest[i].dx = cx - dx * dispx + dmx * dispy;
        dest[i].dy = cy - dy * dispx - dmy * dispy;
    }
}

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    for (s32 i = 0; i < count; i++) {
        s16 rx = src[i].xScale, ry = src[i].yScale;
        u16 theta = src[i].rotation >> 8;
        s32 a = (s32)sineTable[(theta + 64) & 255];
        s32 b = (s32)sineTable[theta];
        s16 dx = ((s32)rx * a) >> 14, dmx = ((s32)rx * b) >> 14;
        s16 dy = ((s32)ry * b) >> 14, dmy = ((s32)ry * a) >> 14;
        CPUWriteHalfWord(dest, dx);         dest += offset;
        CPUWriteHalfWord(dest, -dmx);       dest += offset;
        CPUWriteHalfWord(dest, dy);         dest += offset;
        CPUWriteHalfWord(dest, dmy);        dest += offset;
    }
}

void SoftReset(u32 resetFlags)
{
    puts("SoftReset called. Exiting.");
    sceKernelExitProcess(0);
}

u16 ArcTan(s16 i)
{
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan2(s16 x, s16 y)
{
    if (!y) return (x >= 0) ? 0 : 0x8000;
    if (!x) return (y >= 0) ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) { if (x >= y) return ArcTan((y << 14) / x); }
        else if (-x >= y) return ArcTan((y << 14) / x) + 0x8000;
        return 0x4000 - ArcTan((x << 14) / y);
    } else {
        if (x <= 0) { if (-x > -y) return ArcTan((y << 14) / x) + 0x8000; }
        else if (x >= -y) return ArcTan((y << 14) / x) + 0x10000;
        return 0xC000 - ArcTan((x << 14) / y);
    }
}

u16 Sqrt(u32 num)
{
    if (!num) return 0;
    u32 lower, upper = num, bound = 1;
    while (bound < upper) { upper >>= 1; bound <<= 1; }
    while (1) {
        upper = num;
        u32 accum = 0;
        lower = bound;
        while (1) {
            u32 oldLower = lower;
            if (lower <= upper >> 1) lower <<= 1;
            if (oldLower >= upper >> 1) break;
        }
        while (1) {
            accum <<= 1;
            if (upper >= lower) { ++accum; upper -= lower; }
            if (lower == bound) break;
            lower >>= 1;
        }
        u32 oldBound = bound;
        bound += accum; bound >>= 1;
        if (bound >= oldBound) { bound = oldBound; break; }
    }
    return bound;
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do { out |= (bin % 10) * placeCounter; placeCounter *= 16; } while ((bin /= 10) > 0);
    return out;
}

uint16_t *memsetu16(uint16_t *dst, uint16_t fill, size_t count)
{
    for (size_t i = 0; i < count; i++) *dst++ = fill;
    return dst;
}

/* =========================================================================
 * Scanline renderer (verbatim from sdl2.c, no changes)
 * ========================================================================= */

struct scanlineData {
    uint16_t layers[4][DISPLAY_WIDTH];
    uint16_t spriteLayers[4][DISPLAY_WIDTH];
    uint16_t bgcnts[4];
    uint16_t winMask[DISPLAY_WIDTH];
    char bgtoprio[4];
    char prioritySortedBgs[4][4];
    char prioritySortedBgsCount[4];
};

static const uint16_t bgMapSizes[][2] = { {32,32},{64,32},{32,64},{64,64} };

#define mosaicBGEffectX          (REG_MOSAIC & 0xF)
#define mosaicBGEffectY          ((REG_MOSAIC >> 4) & 0xF)
#define mosaicSpriteEffectX      ((REG_MOSAIC >> 8) & 0xF)
#define mosaicSpriteEffectY      ((REG_MOSAIC >> 12) & 0xF)
#define applyBGHorizontalMosaicEffect(x) (x - (x % (mosaicBGEffectX+1)))
#define applyBGVerticalMosaicEffect(y)   (y - (y % (mosaicBGEffectY+1)))
#define applySpriteHorizontalMosaicEffect(x) (x - (x % (mosaicSpriteEffectX+1)))
#define applySpriteVerticalMosaicEffect(y)   (y - (y % (mosaicSpriteEffectY+1)))

static void RenderBGScanline(int bgNum, uint16_t control, uint16_t hoffs, uint16_t voffs,
                              int lineNum, uint16_t *line)
{
    unsigned int charBaseBlock  = (control >> 2) & 3;
    unsigned int screenBaseBlock = (control >> 8) & 0x1F;
    unsigned int bitsPerPixel   = ((control >> 7) & 1) ? 8 : 4;
    unsigned int mapWidth       = bgMapSizes[control >> 14][0];
    unsigned int mapHeight      = bgMapSizes[control >> 14][1];
    unsigned int mapWidthInPixels  = mapWidth * 8;
    unsigned int mapHeightInPixels = mapHeight * 8;

    uint8_t  *bgtiles = (uint8_t *)BG_CHAR_ADDR(charBaseBlock);
    uint16_t *pal     = (uint16_t *)PLTT;

    if (control & BGCNT_MOSAIC) lineNum = applyBGVerticalMosaicEffect(lineNum);
    hoffs &= 0x1FF; voffs &= 0x1FF;

    for (unsigned int x = 0; x < DISPLAY_WIDTH; x++) {
        uint16_t *bgmap = (uint16_t *)BG_SCREEN_ADDR(screenBaseBlock);
        unsigned int xx = (control & BGCNT_MOSAIC)
                          ? (applyBGHorizontalMosaicEffect(x) + hoffs) & 0x1FF
                          : (x + hoffs) & 0x1FF;
        unsigned int yy = (lineNum + voffs) & 0x1FF;

        if (xx > 255 && mapWidthInPixels > 256)  bgmap += 0x400;
        if (yy > 255 && mapHeightInPixels > 256) bgmap += (mapWidthInPixels > 256) ? 0x800 : 0x400;

        xx &= 0xFF; yy &= 0xFF;

        unsigned int mapX = xx / 8, mapY = yy / 8;
        uint16_t entry = bgmap[mapY * 32 + mapX];
        unsigned int tileNum    = entry & 0x3FF;
        unsigned int paletteNum = (entry >> 12) & 0xF;
        unsigned int tileX = xx % 8, tileY = yy % 8;

        if (entry & (1 << 10)) tileX = 7 - tileX;
        if (entry & (1 << 11)) tileY = 7 - tileY;

        uint16_t tileLoc  = tileNum * (bitsPerPixel * 8);
        uint16_t tileLocY = tileY * bitsPerPixel;
        uint16_t tileLocX = tileX;
        if (bitsPerPixel == 4) tileLocX /= 2;

        uint8_t pixel = bgtiles[tileLoc + tileLocY + tileLocX];
        if (bitsPerPixel == 4) {
            if (tileX & 1) pixel >>= 4; else pixel &= 0xF;
            if (pixel != 0) line[x] = pal[16 * paletteNum + pixel] | 0x8000;
        } else {
            line[x] = pal[pixel] | 0x8000;
        }
    }
}

static inline uint32_t getBgX(int n) { return (n == 2) ? REG_BG2X : REG_BG3X; }
static inline uint32_t getBgY(int n) { return (n == 2) ? REG_BG2Y : REG_BG3Y; }
static inline uint16_t getBgPA(int n) { return (n == 2) ? REG_BG2PA : REG_BG3PA; }
static inline uint16_t getBgPB(int n) { return (n == 2) ? REG_BG2PB : REG_BG3PB; }
static inline uint16_t getBgPC(int n) { return (n == 2) ? REG_BG2PC : REG_BG3PC; }
static inline uint16_t getBgPD(int n) { return (n == 2) ? REG_BG2PD : REG_BG3PD; }

static void RenderRotScaleBGScanline(int bgNum, uint16_t control, uint16_t x, uint16_t y,
                                      int lineNum, uint16_t *line)
{
    vBgCnt *bgcnt = (vBgCnt *)&control;
    unsigned int charBaseBlock   = (control >> 2) & 3;
    unsigned int screenBaseBlock = (control >> 8) & 0x1F;
    unsigned int mapWidth = 1 << (4 + (bgcnt->screenSize));

    uint8_t  *bgtiles = (uint8_t *)BG_CHAR_ADDR(charBaseBlock);
    uint8_t  *bgmap   = (uint8_t *)BG_SCREEN_ADDR(screenBaseBlock);
    uint16_t *pal     = (uint16_t *)PLTT;

    if (control & BGCNT_MOSAIC) lineNum = applyBGVerticalMosaicEffect(lineNum);

    s16 pa = getBgPA(bgNum), pb = getBgPB(bgNum);
    s16 pc = getBgPC(bgNum), pd = getBgPD(bgNum);
    int sizeX = 128, sizeY = 128;
    switch (bgcnt->screenSize) {
        case 1: sizeX = sizeY = 256; break;
        case 2: sizeX = sizeY = 512; break;
        case 3: sizeX = sizeY = 1024; break;
    }
    int maskX = sizeX - 1, maskY = sizeY - 1;
    int yshift = ((control >> 14) & 3) + 4;

    s32 currentX = getBgX(bgNum), currentY = getBgY(bgNum);
    currentX += lineNum * pb; currentY += lineNum * pd;
    int realX = currentX, realY = currentY;

    if (bgcnt->areaOverflowMode) {
        for (int xi = 0; xi < DISPLAY_WIDTH; xi++) {
            int xxx = (realX >> 8) & maskX, yyy = (realY >> 8) & maskY;
            int tile = bgmap[(xxx >> 3) + ((yyy >> 3) << yshift)];
            uint8_t pixel = bgtiles[(tile << 6) + ((yyy & 7) << 3) + (xxx & 7)];
            if (pixel != 0) line[xi] = pal[pixel] | 0x8000;
            realX += pa; realY += pc;
        }
    } else {
        for (int xi = 0; xi < DISPLAY_WIDTH; xi++) {
            int xxx = realX >> 8, yyy = realY >> 8;
            if (xxx >= 0 && yyy >= 0 && xxx < sizeX && yyy < sizeY) {
                int tile = bgmap[(xxx >> 3) + ((yyy >> 3) << yshift)];
                uint8_t pixel = bgtiles[(tile << 6) + ((yyy & 7) << 3) + (xxx & 7)];
                if (pixel != 0) line[xi] = pal[pixel] | 0x8000;
            }
            realX += pa; realY += pc;
        }
    }
    if (control & BGCNT_MOSAIC && mosaicBGEffectX > 0) {
        for (int xi = 0; xi < DISPLAY_WIDTH; xi++)
            line[xi] = line[applyBGHorizontalMosaicEffect(xi)];
    }
}

static const u8 spriteSizes[][2] = { {8,16},{8,32},{16,32},{32,64} };

#define getAlphaBit(x)    ((x >> 15) & 1)
#define getRedChannel(x)  ((x >>  0) & 0x1F)
#define getGreenChannel(x)((x >>  5) & 0x1F)
#define getBlueChannel(x) ((x >> 10) & 0x1F)
#define isbgEnabled(x)    (((REG_DISPCNT >> 8) & 0xF) & (1 << x))

static uint16_t alphaBlendColor(uint16_t A, uint16_t B)
{
    unsigned int eva = REG_BLDALPHA & 0x1F, evb = (REG_BLDALPHA >> 8) & 0x1F;
    unsigned int r = ((getRedChannel(A)  *eva)+(getRedChannel(B)  *evb))>>4;
    unsigned int g = ((getGreenChannel(A)*eva)+(getGreenChannel(B)*evb))>>4;
    unsigned int b = ((getBlueChannel(A) *eva)+(getBlueChannel(B) *evb))>>4;
    if (r>31) r=31; if (g>31) g=31; if (b>31) b=31;
    return r | (g<<5) | (b<<10) | (1<<15);
}

static uint16_t alphaBrightnessIncrease(uint16_t A)
{
    unsigned int evy = REG_BLDY & 0x1F;
    unsigned int r = getRedChannel(A)  + (31-getRedChannel(A)  )*evy/16;
    unsigned int g = getGreenChannel(A)+ (31-getGreenChannel(A))*evy/16;
    unsigned int b = getBlueChannel(A) + (31-getBlueChannel(A) )*evy/16;
    if (r>31) r=31; if (g>31) g=31; if (b>31) b=31;
    return r | (g<<5) | (b<<10) | (1<<15);
}

static uint16_t alphaBrightnessDecrease(uint16_t A)
{
    unsigned int evy = REG_BLDY & 0x1F;
    unsigned int r = getRedChannel(A)  - getRedChannel(A)  *evy/16;
    unsigned int g = getGreenChannel(A)- getGreenChannel(A)*evy/16;
    unsigned int b = getBlueChannel(A) - getBlueChannel(A) *evy/16;
    if (r>31) r=31; if (g>31) g=31; if (b>31) b=31;
    return r | (g<<5) | (b<<10) | (1<<15);
}

static bool alphaBlendSelectTargetB(struct scanlineData *scanline, uint16_t *colorOutput,
                                     char prnum, char prsub, int pixelpos, bool spriteBlendEnabled)
{
    for (unsigned int blndprnum = prnum; blndprnum <= 3; blndprnum++) {
        if (spriteBlendEnabled && getAlphaBit(scanline->spriteLayers[blndprnum][pixelpos])) {
            *colorOutput = scanline->spriteLayers[blndprnum][pixelpos]; return true;
        }
        for (unsigned int blndprsub = prsub; blndprsub < (unsigned)scanline->prioritySortedBgsCount[blndprnum]; blndprsub++) {
            char currLayer = scanline->prioritySortedBgs[blndprnum][blndprsub];
            if (getAlphaBit(scanline->layers[currLayer][pixelpos]) && (REG_BLDCNT & (1<<(8+currLayer))) && isbgEnabled(currLayer))
                { *colorOutput = scanline->layers[currLayer][pixelpos]; return true; }
            if (getAlphaBit(scanline->layers[currLayer][pixelpos]) && isbgEnabled(currLayer) && prnum != blndprnum)
                return false;
        }
        prsub = 0;
    }
    if (REG_BLDCNT & BLDCNT_TGT2_BD) { *colorOutput = *(uint16_t *)PLTT; return true; }
    return false;
}

#define WINMASK_BG0   (1<<0)
#define WINMASK_BG1   (1<<1)
#define WINMASK_BG2   (1<<2)
#define WINMASK_BG3   (1<<3)
#define WINMASK_OBJ   (1<<4)
#define WINMASK_CLR   (1<<5)
#define WINMASK_WINOUT (1<<6)

static bool winCheckHorizontalBounds(u16 left, u16 right, u16 xpos)
{
    if (left > right) return (xpos >= left || xpos < right);
    else              return (xpos >= left && xpos < right);
}

static void DrawSprites(struct scanlineData *scanline, uint16_t vcount, bool windowsEnabled)
{
    void *objtiles = OBJ_VRAM0;
    unsigned int blendMode = (REG_BLDCNT >> 6) & 3;
    bool winShouldBlendPixel = true;
    int16_t matrix[2][2] = {};

    if (!(REG_DISPCNT & (1 << 6))) puts("2-D OBJ Character mapping not supported.");

    for (int i = 127; i >= 0; i--) {
        struct OamData *oam = &((struct OamData *)OAM)[i];
        bool isAffine            = oam->affineMode & 1;
        bool doubleSizeOrDisabled= (oam->affineMode >> 1) & 1;
        bool isSemiTransparent   = (oam->objMode == 1);
        bool isObjWin            = (oam->objMode == 2);

        if (!isAffine && doubleSizeOrDisabled) continue;

        unsigned int width, height;
        if (oam->shape == 0) {
            width = height = (1 << oam->size) * 8;
        } else if (oam->shape == 1) {
            width = spriteSizes[oam->size][1]; height = spriteSizes[oam->size][0];
        } else if (oam->shape == 2) {
            width = spriteSizes[oam->size][0]; height = spriteSizes[oam->size][1];
        } else continue;

        int rect_width = width, rect_height = height;
        int half_width = width/2, half_height = height/2;
        uint16_t *pixels = scanline->spriteLayers[oam->priority];

        int32_t x = oam->x, y = oam->y;
        if (x >= DISPLAY_WIDTH)  x -= 512;
        if (y >= DISPLAY_HEIGHT) y -= 256;

        if (isAffine) {
            u8 mn = oam->matrixNum * 4;
            matrix[0][0] = ((struct OamData *)OAM)[mn+0].affineParam;
            matrix[0][1] = ((struct OamData *)OAM)[mn+1].affineParam;
            matrix[1][0] = ((struct OamData *)OAM)[mn+2].affineParam;
            matrix[1][1] = ((struct OamData *)OAM)[mn+3].affineParam;
            if (doubleSizeOrDisabled) { rect_width*=2; rect_height*=2; half_width*=2; half_height*=2; }
        } else {
            matrix[0][0]=0x100; matrix[0][1]=0; matrix[1][0]=0; matrix[1][1]=0x100;
        }
        x += half_width; y += half_height;

        if (vcount >= (unsigned)(y-half_height) && vcount < (unsigned)(y+half_height)) {
            int local_y = (oam->mosaic) ? applySpriteVerticalMosaicEffect(vcount)-y : vcount-y;
            bool flipX = !isAffine && ((oam->matrixNum>>3) & 1);
            bool flipY = !isAffine && ((oam->matrixNum>>4) & 1);
            bool is8BPP = oam->bpp & 1;

            for (int local_x = -half_width; local_x <= half_width; local_x++) {
                unsigned int global_x = local_x + x;
                if (global_x >= (unsigned)DISPLAY_WIDTH) continue;

                int tex_x, tex_y, local_mosaicX;
                if (oam->mosaic) {
                    local_mosaicX = applySpriteHorizontalMosaicEffect(global_x) - x;
                    tex_x = ((matrix[0][0]*local_mosaicX + matrix[0][1]*local_y)>>8) + (width/2);
                    tex_y = ((matrix[1][0]*local_mosaicX + matrix[1][1]*local_y)>>8) + (height/2);
                } else {
                    tex_x = ((matrix[0][0]*local_x + matrix[0][1]*local_y)>>8) + (width/2);
                    tex_y = ((matrix[1][0]*local_x + matrix[1][1]*local_y)>>8) + (height/2);
                }
                if (tex_x>=(int)width || tex_y>=(int)height || tex_x<0 || tex_y<0) continue;
                if (flipX) tex_x = width-tex_x-1;
                if (flipY) tex_y = height-tex_y-1;

                int tile_x=tex_x%8, tile_y=tex_y%8;
                int block_x=tex_x/8, block_y=tex_y/8;
                int block_offset = block_y*(REG_DISPCNT&0x40 ? (width/8):16) + block_x;
                uint16_t pixel = 0;

                uint8_t  *tiledata = (uint8_t *)objtiles;
                uint16_t *pal2     = (uint16_t *)(PLTT + 0x200);
                if (!is8BPP) {
                    pixel = tiledata[(block_offset+oam->tileNum)*32 + (tile_y*4) + (tile_x/2)];
                    if (tile_x&1) pixel>>=4; else pixel&=0xF;
                    pal2 += oam->paletteNum * 16;
                } else {
                    pixel = tiledata[(block_offset*2+oam->tileNum)*32 + (tile_y*8)+tile_x];
                }

                if (pixel != 0) {
                    uint16_t color = pal2[pixel];
                    if (isObjWin) {
                        if (scanline->winMask[global_x] & WINMASK_WINOUT)
                            scanline->winMask[global_x] = (REG_WINOUT>>8) & 0x3F;
                        continue;
                    }
                    winShouldBlendPixel = (!windowsEnabled || (scanline->winMask[global_x] & WINMASK_CLR));
                    if ((blendMode==1 && (REG_BLDCNT&BLDCNT_TGT1_OBJ) && winShouldBlendPixel) || isSemiTransparent) {
                        uint16_t targetB = 0;
                        if (alphaBlendSelectTargetB(scanline, &targetB, oam->priority, 0, global_x, false))
                            color = alphaBlendColor(color, targetB);
                    } else if ((REG_BLDCNT&BLDCNT_TGT1_OBJ) && winShouldBlendPixel) {
                        if (blendMode==2) color = alphaBrightnessIncrease(color);
                        else if (blendMode==3) color = alphaBrightnessDecrease(color);
                    }
                    pixels[global_x] = color | (1<<15);
                }
            }
        }
    }
}

static void DrawScanline(uint16_t *pixels, uint16_t vcount)
{
    unsigned int mode = REG_DISPCNT & 3;
    unsigned char numOfBgs = (mode == 0) ? 4 : 3;
    int bgnum, prnum;
    struct scanlineData scanline;
    unsigned int blendMode = (REG_BLDCNT >> 6) & 3;
    unsigned int xpos;

    memset(scanline.layers, 0, sizeof(scanline.layers));
    memset(scanline.winMask, 0, sizeof(scanline.winMask));
    memset(scanline.spriteLayers, 0, sizeof(scanline.spriteLayers));
    memset(scanline.prioritySortedBgsCount, 0, sizeof(scanline.prioritySortedBgsCount));

    for (bgnum = 0; bgnum < numOfBgs; bgnum++) {
        uint16_t bgcnt = *(uint16_t *)(REG_ADDR_BG0CNT + bgnum*2);
        uint16_t priority;
        scanline.bgcnts[bgnum] = bgcnt;
        scanline.bgtoprio[bgnum] = priority = (bgcnt & 3);
        char pc = scanline.prioritySortedBgsCount[priority];
        scanline.prioritySortedBgs[priority][pc] = bgnum;
        scanline.prioritySortedBgsCount[priority]++;
    }

    switch (mode) {
    case 0:
        for (bgnum = 3; bgnum >= 0; bgnum--)
            if (isbgEnabled(bgnum)) {
                uint16_t bghoffs = *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum*4);
                uint16_t bgvoffs = *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum*4);
                RenderBGScanline(bgnum, scanline.bgcnts[bgnum], bghoffs, bgvoffs, vcount, scanline.layers[bgnum]);
            }
        break;
    case 1:
        bgnum = 2;
        if (isbgEnabled(bgnum))
            RenderRotScaleBGScanline(bgnum, scanline.bgcnts[bgnum], REG_BG2X, REG_BG2Y, vcount, scanline.layers[bgnum]);
        for (bgnum = 1; bgnum >= 0; bgnum--)
            if (isbgEnabled(bgnum)) {
                uint16_t bghoffs = *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum*4);
                uint16_t bgvoffs = *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum*4);
                RenderBGScanline(bgnum, scanline.bgcnts[bgnum], bghoffs, bgvoffs, vcount, scanline.layers[bgnum]);
            }
        break;
    default:
        printf("Video mode %u unsupported.\n", mode);
        break;
    }

    bool windowsEnabled = false;
    uint16_t WIN0bottom, WIN0top, WIN0right, WIN0left;
    uint16_t WIN1bottom, WIN1top, WIN1right, WIN1left;
    bool WIN0enable = false, WIN1enable = false;

    if (REG_DISPCNT & DISPCNT_WIN0_ON) {
        WIN0bottom = REG_WIN0V & 0xFF; WIN0top = (REG_WIN0V>>8)&0xFF;
        WIN0right  = REG_WIN0H & 0xFF; WIN0left= (REG_WIN0H>>8)&0xFF;
        WIN0enable = (WIN0top>WIN0bottom) ? (vcount>=WIN0top||vcount<WIN0bottom)
                                          : (vcount>=WIN0top&&vcount<WIN0bottom);
        windowsEnabled = true;
    }
    if (REG_DISPCNT & DISPCNT_WIN1_ON) {
        WIN1bottom = REG_WIN1V & 0xFF; WIN1top = (REG_WIN1V>>8)&0xFF;
        WIN1right  = REG_WIN1H & 0xFF; WIN1left= (REG_WIN1H>>8)&0xFF;
        WIN1enable = (WIN1top>WIN1bottom) ? (vcount>=WIN1top||vcount<WIN1bottom)
                                          : (vcount>=WIN1top&&vcount<WIN1bottom);
        windowsEnabled = true;
    }
    if ((REG_DISPCNT & DISPCNT_OBJWIN_ON) && (REG_DISPCNT & DISPCNT_OBJ_ON))
        windowsEnabled = true;

    if (windowsEnabled) {
        for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++) {
            if (WIN0enable && winCheckHorizontalBounds(WIN0left, WIN0right, xpos))
                scanline.winMask[xpos] = REG_WININ & 0x3F;
            else if (WIN1enable && winCheckHorizontalBounds(WIN1left, WIN1right, xpos))
                scanline.winMask[xpos] = (REG_WININ>>8) & 0x3F;
            else
                scanline.winMask[xpos] = (REG_WINOUT & 0x3F) | WINMASK_WINOUT;
        }
    }

    if (REG_DISPCNT & DISPCNT_OBJ_ON)
        DrawSprites(&scanline, vcount, windowsEnabled);

    for (prnum = 3; prnum >= 0; prnum--) {
        for (char prsub = scanline.prioritySortedBgsCount[prnum]-1; prsub >= 0; prsub--) {
            char bg = scanline.prioritySortedBgs[prnum][prsub];
            if (isbgEnabled(bg)) {
                uint16_t *src = scanline.layers[bg];
                for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++) {
                    uint16_t color = src[xpos];
                    bool winEffectEnable = true;
                    if (!getAlphaBit(color)) continue;
                    if (windowsEnabled) {
                        winEffectEnable = (scanline.winMask[xpos] & WINMASK_CLR) >> 5;
                        if (!(scanline.winMask[xpos] & (1<<bg))) continue;
                    }
                    if (blendMode != 0 && (REG_BLDCNT & (1<<bg)) && winEffectEnable) {
                        uint16_t targetB = 0;
                        char isSpriteBlend = (REG_BLDCNT & BLDCNT_TGT2_OBJ) ? 1 : 0;
                        switch (blendMode) {
                        case 1:
                            if (alphaBlendSelectTargetB(&scanline, &targetB, prnum, prsub+1, xpos, isSpriteBlend))
                                color = alphaBlendColor(color, targetB);
                            break;
                        case 2: color = alphaBrightnessIncrease(color); break;
                        case 3: color = alphaBrightnessDecrease(color); break;
                        }
                    }
                    pixels[xpos] = color;
                }
            }
        }
        uint16_t *src = scanline.spriteLayers[prnum];
        for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++) {
            if (getAlphaBit(src[xpos])) {
                if (windowsEnabled && !(scanline.winMask[xpos] & WINMASK_OBJ)) continue;
                pixels[xpos] = src[xpos];
            }
        }
    }
}

static void DrawFrame(uint16_t *pixels)
{
    static uint16_t scanlines[DISPLAY_HEIGHT][DISPLAY_WIDTH];
    unsigned int blendMode = (REG_BLDCNT >> 6) & 3;
    uint16_t backdropColor = *(uint16_t *)PLTT;

    if (REG_BLDCNT & BLDCNT_TGT1_BD) {
        if      (blendMode == 2) backdropColor = alphaBrightnessIncrease(backdropColor);
        else if (blendMode == 3) backdropColor = alphaBrightnessDecrease(backdropColor);
    }
    memsetu16((uint16_t *)scanlines, backdropColor, DISPLAY_WIDTH * DISPLAY_HEIGHT);

    for (int i = 0; i < DISPLAY_HEIGHT; i++) {
        REG_VCOUNT = i;
        if (((REG_DISPSTAT >> 8) & 0xFF) == REG_VCOUNT) {
            REG_DISPSTAT |= INTR_FLAG_VCOUNT;
            if (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR) gIntrTable[0]();
        }
        DrawScanline(scanlines[i], i);
        REG_DISPSTAT |= INTR_FLAG_HBLANK;
        RunDMAs(DMA_HBLANK);
        if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR) gIntrTable[3]();
        REG_DISPSTAT &= ~INTR_FLAG_HBLANK;
        REG_DISPSTAT &= ~INTR_FLAG_VCOUNT;
    }

    for (int i = 0; i < DISPLAY_HEIGHT; i++) {
        uint16_t *src = scanlines[i];
        for (int j = 0; j < DISPLAY_WIDTH; j++)
            pixels[i * DISPLAY_WIDTH + j] = src[j];
    }
}

/* =========================================================================
 * Platform state
 * ========================================================================= */

static SceUID              gVBlankSema     = -1;
static volatile int        gFrameAvailable = 0;
static volatile bool       gIsRunning      = true;
static struct SiiRtcInfo   gInternalClock;
static FILE               *sSaveFile       = NULL;
static volatile u16        gKeys           = 0;

extern void AgbMain(void);
extern void DoSoftReset(void);

/* =========================================================================
 * Save file
 * ========================================================================= */

static void ReadSaveFile(const char *path)
{
    sSaveFile = fopen(path, "r+b");
    if (!sSaveFile) sSaveFile = fopen(path, "w+b");
    if (!sSaveFile) { fprintf(stderr, "Cannot open save: %s\n", path); return; }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    int bytesToRead = (fileSize < (int)sizeof(FLASH_BASE)) ? fileSize : (int)sizeof(FLASH_BASE);
    int bytesRead   = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);
    for (int i = bytesRead; i < (int)sizeof(FLASH_BASE); i++) FLASH_BASE[i] = 0xFF;
}

static void StoreSaveFile(void)
{
    if (sSaveFile) {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
        fflush(sSaveFile);
    }
}

static void CloseSaveFile(void)
{
    if (sSaveFile) { fclose(sSaveFile); sSaveFile = NULL; }
}

void Platform_StoreSaveFile(void) { StoreSaveFile(); }

/* =========================================================================
 * RTC
 * ========================================================================= */

static void UpdateInternalClock(void)
{
    time_t rawTime = time(NULL);
    struct tm *t   = localtime(&rawTime);
    gInternalClock.year      = BinToBcd(t->tm_year - 100);
    gInternalClock.month     = BinToBcd(t->tm_mon) + 1;
    gInternalClock.day       = BinToBcd(t->tm_mday);
    gInternalClock.dayOfWeek = BinToBcd(t->tm_wday);
    gInternalClock.hour      = BinToBcd(t->tm_hour);
    gInternalClock.minute    = BinToBcd(t->tm_min);
    gInternalClock.second    = BinToBcd(t->tm_sec);
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)   { rtc->status = gInternalClock.status; }
void Platform_SetStatus(struct SiiRtcInfo *rtc)   { gInternalClock.status = rtc->status; }

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();
    rtc->year = gInternalClock.year; rtc->month = gInternalClock.month;
    rtc->day  = gInternalClock.day;  rtc->dayOfWeek = gInternalClock.dayOfWeek;
    rtc->hour = gInternalClock.hour; rtc->minute = gInternalClock.minute;
    rtc->second = gInternalClock.second;
}
void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    gInternalClock.month = rtc->month; gInternalClock.day = rtc->day;
    gInternalClock.dayOfWeek = rtc->dayOfWeek; gInternalClock.hour = rtc->hour;
    gInternalClock.minute = rtc->minute; gInternalClock.second = rtc->second;
}
void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();
    rtc->hour = gInternalClock.hour; rtc->minute = gInternalClock.minute;
    rtc->second = gInternalClock.second;
}
void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    gInternalClock.hour = rtc->hour; gInternalClock.minute = rtc->minute;
    gInternalClock.second = rtc->second;
}
void Platform_SetAlarm(u8 *alarmData) { (void)alarmData; }

/* =========================================================================
 * Input
 * ========================================================================= */

u16 Platform_GetKeyInput(void) { return gKeys; }

static u16 ReadVitaKeys(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    u16 keys = 0;
    if (pad.buttons & SCE_CTRL_CROSS)    keys |= A_BUTTON;
    if (pad.buttons & SCE_CTRL_CIRCLE)   keys |= B_BUTTON;
    if (pad.buttons & SCE_CTRL_START)    keys |= START_BUTTON;
    if (pad.buttons & SCE_CTRL_SELECT)   keys |= SELECT_BUTTON;
    if (pad.buttons & SCE_CTRL_LTRIGGER) keys |= L_BUTTON;
    if (pad.buttons & SCE_CTRL_RTRIGGER) keys |= R_BUTTON;
    if (pad.buttons & SCE_CTRL_UP)       keys |= DPAD_UP;
    if (pad.buttons & SCE_CTRL_DOWN)     keys |= DPAD_DOWN;
    if (pad.buttons & SCE_CTRL_LEFT)     keys |= DPAD_LEFT;
    if (pad.buttons & SCE_CTRL_RIGHT)    keys |= DPAD_RIGHT;
    return keys;
}

/* =========================================================================
 * Audio stub — Phase 2 (silence)
 * music_player.c calls SDL_QueueAudio directly; we provide a no-op stub.
 * Phase 5: replace with SceAudio ring buffer.
 * ========================================================================= */

int SDL_QueueAudio(unsigned int dev, const void *data, unsigned int len)
{
    (void)dev; (void)data; (void)len;
    return 0;
}

/* =========================================================================
 * Debug log (forward declarations — defined in main section below)
 * ========================================================================= */
static void dbg(const char *msg);

/* =========================================================================
 * VBlank sync
 * ========================================================================= */

void VBlankIntrWait(void)
{
    static int sVBlankCount = 0;
    if (sVBlankCount < 5) {
        char buf[32];
        sVBlankCount++;
        /* "VBlank N: wait" */
        buf[0]='V'; buf[1]='B'; buf[2]='l'; buf[3]='a'; buf[4]='n'; buf[5]='k';
        buf[6]=' '; buf[7]='0'+sVBlankCount; buf[8]=':'; buf[9]=' ';
        buf[10]='w'; buf[11]='a'; buf[12]='i'; buf[13]='t'; buf[14]='\0';
        dbg(buf);
        gFrameAvailable = 1;
        sceKernelWaitSema(gVBlankSema, 1, NULL);
        /* "VBlank N: woke" */
        buf[10]='w'; buf[11]='o'; buf[12]='k'; buf[13]='e'; buf[14]='\0';
        dbg(buf);
    } else {
        gFrameAvailable = 1;
        sceKernelWaitSema(gVBlankSema, 1, NULL);
    }
}

/* =========================================================================
 * VDraw — software render → vita2d texture
 * ========================================================================= */

/* Vita display: 960x544 RGBA8888, stride = 960 pixels */
/* =========================================================================
 * Direct display framebuffer — no vita2d draw calls needed
 * 960x544 RGBA8888, pitch=960 px, CDRAM aligned to 256KB
 * ========================================================================= */
#include <psp2/display.h>

#define VITA_DISPLAY_WIDTH  960
#define VITA_DISPLAY_HEIGHT 544
#define VITA_FB_STRIDE      960
#define VITA_FB_BYTESIZE    (VITA_FB_STRIDE * VITA_DISPLAY_HEIGHT * 4)
/* CDRAM allocations must be multiples of 256KB */
#define VITA_FB_ALLOC_SIZE  (((VITA_FB_BYTESIZE) + (256*1024-1)) & ~(256*1024-1))

static uint32_t *sDisplayBuf[2] = {NULL, NULL};
static int       sDisplayBufIdx  = 0;

/* Shared GBA framebuffer (software-rendered, BGR555) */
static uint16_t sGbaFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];

static void display_init(void)
{
    for (int i = 0; i < 2; i++) {
        SceUID uid = sceKernelAllocMemBlock("display_fb",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            VITA_FB_ALLOC_SIZE, NULL);
        if (uid < 0) { dbg("main: display_fb alloc FAIL"); return; }
        sceKernelGetMemBlockBase(uid, (void **)&sDisplayBuf[i]);
        memset(sDisplayBuf[i], 0, VITA_FB_ALLOC_SIZE);
    }
    dbg("main: display_fb OK");
}

static void dbg_hex(const char *label, unsigned int val)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[48];
    int i, j = 0;
    for (i = 0; label[i]; i++) buf[j++] = label[i];
    buf[j++] = '='; buf[j++] = '0'; buf[j++] = 'x';
    for (i = 7; i >= 0; i--) buf[j++] = hex[(val >> (i*4)) & 0xF];
    buf[j] = '\0';
    dbg(buf);
}

static void VDraw(void)
{
    sFrameCount++;

    /* Track FadedBuf[2] change window: log every change from frame 480 onwards */
    if (sFrameCount >= 480 && sFrameCount <= 900) {
        static u16 sPrevFaded2 = 0xFFFF; /* sentinel */
        u16 faded2 = gPlttBufferFaded[2];
        u16 unfaded2 = gPlttBufferUnfaded[2];
        if (faded2 != sPrevFaded2) {
            dbg_hex("ChgFrm", (unsigned int)sFrameCount);
            dbg_hex("Faded2", faded2);
            dbg_hex("Unfad2", unfaded2);
            sPrevFaded2 = faded2;
        }
    }

    /* Early frames: basic register/VRAM snapshot (intro sequence) */
    if (sFrameCount <= 20) {
        unsigned int dispcnt = REG_DISPCNT;
        unsigned int bg0cnt  = *(unsigned short *)(REG_BASE + 0x8);
        unsigned int vram0   = *(unsigned int *)VRAM_;
        unsigned int vram7   = *(unsigned int *)(VRAM_ + 0x3800);
        unsigned int pltt0   = *(unsigned short *)PLTT;
        dbg_hex("DISPCNT", dispcnt);
        dbg_hex("BG0CNT", bg0cnt);
        dbg_hex("VRAM[0]", vram0);
        dbg_hex("VRAM[3800]", vram7);
        dbg_hex("PLTT[0]", pltt0);
        dbg_hex("PLTT[4]",   *(unsigned short *)(PLTT + 4));
        dbg_hex("UnfadBuf[2]", gPlttBufferUnfaded[2]);
        dbg_hex("FadedBuf[2]", gPlttBufferFaded[2]);
    }

    /* Periodic title-screen snapshots at frames 120, 240, 360, 480, 600, 720, 840 */
    if (sFrameCount == 120 || sFrameCount == 240 || sFrameCount == 360 || sFrameCount == 480
     || sFrameCount == 600 || sFrameCount == 720 || sFrameCount == 840) {
        static const char sHex2[]="0123456789ABCDEF";
        char hdr[16]; int d=sFrameCount;
        hdr[0]='F';hdr[1]='r';hdr[2]='m';hdr[3]='e';hdr[4]='=';
        hdr[5]='0'+(d/100)%10;hdr[6]='0'+(d/10)%10;hdr[7]='0'+(d%10);hdr[8]='\0';
        dbg(hdr);
        dbg_hex("DISPCNT", REG_DISPCNT);
        dbg_hex("BG0CNT",  *(unsigned short *)(REG_BASE + 0x08));
        dbg_hex("BG1CNT",  *(unsigned short *)(REG_BASE + 0x0A));
        dbg_hex("BG2CNT",  *(unsigned short *)(REG_BASE + 0x0C));
        dbg_hex("BG2PA",   *(unsigned short *)(REG_BASE + 0x20));
        dbg_hex("BG2PB",   *(unsigned short *)(REG_BASE + 0x22));
        dbg_hex("BG2PC",   *(unsigned short *)(REG_BASE + 0x24));
        dbg_hex("BG2PD",   *(unsigned short *)(REG_BASE + 0x26));
        /* VRAM: char base block starts and key tile offsets */
        dbg_hex("VRAM[0]",     *(unsigned int *)(VRAM_));
        dbg_hex("VRAM[40]",    *(unsigned int *)(VRAM_ + 0x40));   /* tile 1 of charbase0 */
        dbg_hex("VRAM[8000]",  *(unsigned int *)(VRAM_ + 0x8000)); /* charbase2 tile 0 */
        dbg_hex("VRAM[BC00]",  *(unsigned int *)(VRAM_ + 0xBC00)); /* charbase2 tile 480 */
        dbg_hex("VRAM[C000]",  *(unsigned int *)(VRAM_ + 0xC000)); /* charbase3 tile 0 */
        /* Tilemap bases */
        dbg_hex("VRAM[4800]",  *(unsigned int *)(VRAM_ + 0x4800)); /* screenbase9  logo */
        dbg_hex("VRAM[D000]",  *(unsigned int *)(VRAM_ + 0xD000)); /* screenbase26 rayquaza */
        dbg_hex("VRAM[D800]",  *(unsigned int *)(VRAM_ + 0xD800)); /* screenbase27 clouds */
        /* Full BG palette sample (palettes 0 and 9) */
        dbg_hex("PLTT[0]",   *(unsigned short *)(PLTT));
        dbg_hex("PLTT[2]",   *(unsigned short *)(PLTT + 2));
        dbg_hex("PLTT[4]",   *(unsigned short *)(PLTT + 4));
        dbg_hex("PLTT[120]", *(unsigned short *)(PLTT + 0x120)); /* palette 9 color 0 */
        dbg_hex("PLTT[122]", *(unsigned short *)(PLTT + 0x122)); /* palette 9 color 1 */
        /* Track palette buffer state to isolate corruption site */
        dbg_hex("UnfadBuf[2]", gPlttBufferUnfaded[2]);
        dbg_hex("FadedBuf[2]", gPlttBufferFaded[2]);
        dbg_hex("UnfadBuf[0]", gPlttBufferUnfaded[0]);
        dbg_hex("FadedBuf[0]", gPlttBufferFaded[0]);
        /* Sample 2 rendered pixels after DrawFrame to see what the renderer produces */
        /* (logged AFTER the DrawFrame call below, so we defer to end of function)    */
    }

    memset(sGbaFrame, 0, sizeof(sGbaFrame));
    DrawFrame(sGbaFrame);
    REG_VCOUNT = 161;

    /* Log rendered pixel samples at snapshot frames */
    if (sFrameCount == 360 || sFrameCount == 480 || sFrameCount == 600 || sFrameCount == 720 || sFrameCount == 840) {
        /* center pixel (120,80) and top-left (0,0) */
        dbg_hex("PX[0,0]",    sGbaFrame[0]);
        dbg_hex("PX[120,80]", sGbaFrame[80 * 240 + 120]);
        dbg_hex("PX[0,80]",   sGbaFrame[80 * 240]);
    }
}

/* Upscale sGbaFrame (240x160 BGR555) → current display buffer (960x544 RGBA8888)
   and flip it on screen. */
static void VBlit(void)
{
    uint32_t *fb = sDisplayBuf[sDisplayBufIdx];

    /* Nearest-neighbour upscale 240x160 → 960x544 */
    for (int dy = 0; dy < VITA_DISPLAY_HEIGHT; dy++) {
        int sy = dy * DISPLAY_HEIGHT / VITA_DISPLAY_HEIGHT;
        const uint16_t *src_row = sGbaFrame + sy * DISPLAY_WIDTH;
        uint32_t       *dst_row = fb        + dy * VITA_FB_STRIDE;
        for (int dx = 0; dx < VITA_DISPLAY_WIDTH; dx++) {
            int sx = dx * DISPLAY_WIDTH / VITA_DISPLAY_WIDTH;
            uint16_t c = src_row[sx] & 0x7FFF;
            uint8_t r = ((c >> 0) & 0x1F) << 3;
            uint8_t g = ((c >> 5) & 0x1F) << 3;
            uint8_t b = ((c >> 10) & 0x1F) << 3;
            dst_row[dx] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
    }

    SceDisplayFrameBuf fb_info = {
        .size        = sizeof(SceDisplayFrameBuf),
        .base        = fb,
        .pitch       = VITA_FB_STRIDE,
        .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        .width       = VITA_DISPLAY_WIDTH,
        .height      = VITA_DISPLAY_HEIGHT,
    };
    sceDisplaySetFrameBuf(&fb_info, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sDisplayBufIdx ^= 1;
}

/* =========================================================================
 * Game thread
 * ========================================================================= */

static int GameThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    dbg("GameThread: calling AgbMain");
    AgbMain();
    dbg("GameThread: AgbMain returned");
    return sceKernelExitDeleteThread(0);
}

/* =========================================================================
 * main
 * ========================================================================= */

/* Debug log — writes to ux0:data/pokeemerald/debug.log */
static void dbg_open(void)
{
    sceIoMkdir("ux0:data/pokeemerald", 0777);
    sDbgLog = fopen("ux0:data/pokeemerald/debug.log", "w");
}
static void dbg(const char *msg)
{
    if (sDbgLog) { fputs(msg, sDbgLog); fputs("\n", sDbgLog); fflush(sDbgLog); }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    dbg_open();
    dbg("main: start");

    /* Create save directory */
    sceIoMkdir("ux0:data/pokeemerald", 0777);

    display_init();

    gVBlankSema = sceKernelCreateSema("vblank", 0, 0, 1, NULL);
    dbg("main: sema created");

    ReadSaveFile("ux0:data/pokeemerald/pokeemerald.sav");
    dbg("main: save loaded");

    cgb_audio_init(42048);
    dbg("main: cgb_audio_init done");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    dbg("main: ctrl mode set");

    memset(&gInternalClock, 0, sizeof(gInternalClock));
    gInternalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();
    dbg("main: clock init done");

    /* Start game logic thread (1 MB stack) */
    SceUID tid = sceKernelCreateThread("AgbMain", GameThread, 0x40, 1*1024*1024, 0, 0, NULL);
    dbg("main: thread created");
    sceKernelStartThread(tid, 0, NULL);
    dbg("main: thread started — entering render loop");

    while (gIsRunning) {
        gKeys = ReadVitaKeys();

        /* Exit: Start + Select */
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT))
            gIsRunning = false;

        if (gFrameAvailable) {
            VDraw();
            VBlit();

            gFrameAvailable = 0;

            REG_DISPSTAT |= INTR_FLAG_VBLANK;
            RunDMAs(DMA_HBLANK);
            /* Always call VBlankIntr (gIntrTable[4]) — bypasses the circular
               DISPSTAT_VBLANK_INTR dependency where CopyBufferedValuesToGpuRegs
               can only run inside VBlankIntr, but VBlankIntr was gated on
               DISPSTAT_VBLANK_INTR being set by CopyBufferedValuesToGpuRegs. */
            if (gIntrTable[4])
                gIntrTable[4]();
            REG_DISPSTAT &= ~INTR_FLAG_VBLANK;

            dbg("main: signal sema");
            sceKernelSignalSema(gVBlankSema, 1);
        } else {
            /* Yield to game thread */
            sceKernelDelayThread(100);
        }
    }

    CloseSaveFile();
    sceKernelDeleteSema(gVBlankSema);
    sceKernelExitProcess(0);
    return 0;
}
