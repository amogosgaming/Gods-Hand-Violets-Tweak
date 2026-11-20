// Procedural DSP synthesizer ported from src/synth.cpp
// Sample rate 22050 Hz for fast, punchy browser synthesis
export const SR = 22050;
const TWO_PI = Math.PI * 2;

export class Rng {
  private s: number;
  constructor(seed = 1) {
    this.s = (seed >>> 0) || 0x9e3779b9;
  }
  next(): number {
    let x = this.s;
    x ^= (x << 13) >>> 0;
    x ^= x >>> 17;
    x ^= (x << 5) >>> 0;
    this.s = x >>> 0;
    return this.s;
  }
  uniform(): number {
    return (this.next() & 0x00ffffff) / 0x01000000;
  }
  signed(): number {
    return this.uniform() * 2 - 1;
  }
  int(lo: number, hi: number): number {
    return lo + (this.next() % Math.max(1, hi - lo + 1));
  }
}

export function midiToHz(midi: number): number {
  return 440 * Math.pow(2, (midi - 69) / 12);
}

function polyBlep(t: number, dt: number): number {
  if (t < dt) {
    const x = t / dt;
    return x + x - x * x - 1;
  }
  if (t > 1 - dt) {
    const x = (t - 1) / dt;
    return x * x + x + x + 1;
  }
  return 0;
}

export function sawSample(phase: number, dt: number): number {
  return 2 * phase - 1 - polyBlep(phase, dt);
}

export function pulseSample(phase: number, dt: number, pw = 0.5): number {
  let v = phase < pw ? 1 : -1;
  v += polyBlep(phase, dt);
  v -= polyBlep((phase + 1 - pw) % 1, dt);
  return v;
}

export class SvfFilter {
  private ic1 = 0;
  private ic2 = 0;
  process(input: number, cutoffHz: number, q = 0.707, mode: 'lp' | 'bp' | 'hp' = 'lp'): number {
    const fc = Math.max(40, Math.min(SR * 0.45, cutoffHz));
    const g = Math.tan((Math.PI * fc) / SR);
    const k = 1 / Math.max(0.5, q);
    const a1 = 1 / (1 + g * (g + k));
    const a2 = g * a1;
    const a3 = g * a2;
    const v3 = input - this.ic2;
    const v1 = a1 * this.ic1 + a2 * v3;
    const v2 = this.ic2 + a2 * this.ic1 + a3 * v3;
    this.ic1 = 2 * v1 - this.ic1;
    this.ic2 = 2 * v2 - this.ic2;
    if (mode === 'bp') return v1;
    if (mode === 'hp') return input - k * v1 - v2;
    return v2;
  }
}

export interface StereoBuf {
  l: Float32Array;
  r: Float32Array;
}

export function makeStereo(samples: number): StereoBuf {
  return { l: new Float32Array(samples), r: new Float32Array(samples) };
}

function addSample(buf: StereoBuf, idx: number, v: number, pan = 0): void {
  if (idx < 0 || idx >= buf.l.length) return;
  const p = Math.max(-1, Math.min(1, pan));
  const gl = Math.cos(((p + 1) * Math.PI) / 4);
  const gr = Math.sin(((p + 1) * Math.PI) / 4);
  buf.l[idx] += v * gl;
  buf.r[idx] += v * gr;
}

export function addKick(buf: StereoBuf, startSec: number, gain = 0.85, punchHz = 155, endHz = 44): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor(0.28 * SR);
  let phase = 0;
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    const freq = endHz + (punchHz - endHz) * Math.exp(-t * 34);
    phase += (TWO_PI * freq) / SR;
    const env = Math.exp(-t * 11) * (1 - Math.exp(-t * 450));
    const click = Math.exp(-t * 180) * 0.35;
    const s = Math.tanh((Math.sin(phase) * 1.35 + click) * env) * gain;
    addSample(buf, start + i, s, 0);
  }
}

export function addSnare(buf: StereoBuf, startSec: number, gain = 0.65, rng: Rng, bright = 1.0): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor(0.22 * SR);
  const svf = new SvfFilter();
  let phase = 0;
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    const freq = 185 * Math.exp(-t * 12) + 115;
    phase += (TWO_PI * freq) / SR;
    const body = Math.sin(phase) * Math.exp(-t * 22) * 0.55;
    const noise = svf.process(rng.signed(), 2600 * bright, 0.9, 'hp') * Math.exp(-t * 15) * 0.85;
    addSample(buf, start + i, (body + noise) * gain, (rng.uniform() - 0.5) * 0.08);
  }
}

export function addClap(buf: StereoBuf, startSec: number, gain = 0.5, rng: Rng): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor(0.2 * SR);
  const svf = new SvfFilter();
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    let env = 0;
    if (t < 0.032) {
      env = Math.pow(Math.sin((t / 0.011) * Math.PI), 2);
    } else {
      env = Math.exp(-(t - 0.032) * 20);
    }
    const n = svf.process(rng.signed(), 1600, 1.4, 'bp');
    addSample(buf, start + i, n * env * gain * 1.4, 0.05);
  }
}

