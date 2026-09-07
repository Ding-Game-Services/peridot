// bus.cpp — D!NG Peridot (Game Boy) memory map and cartridge banking.
//
// Direct port of the MMU + Timer classes in gb-core.js. Kept behaviourally
// identical, including the header comments that explain *why* something is
// done a certain way (mirror mask widths, write-order dependencies, etc.) —
// those were hard-won debugging notes and are worth preserving verbatim.
//
// Per DING_CORE_SDK_RULES.md: this file owns the memory map and address
// decoding only. No CPU, PPU, or APU logic lives here — cross-links to Apu
// go through the narrow public surface declared in peridot.h (fsTimer,
// write()), never into Apu's internals.

#include "peridot.h"
#include <cstring>

// ═══════════════════════════════════════════════
// TIMER
// ═══════════════════════════════════════════════

Timer::Timer(Bus* bus) : bus_(bus) {}

void Timer::step(u32 cycles) {
    u16 prev = div;
    div = (u16)(div + cycles); // wraps naturally at 16 bits
    bus_->io[0x04] = (u8)((div >> 8) & 0xFF);

    if (!(bus_->io[0x07] & 4)) return;

    static const u16 freqs[4] = {512, 8, 32, 128};
    u16 bit = freqs[bus_->io[0x07] & 3];

    if ((prev & bit) && !(div & bit)) {
        u8 tima = (u8)((bus_->io[0x05] + 1) & 0xFF);
        if (!tima) {
            tima = bus_->io[0x06];
            bus_->requestInterrupt(2);
        }
        bus_->io[0x05] = tima;
    }
}

// ═══════════════════════════════════════════════
// PRINTER
// ═══════════════════════════════════════════════

PrinterDevice::PrinterDevice() { reset(); }

void PrinterDevice::reset() {
    state_ = State::Sync1;
    cmd_ = 0; compression_ = 0; dataLen_ = 0; dataRead_ = 0;
    payload_.clear(); tileBuffer_.clear();
    statusByte_ = 0;
}

std::vector<u8> PrinterDevice::decompress(const std::vector<u8>& bytes) const {
    if (!compression_) return bytes;
    std::vector<u8> out;
    size_t i = 0;
    while (i < bytes.size()) {
        u8 ctrl = bytes[i++];
        if ((ctrl & 0x80) == 0) {
            u32 n = (ctrl & 0x7F) + 1;
            for (u32 k = 0; k < n && i < bytes.size(); k++) out.push_back(bytes[i++]);
        } else {
            u32 n = (ctrl & 0x7F) + 2;
            if (i >= bytes.size()) break;
            u8 v = bytes[i++];
            for (u32 k = 0; k < n; k++) out.push_back(v);
        }
    }
    return out;
}

void PrinterDevice::handleCommand() {
    if (cmd_ == 0x01) { tileBuffer_.clear(); statusByte_ = 0; return; }
    if (cmd_ == 0x04) {
        auto decompressed = decompress(payload_);
        tileBuffer_.insert(tileBuffer_.end(), decompressed.begin(), decompressed.end());
        statusByte_ = 0;
        return;
    }
    if (cmd_ == 0x02) { renderAndEmit(); statusByte_ = 0; return; }
    // 0x0F STATUS or anything else: just report current statusByte_.
}

