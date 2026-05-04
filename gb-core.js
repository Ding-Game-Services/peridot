// gb-core.js — D!NG Game Boy Emulator Core
// Version 1.0.0
//
// Pure emulation logic: MMU, Timer, PPU, CPU, APU, GameBoy.
// Zero DOM references — safe to load before any HTML element exists.
// Web Audio API is used by APU.initAudio() / stopAudio() for sample output;
// those are the only browser-API calls and they are intentionally here since
// audio output is intrinsic to the hardware being emulated.
//
// Frontend contract (what the HTML file must provide / call):
//   new GameBoy()            — create system
//   gb.loadROM(arrayBuffer)  — load cart
//   gb.setPalette(colors)    — set 4-colour DMG palette
//   gb.pressButton(btn, down)— forward input (btn 0-7)
//   gb.runFrame()            — advance one frame, returns framebuf
//   gb.apu.initAudio()       — start Web Audio (call after user gesture)
//   gb.apu.stopAudio()       — tear down Web Audio
//   gb.saveState()           — serialisable state snapshot
//   gb.loadState(s)          — restore from snapshot
//   dingBuildState(gb)       — flat address→value map for D!NG engine
//   md5(arrayBuffer)         — uppercase hex MD5 of ROM bytes
//   romHeaderTitle(buf)      — null-terminated title from header bytes
//
// Button index map (matches JOYPAD register bit positions):
//   0=Right  1=Left  2=Up  3=Down  4=A  5=B  6=Select  7=Start

'use strict';

// ═══════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════
const GB_W = 160, GB_H = 144, GB_SCALE = 3;
const GB_CYCLES_PER_FRAME = 70224;

const PALETTES = {
  classic: { label: 'DMG',    colors: [[155,188,15],[139,172,15],[48,98,48],[15,56,15]] },
  pocket:  { label: 'Pocket', colors: [[200,200,168],[160,160,120],[88,88,56],[24,24,8]] },
  amber:   { label: 'Amber',  colors: [[255,198,80],[200,130,20],[120,60,0],[40,15,0]] },
  kirby:   { label: 'Kirby',  colors: [[255,200,220],[240,120,160],[180,40,100],[80,0,40]] },
};

// ═══════════════════════════════════════════════
class MMU {
  constructor() {
    this.vram  = new Uint8Array(0x2000);
    this.wram  = new Uint8Array(0x2000);
    this.oam   = new Uint8Array(0xA0);
    this.hram  = new Uint8Array(0x7F);
    this.io    = new Uint8Array(0x80);
    this.ie    = 0;
    this.ifReg = 0xE0;
    this.fullRom   = null;
    this.mbcType   = 0;
    this.romBanks  = 2;
    this.romBank   = 1;
    this.ramBank   = 0;
    this.ramEn     = false;
    this.eram      = new Uint8Array(0x20000);
    this.romLo     = new Uint8Array(0x4000);
    this.romHi     = new Uint8Array(0x4000);
    this.mbc1Mode   = 0;
    this.mbc1HiBits = 0;
    this.romLoBank  = 0;
    this.rtcRegs      = new Uint8Array(5);
    this.rtcLatched   = new Uint8Array(5);
    this.rtcLatchStep = 0;
    this.rtcSel       = -1;
    this.rtcBase      = Date.now();
    this.buttons = 0xFF;
    this.serialCycles = 0;
    this.apu = null;
    this.timer = null;   // wired up by GameBoy constructor after Timer is created
    this.sramDirty = false;
    this._initIO();
  }
  _initIO() {
    this.io[0x04]=0x1E; this.io[0x40]=0x91;
    this.io[0x41]=0x82; this.io[0x47]=0xFC;
    this.io[0x48]=0xFF; this.io[0x49]=0xFF;
    this.io[0x01]=0x00; this.io[0x02]=0x7E;
    this.ifReg=0xE1;
  }
  loadROM(buf) {
    this.fullRom = new Uint8Array(buf);
    const type   = this.fullRom[0x147];
    const sz     = this.fullRom[0x148];
    const ramSz  = this.fullRom[0x149];
    this.romBanks = sz===0 ? 2 : (2<<sz);
    if      (type===0)              this.mbcType=0;
    else if (type<=0x03)            this.mbcType=1;
    else if (type<=0x06)            this.mbcType=2;
    else if (type>=0x0F&&type<=0x13) this.mbcType=3;
    else if (type>=0x19&&type<=0x1E) this.mbcType=5;
    else                            this.mbcType=1;
    const ramBytes=[0,0x800,0x2000,0x8000,0x20000,0x10000][ramSz]||0x8000;
    this.eram=new Uint8Array(Math.max(ramBytes,0x8000));
    this.romBank=1; this.ramBank=0; this.ramEn=false;
    this.mbc1Mode=0; this.mbc1HiBits=0; this.romLoBank=0;
    this.rtcSel=-1; this.rtcLatchStep=0; this.rtcBase=Date.now();
    for(let i=0;i<0x4000&&i<this.fullRom.length;i++) this.romLo[i]=this.fullRom[i];
    this._bankHi(1);
  }
  _bankLo(bank) {
    bank=bank%this.romBanks; this.romLoBank=bank;
    const off=bank*0x4000;
    for(let i=0;i<0x4000;i++) this.romLo[i]=(off+i<this.fullRom.length)?this.fullRom[off+i]:0xFF;
  }
  _bankHi(bank) {
    if(this.mbcType!==3) bank=Math.max(1,bank%this.romBanks);
    else                 bank=Math.max(1,bank&0x7F)%this.romBanks;
    this.romBank=bank;
    const off=bank*0x4000;
    for(let i=0;i<0x4000;i++) this.romHi[i]=(off+i<this.fullRom.length)?this.fullRom[off+i]:0xFF;
  }
  _updateRTC() {
    const elapsed=Math.floor((Date.now()-this.rtcBase)/1000);
    this.rtcRegs[0]=elapsed%60;
    this.rtcRegs[1]=Math.floor(elapsed/60)%60;
    this.rtcRegs[2]=Math.floor(elapsed/3600)%24;
    const d=Math.floor(elapsed/86400);
    this.rtcRegs[3]=d&0xFF;
    this.rtcRegs[4]=(this.rtcRegs[4]&0xFE)|((d>>8)&1);
  }
  stepSerial(cycles) {
    if(!this.serialCycles) return;
    this.serialCycles-=cycles;
    if(this.serialCycles<=0) {
      this.serialCycles=0; this.io[0x01]=0xFF;
      this.io[0x02]&=0x7F; this.requestInterrupt(3);
    }
  }
  requestInterrupt(n) { this.ifReg|=(1<<n); }
  read(addr) {
    addr&=0xFFFF;
    if(addr<0x4000) return this.fullRom?this.romLo[addr]:0xFF;
    if(addr<0x8000) return this.fullRom?this.romHi[addr-0x4000]:0xFF;
    if(addr<0xA000) return this.vram[addr-0x8000];
    if(addr<0xC000) {
      if(!this.ramEn) return 0xFF;
      if(this.mbcType===3&&this.rtcSel>=0) return this.rtcLatched[this.rtcSel];
      if(this.mbcType===2) { const idx=(addr-0xA000)&0x01FF; return 0xF0|(this.eram[idx]&0x0F); }
      return this.eram[this.ramBank*0x2000+addr-0xA000];
    }
    if(addr<0xE000) return this.wram[addr-0xC000];
    if(addr<0xFE00) return this.wram[addr-0xE000];
    if(addr<0xFEA0) return this.oam[addr-0xFE00];
    if(addr<0xFF00) return 0xFF;
    if(addr===0xFF00) return this._readJoy();
    if(addr===0xFF0F) return this.ifReg;
    if(addr<0xFF80) return this.io[addr-0xFF00];
    if(addr<0xFFFF) return this.hram[addr-0xFF80];
    return this.ie;
  }
  write(addr,val) {
    addr&=0xFFFF; val&=0xFF;
    if(addr<0x8000) {
      switch(this.mbcType) {
        case 1:
          if(addr<0x2000){this.ramEn=(val&0xF)===0xA;}
          else if(addr<0x4000){let lo=val&0x1F;if(!lo)lo=1;if(this.mbc1Mode===0)this._bankHi((this.mbc1HiBits<<5)|lo);else this._bankHi(lo);}
          else if(addr<0x6000){this.mbc1HiBits=val&3;if(this.mbc1Mode===0){this._bankHi((this.mbc1HiBits<<5)|(this.romBank&0x1F));}else{this.ramBank=this.mbc1HiBits;this._bankLo(this.mbc1HiBits<<5);}}
          else{this.mbc1Mode=val&1;if(this.mbc1Mode===0){this.ramBank=0;this._bankLo(0);this._bankHi((this.mbc1HiBits<<5)|(this.romBank&0x1F));}else{this._bankLo(this.mbc1HiBits<<5);this._bankHi(this.romBank&0x1F);}}
          break;
        case 3:
          if(addr<0x2000){this.ramEn=(val&0xF)===0xA;}
          else if(addr<0x4000){this._bankHi(val&0x7F);}
          else if(addr<0x6000){if(val<=0x03){this.rtcSel=-1;this.ramBank=val;}else if(val>=0x08&&val<=0x0C){this.rtcSel=val-0x08;}}
          else{if(val===0x00)this.rtcLatchStep=1;else if(val===0x01&&this.rtcLatchStep===1){this._updateRTC();this.rtcLatched.set(this.rtcRegs);this.rtcLatchStep=0;}else{this.rtcLatchStep=0;}}
          break;
        case 2:
          if(addr<0x4000){if(addr&0x0100){let b=val&0x0F;if(!b)b=1;this._bankHi(b);}else{this.ramEn=(val&0x0F)===0x0A;}}
          break;
        case 5:
          if(addr<0x2000){this.ramEn=(val&0xF)===0xA;}
          else if(addr<0x3000){this._bankHi((this.romBank&0x100)|val);}
          else if(addr<0x4000){this._bankHi((this.romBank&0xFF)|((val&1)<<8));}
          else if(addr<0x6000){this.ramBank=val&0x0F;}
          break;
        default: break;
      }
      return;
    }
    if(addr<0xA000){this.vram[addr-0x8000]=val;return;}
    if(addr<0xC000){
      if(!this.ramEn) return;
      if(this.mbcType===3&&this.rtcSel>=0){this.rtcRegs[this.rtcSel]=val;return;}
      if(this.mbcType===2){this.eram[(addr-0xA000)&0x01FF]=val&0x0F;this.sramDirty=true;return;}
      this.eram[this.ramBank*0x2000+addr-0xA000]=val;this.sramDirty=true;return;
    }
    if(addr<0xE000){this.wram[addr-0xC000]=val;return;}
    if(addr<0xFE00){this.wram[addr-0xE000]=val;return;}
    if(addr<0xFEA0){this.oam[addr-0xFE00]=val;return;}
    if(addr<0xFF00) return;
    if(addr===0xFF00){this.io[0]=val;return;}
    if(addr===0xFF02){this.io[0x02]=val;if((val&0x81)===0x81)this.serialCycles=8192;return;}
    if(addr===0xFF04){
      // Reset the internal 16-bit div counter (not just the readable register).
      // Also reset APU frame sequencer — it's driven by the same clock.
      if(this.timer) this.timer.div=0;
      if(this.apu)   this.apu.fsTimer=0;
      this.io[0x04]=0;
      return;
    }
    if(addr===0xFF0F){this.ifReg=(val&0x1F)|0xE0;return;}
    if(addr===0xFF46){const s=val<<8;for(let i=0;i<0xA0;i++)this.oam[i]=this.read(s+i);return;}
    if(addr<0xFF80){
      // IMPORTANT: write IO first, then notify APU.
      // APU trigger handlers read back NR1x values from mmu.io; calling APU before
      // updating io can make triggers compute frequency from stale values.
      this.io[addr-0xFF00]=val;
      if(addr>=0xFF10&&addr<=0xFF3F&&this.apu) this.apu.write(addr,val);
      return;
    }
    if(addr<0xFFFF){this.hram[addr-0xFF80]=val;return;}
    this.ie=val;
  }
  _readJoy() {
    const sel=this.io[0];let lo=0xF;
    if(!(sel&0x10))lo&=this.buttons&0xF;
    if(!(sel&0x20))lo&=(this.buttons>>4)&0xF;
    return 0xC0|(sel&0x30)|lo;
  }
  pressButton(btn,down) {
    if(down){
      this.buttons&=~(1<<btn);
      const sel=this.io[0],isDir=btn<4;
      const dirSel=!(sel&0x10),actSel=!(sel&0x20);
      if((isDir&&dirSel)||(!isDir&&actSel)) this.requestInterrupt(4);
    } else {
      this.buttons|=(1<<btn);
    }
  }
}