export function addHat(buf: StereoBuf, startSec: number, open: boolean, gain = 0.25, rng: Rng, pan = 0.15): void {
  const start = Math.floor(startSec * SR);
  const dur = open ? 0.28 : 0.055;
  const len = Math.floor(dur * SR);
  const svf = new SvfFilter();
  const decay = open ? 9.5 : 52;
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    const n = svf.process(rng.signed(), 6200, 1.1, 'hp');
    const env = Math.exp(-t * decay);
    addSample(buf, start + i, n * env * gain, pan);
  }
}

export function addCrash(buf: StereoBuf, startSec: number, gain = 0.3, rng: Rng): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor(1.1 * SR);
  const svf = new SvfFilter();
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    const n = svf.process(rng.signed(), 4500, 0.8, 'hp');
    const env = Math.exp(-t * 3.1) * (1 - Math.exp(-t * 160));
    addSample(buf, start + i, n * env * gain, -0.2);
  }
}

export function addBass(
  buf: StereoBuf,
  startSec: number,
  durSec: number,
  midi: number,
  gain = 0.55,
  grit = 1.2,
  pan = 0,
): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor((durSec + 0.06) * SR);
  const freq = midiToHz(midi);
  const dt = freq / SR;
  let ph1 = 0;
  let ph2 = 0.25;
  const svf = new SvfFilter();
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    ph1 = (ph1 + dt) % 1;
    ph2 = (ph2 + dt * 0.5) % 1;
    const raw = sawSample(ph1, dt) * 0.65 + Math.sin(ph2 * TWO_PI) * 0.55;
    const cut = 260 + 1300 * Math.exp(-t * 8.5);
    const f = svf.process(raw, cut, 1.3, 'lp');
    const att = Math.min(1, t / 0.008);
    const rel = t > durSec ? Math.max(0, 1 - (t - durSec) / 0.06) : Math.exp(-t * 1.4);
    addSample(buf, start + i, Math.tanh(f * grit) * att * rel * gain, pan);
  }
}

export function addPadNote(
  buf: StereoBuf,
  startSec: number,
  durSec: number,
  midi: number,
  gain = 0.16,
  pan = 0,
): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor((durSec + 0.25) * SR);
  const freq = midiToHz(midi);
  const dt1 = (freq * 0.996) / SR;
  const dt2 = (freq * 1.004) / SR;
  let p1 = 0;
  let p2 = 0.3;
  const svf = new SvfFilter();
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    p1 = (p1 + dt1) % 1;
    p2 = (p2 + dt2) % 1;
    const s = (sawSample(p1, dt1) + sawSample(p2, dt2)) * 0.5;
    const filt = svf.process(s, 1100 + 500 * Math.sin(t * 2.5), 0.75, 'lp');
    const att = Math.min(1, t / 0.14);
    const rel = t > durSec ? Math.max(0, 1 - (t - durSec) / 0.25) : 1;
    addSample(buf, start + i, filt * att * rel * gain, pan);
  }
}

export function addPluck(
  buf: StereoBuf,
  startSec: number,
  durSec: number,
  midi: number,
  gain = 0.26,
  pan = 0,
  wave: 'pulse' | 'saw' = 'pulse',
): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor((durSec + 0.08) * SR);
  const freq = midiToHz(midi);
  const dt = freq / SR;
  let ph = 0;
  const svf = new SvfFilter();
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    ph = (ph + dt) % 1;
    const raw = wave === 'saw' ? sawSample(ph, dt) : pulseSample(ph, dt, 0.35);
    const cut = 550 + 2800 * Math.exp(-t * 14);
    const s = svf.process(raw, cut, 1.1, 'lp');
    const att = Math.min(1, t / 0.004);
    const dec = Math.exp(-t * 6.5);
    const rel = t > durSec ? Math.max(0, 1 - (t - durSec) / 0.08) : 1;
    addSample(buf, start + i, s * att * dec * rel * gain, pan);
  }
}

export function addBell(buf: StereoBuf, startSec: number, durSec: number, midi: number, gain = 0.2, pan = 0): void {
  const start = Math.floor(startSec * SR);
  const len = Math.floor((durSec + 0.2) * SR);
  const f = midiToHz(midi);
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    const mod = Math.sin(TWO_PI * f * 2.0 * t) * Math.exp(-t * 9) * 1.4;
    const car = Math.sin(TWO_PI * f * t + mod);
    const env = Math.exp(-t * 4.2) * Math.min(1, t / 0.004);
    addSample(buf, start + i, car * env * gain, pan);
  }
}

// Formant-filtered expressive character vocal synth (distinct timbre per character)
export interface VoiceProfile {
  waveform: 'saw' | 'pulse' | 'fm';
  f1: number;
  f2: number;
  vibratoHz: number;
  vibratoCents: number;
  grit: number;
  pan: number;
}