void PrinterDevice::renderAndEmit() {
    u32 tileCount = (u32)tileBuffer_.size() >> 4; // 16 bytes per 8x8 2bpp tile
    if (tileCount == 0) { tileBuffer_.clear(); return; }

    const u32 tilesWide = 20;
    u32 rows = (tileCount + tilesWide - 1) / tilesWide;
    u32 w = tilesWide * 8, h = rows * 8;
    static const u8 shades[4] = {0xFF, 0xAA, 0x55, 0x00}; // DMG-style greyscale
    std::vector<u8> px((size_t)w * h * 4, 0);

    for (u32 t = 0; t < tileCount; t++) {
        u32 tx = (t % tilesWide) * 8, ty = (t / tilesWide) * 8;
        for (u32 row = 0; row < 8; row++) {
            u8 lo = tileBuffer_[t * 16 + row * 2], hi = tileBuffer_[t * 16 + row * 2 + 1];
            for (u32 col = 0; col < 8; col++) {
                u32 bit = 7 - col;
                u8 c = (u8)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
                u8 g = shades[c];
                size_t idx = ((size_t)(ty + row) * w + (tx + col)) * 4;
                px[idx] = g; px[idx + 1] = g; px[idx + 2] = g; px[idx + 3] = 255;
            }
        }
    }
    tileBuffer_.clear();
    if (onImage) onImage(px.data(), w, h, onImageUserdata);
}

bool PrinterDevice::exchangeByte(u8 outByte, u8* inByte) {
    switch (state_) {
        case State::Sync1:
            if (outByte == 0x88) { state_ = State::Sync2; *inByte = 0x00; return true; }
            return false; // not printer traffic — let the next device try
        case State::Sync2:
            if (outByte == 0x33) { state_ = State::Cmd; *inByte = 0x00; return true; }
            state_ = State::Sync1; return false;
        case State::Cmd:
            cmd_ = outByte; state_ = State::Comp; *inByte = 0x00; return true;
        case State::Comp:
            compression_ = outByte; state_ = State::LenLo; *inByte = 0x00; return true;
        case State::LenLo:
            dataLen_ = outByte; state_ = State::LenHi; *inByte = 0x00; return true;
        case State::LenHi:
            dataLen_ |= ((u32)outByte << 8);
            payload_.clear(); dataRead_ = 0;
            state_ = dataLen_ > 0 ? State::Data : State::Ck1;
            *inByte = 0x00; return true;
        case State::Data:
            payload_.push_back(outByte); dataRead_++;
            if (dataRead_ >= dataLen_) state_ = State::Ck1;
            *inByte = 0x00; return true;
        case State::Ck1: state_ = State::Ck2; *inByte = 0x00; return true;
        case State::Ck2: state_ = State::Alive; *inByte = 0x00; return true;
        case State::Alive:
            handleCommand(); state_ = State::Status;
            *inByte = 0x81; return true; // "printer present" marker
        case State::Status:
            state_ = State::Sync1; *inByte = statusByte_; return true;
    }
    return false;
}

// ═══════════════════════════════════════════════
// BUS (MMU)
// ═══════════════════════════════════════════════

Bus::Bus() {
    initIO();
}

void Bus::initIO() {
    io[0x04] = 0x1E; io[0x40] = 0x91;
    io[0x41] = 0x82; io[0x47] = 0xFC;
    io[0x48] = 0xFF; io[0x49] = 0xFF;
    io[0x01] = 0x00; io[0x02] = 0x7E;
    ifReg = 0xE1;
}

void Bus::reset() {
    initIO();
}

