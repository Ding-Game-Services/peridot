// apu.cpp — D!NG Peridot (Game Boy) APU.
//
// Direct port of gb-core.js's APU class. All DSP behaviour — duty/envelope/
// sweep generation, wave/noise channels, the frame sequencer, mixing, and
// the capacitor high-pass filter — is kept identical, including the fixes
// already validated in the JS core (documented inline where they matter:
// envelope period=0 doesn't step, sweep period=0 overflow-check quirk,
// duty phase reset on trigger, etc).
//
// NOT ported: initAudio()/stopAudio() and the ScriptProcessor ring buffer.
// Those are Web Audio (browser) calls — a host/frontend concern per
// DING_CORE_SDK_RULES.md. Finished samples go into a DingAudioBuffer
// instead; the frontend pulls them via ding_core.h's ding_read_audio_samples(),
// same as every other Ding core.
//
// The JS file also carried a large set of one-shot debug counters/log hooks
// used while diagnosing specific hardware-accuracy bugs (env0BugLogCount,
// duty50MismatchLogged, etc). Those were development scaffolding, not
// emulation behaviour, and are not ported — the *fixes* they were built to
// verify are all still here.

#include "peridot.h"
#include <cstring>

// Canonical DMG 8-step duty patterns (duty position increments 0..7):
// 0: 00000001 (12.5%), 1: 00000011 (25%), 2: 00001111 (50%), 3: 11111100 (75%)
static const u8 DUTY_WAVES[4][8] = {
    {0,0,0,0,0,0,0,1},
    {0,0,0,0,0,0,1,1},
    {0,0,0,0,1,1,1,1},
    {1,1,1,1,1,1,0,0},
};

Apu::Apu(Bus* bus) : bus_(bus) {
    ding_audio_init(&audioBuf, storage_, GB_AUDIO_CAPACITY, GB_AUDIO_CHANNELS, GB_SAMPLE_RATE);
}

void Apu::reset() {
    ding_audio_reset(&audioBuf);
    hpL_ = hpR_ = hpPrevL_ = hpPrevR_ = 0;
    fsTimer = 0; fsStep = 0;
    cycleBuf_ = 0.0;
    ch1 = GbApuChannel{};
    ch2 = GbApuChannel{};
    ch3 = GbApuChannel{};
    ch4 = GbApuChannel{};
}

void Apu::triggerCh1() {
    GbApuChannel& c = ch1;
    c.on = true;
    // Duty phase must reset on trigger; otherwise short iconic sounds can
    // start mid-wave and sound wrong.
    c.dutyStep = 0;
    c.freq = ch1Freq();
    c.freqTimer = (s32)((2048 - c.freq) * 4);
    if (!c.lenCounter) c.lenCounter = 64;
    c.volInit = (r(0x12) >> 4) & 0xF;
    c.volDir = (r(0x12) >> 3) & 1;
    c.volPeriod = r(0x12) & 7;
    c.vol = c.volInit;
    c.volTimer = c.volPeriod ? c.volPeriod : 8;
    c.dacOn = (r(0x12) & 0xF8) != 0;
    if (!c.dacOn) c.on = false;

    c.sweepPeriod = (r(0x10) >> 4) & 7;
    c.sweepDir = (r(0x10) >> 3) & 1;
    c.sweepShift = r(0x10) & 7;
    c.shadowFreq = c.freq;
    c.sweepTimer = c.sweepPeriod ? c.sweepPeriod : 8;
    c.sweepEnable = c.sweepPeriod > 0 || c.sweepShift > 0;

    if (c.sweepShift) {
        u32 delta = c.shadowFreq >> c.sweepShift;
        s32 nf = c.sweepDir ? (s32)c.shadowFreq - (s32)delta : (s32)c.shadowFreq + (s32)delta;
        if (nf > 2047) c.on = false;
    }
}

void Apu::triggerCh2() {
    GbApuChannel& c = ch2;
    c.on = true;
    c.freq = ch2Freq();
    c.freqTimer = (s32)((2048 - c.freq) * 4);
    if (!c.lenCounter) c.lenCounter = 64;
    c.volInit = (r(0x17) >> 4) & 0xF;
    c.volDir = (r(0x17) >> 3) & 1;
    c.volPeriod = r(0x17) & 7;
    c.vol = c.volInit;
    c.volTimer = c.volPeriod ? c.volPeriod : 8;
    c.dacOn = (r(0x17) & 0xF8) != 0;
    if (!c.dacOn) c.on = false;
}

