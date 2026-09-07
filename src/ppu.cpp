// ppu.cpp — D!NG Peridot (Game Boy) PPU.
//
// Direct port of gb-core.js's PPU class. Timing (mode 2/3/0/1 cycle
// thresholds), the LCD-off "white screen" behaviour, and the line-rendering
// algorithm (BG/window fetch, sprite priority/sort) are kept identical.
//
// Per DING_CORE_SDK_RULES.md: this file owns video only — it reads bus_->io
// for registers and bus_->vram/oam for tile/sprite data, but never touches
// CPU or APU state.

#include "peridot.h"
#include <algorithm>

Ppu::Ppu(Bus* bus) : bus_(bus) {}

void Ppu::setPalette(const GbColor colors[4]) {
    for (int i = 0; i < 4; i++) palette_[i] = colors[i];
}

void Ppu::blit() {
    for (u32 i = 0; i < GB_W * GB_H; i++) {
        const GbColor& c = palette_[pixels[i]];
        framebuf[i * 4 + 0] = c.r;
        framebuf[i * 4 + 1] = c.g;
        framebuf[i * 4 + 2] = c.b;
        framebuf[i * 4 + 3] = 255;
    }
}

void Ppu::setMode(u8 m) {
    mode_ = m;
    u8 stat = (u8)((bus_->io[0x41] & ~3) | m);
    static const s8 irq[4] = {3, 4, 5, -1};
    if (irq[m] >= 0 && (stat & (1 << irq[m]))) bus_->requestInterrupt(1);
    bus_->io[0x41] = stat;
}

void Ppu::checkLYC() {
    u8 stat = bus_->io[0x41];
    if (ly() == lyc()) {
        stat |= 4;
        if (stat & 0x40) bus_->requestInterrupt(1);
    } else {
        stat &= (u8)~4;
    }
    bus_->io[0x41] = stat;
}

u32 Ppu::tileAddr(u8 idx, bool signed_) const {
    if (signed_) {
        s16 t = (idx < 128) ? idx : (s16)idx - 256;
        return (u32)(0x1000 + t * 16);
    }
    return (u32)idx * 16;
}

void Ppu::step(u32 cycles) {
    frameReady = false;

    if (!(lcdc() & 0x80)) {
        if (ly() != 0 || mode_ != 0 || !lcdWasOff_) {
            setLy(0); cycles_ = 0; mode_ = 0; winLine_ = 0;
            bus_->io[0x41] = (u8)(bus_->io[0x41] & ~3);
            // Real HW: LCD off = white screen. Fill framebuf with palette[0] and blit.
            std::fill(pixels.begin(), pixels.end(), 0);
            blit();
            frameReady = true;
            lcdWasOff_ = true;
        }
        return;
    }

    if (lcdWasOff_) {
        lcdWasOff_ = false; setLy(0); cycles_ = 0; winLine_ = 0;
        setMode(2); checkLYC();
    }

    cycles_ += cycles;
    if (mode_ == 2 && cycles_ >= 80)  { cycles_ -= 80;  setMode(3); }
    if (mode_ == 3 && cycles_ >= 172) { cycles_ -= 172; drawLine(); setMode(0); }
    if (mode_ == 0 && cycles_ >= 204) {
        cycles_ -= 204; setLy((u8)(ly() + 1)); checkLYC();
        if (ly() == 144) { setMode(1); bus_->requestInterrupt(0); blit(); frameReady = true; winLine_ = 0; }
        else { setMode(2); }
    }
    if (mode_ == 1 && cycles_ >= 456) {
        cycles_ -= 456; setLy((u8)(ly() + 1)); checkLYC();
        if (ly() > 153) { setLy(0); winLine_ = 0; checkLYC(); setMode(2); }
    }
}

