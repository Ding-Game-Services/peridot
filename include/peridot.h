#pragma once

// peridot.h — D!NG Game Boy Core (Peridot) shared declarations
//
// Port of gb-core.js's MMU + Timer to C++, following ding-core-sdk conventions
// (see genesis-core for the reference pattern this mirrors).
//
// Step 1 of the bottom-up port: MMU/Timer only. CPU, PPU, APU follow in later
// steps and are forward-declared here so Bus can hold optional pointers to
// them for the few places JS has cross-component side effects (e.g. writing
// DIV resets the APU frame sequencer too).
//
// Printer and Game Boy Camera *frame capture* are host/frontend concerns
// (real camera access, DOM canvas) and are deliberately NOT ported into the
// core — see DING_CORE_SDK_RULES.md "What Belongs Where". The MBC-Camera
// register file itself (camRegs) IS core state and is ported; the frontend
// glue layer will be responsible for feeding it real pixel data later via a
// callback, same shape as gb-core.js's `mmu.cameraCapture`.

#include <cstddef>
#include <vector>

#include "ding_types.h"
#include "ding_audio.h"

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr u32 GB_W = 160;
static constexpr u32 GB_H = 144;
static constexpr u32 GB_CYCLES_PER_FRAME = 70224;

static constexpr u32 GB_VRAM_SIZE = 0x2000;
static constexpr u32 GB_WRAM_SIZE = 0x2000;
static constexpr u32 GB_OAM_SIZE  = 0xA0;
static constexpr u32 GB_HRAM_SIZE = 0x7F;
static constexpr u32 GB_IO_SIZE   = 0x80;
static constexpr u32 GB_ROM_BANK_SIZE = 0x4000;
static constexpr u32 GB_ERAM_MIN_SIZE = 0x8000; // matches JS: eram always sized >= 0x8000

// MBC types — numbering matches gb-core.js's mmu.mbcType, not the raw
// cartridge header byte (that mapping happens in Bus::loadROM, same as JS).
enum GbMbcType : u32 {
    GB_MBC_NONE   = 0,
    GB_MBC1       = 1,
    GB_MBC2       = 2,
    GB_MBC3       = 3,
    GB_MBC5       = 5,
    GB_MBC_CAMERA = 6,
};

// Forward declarations — wired up in later steps (CPU/APU).
// bus.cpp only touches these through the narrow interfaces below, never
// reaching into their internals — keeps the "CPU file must not touch audio"
// style rule intact in reverse (bus must not touch APU internals either).
class Apu;

// ── Timer ────────────────────────────────────────────────────────────────────
// Direct port of gb-core.js's Timer class. DIV/TIMA/TMA/TAC live in Bus::io
// (same as JS keeping them in mmu.io) — Timer only owns the free-running
// 16-bit divider counter, since the visible DIV register is just its high byte.
class Bus;

class Timer {
public:
    explicit Timer(Bus* bus);
    void step(u32 cycles);

    u16 div = 0; // internal 16-bit counter; io[0x04] mirrors (div >> 8)

private:
    Bus* bus_;
};

// ── Game Boy Printer ─────────────────────────────────────────────────────────
// Direct port of gb-core.js's PrinterDevice. This is real emulated hardware
// behaviour (the GBP serial protocol: sync -> command -> compression flag ->
// length -> payload -> checksum -> status exchange), not a host concern —
// it was already zero-DOM in the JS version. What IS a host concern is what
// happens to a finished image (show it, save it) — same split as the PPU
// producing framebuf pixels vs. the frontend blitting them to a canvas.
class PrinterDevice {
public:
    PrinterDevice();
    void reset();

    // Returns true if this byte was claimed (printer protocol in progress or
    // recognized), filling *inByte with the shifted-back byte. Returns false
    // to let the next device in the chain (link cable / netplay) have it —
    // mirrors JS's exchangeByte() returning null to decline.
    bool exchangeByte(u8 outByte, u8* inByte);