void Apu::triggerCh3() {
    GbApuChannel& c = ch3;
    c.on = true;
    c.wavePos = 0;
    c.freq = ch3Freq();
    // (2048-freq)*2 — matches JS's fixed formula (was previously freq*2, wrong).
    c.freqTimer = (s32)((2048 - c.freq) * 2);
    if (!c.lenCounter) c.lenCounter = 256;
    c.dacOn = !!(r(0x1A) & 0x80);
    if (!c.dacOn) c.on = false;
}

void Apu::triggerCh4() {
    GbApuChannel& c = ch4;
    c.on = true;
    c.lfsr = 0x7FFF;
    if (!c.lenCounter) c.lenCounter = 64;
    u8 nr43 = r(0x22);
    u32 ratio4 = nr43 & 7;
    u32 shift4 = (nr43 >> 4) & 0xF;
    u32 div4 = ratio4 == 0 ? 8 : ratio4 * 16;
    // Reset freqTimer like CH1/CH2/CH3 do on their triggers — without this
    // the LFSR clocks at the wrong rate after a re-trigger.
    c.freqTimer = (s32)((div4 << shift4) ? (div4 << shift4) : 8192);
    c.narrowMode = !!(nr43 & 8);
    c.volInit = (r(0x21) >> 4) & 0xF;
    c.volDir = (r(0x21) >> 3) & 1;
    c.volPeriod = r(0x21) & 7;
    c.vol = c.volInit;
    c.volTimer = c.volPeriod ? c.volPeriod : 8;
    c.dacOn = (r(0x21) & 0xF8) != 0;
    if (!c.dacOn) c.on = false;
}

void Apu::write(u16 addr, u8 val) {
    // NR52: APU power toggle — must be handled before the power-off guard below.
    if (addr == 0xFF26) {
        if (!(val & 0x80)) {
            for (u16 i = 0x10; i <= 0x25; i++) bus_->io[i] = 0;
            ch1.on = ch2.on = ch3.on = ch4.on = false;
            ch1.dacOn = ch2.dacOn = ch3.dacOn = ch4.dacOn = false;
            hpL_ = hpR_ = hpPrevL_ = hpPrevR_ = 0;
        }
        return;
    }
    // All other sound register writes are ignored while the APU is powered off.
    if (!(bus_->io[0x26] & 0x80)) return;

    if (addr == 0xFF11) ch1.lenCounter = 64 - (val & 0x3F);
    if (addr == 0xFF16) ch2.lenCounter = 64 - (val & 0x3F);
    if (addr == 0xFF1B) ch3.lenCounter = 256 - val;
    if (addr == 0xFF20) ch4.lenCounter = 64 - (val & 0x3F);

    if (addr == 0xFF12) { ch1.dacOn = (val & 0xF8) != 0; if (!ch1.dacOn) ch1.on = false; }
    if (addr == 0xFF17) { ch2.dacOn = (val & 0xF8) != 0; if (!ch2.dacOn) ch2.on = false; }
    if (addr == 0xFF1A) { ch3.dacOn = !!(val & 0x80);    if (!ch3.dacOn) ch3.on = false; }
    if (addr == 0xFF21) { ch4.dacOn = (val & 0xF8) != 0; if (!ch4.dacOn) ch4.on = false; }

    if (addr == 0xFF14) { ch1.lenEnable = !!(val & 0x40); if (val & 0x80) triggerCh1(); }
    if (addr == 0xFF19) { ch2.lenEnable = !!(val & 0x40); if (val & 0x80) triggerCh2(); }
    if (addr == 0xFF1E) { ch3.lenEnable = !!(val & 0x40); if (val & 0x80) triggerCh3(); }
    if (addr == 0xFF23) { ch4.lenEnable = !!(val & 0x40); if (val & 0x80) triggerCh4(); }
}

