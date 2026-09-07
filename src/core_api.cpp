// core_api.cpp — D!NG Peridot: ding_core.h implementation.
//
// Wraps a static GameBoy* the same way genesis-core's core_api.cpp wraps a
// static Genesis*. This file is the only place that talks to the outside
// world (Hydra, Cockpit, WASM glue, the headless harness) — everything else
// in Peridot only knows about Bus/Cpu/Ppu/Timer/Apu/GameBoy.

#include "peridot.h"
#include "ding_core.h"
#include "ding_audio.h"
#include "ding_md5.h"
#include <cstring>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  define DING_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#  define DING_EXPORT
#endif

extern "C" {

static GameBoy* s_gb = nullptr;

static DingCoreInfo      s_core_info;
static DingVideoInfo     s_video_info;
static DingAudioInfo     s_audio_info;
static DingSaveStateInfo s_save_info;
static DingRomIdentity   s_rom_id;

// Button indices match gb-core.js's JOYPAD bit-position mapping (see the
// header comment at the top of gb-core.js): 0=Right 1=Left 2=Up 3=Down
// 4=A 5=B 6=Select 7=Start.
static const char* s_input_names[8] = {
    "Right", "Left", "Up", "Down", "A", "B", "Select", "Start",
};
static constexpr u32 INPUT_COUNT = 8;
static constexpr u32 REGION_COUNT = 6; // VRAM, WRAM, OAM, HRAM, IO, ROM (SRAM appended conditionally)
static constexpr size_t SAVE_MAX = 128u * 1024u; // VRAM+WRAM+ERAM(up to 128KB)+misc comfortably fits

DING_EXPORT void ding_init() {
    if (!s_gb) s_gb = new GameBoy();

    s_core_info.core_name = "peridot";
    s_core_info.platform_name = "Nintendo Game Boy";
    s_core_info.version = "0.1.0";
    s_core_info.api_version_major = DING_CORE_API_VERSION_MAJOR;
    s_core_info.api_version_minor = DING_CORE_API_VERSION_MINOR;

    s_video_info.base_width = GB_W;
    s_video_info.base_height = GB_H;
    s_video_info.max_width = GB_W;
    s_video_info.max_height = GB_H;
    s_video_info.format = DING_PIXFMT_RGBA8;
    s_video_info.dynamic = 0;

    s_audio_info.sample_rate = GB_SAMPLE_RATE;
    s_audio_info.channels = (u8)GB_AUDIO_CHANNELS;

    s_save_info.method = DING_SAVE_FULL;
    s_save_info.max_size = SAVE_MAX;
    s_save_info.supported = 1;
}

DING_EXPORT void ding_destroy() {
    delete s_gb;
    s_gb = nullptr;
}

DING_EXPORT void ding_reset() {
    if (s_gb) s_gb->reset();
}

DING_EXPORT DingResult ding_load_rom(const u8* data, size_t len) {
    if (!s_gb) return DING_ERR_GENERIC;
    return s_gb->loadROM(data, len) ? DING_OK : DING_ERR_BAD_ROM;
}

DING_EXPORT DingResult ding_load_disc(DingDiscImage* /*disc*/) {
    return DING_ERR_NO_DISC; // cartridge system, no disc support
}

DING_EXPORT DingResult ding_load_bios(u32 /*index*/, const u8* /*data*/, size_t /*len*/) {
    return DING_OK; // DMG has no external BIOS file in this core's scope
}

DING_EXPORT u8 ding_is_disc_swap_pending() { return 0; }
DING_EXPORT void ding_swap_disc(DingDiscImage* /*disc*/) {}

DING_EXPORT void ding_set_region(const char* /*region*/) {
    // DMG timing doesn't vary by region the way NTSC/PAL consoles do.
}

DING_EXPORT void ding_run_frame() {
    if (s_gb) s_gb->runFrame();
}

DING_EXPORT const DingCoreInfo* ding_get_core_info() { return &s_core_info; }
DING_EXPORT const DingVideoInfo* ding_get_video_info() { return &s_video_info; }
DING_EXPORT const DingAudioInfo* ding_get_audio_info() { return &s_audio_info; }

DING_EXPORT const DingRomIdentity* ding_get_rom_identity() {
    std::memset(&s_rom_id, 0, sizeof(s_rom_id));
    if (s_gb && !s_gb->bus.rom.empty()) {
        s_rom_id.method = DING_ID_MD5_FULL;
        ding_md5(s_gb->bus.rom.data(), s_gb->bus.rom.size(), s_rom_id.hash);
    }
    return &s_rom_id;
}

DING_EXPORT const DingSaveStateInfo* ding_get_savestate_info() { return &s_save_info; }

static bool hasSram() {
    // MBC2's "512x4" RAM and camera SRAM both report through the normal
    // eram path — only ROM-only carts genuinely have nothing to expose.
    return s_gb && s_gb->bus.mbcType != GB_MBC_NONE;
}

DING_EXPORT u32 ding_get_memory_region_count() {
    return REGION_COUNT + (hasSram() ? 1u : 0u);
}

DING_EXPORT void ding_get_memory_region(u32 index, DingMemoryRegion* out) {
    if (!out || !s_gb || index >= ding_get_memory_region_count()) return;
    std::memset(out, 0, sizeof(*out));

    switch (index) {
        case 0:
            out->name = "VRAM";
            out->base_addr = 0x8000;
            out->size = s_gb->bus.vram.size();
            out->ptr = s_gb->bus.vram.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
        case 1:
            out->name = "WRAM";
            out->base_addr = 0xC000;
            out->size = s_gb->bus.wram.size();
            out->ptr = s_gb->bus.wram.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
        case 2:
            out->name = "OAM";
            out->base_addr = 0xFE00;
            out->size = s_gb->bus.oam.size();
            out->ptr = s_gb->bus.oam.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
        case 3:
            out->name = "HRAM";
            out->base_addr = 0xFF80;
            out->size = s_gb->bus.hram.size();
            out->ptr = s_gb->bus.hram.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
        case 4:
            out->name = "IO";
            out->base_addr = 0xFF00;
            out->size = s_gb->bus.io.size();
            out->ptr = s_gb->bus.io.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
        case 5:
            // ROM: read-only, exposed purely for debug tooling (mirrors
            // genesis-core's rationale for exposing its ROM region).
            out->name = "ROM";
            out->base_addr = 0x0000;
            out->size = s_gb->bus.rom.size();
            out->ptr = s_gb->bus.rom.empty() ? nullptr : s_gb->bus.rom.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 0;
            break;
        case 6:
            // SRAM stays last and conditional, same rationale as genesis-core:
            // it's the one region that genuinely may not exist (ROM-only carts).
            out->name = "SRAM";
            out->base_addr = 0xA000;
            out->size = s_gb->bus.eram.size();
            out->ptr = s_gb->bus.eram.data();
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
    }
}

DING_EXPORT u32 ding_get_bios_count() { return 0; }
DING_EXPORT void ding_get_bios_descriptor(u32 /*idx*/, DingBiosDescriptor* out) {
    if (out) std::memset(out, 0, sizeof(*out));
}

DING_EXPORT u32 ding_get_input_descriptor_count() { return INPUT_COUNT; }
DING_EXPORT void ding_get_input_descriptor(u32 index, DingInputDescriptor* out) {
    if (!out || index >= INPUT_COUNT) return;
    std::memset(out, 0, sizeof(*out));
    out->name = s_input_names[index];
    out->type = DING_INPUT_BUTTON;
    out->port = 0;
    out->index = (u8)index;
}

DING_EXPORT const u8* ding_get_framebuffer() {
    return s_gb ? s_gb->ppu.framebuf.data() : nullptr;
}

DING_EXPORT void ding_get_current_dimensions(u32* w, u32* h) {
    if (!w || !h) return;
    *w = GB_W; *h = GB_H; // fixed resolution, dynamic == 0
}

DING_EXPORT u32 ding_get_audio_sample_count() {
    return s_gb ? ding_audio_available(&s_gb->apu.audioBuf) : 0;
}

DING_EXPORT u32 ding_read_audio_samples(float* buf, u32 count) {
    if (!s_gb || !buf) return 0;
    return ding_audio_read(&s_gb->apu.audioBuf, buf, count);
}

DING_EXPORT void ding_set_button(u8 port, u8 index, u8 pressed) {
    if (s_gb && port == 0 && index < INPUT_COUNT)
        s_gb->pressButton(index, pressed != 0);
}

DING_EXPORT void ding_set_axis(u8 /*port*/, u8 /*index*/, int16_t /*value*/) {
    // DMG has no analog inputs.
}

DING_EXPORT size_t ding_save_state(u8* buf, size_t buf_size) {
    if (!s_gb || !buf) return 0;
    size_t outSize = 0;
    if (s_gb->saveState(buf, buf_size, &outSize)) return outSize;
    return 0;
}

DING_EXPORT DingResult ding_load_state(const u8* buf, size_t len) {
    if (!s_gb || !buf) return DING_ERR_BAD_STATE;
    return s_gb->loadState(buf, len) ? DING_OK : DING_ERR_BAD_STATE;
}

DING_EXPORT size_t ding_diag_cpu_state(char* buf, size_t size) {
    if (!s_gb || !buf || size == 0) return 0;
    const Cpu& c = s_gb->cpu;
    std::snprintf(buf, size,
        "PC:%04X SP:%04X AF:%04X BC:%04X DE:%04X HL:%04X IME:%d HALT:%d",
        c.pc, c.sp, c.af(), c.bc(), c.de(), c.hl(), c.ime ? 1 : 0, c.halted ? 1 : 0);
    return std::strlen(buf);
}

DING_EXPORT size_t ding_diag_video_state(char* buf, size_t size) {
    if (!s_gb || !buf || size == 0) return 0;
    std::snprintf(buf, size,
        "LCDC:%02X STAT:%02X LY:%02X LYC:%02X SCX:%02X SCY:%02X",
        s_gb->bus.io[0x40], s_gb->bus.io[0x41], s_gb->bus.io[0x44],
        s_gb->bus.io[0x45], s_gb->bus.io[0x43], s_gb->bus.io[0x42]);
    return std::strlen(buf);
}

DING_EXPORT u8 ding_has_error() {
    return (s_gb && s_gb->errorFlag) ? 1 : 0;
}

DING_EXPORT const char* ding_diag_last_error() {
    return (s_gb && s_gb->errorFlag) ? s_gb->errorMsg : nullptr;
}

// ── SRAM helpers (frontend .sav import/export, mirrors genesis-core's) ──────
DING_EXPORT u8*  ding_get_sram()      { return s_gb ? s_gb->bus.eram.data() : nullptr; }
DING_EXPORT u32  ding_get_sram_size() { return s_gb ? (u32)s_gb->bus.eram.size() : 0u; }
DING_EXPORT u8   ding_sram_has()      { return hasSram() ? 1 : 0; }
DING_EXPORT u8   ding_sram_dirty()    { return (s_gb && s_gb->bus.sramDirty) ? 1 : 0; }
DING_EXPORT void ding_sram_clear_dirty() { if (s_gb) s_gb->bus.sramDirty = false; }
DING_EXPORT void ding_write8(u32 addr, u8 val) { if (s_gb) s_gb->bus.write8((u16)addr, val); }

// ── Game Boy Printer hook ────────────────────────────────────────────────────
// Printer output is event-driven (fires when a game sends a PRINT command),
// not a per-frame poll like video/audio, so it's exposed as a callback
// registration rather than a ding_core.h getter — ding_core.h stays generic
// across all platforms and doesn't know what a "printer" is. Host code
// (Hydra/Cockpit/WASM glue) calls this once after ding_init() to receive
// finished printouts, the same way gb-core.js's frontend set gb.printer.onImage.
DING_EXPORT void ding_gb_set_printer_callback(
    void (*callback)(const u8* pixels, u32 width, u32 height, void* userdata),
    void* userdata) {
    if (!s_gb) return;
    s_gb->bus.printer.onImage = callback;
    s_gb->bus.printer.onImageUserdata = userdata;
}

// ── DMG palette control ──────────────────────────────────────────────────────
// ding_core.h's framebuffer is already-colored RGBA8 — it has no generic
// concept of a swappable palette, since most platforms don't have one. The
// frontend's user-selectable DMG palettes (classic/pocket/amber/etc, see
// PALETTES in the HTML) are a Game-Boy-specific feature, so they're exposed
// as a small Peridot extension instead of forcing a generic API shape onto
// every other core. rgb12 is 4 consecutive {r,g,b} triples (shade 0..3).
DING_EXPORT void ding_gb_set_palette(const u8* rgb12) {
    if (!s_gb || !rgb12) return;
    GbColor colors[4];
    for (int i = 0; i < 4; i++) {
        colors[i] = { rgb12[i*3], rgb12[i*3+1], rgb12[i*3+2] };
    }
    s_gb->ppu.setPalette(colors);
}

// Re-renders the current PPU pixel buffer with whatever palette is set,
// without advancing emulation — matches gb-core.js's gb.ppu._blit(), used
// when the frontend swaps palettes while paused so the still frame updates
// immediately instead of waiting for the next runFrame().
DING_EXPORT void ding_gb_reblit() {
    if (s_gb) s_gb->ppu.blit();
}

// ── Register getters for fields ding_core.h has no generic concept of ──────
// (IF/IE are DMG-specific register quirks — IF in particular is a scalar
// separate from the IO byte array on real hardware, see Bus::read8's
// 0xFF0F special case — and MBC state is cartridge-mapper-specific.) These
// exist purely so the frontend's debug panel and dingBuildState()-style
// achievement memory reads can see the same values the JS core exposed.
DING_EXPORT u8  ding_gb_get_if()       { return s_gb ? s_gb->bus.ifReg : 0; }
DING_EXPORT u8  ding_gb_get_ie()       { return s_gb ? s_gb->bus.ie : 0; }
DING_EXPORT u8  ding_gb_get_mbc_type() { return s_gb ? (u8)s_gb->bus.mbcType : 0; }
DING_EXPORT u32 ding_gb_get_ram_bank() { return s_gb ? s_gb->bus.ramBank : 0; }
DING_EXPORT u32 ding_gb_get_rom_bank() { return s_gb ? s_gb->bus.romBank : 0; }
DING_EXPORT u32 ding_gb_get_rom_banks(){ return s_gb ? s_gb->bus.romBanks : 0; }
DING_EXPORT u8  ding_gb_get_ram_en()   { return (s_gb && s_gb->bus.ramEn) ? 1 : 0; }
DING_EXPORT u8  ding_gb_get_mbc1_mode(){ return s_gb ? s_gb->bus.mbc1Mode : 0; }

// Packs CPU registers into a fixed 12-slot u16 buffer for the debug panel:
// [a,f,b,c,d,e,h,l,sp,pc,ime,halted]. One call instead of ~12 round trips.
DING_EXPORT void ding_gb_get_cpu_regs(u16* out12) {
    if (!s_gb || !out12) return;
    const Cpu& c = s_gb->cpu;
    out12[0]=c.a; out12[1]=c.f; out12[2]=c.b; out12[3]=c.c;
    out12[4]=c.d; out12[5]=c.e; out12[6]=c.h; out12[7]=c.l;
    out12[8]=c.sp; out12[9]=c.pc; out12[10]=c.ime?1:0; out12[11]=c.halted?1:0;
}

// Packs the essential APU diagnostic fields the debug panel's APU tab reads.
// Layout (16 x u32): [masterOn, volL, volR, nr51,
//   ch1.on, ch1.dacOn, ch1.vol, ch1.freq, ch1.duty, ch1.len,
//   ch2.on, ch2.dacOn, ch2.vol, ch2.freq, ch2.duty, ch2.len]
// then a second call with a channel index (2 or 3) for ch3/ch4's fields.
// The JS core's per-hypothesis bug-instrumentation counters (envPeriod0BugHits
// etc.) were development scaffolding, not emulation behaviour — already
// dropped when apu.cpp was ported, so they're not in this diagnostic surface.
DING_EXPORT void ding_gb_get_apu_diag(u32* out, u32 channelPair) {
    if (!s_gb || !out) return;
    const Apu& a = s_gb->apu;
    u8 nr50 = s_gb->bus.io[0x24], nr51 = s_gb->bus.io[0x25], nr26 = s_gb->bus.io[0x26];
    out[0] = (nr26 & 0x80) ? 1 : 0;
    out[1] = ((nr50 >> 4) & 7) + 1;
    out[2] = (nr50 & 7) + 1;
    out[3] = nr51;
    const GbApuChannel* cA = (channelPair == 0) ? &a.ch1 : &a.ch3;
    const GbApuChannel* cB = (channelPair == 0) ? &a.ch2 : &a.ch4;
    u8 dutyA = (channelPair == 0) ? ((s_gb->bus.io[0x11] >> 6) & 3) : 0;
    u8 dutyB = (channelPair == 0) ? ((s_gb->bus.io[0x16] >> 6) & 3) : 0;
    out[4]=cA->on; out[5]=cA->dacOn; out[6]=cA->vol; out[7]=cA->freq; out[8]=dutyA; out[9]=cA->lenCounter;
    out[10]=cB->on; out[11]=cB->dacOn; out[12]=cB->vol; out[13]=cB->freq; out[14]=dutyB; out[15]=cB->lenCounter;
}

} // extern "C"