    // Fired once a PRINT command (0x02) completes rendering. pixels is an
    // RGBA8 buffer, width*height*4 bytes, core-owned for the duration of the
    // callback only — host must copy if it needs the data afterward.
    void (*onImage)(const u8* pixels, u32 width, u32 height, void* userdata) = nullptr;
    void* onImageUserdata = nullptr;

private:
    enum class State { Sync1, Sync2, Cmd, Comp, LenLo, LenHi, Data, Ck1, Ck2, Alive, Status };
    State state_ = State::Sync1;
    u8  cmd_ = 0;
    u8  compression_ = 0;
    u32 dataLen_ = 0;
    u32 dataRead_ = 0;
    std::vector<u8> payload_;
    std::vector<u8> tileBuffer_;
    u8  statusByte_ = 0;

    std::vector<u8> decompress(const std::vector<u8>& bytes) const;
    void handleCommand();
    void renderAndEmit();
};

// ── Bus (MMU) ────────────────────────────────────────────────────────────────
// Direct port of gb-core.js's MMU class. Owns all addressable memory and
// cartridge banking logic. PPU/CPU/APU read and write through this in later
// steps exactly as they call mmu.read()/mmu.write() in JS today.
class Bus {
public:
    Bus();

    void reset();                             // re-applies power-on IO defaults
    bool loadROM(const u8* data, size_t len);  // returns false on null/empty ROM

    u8   read8(u16 addr);
    void write8(u16 addr, u8 val);

    void requestInterrupt(u8 n) { ifReg |= (u8)(1u << n); }

    // Joypad
    void pressButton(u8 btn, bool down);

    // Serial clock — real hardware behaviour (shift timing + IRQ) lives here.
    // The built-in PrinterDevice always gets first refusal on serial bytes
    // (matches gb-core.js unshifting PrinterDevice to the front of its
    // device chain), falling through to serialExchange for anything else
    // that wants the port (link cable / netplay), which stays a host
    // concern since it needs real networking.
    void stepSerial(u32 cycles);
    u8 (*serialExchange)(u8 outByte) = nullptr;

    // Optional cross-component link for the DIV-reset side effect
    // (gb-core.js: writing $FF04 also zeroes apu.fsTimer). Left null until
    // the APU step wires it up; write8() no-ops the APU half if unset.
    Apu* apu = nullptr;
    Timer* timer = nullptr; // set by GameBoy after both are constructed

    // ── Raw memory (sized exactly like gb-core.js's typed arrays) ───────────
    std::vector<u8> vram  = std::vector<u8>(GB_VRAM_SIZE);
    std::vector<u8> wram  = std::vector<u8>(GB_WRAM_SIZE);
    std::vector<u8> oam   = std::vector<u8>(GB_OAM_SIZE);
    std::vector<u8> hram  = std::vector<u8>(GB_HRAM_SIZE);
    std::vector<u8> io    = std::vector<u8>(GB_IO_SIZE);
    u8  ie    = 0;
    u8  ifReg = 0xE0;

    // ── Cartridge ────────────────────────────────────────────────────────────
    std::vector<u8> rom;          // full ROM image, JS: fullRom
    std::vector<u8> romLo = std::vector<u8>(GB_ROM_BANK_SIZE); // fixed/low window mirror
    std::vector<u8> romHi = std::vector<u8>(GB_ROM_BANK_SIZE); // switchable/high window mirror
    std::vector<u8> eram  = std::vector<u8>(GB_ERAM_MIN_SIZE);

    GbMbcType mbcType = GB_MBC_NONE;
    u32 romBanks = 2;
    u32 romBank  = 1;
    u32 romLoBank = 0;
    u32 ramBank  = 0;
    bool ramEn   = false;

    u8  mbc1Mode   = 0; // 0 = ROM banking mode, 1 = RAM banking mode
    u8  mbc1HiBits = 0;

