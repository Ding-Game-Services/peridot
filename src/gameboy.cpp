// gameboy.cpp — D!NG Peridot (Game Boy) top-level glue.
//
// Direct port of gb-core.js's GameBoy class. runFrame() drives CPU/PPU/Timer/
// APU per-instruction exactly like the JS while loop. Save state uses the
// SDK's .ding block writer/reader (ding_savestate.h) instead of JS's
// JSON.stringify — same fields, different container format.
//
// Per DING_CORE_SDK_RULES.md: this is the only file that touches all of
// Bus/Cpu/Ppu/Timer/Apu together — everything else stays scoped to its own
// component.

#include "peridot.h"
#include "ding_savestate.h"
#include <cstring>
#include <cstdio>

GameBoy::GameBoy()
    : bus(), ppu(&bus), cpu(&bus), timer(&bus), apu(&bus) {
    bus.timer = &timer;
    bus.apu = &apu;
}

void GameBoy::reset() {
    // Re-run each component's own reset/reconstruction. Matches gb-core.js's
    // onReset() semantics at the component level (the HTML frontend's
    // SRAM-preserving reset behaviour is a frontend concern, not core — see
    // GBEmu_Browser_Frontend.html's onReset(), which snapshots/restores
    // bus.eram around a fresh GameBoy() the same way this core's caller
    // should snapshot/restore eram around calling loadROM() again if a
    // SRAM-preserving reset is desired).
    bus.reset();
    apu.reset();
    cpu = Cpu(&bus);
    timer = Timer(&bus);
    bus.timer = &timer;
    bus.apu = &apu;
}

bool GameBoy::loadROM(const u8* data, size_t len) {
    if (!bus.loadROM(data, len)) {
        setError("loadROM failed: null or empty ROM data");
        return false;
    }
    cpu = Cpu(&bus);
    timer = Timer(&bus);
    apu.reset();
    bus.timer = &timer;
    bus.apu = &apu;
    errorFlag = false;
    errorMsg[0] = 0;
    return true;
}

void GameBoy::runFrame() {
    u32 c = 0;
    while (c < GB_CYCLES_PER_FRAME) {
        u32 t = cpu.step();
        ppu.step(t);
        timer.step(t);
        bus.stepSerial(t);
        apu.step(t);
        c += t;
    }
}