void Ppu::drawLine() {
    u8 lyv = ly();
    if (lyv >= GB_H) return;

    u8 lcdcv = lcdc();
    u32 base = (u32)lyv * GB_W;
    u8 bgPrio[GB_W] = {0};
    bool signed_ = !(lcdcv & 0x10);

    if (lcdcv & 1) {
        u16 map = (lcdcv & 8) ? 0x1C00 : 0x1800;
        u8 bgY = (u8)(lyv + scy());
        u32 tRow = bgY >> 3, tPY = bgY & 7;
        for (u32 x = 0; x < GB_W; x++) {
            u8 bgX = (u8)(x + scx());
            u32 tCol = bgX >> 3, tPX = bgX & 7;
            u8 idx = bus_->vram[map + tRow * 32 + tCol];
            u32 ta = tileAddr(idx, signed_);
            u8 lo = bus_->vram[ta + tPY * 2], hi = bus_->vram[ta + tPY * 2 + 1];
            u32 bit = 7 - tPX;
            u8 ci = (u8)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
            pixels[base + x] = palColor(bgp(), ci);
            bgPrio[x] = ci;
        }
    }

    s16 wxv = wx();
    if ((lcdcv & 0x20) && lyv >= wy() && wxv < (s16)GB_W) {
        u16 map = (lcdcv & 0x40) ? 0x1C00 : 0x1800;
        u32 tRow = winLine_ >> 3, tPY = winLine_ & 7;
        for (s32 x = DING_MAX((s32)0, (s32)wxv); x < (s32)GB_W; x++) {
            u32 wX = (u32)(x - wxv);
            u8 idx = bus_->vram[map + tRow * 32 + (wX >> 3)];
            u32 ta = tileAddr(idx, signed_);
            u8 lo = bus_->vram[ta + tPY * 2], hi = bus_->vram[ta + tPY * 2 + 1];
            u32 bit = 7 - (wX & 7);
            u8 ci = (u8)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
            pixels[base + (u32)x] = palColor(bgp(), ci);
            bgPrio[x] = ci;
        }
        winLine_++;
    }

    if (lcdcv & 2) {
        u32 sprH = (lcdcv & 4) ? 16 : 8;
        struct Spr { s32 y, x; u8 tile, flags; u32 idx; };
        Spr sprs[10];
        u32 sprCount = 0;
        for (u32 i = 0; i < 40 && sprCount < 10; i++) {
            u32 ob = i * 4;
            s32 sy = (s32)bus_->oam[ob] - 16;
            if ((s32)lyv >= sy && (s32)lyv < sy + (s32)sprH) {
                sprs[sprCount++] = Spr{
                    sy,
                    (s32)bus_->oam[ob + 1] - 8,
                    (u8)(bus_->oam[ob + 2] & (sprH == 16 ? 0xFE : 0xFF)),
                    bus_->oam[ob + 3],
                    i
                };
            }
        }
        // Sort descending by x, then by OAM index descending — matches JS's
        // sprs.sort((a,b)=>b.x-a.x||b.idx-a.idx), which draws lowest-priority
        // (highest x / highest index) first so higher-priority sprites end
        // up drawn last and win overlapping pixels.
        for (u32 i = 1; i < sprCount; i++) {
            Spr key = sprs[i];
            s32 j = (s32)i - 1;
            while (j >= 0 && (sprs[j].x < key.x || (sprs[j].x == key.x && sprs[j].idx < key.idx))) {
                sprs[j + 1] = sprs[j];
                j--;
            }
            sprs[j + 1] = key;
        }

        for (u32 si = 0; si < sprCount; si++) {
            const Spr& s = sprs[si];
            bool fx = s.flags & 0x20, fy = s.flags & 0x40, bgOvr = s.flags & 0x80;
            u8 pal = (s.flags & 0x10) ? obp1() : obp0();
            s32 ty = (s32)lyv - s.y;
            if (fy) ty = (s32)sprH - 1 - ty;
            u32 ta = (u32)s.tile * 16 + (u32)ty * 2;
            u8 lo = bus_->vram[ta], hi = bus_->vram[ta + 1];
            for (u32 px = 0; px < 8; px++) {
                s32 sx = s.x + (s32)px;
                if (sx < 0 || sx >= (s32)GB_W) continue;
                u32 bit = fx ? px : 7 - px;
                u8 ci = (u8)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
                if (!ci) continue;
                if (bgOvr && bgPrio[sx]) continue;
                pixels[base + (u32)sx] = palColor(pal, ci);
            }
        }
    }
}