// ═══════════════════════════════════════════════
// TIMER
// ═══════════════════════════════════════════════
class Timer {
  constructor(mmu){this.mmu=mmu;this.div=0;}
  step(cycles){
    const prev=this.div;
    this.div=(this.div+cycles)&0xFFFF;
    this.mmu.io[0x04]=(this.div>>8)&0xFF;
    if(!(this.mmu.io[0x07]&4))return;
    const freqs=[512,8,32,128];
    const bit=freqs[this.mmu.io[0x07]&3];
    if((prev&bit)&&!(this.div&bit)){
      let tima=(this.mmu.io[0x05]+1)&0xFF;
      if(!tima){tima=this.mmu.io[0x06];this.mmu.requestInterrupt(2);}
      this.mmu.io[0x05]=tima;
    }
  }
}

// ═══════════════════════════════════════════════
// PPU
// ═══════════════════════════════════════════════
class PPU {
  constructor(mmu){
    this.mmu=mmu; this.pixels=new Uint8Array(GB_W*GB_H);
    this.framebuf=new Uint8ClampedArray(GB_W*GB_H*4);
    this.cycles=0; this.mode=2; this.winLine=0;
    this.frameReady=false; this.palette=PALETTES.classic.colors;
    this.lcdWasOff=false;
  }
  get lcdc(){return this.mmu.io[0x40];}
  get ly()  {return this.mmu.io[0x44];} set ly(v){this.mmu.io[0x44]=v;}
  get lyc() {return this.mmu.io[0x45];}
  get scy() {return this.mmu.io[0x42];}
  get scx() {return this.mmu.io[0x43];}
  get bgp() {return this.mmu.io[0x47];}
  get obp0(){return this.mmu.io[0x48];}
  get obp1(){return this.mmu.io[0x49];}
  get wy()  {return this.mmu.io[0x4A];}
  get wx()  {return this.mmu.io[0x4B]-7;}
  step(cycles){
    this.frameReady=false;
if(!(this.lcdc&0x80)){
  if(this.ly!==0||this.mode!==0||!this.lcdWasOff){
    this.ly=0;this.cycles=0;this.mode=0;this.winLine=0;
    this.mmu.io[0x41]=(this.mmu.io[0x41]&~3);
    // Real HW: LCD off = white screen. Fill framebuf with palette[0] and blit.
    this.pixels.fill(0);
    this._blit();
    this.frameReady=true;
    this.lcdWasOff=true;
  }
  return;
}
    if(this.lcdWasOff){this.lcdWasOff=false;this.ly=0;this.cycles=0;this.winLine=0;this._setMode(2);this._checkLYC();}
    this.cycles+=cycles;
    if(this.mode===2&&this.cycles>=80) {this.cycles-=80;this._setMode(3);}
    if(this.mode===3&&this.cycles>=172){this.cycles-=172;this._drawLine();this._setMode(0);}
    if(this.mode===0&&this.cycles>=204){
      this.cycles-=204;this.ly++;this._checkLYC();
      if(this.ly===144){this._setMode(1);this.mmu.requestInterrupt(0);this._blit();this.frameReady=true;this.winLine=0;}
      else{this._setMode(2);}
    }
    if(this.mode===1&&this.cycles>=456){
      this.cycles-=456;this.ly++;this._checkLYC();
      if(this.ly>153){this.ly=0;this.winLine=0;this._checkLYC();this._setMode(2);}
    }
  }
  _setMode(m){
    this.mode=m;let stat=(this.mmu.io[0x41]&~3)|m;
    const irq=[3,4,5,-1];
    if(irq[m]>=0&&(stat&(1<<irq[m])))this.mmu.requestInterrupt(1);
    this.mmu.io[0x41]=stat;
  }
  _checkLYC(){
    let stat=this.mmu.io[0x41];
    if(this.ly===this.lyc){stat|=4;if(stat&0x40)this.mmu.requestInterrupt(1);}
    else stat&=~4;
    this.mmu.io[0x41]=stat;
  }
  _tileAddr(idx,signed){
    if(signed){const t=idx<128?idx:idx-256;return 0x1000+t*16;}
    return idx*16;
  }
  _palColor(pal,ci){return(pal>>(ci*2))&3;}
  _drawLine(){
    const ly=this.ly;if(ly>=GB_H)return;
    const lcdc=this.lcdc,base=ly*GB_W,bgPrio=new Uint8Array(GB_W),signed=!(lcdc&0x10);
    if(lcdc&1){
      const map=(lcdc&8)?0x1C00:0x1800;
      const bgY=(ly+this.scy)&0xFF,tRow=bgY>>3,tPY=bgY&7;
      for(let x=0;x<GB_W;x++){
        const bgX=(x+this.scx)&0xFF,tCol=bgX>>3,tPX=bgX&7;
        const idx=this.mmu.vram[map+tRow*32+tCol];
        const ta=this._tileAddr(idx,signed);
        const lo=this.mmu.vram[ta+tPY*2],hi=this.mmu.vram[ta+tPY*2+1];
        const bit=7-tPX,ci=((hi>>bit)&1)<<1|((lo>>bit)&1);
        this.pixels[base+x]=this._palColor(this.bgp,ci);bgPrio[x]=ci;
      }
    }
    const wx=this.wx;
    if((lcdc&0x20)&&ly>=this.wy&&wx<GB_W){
      const map=(lcdc&0x40)?0x1C00:0x1800;
      const tRow=this.winLine>>3,tPY=this.winLine&7;
      for(let x=Math.max(0,wx);x<GB_W;x++){
        const wX=x-wx,idx=this.mmu.vram[map+tRow*32+(wX>>3)];
        const ta=this._tileAddr(idx,signed);
        const lo=this.mmu.vram[ta+tPY*2],hi=this.mmu.vram[ta+tPY*2+1];
        const bit=7-(wX&7),ci=((hi>>bit)&1)<<1|((lo>>bit)&1);
        this.pixels[base+x]=this._palColor(this.bgp,ci);bgPrio[x]=ci;
      }
      this.winLine++;
    }
    if(lcdc&2){
      const sprH=(lcdc&4)?16:8,sprs=[];
      for(let i=0;i<40&&sprs.length<10;i++){
        const ob=i*4,sy=this.mmu.oam[ob]-16;
        if(ly>=sy&&ly<sy+sprH)
          sprs.push({y:sy,x:this.mmu.oam[ob+1]-8,tile:this.mmu.oam[ob+2]&(sprH===16?0xFE:0xFF),flags:this.mmu.oam[ob+3],idx:i});
      }
      sprs.sort((a,b)=>b.x-a.x||b.idx-a.idx);
      for(const s of sprs){
        const fx=s.flags&0x20,fy=s.flags&0x40,bgOvr=s.flags&0x80;
        const pal=(s.flags&0x10)?this.obp1:this.obp0;
        let ty=ly-s.y;if(fy)ty=sprH-1-ty;
        const ta=s.tile*16+ty*2,lo=this.mmu.vram[ta],hi=this.mmu.vram[ta+1];
        for(let px=0;px<8;px++){
          const sx=s.x+px;if(sx<0||sx>=GB_W)continue;
          const bit=fx?px:7-px,ci=((hi>>bit)&1)<<1|((lo>>bit)&1);
          if(!ci)continue;if(bgOvr&&bgPrio[sx])continue;
          this.pixels[base+sx]=this._palColor(pal,ci);
        }
      }
    }
  }
  _blit(){
    for(let i=0;i<GB_W*GB_H;i++){
      const [r,g,b]=this.palette[this.pixels[i]];
      this.framebuf[i*4]=r;this.framebuf[i*4+1]=g;this.framebuf[i*4+2]=b;this.framebuf[i*4+3]=255;
    }
  }
}

