// cpu.cpp — D!NG Peridot (Game Boy) CPU (SM83).
//
// Direct port of gb-core.js's CPU class. Opcode table, cycle counts, and flag
// behaviour are kept identical instruction-for-instruction — this file is a
// mechanical translation, not a rewrite, so any latent bug or fix already
// validated in the JS core carries over unchanged.
//
// Per DING_CORE_SDK_RULES.md: this file owns the primary CPU only. It talks
// to memory exclusively through Bus::read8/write8, never touching VRAM/OAM/
// IO internals directly.

#include "peridot.h"

Cpu::Cpu(Bus* bus) : bus_(bus) {}

// ── Memory / stack helpers ───────────────────────────────────────────────────
u8 Cpu::rb() { return bus_->read8(pc++); }
u16 Cpu::rw() { u8 lo = rb(); return (u16)(lo | (rb() << 8)); }

void Cpu::push(u16 v) {
    sp = (u16)(sp - 2);
    bus_->write8(sp, (u8)(v & 0xFF));
    bus_->write8((u16)(sp + 1), (u8)((v >> 8) & 0xFF));
}
u16 Cpu::pop() {
    u8 lo = bus_->read8(sp);
    u8 hi = bus_->read8((u16)(sp + 1));
    sp = (u16)(sp + 2);
    return (u16)(lo | (hi << 8));
}