void Apu::clockLen(GbApuChannel& c) {
    if (c.lenEnable && c.lenCounter > 0) {
        c.lenCounter--;
        if (!c.lenCounter) c.on = false;
    }
}

void Apu::clockSweep() {
    GbApuChannel& c = ch1;
    if (!c.sweepEnable) return;
    if (c.sweepTimer > 0) c.sweepTimer--;
    if (c.sweepTimer <= 0) {
        c.sweepTimer = c.sweepPeriod ? c.sweepPeriod : 8;
        if (c.sweepPeriod > 0) {
            u32 delta = c.shadowFreq >> c.sweepShift;
            s32 nf = c.sweepDir ? (s32)c.shadowFreq - (s32)delta : (s32)c.shadowFreq + (s32)delta;
            if (nf > 2047) { c.on = false; return; }
            if (c.sweepShift > 0) {
                c.shadowFreq = (u32)nf;
                c.freq = (u32)nf;
                bus_->io[0x13] = (u8)(nf & 0xFF);
                bus_->io[0x14] = (u8)((bus_->io[0x14] & 0xF8) | ((nf >> 8) & 0x07));
                // Second overflow check — only valid when sweepShift > 0.
                // When shift=0, nf>>0 = nf, making nf2 = 2*nf which would
                // always overflow, so it's gated behind the shift>0 branch.
                s32 nf2 = c.sweepDir ? nf - (nf >> c.sweepShift) : nf + (nf >> c.sweepShift);
                if (nf2 > 2047) c.on = false;
            }
        }
    }
}

void Apu::clockVol(GbApuChannel& c) {
    // Per DMG hardware: period=0 means the envelope timer does not step.
    // Volume stays constant at volInit. Do NOT treat period=0 as period=8.
    if (!c.volPeriod) return;
    if (c.volTimer > 0) c.volTimer--;
    if (c.volTimer <= 0) {
        c.volTimer = c.volPeriod;
        if (c.volDir && c.vol < 15) c.vol++;
        else if (!c.volDir && c.vol > 0) c.vol--;
    }
}

void Apu::stepFS() {
    u32 s = fsStep;
    if (s % 2 == 0) { clockLen(ch1); clockLen(ch2); clockLen(ch3); clockLen(ch4); }
    if (s == 2 || s == 6) clockSweep();
    if (s == 7) { clockVol(ch1); clockVol(ch2); clockVol(ch4); }
    fsStep = (fsStep + 1) & 7;
}