// ═══════════════════════════════════════════════
// CPU
// ═══════════════════════════════════════════════
class CPU {
  constructor(mmu){
    this.mmu=mmu;
    this.a=0x01;this.f=0xB0;this.b=0x00;this.c=0x13;
    this.d=0x00;this.e=0xD8;this.h=0x01;this.l=0x4D;
    this.sp=0xFFFE;this.pc=0x0100;
    this.halted=false;this.ime=false;this.pendingIME=0;this.haltBug=false;
  }
  get zf(){return(this.f>>7)&1;} set zf(v){v?this.f|=0x80:this.f&=0x70;}
  get nf(){return(this.f>>6)&1;} set nf(v){v?this.f|=0x40:this.f&=0xB0;}
  get hf(){return(this.f>>5)&1;} set hf(v){v?this.f|=0x20:this.f&=0xD0;}
  get cf(){return(this.f>>4)&1;} set cf(v){v?this.f|=0x10:this.f&=0xE0;}
  get af(){return(this.a<<8)|(this.f&0xF0);} set af(v){this.a=(v>>8)&0xFF;this.f=v&0xF0;}
  get bc(){return(this.b<<8)|this.c;}         set bc(v){this.b=(v>>8)&0xFF;this.c=v&0xFF;}
  get de(){return(this.d<<8)|this.e;}         set de(v){this.d=(v>>8)&0xFF;this.e=v&0xFF;}
  get hl(){return(this.h<<8)|this.l;}         set hl(v){this.h=(v>>8)&0xFF;this.l=v&0xFF;}
  rb(){return this.mmu.read(this.pc++);}
  rw(){const lo=this.rb();return lo|(this.rb()<<8);}
  push(v){this.sp=(this.sp-2)&0xFFFF;this.mmu.write(this.sp,v&0xFF);this.mmu.write(this.sp+1,(v>>8)&0xFF);}
  pop(){const lo=this.mmu.read(this.sp),hi=this.mmu.read(this.sp+1);this.sp=(this.sp+2)&0xFFFF;return lo|(hi<<8);}
  ADD(v){const r=this.a+v;this.nf=0;this.hf=(this.a&0xF)+(v&0xF)>0xF?1:0;this.cf=r>0xFF?1:0;this.a=r&0xFF;this.zf=!this.a?1:0;}
  ADC(v){const c=this.cf,r=this.a+v+c;this.nf=0;this.hf=(this.a&0xF)+(v&0xF)+c>0xF?1:0;this.cf=r>0xFF?1:0;this.a=r&0xFF;this.zf=!this.a?1:0;}
  SUB(v){this.nf=1;this.hf=(this.a&0xF)<(v&0xF)?1:0;this.cf=this.a<v?1:0;this.a=(this.a-v)&0xFF;this.zf=!this.a?1:0;}
  SBC(v){const c=this.cf,r=this.a-v-c;this.nf=1;this.hf=(this.a&0xF)<(v&0xF)+c?1:0;this.cf=r<0?1:0;this.a=r&0xFF;this.zf=!this.a?1:0;}
  AND(v){this.a&=v;this.zf=!this.a?1:0;this.nf=0;this.hf=1;this.cf=0;}
  XOR(v){this.a^=v;this.zf=!this.a?1:0;this.nf=0;this.hf=0;this.cf=0;}
  OR(v) {this.a|=v;this.zf=!this.a?1:0;this.nf=0;this.hf=0;this.cf=0;}
  CP(v) {const r=this.a-v;this.nf=1;this.hf=(this.a&0xF)<(v&0xF)?1:0;this.cf=this.a<v?1:0;this.zf=!r?1:0;}
  INC(v){const r=(v+1)&0xFF;this.nf=0;this.hf=(v&0xF)===0xF?1:0;this.zf=!r?1:0;return r;}
  DEC(v){const r=(v-1)&0xFF;this.nf=1;this.hf=(v&0xF)===0?1:0;this.zf=!r?1:0;return r;}
  ADDHL(v){const r=this.hl+v;this.nf=0;this.hf=(this.hl&0xFFF)+(v&0xFFF)>0xFFF?1:0;this.cf=r>0xFFFF?1:0;this.hl=r&0xFFFF;}
  RLC(v){const c=v>>7;v=((v<<1)|c)&0xFF;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  RRC(v){const c=v&1;v=((v>>1)|(c<<7))&0xFF;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  RL(v) {const c=v>>7;v=((v<<1)|this.cf)&0xFF;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  RR(v) {const c=v&1;v=((v>>1)|(this.cf<<7))&0xFF;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  SLA(v){const c=v>>7;v=(v<<1)&0xFF;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  SRA(v){const c=v&1;v=((v>>1)|(v&0x80))&0xFF;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  SWAP(v){v=((v<<4)|(v>>4))&0xFF;this.zf=!v?1:0;this.nf=0;this.hf=0;this.cf=0;return v;}
  SRL(v){const c=v&1;v=v>>1;this.cf=c;this.zf=!v?1:0;this.nf=0;this.hf=0;return v;}
  BIT(b,v){this.zf=((v>>b)&1)?0:1;this.nf=0;this.hf=1;}
  RES(b,v){return v&~(1<<b);}
  SET(b,v){return v|(1<<b);}
  step(){
    if(this.pendingIME){this.pendingIME--;if(!this.pendingIME)this.ime=true;}
    const pend=this.mmu.ie&(this.mmu.ifReg&0x1F);
    if(pend){
      this.halted=false;
      if(this.ime){
        this.ime=false;
        for(let i=0;i<5;i++){
          if(pend&(1<<i)){
            this.mmu.ifReg&=~(1<<i);this.push(this.pc);this.pc=0x40+i*8;return 20;
          }
        }
      }
    }
    if(this.halted)return 4;
    const op=this.mmu.read(this.pc);
    if(this.haltBug){this.haltBug=false;}else{this.pc=(this.pc+1)&0xFFFF;}
    return this._exec(op);
  }
  _exec(op){
    const m=this.mmu;
    const jr=(e)=>{const s=e>127?e-256:e;this.pc=(this.pc+s)&0xFFFF;};
    switch(op){
      case 0x00:return 4;
      case 0x01:this.bc=this.rw();return 12;
      case 0x02:m.write(this.bc,this.a);return 8;
      case 0x03:this.bc=(this.bc+1)&0xFFFF;return 8;
      case 0x04:this.b=this.INC(this.b);return 4;
      case 0x05:this.b=this.DEC(this.b);return 4;
      case 0x06:this.b=this.rb();return 8;
      case 0x07:{const c=this.a>>7;this.a=((this.a<<1)|c)&0xFF;this.cf=c;this.zf=0;this.nf=0;this.hf=0;return 4;}
      case 0x08:{const a=this.rw();m.write(a,this.sp&0xFF);m.write(a+1,this.sp>>8);return 20;}
      case 0x09:this.ADDHL(this.bc);return 8;
      case 0x0A:this.a=m.read(this.bc);return 8;
      case 0x0B:this.bc=(this.bc-1)&0xFFFF;return 8;
      case 0x0C:this.c=this.INC(this.c);return 4;
      case 0x0D:this.c=this.DEC(this.c);return 4;
      case 0x0E:this.c=this.rb();return 8;
      case 0x0F:{const c=this.a&1;this.a=((this.a>>1)|(c<<7))&0xFF;this.cf=c;this.zf=0;this.nf=0;this.hf=0;return 4;}
      case 0x10:this.rb();return 4;
      case 0x11:this.de=this.rw();return 12;
      case 0x12:m.write(this.de,this.a);return 8;
      case 0x13:this.de=(this.de+1)&0xFFFF;return 8;
      case 0x14:this.d=this.INC(this.d);return 4;
      case 0x15:this.d=this.DEC(this.d);return 4;
      case 0x16:this.d=this.rb();return 8;
      case 0x17:{const c=this.a>>7;this.a=((this.a<<1)|this.cf)&0xFF;this.cf=c;this.zf=0;this.nf=0;this.hf=0;return 4;}
      case 0x18:jr(this.rb());return 12;
      case 0x19:this.ADDHL(this.de);return 8;
      case 0x1A:this.a=m.read(this.de);return 8;
      case 0x1B:this.de=(this.de-1)&0xFFFF;return 8;
      case 0x1C:this.e=this.INC(this.e);return 4;
      case 0x1D:this.e=this.DEC(this.e);return 4;
      case 0x1E:this.e=this.rb();return 8;
      case 0x1F:{const c=this.a&1;this.a=((this.a>>1)|(this.cf<<7))&0xFF;this.cf=c;this.zf=0;this.nf=0;this.hf=0;return 4;}
      case 0x20:{const e=this.rb();if(!this.zf){jr(e);return 12;}return 8;}
      case 0x21:this.hl=this.rw();return 12;
      case 0x22:m.write(this.hl,this.a);this.hl=(this.hl+1)&0xFFFF;return 8;
      case 0x23:this.hl=(this.hl+1)&0xFFFF;return 8;
      case 0x24:this.h=this.INC(this.h);return 4;
      case 0x25:this.h=this.DEC(this.h);return 4;
      case 0x26:this.h=this.rb();return 8;
      case 0x27:{let a=this.a;if(!this.nf){if(this.hf||(a&0xF)>9)a+=6;if(this.cf||a>0x9F){a+=0x60;this.cf=1;}}else{if(this.hf)a-=6;if(this.cf)a-=0x60;}this.a=a&0xFF;this.zf=!this.a?1:0;this.hf=0;return 4;}
      case 0x28:{const e=this.rb();if(this.zf){jr(e);return 12;}return 8;}
      case 0x29:this.ADDHL(this.hl);return 8;
      case 0x2A:this.a=m.read(this.hl);this.hl=(this.hl+1)&0xFFFF;return 8;
      case 0x2B:this.hl=(this.hl-1)&0xFFFF;return 8;
      case 0x2C:this.l=this.INC(this.l);return 4;
      case 0x2D:this.l=this.DEC(this.l);return 4;
      case 0x2E:this.l=this.rb();return 8;
      case 0x2F:this.a=(~this.a)&0xFF;this.nf=1;this.hf=1;return 4;
      case 0x30:{const e=this.rb();if(!this.cf){jr(e);return 12;}return 8;}
      case 0x31:this.sp=this.rw();return 12;
      case 0x32:m.write(this.hl,this.a);this.hl=(this.hl-1)&0xFFFF;return 8;
      case 0x33:this.sp=(this.sp+1)&0xFFFF;return 8;
      case 0x34:{const v=this.INC(m.read(this.hl));m.write(this.hl,v);return 12;}
      case 0x35:{const v=this.DEC(m.read(this.hl));m.write(this.hl,v);return 12;}
      case 0x36:m.write(this.hl,this.rb());return 12;
      case 0x37:this.cf=1;this.nf=0;this.hf=0;return 4;
      case 0x38:{const e=this.rb();if(this.cf){jr(e);return 12;}return 8;}
      case 0x39:this.ADDHL(this.sp);return 8;
      case 0x3A:this.a=m.read(this.hl);this.hl=(this.hl-1)&0xFFFF;return 8;
      case 0x3B:this.sp=(this.sp-1)&0xFFFF;return 8;
      case 0x3C:this.a=this.INC(this.a);return 4;
      case 0x3D:this.a=this.DEC(this.a);return 4;
      case 0x3E:this.a=this.rb();return 8;
      case 0x3F:this.cf^=1;this.nf=0;this.hf=0;return 4;
      case 0x40:return 4;case 0x41:this.b=this.c;return 4;case 0x42:this.b=this.d;return 4;
      case 0x43:this.b=this.e;return 4;case 0x44:this.b=this.h;return 4;case 0x45:this.b=this.l;return 4;
      case 0x46:this.b=m.read(this.hl);return 8;case 0x47:this.b=this.a;return 4;
      case 0x48:this.c=this.b;return 4;case 0x49:return 4;case 0x4A:this.c=this.d;return 4;
      case 0x4B:this.c=this.e;return 4;case 0x4C:this.c=this.h;return 4;case 0x4D:this.c=this.l;return 4;
      case 0x4E:this.c=m.read(this.hl);return 8;case 0x4F:this.c=this.a;return 4;
      case 0x50:this.d=this.b;return 4;case 0x51:this.d=this.c;return 4;case 0x52:return 4;
      case 0x53:this.d=this.e;return 4;case 0x54:this.d=this.h;return 4;case 0x55:this.d=this.l;return 4;
      case 0x56:this.d=m.read(this.hl);return 8;case 0x57:this.d=this.a;return 4;
      case 0x58:this.e=this.b;return 4;case 0x59:this.e=this.c;return 4;case 0x5A:this.e=this.d;return 4;
      case 0x5B:return 4;case 0x5C:this.e=this.h;return 4;case 0x5D:this.e=this.l;return 4;
      case 0x5E:this.e=m.read(this.hl);return 8;case 0x5F:this.e=this.a;return 4;
      case 0x60:this.h=this.b;return 4;case 0x61:this.h=this.c;return 4;case 0x62:this.h=this.d;return 4;
      case 0x63:this.h=this.e;return 4;case 0x64:return 4;case 0x65:this.h=this.l;return 4;
      case 0x66:this.h=m.read(this.hl);return 8;case 0x67:this.h=this.a;return 4;
      case 0x68:this.l=this.b;return 4;case 0x69:this.l=this.c;return 4;case 0x6A:this.l=this.d;return 4;
      case 0x6B:this.l=this.e;return 4;case 0x6C:this.l=this.h;return 4;case 0x6D:return 4;
      case 0x6E:this.l=m.read(this.hl);return 8;case 0x6F:this.l=this.a;return 4;
      case 0x70:m.write(this.hl,this.b);return 8;case 0x71:m.write(this.hl,this.c);return 8;
      case 0x72:m.write(this.hl,this.d);return 8;case 0x73:m.write(this.hl,this.e);return 8;
      case 0x74:m.write(this.hl,this.h);return 8;case 0x75:m.write(this.hl,this.l);return 8;
      case 0x76:
        if(!this.ime&&(this.mmu.ie&this.mmu.ifReg&0x1F))this.haltBug=true;
        else this.halted=true;
        return 4;
      case 0x77:m.write(this.hl,this.a);return 8;
      case 0x78:this.a=this.b;return 4;case 0x79:this.a=this.c;return 4;case 0x7A:this.a=this.d;return 4;
      case 0x7B:this.a=this.e;return 4;case 0x7C:this.a=this.h;return 4;case 0x7D:this.a=this.l;return 4;
      case 0x7E:this.a=m.read(this.hl);return 8;case 0x7F:return 4;
      case 0x80:this.ADD(this.b);return 4;case 0x81:this.ADD(this.c);return 4;
      case 0x82:this.ADD(this.d);return 4;case 0x83:this.ADD(this.e);return 4;
      case 0x84:this.ADD(this.h);return 4;case 0x85:this.ADD(this.l);return 4;
      case 0x86:this.ADD(m.read(this.hl));return 8;case 0x87:this.ADD(this.a);return 4;
      case 0x88:this.ADC(this.b);return 4;case 0x89:this.ADC(this.c);return 4;
      case 0x8A:this.ADC(this.d);return 4;case 0x8B:this.ADC(this.e);return 4;
      case 0x8C:this.ADC(this.h);return 4;case 0x8D:this.ADC(this.l);return 4;
      case 0x8E:this.ADC(m.read(this.hl));return 8;case 0x8F:this.ADC(this.a);return 4;
      case 0x90:this.SUB(this.b);return 4;case 0x91:this.SUB(this.c);return 4;
      case 0x92:this.SUB(this.d);return 4;case 0x93:this.SUB(this.e);return 4;
      case 0x94:this.SUB(this.h);return 4;case 0x95:this.SUB(this.l);return 4;
      case 0x96:this.SUB(m.read(this.hl));return 8;case 0x97:this.SUB(this.a);return 4;
      case 0x98:this.SBC(this.b);return 4;case 0x99:this.SBC(this.c);return 4;
      case 0x9A:this.SBC(this.d);return 4;case 0x9B:this.SBC(this.e);return 4;
      case 0x9C:this.SBC(this.h);return 4;case 0x9D:this.SBC(this.l);return 4;
      case 0x9E:this.SBC(m.read(this.hl));return 8;case 0x9F:this.SBC(this.a);return 4;
      case 0xA0:this.AND(this.b);return 4;case 0xA1:this.AND(this.c);return 4;
      case 0xA2:this.AND(this.d);return 4;case 0xA3:this.AND(this.e);return 4;
      case 0xA4:this.AND(this.h);return 4;case 0xA5:this.AND(this.l);return 4;
      case 0xA6:this.AND(m.read(this.hl));return 8;case 0xA7:this.AND(this.a);return 4;
      case 0xA8:this.XOR(this.b);return 4;case 0xA9:this.XOR(this.c);return 4;
      case 0xAA:this.XOR(this.d);return 4;case 0xAB:this.XOR(this.e);return 4;
      case 0xAC:this.XOR(this.h);return 4;case 0xAD:this.XOR(this.l);return 4;
      case 0xAE:this.XOR(m.read(this.hl));return 8;case 0xAF:this.XOR(this.a);return 4;
      case 0xB0:this.OR(this.b);return 4;case 0xB1:this.OR(this.c);return 4;
      case 0xB2:this.OR(this.d);return 4;case 0xB3:this.OR(this.e);return 4;
      case 0xB4:this.OR(this.h);return 4;case 0xB5:this.OR(this.l);return 4;
      case 0xB6:this.OR(m.read(this.hl));return 8;case 0xB7:this.OR(this.a);return 4;
      case 0xB8:this.CP(this.b);return 4;case 0xB9:this.CP(this.c);return 4;
      case 0xBA:this.CP(this.d);return 4;case 0xBB:this.CP(this.e);return 4;
      case 0xBC:this.CP(this.h);return 4;case 0xBD:this.CP(this.l);return 4;
      case 0xBE:this.CP(m.read(this.hl));return 8;case 0xBF:this.CP(this.a);return 4;
      case 0xC0:if(!this.zf){this.pc=this.pop();return 20;}return 8;
      case 0xC1:this.bc=this.pop();return 12;
      case 0xC2:{const a=this.rw();if(!this.zf){this.pc=a;return 16;}return 12;}
      case 0xC3:this.pc=this.rw();return 16;
      case 0xC4:{const a=this.rw();if(!this.zf){this.push(this.pc);this.pc=a;return 24;}return 12;}
      case 0xC5:this.push(this.bc);return 16;
      case 0xC6:this.ADD(this.rb());return 8;
      case 0xC7:this.push(this.pc);this.pc=0x00;return 16;
      case 0xC8:if(this.zf){this.pc=this.pop();return 20;}return 8;
      case 0xC9:this.pc=this.pop();return 16;
      case 0xCA:{const a=this.rw();if(this.zf){this.pc=a;return 16;}return 12;}
      case 0xCB:return this._execCB();
      case 0xCC:{const a=this.rw();if(this.zf){this.push(this.pc);this.pc=a;return 24;}return 12;}
      case 0xCD:{const a=this.rw();this.push(this.pc);this.pc=a;return 24;}
      case 0xCE:this.ADC(this.rb());return 8;
      case 0xCF:this.push(this.pc);this.pc=0x08;return 16;
      case 0xD0:if(!this.cf){this.pc=this.pop();return 20;}return 8;
      case 0xD1:this.de=this.pop();return 12;
      case 0xD2:{const a=this.rw();if(!this.cf){this.pc=a;return 16;}return 12;}
      case 0xD4:{const a=this.rw();if(!this.cf){this.push(this.pc);this.pc=a;return 24;}return 12;}
      case 0xD5:this.push(this.de);return 16;
      case 0xD6:this.SUB(this.rb());return 8;
      case 0xD7:this.push(this.pc);this.pc=0x10;return 16;
      case 0xD8:if(this.cf){this.pc=this.pop();return 20;}return 8;
      case 0xD9:this.pc=this.pop();this.ime=true;return 16;
      case 0xDA:{const a=this.rw();if(this.cf){this.pc=a;return 16;}return 12;}
      case 0xDC:{const a=this.rw();if(this.cf){this.push(this.pc);this.pc=a;return 24;}return 12;}
      case 0xDE:this.SBC(this.rb());return 8;
      case 0xDF:this.push(this.pc);this.pc=0x18;return 16;
      case 0xE0:m.write(0xFF00|this.rb(),this.a);return 12;
      case 0xE1:this.hl=this.pop();return 12;
      case 0xE2:m.write(0xFF00|this.c,this.a);return 8;
      case 0xE5:this.push(this.hl);return 16;
      case 0xE6:this.AND(this.rb());return 8;
      case 0xE7:this.push(this.pc);this.pc=0x20;return 16;
      case 0xE8:{const e=this.rb(),s=e>127?e-256:e;this.hf=((this.sp&0xF)+(e&0xF))>0xF?1:0;this.cf=((this.sp&0xFF)+(e&0xFF))>0xFF?1:0;this.sp=(this.sp+s)&0xFFFF;this.zf=0;this.nf=0;return 16;}
      case 0xE9:this.pc=this.hl;return 4;
      case 0xEA:m.write(this.rw(),this.a);return 16;
      case 0xEE:this.XOR(this.rb());return 8;
      case 0xEF:this.push(this.pc);this.pc=0x28;return 16;
      case 0xF0:this.a=m.read(0xFF00|this.rb());return 12;
      case 0xF1:this.af=this.pop();return 12;
      case 0xF2:this.a=m.read(0xFF00|this.c);return 8;
      case 0xF3:this.ime=false;this.pendingIME=0;return 4;
      case 0xF5:this.push(this.af);return 16;
      case 0xF6:this.OR(this.rb());return 8;
      case 0xF7:this.push(this.pc);this.pc=0x30;return 16;
      case 0xF8:{const e=this.rb(),s=e>127?e-256:e;this.hf=((this.sp&0xF)+(e&0xF))>0xF?1:0;this.cf=((this.sp&0xFF)+(e&0xFF))>0xFF?1:0;this.hl=(this.sp+s)&0xFFFF;this.zf=0;this.nf=0;return 12;}
      case 0xF9:this.sp=this.hl;return 8;
      case 0xFA:this.a=m.read(this.rw());return 16;
      case 0xFB:this.pendingIME=2;return 4;
      case 0xFE:this.CP(this.rb());return 8;
      case 0xFF:this.push(this.pc);this.pc=0x38;return 16;
      default:return 4;
    }
  }
  _execCB(){
    const op=this.rb(),r=op&7,bit=(op>>3)&7,m=this.mmu;
    const G=()=>[this.b,this.c,this.d,this.e,this.h,this.l,m.read(this.hl),this.a][r];
    const S=(v)=>{switch(r){case 0:this.b=v;break;case 1:this.c=v;break;case 2:this.d=v;break;
      case 3:this.e=v;break;case 4:this.h=v;break;case 5:this.l=v;break;
      case 6:m.write(this.hl,v);break;case 7:this.a=v;break;}};
    const base=r===6?16:8,v=G();
    if(op<0x40){const fns=[this.RLC,this.RRC,this.RL,this.RR,this.SLA,this.SRA,this.SWAP,this.SRL];S(fns[(op>>3)&7].call(this,v));return base;}
    else if(op<0x80){this.BIT(bit,v);return r===6?12:8;}
    else if(op<0xC0){S(this.RES(bit,v));return base;}
    else{S(this.SET(bit,v));return base;}
  }
}

// ═══════════════════════════════════════════════
// APU
// ═══════════════════════════════════════════════
const SAMPLE_RATE  = 44100;
const CPU_FREQ     = 4194304;
// Canonical DMG 8-step duty patterns (duty position increments 0..7):
// 0: 00000001 (12.5%), 1: 00000011 (25%), 2: 00001111 (50%), 3: 11111100 (75%)
const DUTY_WAVES   = [
  [0,0,0,0,0,0,0,1],
  [0,0,0,0,0,0,1,1],
  [0,0,0,0,1,1,1,1],
  [1,1,1,1,1,1,0,0],
];
// Ring buffer size — ~93 ms of audio, must be power of two
const APU_BUF_SIZE = 4096;
const APU_BUF_MASK = APU_BUF_SIZE - 1;

class APU {
  constructor(mmu){
    this.mmu = mmu;
    this.ctx  = null;
    this.node = null;
    // Float32 ring buffers — eliminates O(n) Array.shift() in the audio hot path
    this.bufL      = new Float32Array(APU_BUF_SIZE);
    this.bufR      = new Float32Array(APU_BUF_SIZE);
    this.bufWrite  = 0;
    this.bufRead   = 0;
    this.lastL     = 0;   // last output sample — used for smooth underrun fill
    this.lastR     = 0;
    this.cycleBuf  = 0;
    this.cyclesPerSample = CPU_FREQ / SAMPLE_RATE;
    this.fsTimer   = 0;
    this.fsStep    = 0;
    // Capacitor high-pass filter — removes DC bias, matching real hardware behaviour.
    // α = exp(-2π·40/44100) ≈ 0.9943 gives ~40 Hz cutoff, well below any musical pitch.
    this.hpAlpha  = 0.9943;
    this.hpL      = 0;   // capacitor charge L
    this.hpR      = 0;   // capacitor charge R
    this.hpPrevL  = 0;   // previous raw input L
    this.hpPrevR  = 0;   // previous raw input R
    this.ch1 = {on:false,dacOn:false,dutyStep:0,freqTimer:8192,lenCounter:0,lenEnable:false,
                vol:0,volInit:0,volDir:0,volTimer:0,volPeriod:0,
                sweepTimer:0,sweepPeriod:0,sweepDir:0,sweepShift:0,sweepEnable:false,
                shadowFreq:0,freq:0,output:0};
    this.ch2 = {on:false,dacOn:false,dutyStep:0,freqTimer:8192,lenCounter:0,lenEnable:false,
                vol:0,volInit:0,volDir:0,volTimer:0,volPeriod:0,freq:0,output:0};
    this.ch3 = {on:false,dacOn:false,freqTimer:8192,wavePos:0,lenCounter:0,lenEnable:false,
                freq:0,output:0};
    this.ch4 = {on:false,dacOn:false,lfsr:0x7FFF,freqTimer:8192,lenCounter:0,lenEnable:false,
                vol:0,volInit:0,volDir:0,volTimer:0,volPeriod:0,narrowMode:false,output:0};
    // Debug instrumentation counters (kept small to avoid log spam).
    this._env0BugLogCount = 0;
    this._sweep0BugLogCount = 0;
    this._masterVolMuteMismatchCount = 0;
    this._initAudioLogEmitted = false;
    this._duty50MismatchLogged = false;
    this._duty50PatternMismatch = false;
    this._duty50PatternMismatchCount = 0;
    this._lastAudioDebug = null;
    this._lastTriggerCh1 = null;
    this.diag = {ch1:{},ch2:{},ch3:{},ch4:{}};
  }

  initAudio(){
    if (this.ctx) return;
    this.ctx  = new (window.AudioContext || window.webkitAudioContext)({sampleRate: SAMPLE_RATE});
    // #region agent log
    if (!this._initAudioLogEmitted){
      this._initAudioLogEmitted = true;
    }
    // #endregion
    this.node = this.ctx.createScriptProcessor(2048, 0, 2);
    this.node.onaudioprocess = (e) => {
      const L = e.outputBuffer.getChannelData(0);
      const R = e.outputBuffer.getChannelData(1);
      const len = L.length;
      for (let i = 0; i < len; i++) {
        if (this.bufRead < this.bufWrite) {
          this.lastL = this.bufL[this.bufRead & APU_BUF_MASK];
          this.lastR = this.bufR[this.bufRead & APU_BUF_MASK];
          this.bufRead++;
        } else {
          // Buffer underrun — decay toward silence to avoid a hard click
          this.lastL *= 0.998;
          this.lastR *= 0.998;
        }
        L[i] = this.lastL;
        R[i] = this.lastR;
      }
    };
    this.node.connect(this.ctx.destination);
  }

  stopAudio(){
    if (this.node) { try { this.node.disconnect(); } catch(e){} this.node = null; }
    if (this.ctx)  { try { this.ctx.close();       } catch(e){} this.ctx  = null; }
    this.bufWrite = 0;
    this.bufRead  = 0;
    this.lastL    = 0;
    this.lastR    = 0;
    this.hpL = this.hpR = this.hpPrevL = this.hpPrevR = 0;
  }

  _r(off){ return this.mmu.io[off]; }
  _ch1Freq(){ return ((this._r(0x14)&7)<<8) | this._r(0x13); }
  _ch2Freq(){ return ((this._r(0x19)&7)<<8) | this._r(0x18); }
  _ch3Freq(){ return ((this._r(0x1E)&7)<<8) | this._r(0x1D); }

  _triggerCh1(){
    const c = this.ch1;
    c.on          = true;
    // Hypothesis H5: duty phase must reset on trigger; otherwise short iconic
    // sounds (Game Freak logo) can start mid-wave and sound wrong.
    c.dutyStep    = 0;
    c.freq        = this._ch1Freq();
    c.freqTimer   = (2048 - c.freq) * 4;
    if (!c.lenCounter) c.lenCounter = 64;
    c.volInit     = (this._r(0x12) >> 4) & 0xF;
    c.volDir      = (this._r(0x12) >> 3) & 1;
    c.volPeriod   = this._r(0x12) & 7;
    c.vol         = c.volInit;
    c.volTimer    = c.volPeriod || 8;
    c.dacOn       = (this._r(0x12) & 0xF8) !== 0;
    if (!c.dacOn) c.on = false;
    // Sweep
    c.sweepPeriod  = (this._r(0x10) >> 4) & 7;
    c.sweepDir     = (this._r(0x10) >> 3) & 1;
    c.sweepShift   = this._r(0x10) & 7;
    c.shadowFreq   = c.freq;
    c.sweepTimer   = c.sweepPeriod || 8;
    c.sweepEnable  = c.sweepPeriod > 0 || c.sweepShift > 0;
    // Immediate overflow check if shift is set
    if (c.sweepShift) {
      const delta = c.shadowFreq >> c.sweepShift;
      if ((c.sweepDir ? c.shadowFreq - delta : c.shadowFreq + delta) > 2047) c.on = false;
    }
    // Record trigger snapshot for on-screen debug.
    this._lastTriggerCh1 = {
      nr10:this._r(0x10), nr11:this._r(0x11), nr12:this._r(0x12), nr13:this._r(0x13), nr14:this._r(0x14),
      dutyStep:c.dutyStep, volInit:c.volInit, volPeriod:c.volPeriod, sweepPeriod:c.sweepPeriod, sweepShift:c.sweepShift,
      freq:c.freq, fsStep:this.fsStep,
    };
  }

  _triggerCh2(){
    const c = this.ch2;
    c.on        = true;
    c.freq      = this._ch2Freq();
    c.freqTimer = (2048 - c.freq) * 4;
    if (!c.lenCounter) c.lenCounter = 64;
    c.volInit   = (this._r(0x17) >> 4) & 0xF;
    c.volDir    = (this._r(0x17) >> 3) & 1;
    c.volPeriod = this._r(0x17) & 7;
    c.vol       = c.volInit;
    c.volTimer  = c.volPeriod || 8;
    c.dacOn     = (this._r(0x17) & 0xF8) !== 0;
    if (!c.dacOn) c.on = false;
  }

  _triggerCh3(){
    const c = this.ch3;
    c.on        = true;
    c.wavePos   = 0;
    c.freq      = this._ch3Freq();
    // FIXED: was c.freq*2 — completely wrong. Correct formula: (2048-freq)*2
    c.freqTimer = (2048 - c.freq) * 2;
    if (!c.lenCounter) c.lenCounter = 256;
    c.dacOn     = !!(this._r(0x1A) & 0x80);
    if (!c.dacOn) c.on = false;
  }

_triggerCh4(){
    const c    = this.ch4;
    c.on       = true;
    c.lfsr     = 0x7FFF;
    if (!c.lenCounter) c.lenCounter = 64;
    // Read NR43 once and derive both freqTimer and narrowMode from it.
    const nr43     = this._r(0x22);
    const ratio4   = nr43 & 7;
    const shift4   = (nr43 >> 4) & 0xF;
    const div4     = ratio4 === 0 ? 8 : ratio4 * 16;
    // Reset freqTimer like CH1/CH2/CH3 do on their triggers.
    // Without this the LFSR clocks at wrong rate after re-trigger.
    c.freqTimer  = (div4 << shift4) || 8192;
    c.narrowMode = !!(nr43 & 8);
    c.volInit    = (this._r(0x21) >> 4) & 0xF;
    c.volDir     = (this._r(0x21) >> 3) & 1;
    c.volPeriod  = this._r(0x21) & 7;
    c.vol        = c.volInit;
    c.volTimer   = c.volPeriod || 8;
    c.dacOn      = (this._r(0x21) & 0xF8) !== 0;
    if (!c.dacOn) c.on = false;
  }

  write(addr, val){
    // NR52: APU power toggle — must be handled before the power-off guard below
    if (addr === 0xFF26) {
      if (!(val & 0x80)) {
        // Power off — clear all sound registers and stop all channels
        for (let i = 0x10; i <= 0x25; i++) this.mmu.io[i] = 0;
        this.ch1.on = this.ch2.on = this.ch3.on = this.ch4.on = false;
        this.ch1.dacOn = this.ch2.dacOn = this.ch3.dacOn = this.ch4.dacOn = false;
        // Also reset HP filter — no audio was playing, silence the capacitor
        this.hpL = this.hpR = this.hpPrevL = this.hpPrevR = 0;
      }
      return;
    }
    // All other sound register writes are ignored while the APU is powered off
    if (!(this.mmu.io[0x26] & 0x80)) return;
    // Length counters (written to NRx1)
    if (addr === 0xFF11) this.ch1.lenCounter = 64  - (val & 0x3F);
    if (addr === 0xFF16) this.ch2.lenCounter = 64  - (val & 0x3F);
    if (addr === 0xFF1B) this.ch3.lenCounter = 256 - val;
    if (addr === 0xFF20) this.ch4.lenCounter = 64  - (val & 0x3F);
    // DAC enable/disable
    if (addr === 0xFF12){ this.ch1.dacOn = (val & 0xF8) !== 0; if (!this.ch1.dacOn) this.ch1.on = false; }
    if (addr === 0xFF17){ this.ch2.dacOn = (val & 0xF8) !== 0; if (!this.ch2.dacOn) this.ch2.on = false; }
    if (addr === 0xFF1A){ this.ch3.dacOn = !!(val & 0x80);     if (!this.ch3.dacOn) this.ch3.on = false; }
    if (addr === 0xFF21){ this.ch4.dacOn = (val & 0xF8) !== 0; if (!this.ch4.dacOn) this.ch4.on = false; }
    // Triggers and length-enable
    if (addr === 0xFF14){ this.ch1.lenEnable = !!(val & 0x40); if (val & 0x80) this._triggerCh1(); }
    if (addr === 0xFF19){ this.ch2.lenEnable = !!(val & 0x40); if (val & 0x80) this._triggerCh2(); }
    if (addr === 0xFF1E){ this.ch3.lenEnable = !!(val & 0x40); if (val & 0x80) this._triggerCh3(); }
    if (addr === 0xFF23){ this.ch4.lenEnable = !!(val & 0x40); if (val & 0x80) this._triggerCh4(); }
  }

  _clockLen(c){ if (c.lenEnable && c.lenCounter > 0){ c.lenCounter--; if (!c.lenCounter) c.on = false; } }

  _clockSweep(){
    const c = this.ch1;
    if (!c.sweepEnable) return;
    if (--c.sweepTimer <= 0){
      c.sweepTimer = c.sweepPeriod || 8;
      // Hypothesis H2: sweepPeriod==0 with non-zero shift should still clock
      // as period 8, but the current code only applies when sweepPeriod > 0.
      if (c.on && c.sweepPeriod === 0 && c.sweepShift > 0 && this._sweep0BugLogCount < 4){
        this._sweep0BugLogCount++;
        const nr10 = this._r(0x10);
        // #region agent log
        // #endregion
      }
      if (c.sweepPeriod > 0) {
        const delta = c.shadowFreq >> c.sweepShift;
        const nf    = c.sweepDir ? c.shadowFreq - delta : c.shadowFreq + delta;
        // First overflow check
        if (nf > 2047) { c.on = false; return; }
        if (c.sweepShift > 0) {
          // Update shadow freq, c1.freq, AND the hardware registers so live
          // frequency reads from NR13/NR14 pick up the new value.
          c.shadowFreq = nf;
          c.freq       = nf;
          this.mmu.io[0x13] = nf & 0xFF;
          this.mmu.io[0x14] = (this.mmu.io[0x14] & 0xF8) | ((nf >> 8) & 0x07);
          // Second overflow check — only valid when sweepShift > 0.
          // When shift=0, nf>>0 = nf, making nf2 = 2*nf which would always overflow.
          const nf2 = c.sweepDir ? nf - (nf >> c.sweepShift) : nf + (nf >> c.sweepShift);
          if (nf2 > 2047) c.on = false;
        }
      }
    }
  }

_clockVol(c){
    // Per DMG hardware (Pan Docs): period=0 → envelope timer does not step.
    // Volume stays constant at volInit. Do NOT treat as period=8 — that
    // hypothesis (H1) caused CH4 to ramp to max volume in Tetris.
    if (!c.volPeriod) return;
    if (--c.volTimer <= 0){
      c.volTimer = c.volPeriod;
      if      (c.volDir && c.vol < 15) c.vol++;
      else if (!c.volDir && c.vol > 0)  c.vol--;
    }
  }

  _stepFS(){
    const s = this.fsStep;
    if (s % 2 === 0){ this._clockLen(this.ch1); this._clockLen(this.ch2); this._clockLen(this.ch3); this._clockLen(this.ch4); }
    if (s === 2 || s === 6) this._clockSweep();
    if (s === 7){ this._clockVol(this.ch1); this._clockVol(this.ch2); this._clockVol(this.ch4); }
    this.fsStep = (this.fsStep + 1) & 7;
  }

  step(cycles){
    if (!(this._r(0x26) & 0x80)){
      this.ch1.on = this.ch2.on = this.ch3.on = this.ch4.on = false;
      return;
    }
    this.fsTimer += cycles;
    while (this.fsTimer >= 8192){ this.fsTimer -= 8192; this._stepFS(); }

    const c1 = this.ch1;
    if (c1.on){
      c1.freqTimer -= cycles;
      while (c1.freqTimer <= 0){
        c1.freq = this._ch1Freq();
        c1.freqTimer += (2048 - c1.freq) * 4 || 8192;
        c1.dutyStep = (c1.dutyStep + 1) & 7;
      }
      const duty = (this._r(0x11) >> 6) & 3;
      c1.output = DUTY_WAVES[duty][c1.dutyStep] ? c1.vol : 0;
      // Hypothesis H4: square duty table entry for duty==50% (index 2)
      // is wrong/timbre-inaccurate.
      if (!this._duty50MismatchLogged && duty === 2 && c1.output > 0){
        this._duty50MismatchLogged = true;
        // Canonical DMG 8-step pattern for duty==50% with dutyPos starting at 0:
        // 0,0,0,0,1,1,1,1  (00001111).
        const expectedDuty2 = [0,0,0,0,1,1,1,1];
        const actualDuty2 = DUTY_WAVES[2].slice();
        const step0Ok = actualDuty2[0] === expectedDuty2[0];
        const mismatchSteps = expectedDuty2.map((v,i)=> (v===actualDuty2[i]) ? null : i).filter(v=>v!==null);
        const duty50Mismatch = mismatchSteps.length > 0;
        this._duty50PatternMismatch = duty50Mismatch;
        if (duty50Mismatch) this._duty50PatternMismatchCount++;
        // #region agent log
        // #endregion
      }
    } else { c1.output = 0; }

    const c2 = this.ch2;
    if (c2.on){
      c2.freqTimer -= cycles;
      while (c2.freqTimer <= 0){
        c2.freq = this._ch2Freq();
        c2.freqTimer += (2048 - c2.freq) * 4 || 8192;
        c2.dutyStep = (c2.dutyStep + 1) & 7;
      }
      const duty2 = (this._r(0x16) >> 6) & 3;
      c2.output = DUTY_WAVES[duty2][c2.dutyStep] ? c2.vol : 0;
    } else { c2.output = 0; }

    const c3 = this.ch3;
    if (c3.on && c3.dacOn){
      c3.freqTimer -= cycles;
      while (c3.freqTimer <= 0){
        c3.freq = this._ch3Freq();
        c3.freqTimer += (2048 - c3.freq) * 2 || 8192;
        c3.wavePos = (c3.wavePos + 1) & 31;
      }
      const volCode = (this._r(0x1C) >> 5) & 3;
      const shift   = [4, 0, 1, 2][volCode];
      const wByte   = this.mmu.io[0x30 + (c3.wavePos >> 1)];
      const nibble  = (c3.wavePos & 1) ? (wByte & 0xF) : (wByte >> 4) & 0xF;
      c3.output = nibble >> shift;
    } else { c3.output = 0; }

const c4 = this.ch4;
    if (c4.on){
      c4.freqTimer -= cycles;
      const nr43  = this._r(0x22);
      const ratio = nr43 & 7;
      const shift = (nr43 >> 4) & 0xF;
      if (shift < 14) {
        const div  = ratio === 0 ? 8 : ratio * 16;
        const inc4 = div << shift;
        while (c4.freqTimer <= 0){
          c4.freqTimer += inc4;
          const xor = (c4.lfsr & 1) ^ ((c4.lfsr >> 1) & 1);
          c4.lfsr = (c4.lfsr >> 1) | (xor << 14);
          if (c4.narrowMode){ c4.lfsr &= ~(1 << 6); c4.lfsr |= xor << 6; }
        }
        c4.output = (c4.lfsr & 1) ? 0 : c4.vol;
      } else {
        // shift >= 14: undefined hardware behaviour → explicit silence.
        // Clamp the timer so a later normal trigger doesn't inherit a
        // deeply-negative value and spin the LFSR thousands of times.
        c4.freqTimer = 8192;
        c4.output = 0;
      }
    } else { c4.output = 0; }

    // Generate samples
    this.cycleBuf += cycles;
    while (this.cycleBuf >= this.cyclesPerSample){
      this.cycleBuf -= this.cyclesPerSample;
      const nr50 = this._r(0x24);
      const nr51 = this._r(0x25);
      const volL = ((nr50 >> 4) & 7) + 1;
      const volR = (nr50 & 7) + 1;
      // Sum raw [0-15] outputs per side for routed channels.
      // Max possible per side = 4 channels × 15 = 60.
      let mixL = 0, mixR = 0;
      if (nr51 & 0x10) mixL += c1.output;  if (nr51 & 0x01) mixR += c1.output;
      if (nr51 & 0x20) mixL += c2.output;  if (nr51 & 0x02) mixR += c2.output;
      if (nr51 & 0x40) mixL += c3.output;  if (nr51 & 0x04) mixR += c3.output;
      if (nr51 & 0x80) mixL += c4.output;  if (nr51 & 0x08) mixR += c4.output;
      // Hypothesis H3: NR50 volume bits set to 0 should mute output,
      // but the current scaling adds +1, producing non-zero volume at 0.
      if (this._masterVolMuteMismatchCount < 5){
        const volBitsL = (nr50 >> 4) & 7;
        const volBitsR = nr50 & 7;
        if (((volBitsL === 0) && (mixL > 0)) || ((volBitsR === 0) && (mixR > 0))){
          this._masterVolMuteMismatchCount++;
          // #region agent log
          // #endregion
        }
      }
      // Scale [0,60] → [0,1], apply master volume.
      // Don't subtract a DC offset here — the high-pass filter below handles that,
      // exactly as the real hardware's output capacitor does.
      const rawL = (mixL / 60) * (volL / 8);
      const rawR = (mixR / 60) * (volR / 8);
      // Single-pole high-pass filter (α≈0.9943 ≈ 40 Hz corner freq at 44100 Hz).
      // Removes DC bias from the DAC output, preserving all audible frequencies.
      this.hpL = this.hpAlpha * (this.hpL + rawL - this.hpPrevL);
      this.hpR = this.hpAlpha * (this.hpR + rawR - this.hpPrevR);
      this.hpPrevL = rawL;
      this.hpPrevR = rawR;
      // Surface last computed mix data for runtime diagnostics.
      this._lastAudioDebug = { nr50, nr51, volL, volR, mixL, mixR, rawL, rawR, hpL: this.hpL, hpR: this.hpR };
      const l = this.hpL * 0.95;
      const r = this.hpR * 0.95;
      // Push to ring buffer — drop sample if buffer is full (fast-forward path)
      if (this.bufWrite - this.bufRead < APU_BUF_SIZE){
        this.bufL[this.bufWrite & APU_BUF_MASK] = l;
        this.bufR[this.bufWrite & APU_BUF_MASK] = r;
        this.bufWrite++;
      }
    }

    // Update NR52 channel-status bits (0-3) — some games poll these
    this.mmu.io[0x26] = (this.mmu.io[0x26] & 0x80)
      | (c1.on ? 0x01 : 0)
      | (c2.on ? 0x02 : 0)
      | (c3.on ? 0x04 : 0)
      | (c4.on ? 0x08 : 0);

    // Diagnostics (used by debug panel)
    const nr50 = this._r(0x24), nr51 = this._r(0x25);
    this.diag = {
      masterOn: !!(this._r(0x26) & 0x80),
      volL: ((nr50 >> 4) & 7) + 1, volR: (nr50 & 7) + 1, nr51,
      debug: {
        envPeriod0BugHits: this._env0BugLogCount,
        sweepPeriod0ShiftHits: this._sweep0BugLogCount,
        masterVolMuteMismatchHits: this._masterVolMuteMismatchCount,
        duty50MismatchLogged: this._duty50MismatchLogged,
        duty50PatternMismatchCount: this._duty50PatternMismatchCount,
        duty50PatternMismatch: this._duty50PatternMismatch,
        lastAudio: this._lastAudioDebug,
        lastTriggerCh1: this._lastTriggerCh1,
      },
      ch1:{ on:c1.on, dacOn:c1.dacOn, vol:c1.vol, freq:c1.freq, duty:(this._r(0x11)>>6)&3, len:c1.lenCounter, sweepOn:c1.sweepEnable, output:c1.output },
      ch2:{ on:c2.on, dacOn:c2.dacOn, vol:c2.vol, freq:c2.freq, duty:(this._r(0x16)>>6)&3, len:c2.lenCounter, output:c2.output },
      ch3:{ on:c3.on, dacOn:c3.dacOn, wavePos:c3.wavePos, volCode:(this._r(0x1C)>>5)&3, len:c3.lenCounter, output:c3.output },
      ch4:{ on:c4.on, dacOn:c4.dacOn, vol:c4.vol, narrow:c4.narrowMode, len:c4.lenCounter, output:c4.output },
    };
  }
}

// ═══════════════════════════════════════════════
// GAME BOY
// ═══════════════════════════════════════════════
class GameBoy {
  constructor(){
    this.mmu=new MMU(); this.ppu=new PPU(this.mmu);
    this.cpu=new CPU(this.mmu); this.timer=new Timer(this.mmu);
    this.apu=new APU(this.mmu); this.mmu.apu=this.apu; this.mmu.timer=this.timer;
  }
  loadROM(buf){this.mmu.loadROM(buf);}
  runFrame(){
    let c=0;
    while(c<GB_CYCLES_PER_FRAME){
      const t=this.cpu.step();
      this.ppu.step(t);this.timer.step(t);this.mmu.stepSerial(t);this.apu.step(t);c+=t;
    }
    return this.ppu.framebuf;
  }
  setPalette(p){this.ppu.palette=p;}
  pressButton(b,d){this.mmu.pressButton(b,d);}

  // ── Save / Load state ─────────────────────────────────────────
  saveState() {
    const cpu = this.cpu, mmu = this.mmu, ppu = this.ppu;
    const timer = this.timer, apu = this.apu;
    const serCh = ch => [
      ch.on?1:0, ch.dacOn?1:0,
      ch.dutyStep??0, ch.wavePos??0,
      (ch.freqTimer>>8)&0xFF, ch.freqTimer&0xFF,
      ch.lenCounter&0xFF, (ch.lenCounter>>8)&0xFF,
      ch.lenEnable?1:0, ch.vol??0, ch.volInit??0,
      ch.volDir??0, ch.volTimer??0, ch.volPeriod??0,
      (ch.freq??0)>>8, (ch.freq??0)&0xFF,
      ch.output??0, ch.narrow??0, ch.narrowMode?1:0,
      ch.sweepTimer??0, ch.sweepPeriod??0, ch.sweepDir??0,
      ch.sweepShift??0, ch.sweepEnable?1:0,
      (ch.shadowFreq??0)>>8, (ch.shadowFreq??0)&0xFF,
      ch.volShift??0, ch.volCode??0,
      (ch.lfsr??0)>>8, (ch.lfsr??0)&0xFF,
    ];
    return {
      cpu:   { a:cpu.a, f:cpu.f, b:cpu.b, c:cpu.c, d:cpu.d, e:cpu.e, h:cpu.h, l:cpu.l,
               sp:cpu.sp, pc:cpu.pc, halted:cpu.halted, ime:cpu.ime,
               pendingIME:cpu.pendingIME, haltBug:cpu.haltBug },
      timer: { div:timer.div },
      ppu:   { cycles:ppu.cycles, mode:ppu.mode, winLine:ppu.winLine, lcdWasOff:ppu.lcdWasOff },
      mmu:   { ie:mmu.ie, ifReg:mmu.ifReg, romBank:mmu.romBank, ramBank:mmu.ramBank,
               ramEn:mmu.ramEn, mbc1Mode:mmu.mbc1Mode, mbc1HiBits:mmu.mbc1HiBits,
               romLoBank:mmu.romLoBank, rtcSel:mmu.rtcSel, rtcLatchStep:mmu.rtcLatchStep,
               wram:   Array.from(mmu.wram),
               vram:   Array.from(mmu.vram),
               oam:    Array.from(mmu.oam),
               hram:   Array.from(mmu.hram),
               io:     Array.from(mmu.io),
               eram:   Array.from(mmu.eram),
               rtcRegs:    Array.from(mmu.rtcRegs),
               rtcLatched: Array.from(mmu.rtcLatched) },
      apu:   { fsTimer:apu.fsTimer, fsStep:apu.fsStep,
               ch1:serCh(apu.ch1), ch2:serCh(apu.ch2),
               ch3:serCh(apu.ch3), ch4:serCh(apu.ch4) },
    };
  }

  loadState(s) {
    const cpu = this.cpu, mmu = this.mmu, ppu = this.ppu;
    const timer = this.timer, apu = this.apu;
    // CPU
    Object.assign(cpu, s.cpu);
    // Timer
    timer.div = s.timer.div;
    // PPU
    Object.assign(ppu, {cycles:s.ppu.cycles, mode:s.ppu.mode, winLine:s.ppu.winLine, lcdWasOff:s.ppu.lcdWasOff});
    // MMU scalars
    const m = s.mmu;
    mmu.ie=m.ie; mmu.ifReg=m.ifReg; mmu.romBank=m.romBank; mmu.ramBank=m.ramBank;
    mmu.ramEn=m.ramEn; mmu.mbc1Mode=m.mbc1Mode; mmu.mbc1HiBits=m.mbc1HiBits;
    mmu.romLoBank=m.romLoBank; mmu.rtcSel=m.rtcSel; mmu.rtcLatchStep=m.rtcLatchStep;
    // MMU arrays
    mmu.wram.set(m.wram);  mmu.vram.set(m.vram);
    mmu.oam.set(m.oam);    mmu.hram.set(m.hram);
    mmu.io.set(m.io);
    if (m.eram.length <= mmu.eram.length) mmu.eram.set(m.eram);
    mmu.rtcRegs.set(m.rtcRegs); mmu.rtcLatched.set(m.rtcLatched);
    // Re-bank ROM to saved bank
    mmu._bankLo(m.romLoBank); mmu._bankHi(m.romBank);
    // APU channels
    const desCh = (ch, data) => {
      ch.on=!!data[0]; ch.dacOn=!!data[1];
      ch.dutyStep=data[2]; ch.wavePos=data[3];
      ch.freqTimer=(data[4]<<8)|data[5];
      ch.lenCounter=(data[6])|(data[7]<<8);
      ch.lenEnable=!!data[8]; ch.vol=data[9]; ch.volInit=data[10];
      ch.volDir=data[11]; ch.volTimer=data[12]; ch.volPeriod=data[13];
      ch.freq=(data[14]<<8)|data[15]; ch.output=data[16];
      ch.narrow=data[17]; ch.narrowMode=!!data[18];
      ch.sweepTimer=data[19]; ch.sweepPeriod=data[20]; ch.sweepDir=data[21];
      ch.sweepShift=data[22]; ch.sweepEnable=!!data[23];
      ch.shadowFreq=(data[24]<<8)|data[25];
      ch.volShift=data[26]; ch.volCode=data[27];
      ch.lfsr=(data[28]<<8)|data[29];
    };
    apu.fsTimer=s.apu.fsTimer; apu.fsStep=s.apu.fsStep;
    desCh(apu.ch1,s.apu.ch1); desCh(apu.ch2,s.apu.ch2);
    desCh(apu.ch3,s.apu.ch3); desCh(apu.ch4,s.apu.ch4);
  }
}


// ═══════════════════════════════════════════════
// GB PLATFORM — memory snapshot for D!NG engine
// ═══════════════════════════════════════════════
// Build flat state object from GB memory. Keys: uppercase hex address strings.
// This is the platform-specific counterpart to ding-engine.js's dingReadMem().
function dingBuildState(gb) {
  if (!gb) return {};
  const state = {};
  const mmu = gb.mmu;
  // VRAM 0x8000–0x9FFF (8 KB) — tile data + BG maps ($9800/$9C00)
  for (let i = 0; i < mmu.vram.length; i++)
    state['0x' + (0x8000 + i).toString(16).toUpperCase()] = mmu.vram[i];
  // Cartridge RAM 0xA000–0xBFFF (current bank window, 8 KB)
  const eramOff = mmu.ramBank * 0x2000;
  for (let i = 0; i < 0x2000; i++)
    state['0x' + (0xA000 + i).toString(16).toUpperCase()] = mmu.eram[eramOff + i] ?? 0;
  // WRAM 0xC000–0xDFFF (8 KB)
  for (let i = 0; i < mmu.wram.length; i++)
    state['0x' + (0xC000 + i).toString(16).toUpperCase()] = mmu.wram[i];
  // OAM 0xFE00–0xFE9F
  for (let i = 0; i < mmu.oam.length; i++)
    state['0x' + (0xFE00 + i).toString(16).toUpperCase()] = mmu.oam[i];
  // IO registers 0xFF00–0xFF7F
  for (let i = 0; i < mmu.io.length; i++)
    state['0x' + (0xFF00 + i).toString(16).toUpperCase()] = mmu.io[i];
  // IF register (live — stored separately from io array)
  state['0xFF0F'] = mmu.ifReg;
  // HRAM 0xFF80–0xFFFE
  for (let i = 0; i < mmu.hram.length; i++)
    state['0x' + (0xFF80 + i).toString(16).toUpperCase()] = mmu.hram[i];
  // IE 0xFFFF
  state['0xFFFF'] = mmu.ie;
  return state;
}

// Pure JS MD5, RFC 1321. No external deps, no GPL.
// Returns uppercase 32-char hex.
const MD5_T = new Uint32Array(64);
for (let i = 0; i < 64; i++)
  MD5_T[i] = (Math.abs(Math.sin(i + 1)) * 0x100000000) >>> 0;

const MD5_S = [
   7,12,17,22,  7,12,17,22,  7,12,17,22,  7,12,17,22,
   5, 9,14,20,  5, 9,14,20,  5, 9,14,20,  5, 9,14,20,
   4,11,16,23,  4,11,16,23,  4,11,16,23,  4,11,16,23,
   6,10,15,21,  6,10,15,21,  6,10,15,21,  6,10,15,21,
];

function md5(buf) {
  const bytes  = new Uint8Array(buf);
  const msgLen = bytes.length;
  const bitLo  = (msgLen * 8) >>> 0;
  const bitHi  = Math.floor(msgLen / 0x20000000) >>> 0;
  const padLen = ((msgLen % 64) < 56) ? (56 - msgLen % 64) : (120 - msgLen % 64);
  const padded = new Uint8Array(msgLen + padLen + 8);
  padded.set(bytes);
  padded[msgLen] = 0x80;
  padded[msgLen + padLen    ] = (bitLo)        & 0xFF;
  padded[msgLen + padLen + 1] = (bitLo >>>  8) & 0xFF;
  padded[msgLen + padLen + 2] = (bitLo >>> 16) & 0xFF;
  padded[msgLen + padLen + 3] = (bitLo >>> 24) & 0xFF;
  padded[msgLen + padLen + 4] = (bitHi)        & 0xFF;
  padded[msgLen + padLen + 5] = (bitHi >>>  8) & 0xFF;
  padded[msgLen + padLen + 6] = (bitHi >>> 16) & 0xFF;
  padded[msgLen + padLen + 7] = (bitHi >>> 24) & 0xFF;
  let a0 = 0x67452301, b0 = 0xEFCDAB89, c0 = 0x98BADCFE, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let j = 0; j < 16; j++)
      M[j] = padded[off+j*4] | (padded[off+j*4+1]<<8) |
              (padded[off+j*4+2]<<16) | (padded[off+j*4+3]<<24);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if      (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5*i+1) % 16; }
      else if (i < 48) { F = B ^ C ^ D;           g = (3*i+5) % 16; }
      else             { F = C ^ (B | ~D);         g = (7*i)   % 16; }
      F = (F + A + MD5_T[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      B = (B + ((F << MD5_S[i]) | (F >>> (32 - MD5_S[i])))) >>> 0;
    }
    a0 = (a0+A)>>>0; b0 = (b0+B)>>>0; c0 = (c0+C)>>>0; d0 = (d0+D)>>>0;
  }
  const le = v => [v,v>>>8,v>>>16,v>>>24].map(b => (b&0xFF).toString(16).padStart(2,'0')).join('');
  return (le(a0) + le(b0) + le(c0) + le(d0)).toUpperCase();
}

// Extract the ROM header title (bytes 0x0134–0x0143, null-terminated ASCII).
function romHeaderTitle(buf) {
  const bytes = new Uint8Array(buf);
  let title = '';
  for (let i = 0x0134; i <= 0x0143; i++) {
    if (!bytes[i]) break;
    title += String.fromCharCode(bytes[i]);
  }
  return title.trim() || '(no title)';
}