    // MBC3 real-time clock
    u8  rtcRegs[5]    = {0, 0, 0, 0, 0};
    u8  rtcLatched[5] = {0, 0, 0, 0, 0};
    s32 rtcSel        = -1;
    u8  rtcLatchStep  = 0;
    s64 rtcBase       = 0; // seconds since epoch at last latch base; host supplies via setRtcBase()

    // MBC-Camera register file. Frame capture itself is a host/frontend
    // concern (see file header comment) — the frontend calls
    // Bus::cameraCapture(...) equivalent once that glue exists.
    bool camRegMode = false;
    std::vector<u8> camRegs = std::vector<u8>(0x36);

    // ── Joypad / serial ──────────────────────────────────────────────────────
    u8 buttons = 0xFF;
    u32 serialCycles = 0;

    // ── SRAM dirty flag (frontend uses this to know when to flush a .sav) ───
    bool sramDirty = false;

    // Built-in Game Boy Printer accessory — always present on the serial
    // port, exactly as gb-core.js's GameBoy constructor always creates one
    // and unshifts it onto the device chain. Host wires up
    // printer.onImage to display/save finished printouts.
    PrinterDevice printer;

    // Exposed for GameBoy::loadState() to restore exact bank state after a
    // save-state load, without relying on re-deriving it through write8()
    // (which only works for MBC types whose bank-select writes are
    // idempotent — not a safe assumption to lean on for state restore).
    void bankLo(u32 bank);
    void bankHi(u32 bank);

private:
    void initIO();
    void updateRTC();
    u8   readJoy();
};

// ── CPU ──────────────────────────────────────────────────────────────────────
// Direct port of gb-core.js's CPU class (SM83 core). Reads/writes go through
// Bus exactly as JS calls mmu.read()/mmu.write(). Returns cycle count per
// step(), same contract as JS (GameBoy.runFrame() sums these against
// GB_CYCLES_PER_FRAME).
class Cpu {
public:
    explicit Cpu(Bus* bus);

    u32 step(); // executes one instruction (or services a pending IRQ), returns cycles

    // ── Registers ────────────────────────────────────────────────────────────
    u8  a = 0x01, f = 0xB0, b = 0x00, c = 0x13;
    u8  d = 0x00, e = 0xD8, h = 0x01, l = 0x4D;
    u16 sp = 0xFFFE, pc = 0x0100;
    bool halted = false;
    bool ime = false;
    u32  pendingIME = 0;
    bool haltBug = false;

    // ── Flag accessors (bit positions match JS: Z=7 N=6 H=5 C=4) ────────────
    u8   zf() const { return (f >> 7) & 1; }
    void setZf(u8 v) { if (v) f |= 0x80; else f &= 0x70; }
    u8   nf() const { return (f >> 6) & 1; }
    void setNf(u8 v) { if (v) f |= 0x40; else f &= 0xB0; }
    u8   hf() const { return (f >> 5) & 1; }
    void setHf(u8 v) { if (v) f |= 0x20; else f &= 0xD0; }
    u8   cf() const { return (f >> 4) & 1; }
    void setCf(u8 v) { if (v) f |= 0x10; else f &= 0xE0; }

    u16  af() const { return (u16)((a << 8) | (f & 0xF0)); }
    void setAf(u16 v) { a = (u8)((v >> 8) & 0xFF); f = (u8)(v & 0xF0); }
    u16  bc() const { return (u16)((b << 8) | c); }
    void setBc(u16 v) { b = (u8)((v >> 8) & 0xFF); c = (u8)(v & 0xFF); }
    u16  de() const { return (u16)((d << 8) | e); }
    void setDe(u16 v) { d = (u8)((v >> 8) & 0xFF); e = (u8)(v & 0xFF); }
    u16  hl() const { return (u16)((h << 8) | l); }
    void setHl(u16 v) { h = (u8)((v >> 8) & 0xFF); l = (u8)(v & 0xFF); }

private:
    Bus* bus_;