bool Bus::loadROM(const u8* data, size_t len) {
    if (!data || len == 0) return false;

    rom.assign(data, data + len);

    u8 type  = rom.size() > 0x147 ? rom[0x147] : 0;
    u8 sz    = rom.size() > 0x148 ? rom[0x148] : 0;
    u8 ramSz = rom.size() > 0x149 ? rom[0x149] : 0;

    romBanks = (sz == 0) ? 2u : (2u << sz);

    if      (type == 0)                    mbcType = GB_MBC_NONE;
    else if (type <= 0x03)                 mbcType = GB_MBC1;
    else if (type <= 0x06)                 mbcType = GB_MBC2;
    else if (type >= 0x0F && type <= 0x13) mbcType = GB_MBC3;
    else if (type >= 0x19 && type <= 0x1E) mbcType = GB_MBC5;
    else if (type == 0xFC)                 mbcType = GB_MBC_CAMERA;
    else                                    mbcType = GB_MBC1;

    static const u32 ramBytesTable[6] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    u32 ramBytes = (ramSz < 6) ? ramBytesTable[ramSz] : 0x8000;
    if (ramBytes == 0 && !(ramSz < 6)) ramBytes = 0x8000; // matches JS's `|| 0x8000` fallback
    eram.assign(DING_MAX(ramBytes, (u32)GB_ERAM_MIN_SIZE), 0);

    romBank = 1; ramBank = 0; ramEn = false;
    mbc1Mode = 0; mbc1HiBits = 0; romLoBank = 0;
    camRegMode = false; std::fill(camRegs.begin(), camRegs.end(), 0);
    rtcSel = -1; rtcLatchStep = 0;
    // rtcBase: host should call a future setRtcBase(now) after loadROM if it
    // wants wall-clock RTC behaviour identical to JS's `Date.now()` seed.
    // Left at whatever the caller set (defaults to 0) — matches step-1 scope.

    romLo.assign(GB_ROM_BANK_SIZE, 0xFF);
    size_t copyLen = DING_MIN((size_t)GB_ROM_BANK_SIZE, rom.size());
    std::memcpy(romLo.data(), rom.data(), copyLen);

    bankHi(1);
    return true;
}

void Bus::bankLo(u32 bank) {
    bank = bank % romBanks;
    romLoBank = bank;
    size_t off = (size_t)bank * GB_ROM_BANK_SIZE;
    for (u32 i = 0; i < GB_ROM_BANK_SIZE; i++)
        romLo[i] = (off + i < rom.size()) ? rom[off + i] : 0xFF;
}

void Bus::bankHi(u32 bank) {
    if (mbcType != GB_MBC3) bank = DING_MAX(1u, bank % romBanks);
    else                    bank = DING_MAX(1u, bank & 0x7F) % romBanks;
    romBank = bank;
    size_t off = (size_t)bank * GB_ROM_BANK_SIZE;
    for (u32 i = 0; i < GB_ROM_BANK_SIZE; i++)
        romHi[i] = (off + i < rom.size()) ? rom[off + i] : 0xFF;
}

void Bus::updateRTC() {
    // JS seeds this from Date.now(); the core stays platform-agnostic and
    // expects the host to keep rtcBase in the same units (seconds).
    s64 elapsed = rtcBase; // placeholder until host wall-clock wiring lands
    rtcRegs[0] = (u8)(elapsed % 60);
    rtcRegs[1] = (u8)((elapsed / 60) % 60);
    rtcRegs[2] = (u8)((elapsed / 3600) % 24);
    s64 d = elapsed / 86400;
    rtcRegs[3] = (u8)(d & 0xFF);
    rtcRegs[4] = (u8)((rtcRegs[4] & 0xFE) | ((d >> 8) & 1));
}

void Bus::stepSerial(u32 cycles) {
    if (!serialCycles) return;
    if (cycles >= serialCycles) serialCycles = 0;
    else { serialCycles -= cycles; return; }

    u8 outByte = io[0x01];
    u8 inByte = 0xFF; // default: nothing attached — matches JS fallback

    // Printer always gets first refusal, matching gb-core.js unshifting
    // PrinterDevice to the front of its serial device chain — it needs to
    // recognize its own sync sequence before anything else sees the byte.
    if (!printer.exchangeByte(outByte, &inByte)) {
        if (serialExchange) inByte = serialExchange(outByte);
        else inByte = 0xFF;
    }

    io[0x01] = inByte;
    io[0x02] &= 0x7F;
    requestInterrupt(3);
}

u8 Bus::readJoy() {
    u8 sel = io[0];
    u8 lo = 0xF;
    if (!(sel & 0x10)) lo &= buttons & 0xF;
    if (!(sel & 0x20)) lo &= (buttons >> 4) & 0xF;
    return (u8)(0xC0 | (sel & 0x30) | lo);
}