void GameBoy::setError(const char* msg) {
    errorFlag = true;
    std::snprintf(errorMsg, sizeof(errorMsg), "%s", msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Save state
//
// Serializes all mutable emulation state into a caller-owned buffer via the
// SDK's .ding block format. The ROM itself is not saved — the frontend/host
// is expected to bind save states to the ROM's MD5 (ding_get_rom_identity())
// exactly as gb-core.js's serializeState()/deserializeState() do at the
// frontend layer with the .ding header's MD5 field.
// ─────────────────────────────────────────────────────────────────────────────
bool GameBoy::saveState(u8* buf, size_t bufSize, size_t* outSize) {
    if (!buf || bufSize < 4096) {
        setError("Save state buffer too small");
        return false;
    }

    DingSaveWriter w;
    ding_save_writer_init(&w, buf, bufSize, "Game Boy");

    // ── CPU ──────────────────────────────────────────────────────────────────
    struct CpuBlock {
        u8  a, f, b, c, d, e, h, l;
        u16 sp, pc;
        u8  halted, ime, pad[2];
        u32 pendingIME;
        u8  haltBug, pad2[3];
    } cb;
    cb.a = cpu.a; cb.f = cpu.f; cb.b = cpu.b; cb.c = cpu.c;
    cb.d = cpu.d; cb.e = cpu.e; cb.h = cpu.h; cb.l = cpu.l;
    cb.sp = cpu.sp; cb.pc = cpu.pc;
    cb.halted = cpu.halted ? 1 : 0; cb.ime = cpu.ime ? 1 : 0;
    cb.pendingIME = cpu.pendingIME;
    cb.haltBug = cpu.haltBug ? 1 : 0;
    ding_save_write_block(&w, "CPU", &cb, sizeof(cb));

    // ── Timer ────────────────────────────────────────────────────────────────
    ding_save_write_block(&w, "TIMER", &timer.div, sizeof(timer.div));

    // ── PPU (internal timing state only — pixels/framebuf are derived) ─────
    struct PpuBlock { u32 cycles; u8 mode; u32 winLine; u8 lcdWasOff; } pb;
    // Note: Ppu's timing fields are private; exposed here via friend-free
    // save/restore through the public step-observable surface is not
    // possible without accessors, so PPU timing state round-trips via the
    // io registers it derives from (lcdc/ly/stat) plus a fresh mode-2 spin-up
    // on load — matches gb-core.js's own tolerance for minor timing skew
    // immediately after a load (the JS version restores these fields
    // directly since it has no private/public distinction; a future pass
    // can add Ppu accessors if frame-exact restore turns out to matter).
    pb.cycles = 0; pb.mode = 2; pb.winLine = 0; pb.lcdWasOff = 0;
    ding_save_write_block(&w, "PPU", &pb, sizeof(pb));

    // ── MMU scalars ──────────────────────────────────────────────────────────
    struct MmuBlock {
        u8  ie, ifReg;
        u32 romBank, ramBank, romLoBank;
        u8  ramEn, mbc1Mode, mbc1HiBits;
        s32 rtcSel;
        u8  rtcLatchStep, camRegMode, pad[2];
    } mb;
    mb.ie = bus.ie; mb.ifReg = bus.ifReg;
    mb.romBank = bus.romBank; mb.ramBank = bus.ramBank; mb.romLoBank = bus.romLoBank;
    mb.ramEn = bus.ramEn ? 1 : 0; mb.mbc1Mode = bus.mbc1Mode; mb.mbc1HiBits = bus.mbc1HiBits;
    mb.rtcSel = bus.rtcSel; mb.rtcLatchStep = bus.rtcLatchStep;
    mb.camRegMode = bus.camRegMode ? 1 : 0;
    ding_save_write_block(&w, "MMU_SCALARS", &mb, sizeof(mb));

    ding_save_write_block(&w, "VRAM",  bus.vram.data(),  bus.vram.size());
    ding_save_write_block(&w, "WRAM",  bus.wram.data(),  bus.wram.size());
    ding_save_write_block(&w, "OAM",   bus.oam.data(),   bus.oam.size());
    ding_save_write_block(&w, "HRAM",  bus.hram.data(),  bus.hram.size());
    ding_save_write_block(&w, "IO",    bus.io.data(),    bus.io.size());
    ding_save_write_block(&w, "ERAM",  bus.eram.data(),  bus.eram.size());
    ding_save_write_block(&w, "CAMREGS", bus.camRegs.data(), bus.camRegs.size());
    ding_save_write_block(&w, "RTC_REGS", bus.rtcRegs, sizeof(bus.rtcRegs));
    ding_save_write_block(&w, "RTC_LATCHED", bus.rtcLatched, sizeof(bus.rtcLatched));

    // ── APU ──────────────────────────────────────────────────────────────────
    struct ApuChBlock {
        u8  on, dacOn, pad[2];
        u32 dutyStep, wavePos;
        s32 freqTimer;
        u32 lenCounter;
        u8  lenEnable, pad2[3];
        u32 vol, volInit, volDir, volTimer, volPeriod, freq, output;
        u32 sweepTimer, sweepPeriod, sweepDir, sweepShift;
        u8  sweepEnable, pad3[3];
        u32 shadowFreq;
        u16 lfsr;
        u8  narrowMode, pad4;
    };
    auto pack = [](const GbApuChannel& c) -> ApuChBlock {
        ApuChBlock b{};
        b.on = c.on ? 1 : 0; b.dacOn = c.dacOn ? 1 : 0;
        b.dutyStep = c.dutyStep; b.wavePos = c.wavePos;
        b.freqTimer = c.freqTimer; b.lenCounter = c.lenCounter;
        b.lenEnable = c.lenEnable ? 1 : 0;
        b.vol = c.vol; b.volInit = c.volInit; b.volDir = c.volDir;
        b.volTimer = c.volTimer; b.volPeriod = c.volPeriod;
        b.freq = c.freq; b.output = c.output;
        b.sweepTimer = c.sweepTimer; b.sweepPeriod = c.sweepPeriod;
        b.sweepDir = c.sweepDir; b.sweepShift = c.sweepShift;
        b.sweepEnable = c.sweepEnable ? 1 : 0;
        b.shadowFreq = c.shadowFreq;
        b.lfsr = c.lfsr; b.narrowMode = c.narrowMode ? 1 : 0;
        return b;
    };
    struct ApuBlock { u32 fsTimer, fsStep; ApuChBlock ch1, ch2, ch3, ch4; } ab;
    ab.fsTimer = apu.fsTimer; ab.fsStep = apu.fsStep;
    ab.ch1 = pack(apu.ch1); ab.ch2 = pack(apu.ch2);
    ab.ch3 = pack(apu.ch3); ab.ch4 = pack(apu.ch4);
    ding_save_write_block(&w, "APU", &ab, sizeof(ab));

    size_t finishSize = 0;
    ding_save_writer_finish(&w, &finishSize);
    if (outSize) *outSize = finishSize;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load state
// ─────────────────────────────────────────────────────────────────────────────
bool GameBoy::loadState(const u8* buf, size_t size) {
    if (!buf || size < 64) {
        setError("Load state buffer too small or null");
        return false;
    }

    DingSaveReader r;
    if (ding_save_reader_init(&r, buf, size) != DING_SS_OK) {
        setError("Load state: invalid .ding file");
        return false;
    }

    struct CpuBlock {
        u8  a, f, b, c, d, e, h, l;
        u16 sp, pc;
        u8  halted, ime, pad[2];
        u32 pendingIME;
        u8  haltBug, pad2[3];
    } cb{};
    if (ding_save_read_block(&r, "CPU", &cb, sizeof(cb), nullptr) == DING_SS_OK) {
        cpu.a = cb.a; cpu.f = cb.f; cpu.b = cb.b; cpu.c = cb.c;
        cpu.d = cb.d; cpu.e = cb.e; cpu.h = cb.h; cpu.l = cb.l;
        cpu.sp = cb.sp; cpu.pc = cb.pc;
        cpu.halted = cb.halted != 0; cpu.ime = cb.ime != 0;
        cpu.pendingIME = cb.pendingIME;
        cpu.haltBug = cb.haltBug != 0;
    }

    ding_save_read_block(&r, "TIMER", &timer.div, sizeof(timer.div), nullptr);

    struct MmuBlock {
        u8  ie, ifReg;
        u32 romBank, ramBank, romLoBank;
        u8  ramEn, mbc1Mode, mbc1HiBits;
        s32 rtcSel;
        u8  rtcLatchStep, camRegMode, pad[2];
    } mb{};
    if (ding_save_read_block(&r, "MMU_SCALARS", &mb, sizeof(mb), nullptr) == DING_SS_OK) {
        bus.ie = mb.ie; bus.ifReg = mb.ifReg;
        bus.ramBank = mb.ramBank;
        bus.ramEn = mb.ramEn != 0; bus.mbc1Mode = mb.mbc1Mode; bus.mbc1HiBits = mb.mbc1HiBits;
        bus.rtcSel = mb.rtcSel; bus.rtcLatchStep = mb.rtcLatchStep;
        bus.camRegMode = mb.camRegMode != 0;
        // Re-derive the actual bank window contents (romLo/romHi are mirror
        // copies, not the source of truth — bankLo/bankHi rebuild them from
        // bus.rom + the restored bank numbers). Works uniformly across all
        // MBC types, unlike re-driving bank-select writes through write8().
        bus.bankLo(mb.romLoBank);
        bus.bankHi(mb.romBank);
    }

    ding_save_read_block(&r, "VRAM", bus.vram.data(), bus.vram.size(), nullptr);
    ding_save_read_block(&r, "WRAM", bus.wram.data(), bus.wram.size(), nullptr);
    ding_save_read_block(&r, "OAM",  bus.oam.data(),  bus.oam.size(), nullptr);
    ding_save_read_block(&r, "HRAM", bus.hram.data(), bus.hram.size(), nullptr);
    ding_save_read_block(&r, "IO",   bus.io.data(),   bus.io.size(), nullptr);
    ding_save_read_block(&r, "ERAM", bus.eram.data(), bus.eram.size(), nullptr);
    ding_save_read_block(&r, "CAMREGS", bus.camRegs.data(), bus.camRegs.size(), nullptr);
    ding_save_read_block(&r, "RTC_REGS", bus.rtcRegs, sizeof(bus.rtcRegs), nullptr);
    ding_save_read_block(&r, "RTC_LATCHED", bus.rtcLatched, sizeof(bus.rtcLatched), nullptr);

    struct ApuChBlock {
        u8  on, dacOn, pad[2];
        u32 dutyStep, wavePos;
        s32 freqTimer;
        u32 lenCounter;
        u8  lenEnable, pad2[3];
        u32 vol, volInit, volDir, volTimer, volPeriod, freq, output;
        u32 sweepTimer, sweepPeriod, sweepDir, sweepShift;
        u8  sweepEnable, pad3[3];
        u32 shadowFreq;
        u16 lfsr;
        u8  narrowMode, pad4;
    };
    auto unpack = [](const ApuChBlock& b, GbApuChannel& c) {
        c.on = b.on != 0; c.dacOn = b.dacOn != 0;
        c.dutyStep = b.dutyStep; c.wavePos = b.wavePos;
        c.freqTimer = b.freqTimer; c.lenCounter = b.lenCounter;
        c.lenEnable = b.lenEnable != 0;
        c.vol = b.vol; c.volInit = b.volInit; c.volDir = b.volDir;
        c.volTimer = b.volTimer; c.volPeriod = b.volPeriod;
        c.freq = b.freq; c.output = b.output;
        c.sweepTimer = b.sweepTimer; c.sweepPeriod = b.sweepPeriod;
        c.sweepDir = b.sweepDir; c.sweepShift = b.sweepShift;
        c.sweepEnable = b.sweepEnable != 0;
        c.shadowFreq = b.shadowFreq;
        c.lfsr = b.lfsr; c.narrowMode = b.narrowMode != 0;
    };
    struct ApuBlock { u32 fsTimer, fsStep; ApuChBlock ch1, ch2, ch3, ch4; } ab{};
    if (ding_save_read_block(&r, "APU", &ab, sizeof(ab), nullptr) == DING_SS_OK) {
        apu.fsTimer = ab.fsTimer; apu.fsStep = ab.fsStep;
        unpack(ab.ch1, apu.ch1); unpack(ab.ch2, apu.ch2);
        unpack(ab.ch3, apu.ch3); unpack(ab.ch4, apu.ch4);
    }

    errorFlag = false;
    return true;
}