    u8   rb();
    u16  rw();
    void push(u16 v);
    u16  pop();

    void ADD(u8 v); void ADC(u8 v); void SUB(u8 v); void SBC(u8 v);
    void AND(u8 v); void XOR(u8 v); void OR(u8 v);  void CP(u8 v);
    u8   INC(u8 v); u8   DEC(u8 v); void ADDHL(u16 v);
    u8   RLC(u8 v); u8   RRC(u8 v); u8   RL(u8 v);  u8   RR(u8 v);
    u8   SLA(u8 v); u8   SRA(u8 v); u8   SWAP(u8 v); u8   SRL(u8 v);
    void testBit(u8 bit, u8 v);
    u8   RES(u8 bit, u8 v) { return v & (u8)~(1u << bit); }
    u8   SET(u8 bit, u8 v) { return v | (u8)(1u << bit); }

    u32  exec(u8 op);
    u32  execCB();
};

// ── PPU ──────────────────────────────────────────────────────────────────────
// Direct port of gb-core.js's PPU class. Produces an RGBA8 framebuffer, same
// pixel format ding_core.h expects (DING_PIXFMT_RGBA8) so core_api.cpp can
// hand this straight out via ding_get_framebuffer() once glue is wired up.
struct GbColor { u8 r, g, b; };

class Ppu {
public:
    explicit Ppu(Bus* bus);

    void step(u32 cycles);
    void setPalette(const GbColor colors[4]);
    void blit(); // public: frontend/glue calls this directly on palette swap mid-pause (mirrors gb.ppu._blit() in gb-core.js)

    std::vector<u8> pixels   = std::vector<u8>(GB_W * GB_H);        // 2-bit-per-pixel palette indices (0-3)
    std::vector<u8> framebuf = std::vector<u8>(GB_W * GB_H * 4, 0); // RGBA8
    bool frameReady = false;

private:
    Bus* bus_;
    u32  cycles_ = 0;
    u8   mode_ = 2;
    u32  winLine_ = 0;
    bool lcdWasOff_ = false;
    GbColor palette_[4] = {{155,188,15},{139,172,15},{48,98,48},{15,56,15}}; // default: classic DMG

    // Register accessors — mirror the JS getters, all backed by bus_->io
    u8  lcdc() const { return bus_->io[0x40]; }
    u8  ly()   const { return bus_->io[0x44]; }
    void setLy(u8 v) { bus_->io[0x44] = v; }
    u8  lyc()  const { return bus_->io[0x45]; }
    u8  scy()  const { return bus_->io[0x42]; }
    u8  scx()  const { return bus_->io[0x43]; }
    u8  bgp()  const { return bus_->io[0x47]; }
    u8  obp0() const { return bus_->io[0x48]; }
    u8  obp1() const { return bus_->io[0x49]; }
    u8  wy()   const { return bus_->io[0x4A]; }
    s16 wx()   const { return (s16)bus_->io[0x4B] - 7; }

    void setMode(u8 m);
    void checkLYC();
    u32  tileAddr(u8 idx, bool signed_) const;
    u8   palColor(u8 pal, u8 ci) const { return (pal >> (ci * 2)) & 3; }
    void drawLine();
};

// ── APU ──────────────────────────────────────────────────────────────────────
// Direct port of gb-core.js's APU class (4-channel DMG sound). DSP logic
// (duty/envelope/sweep/wave/noise generation, mixing, high-pass filter) is
// kept identical to JS. What's NOT ported: initAudio()/stopAudio() and the
// ScriptProcessor ring-buffer — those are Web Audio calls, a browser/host
// concern per SDK rules. Finished samples go through ding_audio.h's
// DingAudioBuffer instead, same pattern every other Ding core uses.
static constexpr u32 GB_SAMPLE_RATE = 44100;
static constexpr u32 GB_CPU_FREQ    = 4194304;
static constexpr u32 GB_AUDIO_CHANNELS = 2;
static constexpr u32 GB_AUDIO_CAPACITY = 4096;
static constexpr u32 GB_AUDIO_STORAGE  = GB_AUDIO_CAPACITY * GB_AUDIO_CHANNELS;