void Bus::pressButton(u8 btn, bool down) {
    if (down) {
        buttons &= (u8)~(1u << btn);
        u8 sel = io[0];
        bool isDir = btn < 4;
        bool dirSel = !(sel & 0x10);
        bool actSel = !(sel & 0x20);
        if ((isDir && dirSel) || (!isDir && actSel)) requestInterrupt(4);
    } else {
        buttons |= (u8)(1u << btn);
    }
}

u8 Bus::read8(u16 addr) {
    if (addr < 0x4000) return !rom.empty() ? romLo[addr] : 0xFF;
    if (addr < 0x8000) return !rom.empty() ? romHi[addr - 0x4000] : 0xFF;
    if (addr < 0xA000) return vram[addr - 0x8000];
    if (addr < 0xC000) {
        if (mbcType == GB_MBC_CAMERA && camRegMode) {
            u32 idx = addr - 0xA000;
            return idx < 0x36 ? camRegs[idx] : 0x00;
        }
        if (!ramEn) return 0xFF;
        if (mbcType == GB_MBC3 && rtcSel >= 0) return rtcLatched[rtcSel];
        if (mbcType == GB_MBC2) {
            u32 idx = (addr - 0xA000) & 0x01FF;
            return (u8)(0xF0 | (eram[idx] & 0x0F));
        }
        return eram[(size_t)ramBank * 0x2000 + (addr - 0xA000)];
    }
    if (addr < 0xE000) return wram[addr - 0xC000];
    if (addr < 0xFE00) return wram[addr - 0xE000];
    if (addr < 0xFEA0) return oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;
    if (addr == 0xFF00) return readJoy();
    if (addr == 0xFF0F) return ifReg;
    if (addr < 0xFF80) return io[addr - 0xFF00];
    if (addr < 0xFFFF) return hram[addr - 0xFF80];
    return ie;
}