void Apu::step(u32 cycles) {
    if (!(r(0x26) & 0x80)) {
        ch1.on = ch2.on = ch3.on = ch4.on = false;
        return;
    }

    fsTimer += cycles;
    while (fsTimer >= 8192) { fsTimer -= 8192; stepFS(); }

    // ── CH1 (square + sweep) ──
    if (ch1.on) {
        ch1.freqTimer -= (s32)cycles;
        while (ch1.freqTimer <= 0) {
            ch1.freq = ch1Freq();
            s32 add = (s32)((2048 - ch1.freq) * 4);
            ch1.freqTimer += add ? add : 8192;
            ch1.dutyStep = (ch1.dutyStep + 1) & 7;
        }
        u32 duty = (r(0x11) >> 6) & 3;
        ch1.output = DUTY_WAVES[duty][ch1.dutyStep] ? ch1.vol : 0;
    } else { ch1.output = 0; }

    // ── CH2 (square) ──
    if (ch2.on) {
        ch2.freqTimer -= (s32)cycles;
        while (ch2.freqTimer <= 0) {
            ch2.freq = ch2Freq();
            s32 add = (s32)((2048 - ch2.freq) * 4);
            ch2.freqTimer += add ? add : 8192;
            ch2.dutyStep = (ch2.dutyStep + 1) & 7;
        }
        u32 duty2 = (r(0x16) >> 6) & 3;
        ch2.output = DUTY_WAVES[duty2][ch2.dutyStep] ? ch2.vol : 0;
    } else { ch2.output = 0; }

    // ── CH3 (wave) ──
    if (ch3.on && ch3.dacOn) {
        ch3.freqTimer -= (s32)cycles;
        while (ch3.freqTimer <= 0) {
            ch3.freq = ch3Freq();
            s32 add = (s32)((2048 - ch3.freq) * 2);
            ch3.freqTimer += add ? add : 8192;
            ch3.wavePos = (ch3.wavePos + 1) & 31;
        }
        u32 volCode = (r(0x1C) >> 5) & 3;
        static const u32 shiftTable[4] = {4, 0, 1, 2};
        u32 shift = shiftTable[volCode];
        u8 wByte = bus_->io[0x30 + (ch3.wavePos >> 1)];
        u8 nibble = (ch3.wavePos & 1) ? (wByte & 0xF) : ((wByte >> 4) & 0xF);
        ch3.output = nibble >> shift;
    } else { ch3.output = 0; }

    // ── CH4 (noise) ──
    if (ch4.on) {
        ch4.freqTimer -= (s32)cycles;
        u8 nr43 = r(0x22);
        u32 ratio = nr43 & 7;
        u32 shift = (nr43 >> 4) & 0xF;
        if (shift < 14) {
            u32 div = ratio == 0 ? 8 : ratio * 16;
            s32 inc4 = (s32)(div << shift);
            while (ch4.freqTimer <= 0) {
                ch4.freqTimer += inc4;
                u16 xorBit = (u16)((ch4.lfsr & 1) ^ ((ch4.lfsr >> 1) & 1));
                ch4.lfsr = (u16)((ch4.lfsr >> 1) | (xorBit << 14));
                if (ch4.narrowMode) {
                    ch4.lfsr &= (u16)~(1 << 6);
                    ch4.lfsr |= (u16)(xorBit << 6);
                }
            }
            ch4.output = (ch4.lfsr & 1) ? 0 : ch4.vol;
        } else {
            // shift >= 14: undefined hardware behaviour -> explicit silence.
            // Clamp the timer so a later normal trigger doesn't inherit a
            // deeply-negative value and spin the LFSR thousands of times.
            ch4.freqTimer = 8192;
            ch4.output = 0;
        }
    } else { ch4.output = 0; }

    // ── Mix + sample generation ──
    cycleBuf_ += cycles;
    while (cycleBuf_ >= cyclesPerSample_) {
        cycleBuf_ -= cyclesPerSample_;

        u8 nr50 = r(0x24), nr51 = r(0x25);
        u32 volL = ((nr50 >> 4) & 7) + 1;
        u32 volR = (nr50 & 7) + 1;

        u32 mixL = 0, mixR = 0;
        if (nr51 & 0x10) mixL += ch1.output;
        if (nr51 & 0x01) mixR += ch1.output;
        if (nr51 & 0x20) mixL += ch2.output;
        if (nr51 & 0x02) mixR += ch2.output;
        if (nr51 & 0x40) mixL += ch3.output;
        if (nr51 & 0x04) mixR += ch3.output;
        if (nr51 & 0x80) mixL += ch4.output;
        if (nr51 & 0x08) mixR += ch4.output;

        // Scale [0,60] -> [0,1], apply master volume. No DC-offset subtraction
        // here — the high-pass filter below handles that, same as the real
        // hardware's output capacitor.
        double rawL = ((double)mixL / 60.0) * ((double)volL / 8.0);
        double rawR = ((double)mixR / 60.0) * ((double)volR / 8.0);

        // Single-pole high-pass filter (alpha ~= 0.9943 ~= 40 Hz corner at
        // 44100 Hz). Removes DC bias from the DAC output.
        hpL_ = HP_ALPHA * (hpL_ + rawL - hpPrevL_);
        hpR_ = HP_ALPHA * (hpR_ + rawR - hpPrevR_);
        hpPrevL_ = rawL;
        hpPrevR_ = rawR;

        float frame[2] = { (float)(hpL_ * 0.95), (float)(hpR_ * 0.95) };
        ding_audio_write_sample(&audioBuf, frame);
    }

    // Update NR52 channel-status bits (0-3) — some games poll these.
    bus_->io[0x26] = (u8)((bus_->io[0x26] & 0x80)
        | (ch1.on ? 0x01 : 0) | (ch2.on ? 0x02 : 0)
        | (ch3.on ? 0x04 : 0) | (ch4.on ? 0x08 : 0));
}