struct GbApuChannel {
    bool on = false, dacOn = false;
    u32  dutyStep = 0, wavePos = 0;
    s32  freqTimer = 8192;
    u32  lenCounter = 0;
    bool lenEnable = false;
    u32  vol = 0, volInit = 0, volDir = 0, volTimer = 0, volPeriod = 0;
    u32  freq = 0;
    u32  output = 0;
    // CH1-only (sweep)
    u32  sweepTimer = 0, sweepPeriod = 0, sweepDir = 0, sweepShift = 0;
    bool sweepEnable = false;
    u32  shadowFreq = 0;
    // CH4-only (noise)
    u16  lfsr = 0x7FFF;
    bool narrowMode = false;
};

class Apu {
public:
    explicit Apu(Bus* bus);

    void write(u16 addr, u8 val);
    void step(u32 cycles);
    void reset();

    u32 fsTimer = 0; // exposed so Bus can zero it on $FF04 write (DIV reset), matches gb-core.js
    u32 fsStep = 0;

    GbApuChannel ch1, ch2, ch3, ch4;

    // Ring buffer handed straight to ding_core.h's audio functions once
    // core_api.cpp is wired up — see file header comment.
    DingAudioBuffer audioBuf;

private:
    Bus* bus_;
    float storage_[GB_AUDIO_STORAGE];

    double cycleBuf_ = 0.0;
    double cyclesPerSample_ = (double)GB_CPU_FREQ / GB_SAMPLE_RATE;

    // Capacitor high-pass filter state — see .cpp for the derivation.
    static constexpr double HP_ALPHA = 0.9943;
    double hpL_ = 0, hpR_ = 0, hpPrevL_ = 0, hpPrevR_ = 0;

    u8  r(u16 off) const { return bus_->io[off]; }
    u32 ch1Freq() const { return (u32)(((r(0x14) & 7) << 8) | r(0x13)); }
    u32 ch2Freq() const { return (u32)(((r(0x19) & 7) << 8) | r(0x18)); }
    u32 ch3Freq() const { return (u32)(((r(0x1E) & 7) << 8) | r(0x1D)); }

    void triggerCh1();
    void triggerCh2();
    void triggerCh3();
    void triggerCh4();

    void clockLen(GbApuChannel& c);
    void clockSweep();
    void clockVol(GbApuChannel& c);
    void stepFS();
};

// ── GameBoy (top-level glue) ─────────────────────────────────────────────────
// Direct port of gb-core.js's GameBoy class — owns one of each component and
// drives them per-instruction, exactly like gb.runFrame()'s while loop.
// This is the type ding_core.h's core_api.cpp wraps in a static pointer,
// same pattern as Genesis's core_api.cpp wrapping a static Genesis*.
class GameBoy {
public:
    GameBoy();

    Bus bus;
    Ppu ppu;
    Cpu cpu;
    Timer timer;
    Apu apu;

    void reset();                              // re-creates sub-objects at power-on state
    bool loadROM(const u8* data, size_t len);
    void runFrame();                           // advances exactly one frame (returns via ppu.framebuf)
    void pressButton(u8 btn, bool down) { bus.pressButton(btn, down); }

    // Save state — full-state serialization via ding_savestate.h's block
    // writer/reader, same shape as every other Ding core. The .sav (battery
    // SRAM) path is separate and handled by the frontend directly reading
    // bus.eram, same as gb-core.js leaves SRAM export/import to the HTML file.
    bool saveState(u8* buf, size_t bufSize, size_t* outSize);
    bool loadState(const u8* buf, size_t size);

    bool errorFlag = false;
    char errorMsg[256] = {0};

private:
    void setError(const char* msg);
};