export function voiceProfileFor(charId: string, isPlayer: boolean): VoiceProfile {
  if (isPlayer || charId === 'herald') {
    return { waveform: 'pulse', f1: 780, f2: 2150, vibratoHz: 5.8, vibratoCents: 14, grit: 1.15, pan: 0.22 };
  }
  switch (charId) {
    case 'principality':
      return { waveform: 'saw', f1: 580, f2: 1750, vibratoHz: 5.2, vibratoCents: 18, grit: 1.3, pan: -0.22 };
    case 'hand':
      return { waveform: 'fm', f1: 680, f2: 2400, vibratoHz: 6.4, vibratoCents: 12, grit: 1.45, pan: -0.22 };
    case 'demise':
      return { waveform: 'saw', f1: 480, f2: 1500, vibratoHz: 6.8, vibratoCents: 22, grit: 2.1, pan: -0.22 };
    default:
      return { waveform: 'pulse', f1: 620, f2: 1900, vibratoHz: 5.5, vibratoCents: 15, grit: 1.25, pan: -0.22 };
  }
}

export function addVoiceNote(
  buf: StereoBuf,
  startSec: number,
  durSec: number,
  midi: number,
  profile: VoiceProfile,
  vowelIdx: number,
  gain = 0.44,
): void {
  const start = Math.floor(startSec * SR);
  const dur = Math.max(0.08, durSec);
  const len = Math.floor((dur + 0.05) * SR);
  const baseHz = midiToHz(midi);
  const svf1 = new SvfFilter();
  const svf2 = new SvfFilter();
  const vowelMul = [
    [1.0, 1.0],
    [0.78, 1.22],
    [1.18, 0.86],
    [0.9, 1.1],
  ][vowelIdx & 3];
  const f1 = profile.f1 * vowelMul[0];
  const f2 = profile.f2 * vowelMul[1];

  let ph = 0;
  for (let i = 0; i < len; i++) {
    const t = i / SR;
    // Brief pitch scoop at note attack like FNF vocals
    const scoop = Math.pow(2, (-1.8 * Math.exp(-t * 36)) / 12);
    const vib = Math.pow(2, (Math.sin(TWO_PI * profile.vibratoHz * t) * profile.vibratoCents * Math.min(1, t / 0.14)) / 1200);
    const hz = baseHz * scoop * vib;
    const dt = hz / SR;
    ph = (ph + dt) % 1;

    let osc = 0;
    if (profile.waveform === 'pulse') {
      osc = pulseSample(ph, dt, 0.42) * 0.7 + sawSample(ph, dt) * 0.3;
    } else if (profile.waveform === 'fm') {
      osc = Math.sin(ph * TWO_PI + Math.sin(ph * TWO_PI * 2.0) * 1.35);
    } else {
      osc = sawSample(ph, dt);
    }

    const v1 = svf1.process(osc, f1, 2.6, 'bp');
    const v2 = svf2.process(osc, f2, 3.0, 'bp');
    const body = osc * 0.25 + v1 * 0.85 + v2 * 0.55;
    const att = Math.min(1, t / 0.014);
    const rel = t > dur ? Math.max(0, 1 - (t - dur) / 0.05) : 1;
    const out = Math.tanh(body * profile.grit) * att * rel * gain;
    addSample(buf, start + i, out, profile.pan);
  }
}

// Lightweight Schroeder reverb (4 parallel combs + stereo spread)
export function applyReverb(buf: StereoBuf, wet = 0.14): void {
  const n = buf.l.length;
  const delays = [677, 811, 967, 1123];
  const g = 0.66;
  const outL = new Float32Array(n);
  const outR = new Float32Array(n);

  for (let c = 0; c < delays.length; c++) {
    const d = delays[c];
    const line = new Float32Array(d);
    let idx = 0;
    const target = c % 2 === 0 ? outL : outR;
    for (let i = 0; i < n; i++) {
      const inp = (buf.l[i] + buf.r[i]) * 0.5;
      const y = line[idx];
      line[idx] = inp + y * g;
      idx++;
      if (idx >= d) idx = 0;
      target[i] += y * 0.5;
    }
  }
  const dry = 1 - wet * 0.4;
  for (let i = 0; i < n; i++) {
    buf.l[i] = buf.l[i] * dry + outL[i] * wet;
    buf.r[i] = buf.r[i] * dry + outR[i] * wet;
  }
}

export function normalizeStereo(buf: StereoBuf, peakTarget = 0.92): void {
  let peak = 0.001;
  for (let i = 0; i < buf.l.length; i++) {
    const a = Math.abs(buf.l[i]);
    const b = Math.abs(buf.r[i]);
    if (a > peak) peak = a;
    if (b > peak) peak = b;
  }
  const scale = peakTarget / peak;
  for (let i = 0; i < buf.l.length; i++) {
    buf.l[i] = Math.tanh(buf.l[i] * scale);
    buf.r[i] = Math.tanh(buf.r[i] * scale);
  }
}