void Bus::write8(u16 addr, u8 val) {
    if (addr < 0x8000) {
        switch (mbcType) {
            case GB_MBC1:
                if (addr < 0x2000) {
                    ramEn = (val & 0xF) == 0xA;
                } else if (addr < 0x4000) {
                    u32 lo = val & 0x1F;
                    if (!lo) lo = 1;
                    if (mbc1Mode == 0) bankHi((mbc1HiBits << 5) | lo);
                    else               bankHi(lo);
                } else if (addr < 0x6000) {
                    mbc1HiBits = val & 3;
                    if (mbc1Mode == 0) {
                        bankHi((mbc1HiBits << 5) | (romBank & 0x1F));
                    } else {
                        ramBank = mbc1HiBits;
                        bankLo(mbc1HiBits << 5);
                    }
                } else {
                    mbc1Mode = val & 1;
                    if (mbc1Mode == 0) {
                        ramBank = 0;
                        bankLo(0);
                        bankHi((mbc1HiBits << 5) | (romBank & 0x1F));
                    } else {
                        bankLo(mbc1HiBits << 5);
                        bankHi(romBank & 0x1F);
                    }
                }
                break;

            case GB_MBC3:
                if (addr < 0x2000) {
                    ramEn = (val & 0xF) == 0xA;
                } else if (addr < 0x4000) {
                    bankHi(val & 0x7F);
                } else if (addr < 0x6000) {
                    if (val <= 0x03) { rtcSel = -1; ramBank = val; }
                    else if (val >= 0x08 && val <= 0x0C) { rtcSel = val - 0x08; }
                } else {
                    if (val == 0x00) {
                        rtcLatchStep = 1;
                    } else if (val == 0x01 && rtcLatchStep == 1) {
                        updateRTC();
                        std::memcpy(rtcLatched, rtcRegs, sizeof(rtcRegs));
                        rtcLatchStep = 0;
                    } else {
                        rtcLatchStep = 0;
                    }
                }
                break;

            case GB_MBC2:
                if (addr < 0x4000) {
                    if (addr & 0x0100) {
                        u32 b = val & 0x0F;
                        if (!b) b = 1;
                        bankHi(b);
                    } else {
                        ramEn = (val & 0x0F) == 0x0A;
                    }
                }
                break;

            case GB_MBC5:
                if (addr < 0x2000) {
                    ramEn = (val & 0xF) == 0xA;
                } else if (addr < 0x3000) {
                    bankHi((romBank & 0x100) | val);
                } else if (addr < 0x4000) {
                    bankHi((romBank & 0xFF) | ((val & 1) << 8));
                } else if (addr < 0x6000) {
                    ramBank = val & 0x0F;
                }
                break;

            case GB_MBC_CAMERA:
                if (addr < 0x2000) {
                    ramEn = (val & 0x0F) == 0x0A;
                } else if (addr < 0x4000) {
                    u32 b = val & 0x3F;
                    if (!b) b = 1;
                    bankHi(b);
                } else if (addr < 0x6000) {
                    // RAMB register (actually at 4000-5FFF, not 0000-1FFF):
                    // 0x10 = expose camera registers at A000-A035, else
                    // selects one of 16 SRAM banks.
                    if (val == 0x10) { camRegMode = true; }
                    else { camRegMode = false; ramBank = val & 0x0F; }
                }
                break;

            default: break;
        }
        return;
    }

    if (addr < 0xA000) { vram[addr - 0x8000] = val; return; }

    if (addr < 0xC000) {
        if (mbcType == GB_MBC_CAMERA && camRegMode) {
            u32 idx = addr - 0xA000;
            if (idx < 0x36) {
                camRegs[idx] = val;
                // Capture-trigger bit: real capture is a host/frontend
                // concern (see peridot.h header comment) — flagged here so
                // the glue layer can notice and service it once wired up.
                // JS: if(idx===0&&(val&0x01)) this._doCameraCapture();
            }
            return;
        }
        if (!ramEn) return;
        if (mbcType == GB_MBC3 && rtcSel >= 0) { rtcRegs[rtcSel] = val; return; }
        if (mbcType == GB_MBC2) { eram[(addr - 0xA000) & 0x01FF] = val & 0x0F; sramDirty = true; return; }
        eram[(size_t)ramBank * 0x2000 + (addr - 0xA000)] = val;
        sramDirty = true;
        return;
    }

    if (addr < 0xE000) { wram[addr - 0xC000] = val; return; }
    if (addr < 0xFE00) { wram[addr - 0xE000] = val; return; }
    if (addr < 0xFEA0) { oam[addr - 0xFE00] = val; return; }
    if (addr < 0xFF00) return;

    if (addr == 0xFF00) { io[0] = val; return; }
    if (addr == 0xFF02) {
        io[0x02] = val;
        if ((val & 0x81) == 0x81) serialCycles = 8192;
        return;
    }
    if (addr == 0xFF04) {
        // Reset the internal 16-bit div counter (not just the readable
        // register). Also reset APU frame sequencer — it's driven by the
        // same clock.
        if (timer) timer->div = 0;
        if (apu)   apu->fsTimer = 0;
        io[0x04] = 0;
        return;
    }
    if (addr == 0xFF0F) { ifReg = (val & 0x1F) | 0xE0; return; }
    if (addr == 0xFF46) {
        u16 s = (u16)(val << 8);
        for (u32 i = 0; i < 0xA0; i++) oam[i] = read8((u16)(s + i));
        return;
    }
    if (addr < 0xFF80) {
        // IMPORTANT: write IO first, then notify APU. APU trigger handlers
        // read back NRx values from io; calling APU before updating io can
        // make triggers compute frequency from stale values.
        io[addr - 0xFF00] = val;
        if (addr >= 0xFF10 && addr <= 0xFF3F && apu) apu->write(addr, val);
        return;
    }
    if (addr < 0xFFFF) { hram[addr - 0xFF80] = val; return; }
    ie = val;
}