// ── ALU ──────────────────────────────────────────────────────────────────────
void Cpu::ADD(u8 v) {
    u32 r = a + v;
    setNf(0); setHf(((a & 0xF) + (v & 0xF)) > 0xF ? 1 : 0);
    setCf(r > 0xFF ? 1 : 0);
    a = (u8)(r & 0xFF);
    setZf(!a ? 1 : 0);
}
void Cpu::ADC(u8 v) {
    u8 c = cf();
    u32 r = a + v + c;
    setNf(0); setHf(((a & 0xF) + (v & 0xF) + c) > 0xF ? 1 : 0);
    setCf(r > 0xFF ? 1 : 0);
    a = (u8)(r & 0xFF);
    setZf(!a ? 1 : 0);
}
void Cpu::SUB(u8 v) {
    setNf(1); setHf((a & 0xF) < (v & 0xF) ? 1 : 0);
    setCf(a < v ? 1 : 0);
    a = (u8)((a - v) & 0xFF);
    setZf(!a ? 1 : 0);
}
void Cpu::SBC(u8 v) {
    u8 c = cf();
    s32 r = (s32)a - v - c;
    setNf(1); setHf((a & 0xF) < ((v & 0xF) + c) ? 1 : 0);
    setCf(r < 0 ? 1 : 0);
    a = (u8)(r & 0xFF);
    setZf(!a ? 1 : 0);
}
void Cpu::AND(u8 v) { a &= v; setZf(!a ? 1 : 0); setNf(0); setHf(1); setCf(0); }
void Cpu::XOR(u8 v) { a ^= v; setZf(!a ? 1 : 0); setNf(0); setHf(0); setCf(0); }
void Cpu::OR(u8 v)  { a |= v; setZf(!a ? 1 : 0); setNf(0); setHf(0); setCf(0); }
void Cpu::CP(u8 v) {
    u8 r = (u8)(a - v);
    setNf(1); setHf((a & 0xF) < (v & 0xF) ? 1 : 0);
    setCf(a < v ? 1 : 0);
    setZf(!r ? 1 : 0);
}
u8 Cpu::INC(u8 v) {
    u8 r = (u8)(v + 1);
    setNf(0); setHf((v & 0xF) == 0xF ? 1 : 0); setZf(!r ? 1 : 0);
    return r;
}
u8 Cpu::DEC(u8 v) {
    u8 r = (u8)(v - 1);
    setNf(1); setHf((v & 0xF) == 0 ? 1 : 0); setZf(!r ? 1 : 0);
    return r;
}
void Cpu::ADDHL(u16 v) {
    u32 r = hl() + v;
    setNf(0); setHf(((hl() & 0xFFF) + (v & 0xFFF)) > 0xFFF ? 1 : 0);
    setCf(r > 0xFFFF ? 1 : 0);
    setHl((u16)(r & 0xFFFF));
}
u8 Cpu::RLC(u8 v) { u8 c = v >> 7; v = (u8)((v << 1) | c); setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
u8 Cpu::RRC(u8 v) { u8 c = v & 1;  v = (u8)((v >> 1) | (c << 7)); setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
u8 Cpu::RL(u8 v)  { u8 c = v >> 7; v = (u8)((v << 1) | cf());     setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
u8 Cpu::RR(u8 v)  { u8 c = v & 1;  v = (u8)((v >> 1) | (cf() << 7)); setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
u8 Cpu::SLA(u8 v) { u8 c = v >> 7; v = (u8)(v << 1); setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
u8 Cpu::SRA(u8 v) { u8 c = v & 1;  v = (u8)((v >> 1) | (v & 0x80)); setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
u8 Cpu::SWAP(u8 v) { v = (u8)((v << 4) | (v >> 4)); setZf(!v?1:0); setNf(0); setHf(0); setCf(0); return v; }
u8 Cpu::SRL(u8 v) { u8 c = v & 1; v = (u8)(v >> 1); setCf(c); setZf(!v?1:0); setNf(0); setHf(0); return v; }
void Cpu::testBit(u8 bit, u8 v) { setZf(((v >> bit) & 1) ? 0 : 1); setNf(0); setHf(1); }

// ── step() ───────────────────────────────────────────────────────────────────
u32 Cpu::step() {
    u8 pend = (u8)(bus_->ie & (bus_->ifReg & 0x1F));
    if (pend) {
        halted = false;
        if (ime) {
            ime = false;
            for (u8 i = 0; i < 5; i++) {
                if (pend & (1u << i)) {
                    bus_->ifReg &= (u8)~(1u << i);
                    push(pc);
                    pc = (u16)(0x40 + i * 8);
                    return 20;
                }
            }
        }
    }

    if (pendingIME) { pendingIME--; if (!pendingIME) ime = true; }
    if (halted) return 4;

    u8 op = bus_->read8(pc);
    if (haltBug) haltBug = false;
    else pc = (u16)(pc + 1);

    return exec(op);
}

// ── exec() — main opcode table ───────────────────────────────────────────────
u32 Cpu::exec(u8 op) {
    auto jr = [&](u8 e) {
        s16 s = (e > 127) ? (s16)(e - 256) : (s16)e;
        pc = (u16)(pc + s);
    };

    switch (op) {
        case 0x00: return 4;
        case 0x01: setBc(rw()); return 12;
        case 0x02: bus_->write8(bc(), a); return 8;
        case 0x03: setBc((u16)(bc() + 1)); return 8;
        case 0x04: b = INC(b); return 4;
        case 0x05: b = DEC(b); return 4;
        case 0x06: b = rb(); return 8;
        case 0x07: { u8 cc = a >> 7; a = (u8)((a << 1) | cc); setCf(cc); setZf(0); setNf(0); setHf(0); return 4; }
        case 0x08: { u16 addr = rw(); bus_->write8(addr, (u8)(sp & 0xFF)); bus_->write8((u16)(addr+1), (u8)(sp >> 8)); return 20; }
        case 0x09: ADDHL(bc()); return 8;
        case 0x0A: a = bus_->read8(bc()); return 8;
        case 0x0B: setBc((u16)(bc() - 1)); return 8;
        case 0x0C: c = INC(c); return 4;
        case 0x0D: c = DEC(c); return 4;
        case 0x0E: c = rb(); return 8;
        case 0x0F: { u8 cc = a & 1; a = (u8)((a >> 1) | (cc << 7)); setCf(cc); setZf(0); setNf(0); setHf(0); return 4; }
        case 0x10: rb(); return 4;
        case 0x11: setDe(rw()); return 12;
        case 0x12: bus_->write8(de(), a); return 8;
        case 0x13: setDe((u16)(de() + 1)); return 8;
        case 0x14: d = INC(d); return 4;
        case 0x15: d = DEC(d); return 4;
        case 0x16: d = rb(); return 8;
        case 0x17: { u8 cc = a >> 7; a = (u8)((a << 1) | cf()); setCf(cc); setZf(0); setNf(0); setHf(0); return 4; }
        case 0x18: jr(rb()); return 12;
        case 0x19: ADDHL(de()); return 8;
        case 0x1A: a = bus_->read8(de()); return 8;
        case 0x1B: setDe((u16)(de() - 1)); return 8;
        case 0x1C: e = INC(e); return 4;
        case 0x1D: e = DEC(e); return 4;
        case 0x1E: e = rb(); return 8;
        case 0x1F: { u8 cc = a & 1; a = (u8)((a >> 1) | (cf() << 7)); setCf(cc); setZf(0); setNf(0); setHf(0); return 4; }
        case 0x20: { u8 ee = rb(); if (!zf()) { jr(ee); return 12; } return 8; }
        case 0x21: setHl(rw()); return 12;
        case 0x22: bus_->write8(hl(), a); setHl((u16)(hl() + 1)); return 8;
        case 0x23: setHl((u16)(hl() + 1)); return 8;
        case 0x24: h = INC(h); return 4;
        case 0x25: h = DEC(h); return 4;
        case 0x26: h = rb(); return 8;
        case 0x27: {
            u16 aa = a;
            if (!nf()) {
                if (hf() || (aa & 0xF) > 9) aa += 6;
                if (cf() || aa > 0x9F) { aa += 0x60; setCf(1); }
            } else {
                if (hf()) aa -= 6;
                if (cf()) aa -= 0x60;
            }
            a = (u8)(aa & 0xFF);
            setZf(!a ? 1 : 0); setHf(0);
            return 4;
        }
        case 0x28: { u8 ee = rb(); if (zf()) { jr(ee); return 12; } return 8; }
        case 0x29: ADDHL(hl()); return 8;
        case 0x2A: a = bus_->read8(hl()); setHl((u16)(hl() + 1)); return 8;
        case 0x2B: setHl((u16)(hl() - 1)); return 8;
        case 0x2C: l = INC(l); return 4;
        case 0x2D: l = DEC(l); return 4;
        case 0x2E: l = rb(); return 8;
        case 0x2F: a = (u8)(~a & 0xFF); setNf(1); setHf(1); return 4;
        case 0x30: { u8 ee = rb(); if (!cf()) { jr(ee); return 12; } return 8; }
        case 0x31: sp = rw(); return 12;
        case 0x32: bus_->write8(hl(), a); setHl((u16)(hl() - 1)); return 8;
        case 0x33: sp = (u16)(sp + 1); return 8;
        case 0x34: { u8 v = INC(bus_->read8(hl())); bus_->write8(hl(), v); return 12; }
        case 0x35: { u8 v = DEC(bus_->read8(hl())); bus_->write8(hl(), v); return 12; }
        case 0x36: bus_->write8(hl(), rb()); return 12;
        case 0x37: setCf(1); setNf(0); setHf(0); return 4;
        case 0x38: { u8 ee = rb(); if (cf()) { jr(ee); return 12; } return 8; }
        case 0x39: ADDHL(sp); return 8;
        case 0x3A: a = bus_->read8(hl()); setHl((u16)(hl() - 1)); return 8;
        case 0x3B: sp = (u16)(sp - 1); return 8;
        case 0x3C: a = INC(a); return 4;
        case 0x3D: a = DEC(a); return 4;
        case 0x3E: a = rb(); return 8;
        case 0x3F: setCf(cf() ^ 1); setNf(0); setHf(0); return 4;

        case 0x40: return 4; case 0x41: b = c; return 4; case 0x42: b = d; return 4;
        case 0x43: b = e; return 4; case 0x44: b = h; return 4; case 0x45: b = l; return 4;
        case 0x46: b = bus_->read8(hl()); return 8; case 0x47: b = a; return 4;
        case 0x48: c = b; return 4; case 0x49: return 4; case 0x4A: c = d; return 4;
        case 0x4B: c = e; return 4; case 0x4C: c = h; return 4; case 0x4D: c = l; return 4;
        case 0x4E: c = bus_->read8(hl()); return 8; case 0x4F: c = a; return 4;
        case 0x50: d = b; return 4; case 0x51: d = c; return 4; case 0x52: return 4;
        case 0x53: d = e; return 4; case 0x54: d = h; return 4; case 0x55: d = l; return 4;
        case 0x56: d = bus_->read8(hl()); return 8; case 0x57: d = a; return 4;
        case 0x58: e = b; return 4; case 0x59: e = c; return 4; case 0x5A: e = d; return 4;
        case 0x5B: return 4; case 0x5C: e = h; return 4; case 0x5D: e = l; return 4;
        case 0x5E: e = bus_->read8(hl()); return 8; case 0x5F: e = a; return 4;
        case 0x60: h = b; return 4; case 0x61: h = c; return 4; case 0x62: h = d; return 4;
        case 0x63: h = e; return 4; case 0x64: return 4; case 0x65: h = l; return 4;
        case 0x66: h = bus_->read8(hl()); return 8; case 0x67: h = a; return 4;
        case 0x68: l = b; return 4; case 0x69: l = c; return 4; case 0x6A: l = d; return 4;
        case 0x6B: l = e; return 4; case 0x6C: l = h; return 4; case 0x6D: return 4;
        case 0x6E: l = bus_->read8(hl()); return 8; case 0x6F: l = a; return 4;
        case 0x70: bus_->write8(hl(), b); return 8; case 0x71: bus_->write8(hl(), c); return 8;
        case 0x72: bus_->write8(hl(), d); return 8; case 0x73: bus_->write8(hl(), e); return 8;
        case 0x74: bus_->write8(hl(), h); return 8; case 0x75: bus_->write8(hl(), l); return 8;
        case 0x76:
            if (!ime && (bus_->ie & bus_->ifReg & 0x1F)) haltBug = true;
            else halted = true;
            return 4;
        case 0x77: bus_->write8(hl(), a); return 8;
        case 0x78: a = b; return 4; case 0x79: a = c; return 4; case 0x7A: a = d; return 4;
        case 0x7B: a = e; return 4; case 0x7C: a = h; return 4; case 0x7D: a = l; return 4;
        case 0x7E: a = bus_->read8(hl()); return 8; case 0x7F: return 4;

        case 0x80: ADD(b); return 4; case 0x81: ADD(c); return 4;
        case 0x82: ADD(d); return 4; case 0x83: ADD(e); return 4;
        case 0x84: ADD(h); return 4; case 0x85: ADD(l); return 4;
        case 0x86: ADD(bus_->read8(hl())); return 8; case 0x87: ADD(a); return 4;
        case 0x88: ADC(b); return 4; case 0x89: ADC(c); return 4;
        case 0x8A: ADC(d); return 4; case 0x8B: ADC(e); return 4;
        case 0x8C: ADC(h); return 4; case 0x8D: ADC(l); return 4;
        case 0x8E: ADC(bus_->read8(hl())); return 8; case 0x8F: ADC(a); return 4;
        case 0x90: SUB(b); return 4; case 0x91: SUB(c); return 4;
        case 0x92: SUB(d); return 4; case 0x93: SUB(e); return 4;
        case 0x94: SUB(h); return 4; case 0x95: SUB(l); return 4;
        case 0x96: SUB(bus_->read8(hl())); return 8; case 0x97: SUB(a); return 4;
        case 0x98: SBC(b); return 4; case 0x99: SBC(c); return 4;
        case 0x9A: SBC(d); return 4; case 0x9B: SBC(e); return 4;
        case 0x9C: SBC(h); return 4; case 0x9D: SBC(l); return 4;
        case 0x9E: SBC(bus_->read8(hl())); return 8; case 0x9F: SBC(a); return 4;
        case 0xA0: AND(b); return 4; case 0xA1: AND(c); return 4;
        case 0xA2: AND(d); return 4; case 0xA3: AND(e); return 4;
        case 0xA4: AND(h); return 4; case 0xA5: AND(l); return 4;
        case 0xA6: AND(bus_->read8(hl())); return 8; case 0xA7: AND(a); return 4;
        case 0xA8: XOR(b); return 4; case 0xA9: XOR(c); return 4;
        case 0xAA: XOR(d); return 4; case 0xAB: XOR(e); return 4;
        case 0xAC: XOR(h); return 4; case 0xAD: XOR(l); return 4;
        case 0xAE: XOR(bus_->read8(hl())); return 8; case 0xAF: XOR(a); return 4;
        case 0xB0: OR(b); return 4; case 0xB1: OR(c); return 4;
        case 0xB2: OR(d); return 4; case 0xB3: OR(e); return 4;
        case 0xB4: OR(h); return 4; case 0xB5: OR(l); return 4;
        case 0xB6: OR(bus_->read8(hl())); return 8; case 0xB7: OR(a); return 4;
        case 0xB8: CP(b); return 4; case 0xB9: CP(c); return 4;
        case 0xBA: CP(d); return 4; case 0xBB: CP(e); return 4;
        case 0xBC: CP(h); return 4; case 0xBD: CP(l); return 4;
        case 0xBE: CP(bus_->read8(hl())); return 8; case 0xBF: CP(a); return 4;

        case 0xC0: if (!zf()) { pc = pop(); return 20; } return 8;
        case 0xC1: setBc(pop()); return 12;
        case 0xC2: { u16 aa = rw(); if (!zf()) { pc = aa; return 16; } return 12; }
        case 0xC3: pc = rw(); return 16;
        case 0xC4: { u16 aa = rw(); if (!zf()) { push(pc); pc = aa; return 24; } return 12; }
        case 0xC5: push(bc()); return 16;
        case 0xC6: ADD(rb()); return 8;
        case 0xC7: push(pc); pc = 0x00; return 16;
        case 0xC8: if (zf()) { pc = pop(); return 20; } return 8;
        case 0xC9: pc = pop(); return 16;
        case 0xCA: { u16 aa = rw(); if (zf()) { pc = aa; return 16; } return 12; }
        case 0xCB: return execCB();
        case 0xCC: { u16 aa = rw(); if (zf()) { push(pc); pc = aa; return 24; } return 12; }
        case 0xCD: { u16 aa = rw(); push(pc); pc = aa; return 24; }
        case 0xCE: ADC(rb()); return 8;
        case 0xCF: push(pc); pc = 0x08; return 16;
        case 0xD0: if (!cf()) { pc = pop(); return 20; } return 8;
        case 0xD1: setDe(pop()); return 12;
        case 0xD2: { u16 aa = rw(); if (!cf()) { pc = aa; return 16; } return 12; }
        case 0xD4: { u16 aa = rw(); if (!cf()) { push(pc); pc = aa; return 24; } return 12; }
        case 0xD5: push(de()); return 16;
        case 0xD6: SUB(rb()); return 8;
        case 0xD7: push(pc); pc = 0x10; return 16;
        case 0xD8: if (cf()) { pc = pop(); return 20; } return 8;
        case 0xD9: pc = pop(); ime = true; return 16;
        case 0xDA: { u16 aa = rw(); if (cf()) { pc = aa; return 16; } return 12; }
        case 0xDC: { u16 aa = rw(); if (cf()) { push(pc); pc = aa; return 24; } return 12; }
        case 0xDE: SBC(rb()); return 8;
        case 0xDF: push(pc); pc = 0x18; return 16;
        case 0xE0: bus_->write8((u16)(0xFF00 | rb()), a); return 12;
        case 0xE1: setHl(pop()); return 12;
        case 0xE2: bus_->write8((u16)(0xFF00 | c), a); return 8;
        case 0xE5: push(hl()); return 16;
        case 0xE6: AND(rb()); return 8;
        case 0xE7: push(pc); pc = 0x20; return 16;
        case 0xE8: {
            u8 ee = rb();
            s16 s = (ee > 127) ? (s16)(ee - 256) : (s16)ee;
            setHf(((sp & 0xF) + (ee & 0xF)) > 0xF ? 1 : 0);
            setCf(((sp & 0xFF) + (ee & 0xFF)) > 0xFF ? 1 : 0);
            sp = (u16)(sp + s);
            setZf(0); setNf(0);
            return 16;
        }
        case 0xE9: pc = hl(); return 4;
        case 0xEA: bus_->write8(rw(), a); return 16;
        case 0xEE: XOR(rb()); return 8;
        case 0xEF: push(pc); pc = 0x28; return 16;
        case 0xF0: a = bus_->read8((u16)(0xFF00 | rb())); return 12;
        case 0xF1: setAf(pop()); return 12;
        case 0xF2: a = bus_->read8((u16)(0xFF00 | c)); return 8;
        case 0xF3: ime = false; pendingIME = 0; return 4;
        case 0xF5: push(af()); return 16;
        case 0xF6: OR(rb()); return 8;
        case 0xF7: push(pc); pc = 0x30; return 16;
        case 0xF8: {
            u8 ee = rb();
            s16 s = (ee > 127) ? (s16)(ee - 256) : (s16)ee;
            setHf(((sp & 0xF) + (ee & 0xF)) > 0xF ? 1 : 0);
            setCf(((sp & 0xFF) + (ee & 0xFF)) > 0xFF ? 1 : 0);
            setHl((u16)(sp + s));
            setZf(0); setNf(0);
            return 12;
        }
        case 0xF9: sp = hl(); return 8;
        case 0xFA: a = bus_->read8(rw()); return 16;
        case 0xFB: pendingIME = 1; return 4;
        case 0xFE: CP(rb()); return 8;
        case 0xFF: push(pc); pc = 0x38; return 16;
        default: return 4;
    }
}

// ── execCB() — $CB-prefixed bit-manipulation opcodes ─────────────────────────
u32 Cpu::execCB() {
    u8 op = rb();
    u8 r = op & 7;
    u8 bit = (op >> 3) & 7;

    auto G = [&]() -> u8 {
        switch (r) {
            case 0: return b; case 1: return c; case 2: return d; case 3: return e;
            case 4: return h; case 5: return l; case 6: return bus_->read8(hl());
            default: return a;
        }
    };
    auto S = [&](u8 v) {
        switch (r) {
            case 0: b = v; break; case 1: c = v; break; case 2: d = v; break;
            case 3: e = v; break; case 4: h = v; break; case 5: l = v; break;
            case 6: bus_->write8(hl(), v); break; case 7: a = v; break;
        }
    };

    u32 base = (r == 6) ? 16 : 8;
    u8 v = G();

    if (op < 0x40) {
        u8 result;
        switch ((op >> 3) & 7) {
            case 0: result = RLC(v); break;
            case 1: result = RRC(v); break;
            case 2: result = RL(v);  break;
            case 3: result = RR(v);  break;
            case 4: result = SLA(v); break;
            case 5: result = SRA(v); break;
            case 6: result = SWAP(v); break;
            default: result = SRL(v); break;
        }
        S(result);
        return base;
    } else if (op < 0x80) {
        testBit(bit, v);
        return (r == 6) ? 12 : 8;
    } else if (op < 0xC0) {
        S(RES(bit, v));
        return base;
    } else {
        S(SET(bit, v));
        return base;
    }
}
