// Spectral-flux AutoChart engine ported from src/autochart.cpp
// Radix-2 FFT, spectral flux onset detection, autocorrelation BPM estimation, spectral centroid 4K lane mapping
import { Chart, CharId, Difficulty, Note, SectionInfo } from './types';

function fftInPlace(re: Float32Array, im: Float32Array): void {
  const n = re.length;
  let j = 0;
  for (let i = 1; i < n; i++) {
    let bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j) {
      const tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      const ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wRe = Math.cos(ang);
    const wIm = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let curRe = 1;
      let curIm = 0;
      const half = len >> 1;
      for (let k = 0; k < half; k++) {
        const uRe = re[i + k];
        const uIm = im[i + k];
        const vRe = re[i + k + half] * curRe - im[i + k + half] * curIm;
        const vIm = re[i + k + half] * curIm + im[i + k + half] * curRe;
        re[i + k] = uRe + vRe;
        im[i + k] = uIm + vIm;
        re[i + k + half] = uRe - vRe;
        im[i + k + half] = uIm - vIm;
        const nextRe = curRe * wRe - curIm * wIm;
        curIm = curRe * wIm + curIm * wRe;
        curRe = nextRe;
      }
    }
  }
}

export function audioBufferToMono(buf: AudioBuffer): { mono: Float32Array; sampleRate: number } {
  const len = buf.length;
  const mono = new Float32Array(len);
  const ch = buf.numberOfChannels;
  for (let c = 0; c < ch; c++) {
    const data = buf.getChannelData(c);
    for (let i = 0; i < len; i++) {
      mono[i] += data[i] / ch;
    }
  }
  return { mono, sampleRate: buf.sampleRate };
}

export interface AutoChartOptions {
  title: string;
  difficulty: Difficulty;
  opponent?: CharId;
  stage?: number;
  accentHex?: number;
  knownBpm?: number;
}

export function autoChartFromMono(mono: Float32Array, sampleRate: number, opts: AutoChartOptions): Chart {
  // Downsample to ~22050 Hz if higher to keep FFT fast and consistent
  let data = mono;
  let sr = sampleRate;
  if (sr > 30000) {
    const dec = Math.round(sr / 22050);
    const out = new Float32Array(Math.floor(mono.length / dec));
    for (let i = 0; i < out.length; i++) out[i] = mono[i * dec];
    data = out;
    sr = sr / dec;
  }

  const winSize = 1024;
  const hop = 256;
  const half = winSize >> 1;
  const numFrames = Math.max(0, Math.floor((data.length - winSize) / hop));
  const hann = new Float32Array(winSize);
  for (let i = 0; i < winSize; i++) {
    hann[i] = 0.5 * (1 - Math.cos((2 * Math.PI * i) / (winSize - 1)));
  }

  const re = new Float32Array(winSize);
  const im = new Float32Array(winSize);
  const prevMag = new Float32Array(half);
  const flux = new Float32Array(numFrames);
  const centroid = new Float32Array(numFrames);
  const energy = new Float32Array(numFrames);

  for (let f = 0; f < numFrames; f++) {
    const off = f * hop;
    for (let i = 0; i < winSize; i++) {
      re[i] = data[off + i] * hann[i];
      im[i] = 0;
    }
    fftInPlace(re, im);

    let fl = 0;
    let cNum = 0;
    let cDen = 1e-6;
    let en = 0;
    for (let k = 1; k < half; k++) {
      const mag = Math.hypot(re[k], im[k]);
      const diff = mag - prevMag[k];
      if (diff > 0) fl += diff;
      prevMag[k] = mag;
      cNum += k * mag;
      cDen += mag;
      en += mag * mag;
    }
    flux[f] = fl;
    centroid[f] = cNum / cDen;
    energy[f] = Math.sqrt(en / half);
  }

  // Adaptive local threshold for onset peaks
  const frameSec = hop / sr;
  const winHalf = 10;
  const diffThresh = opts.difficulty === 0 ? 1.85 : opts.difficulty === 1 ? 1.48 : opts.difficulty === 2 ? 1.22 : 1.02;
  const minGapSec = opts.difficulty === 0 ? 0.24 : opts.difficulty === 1 ? 0.16 : opts.difficulty === 2 ? 0.11 : 0.075;

  const onsets: { sec: number; strength: number; cent: number; en: number }[] = [];
  let lastSec = -1;
  for (let f = 2; f < numFrames - 2; f++) {
    let mean = 0;
    const lo = Math.max(0, f - winHalf);
    const hi = Math.min(numFrames - 1, f + winHalf);
    for (let k = lo; k <= hi; k++) mean += flux[k];
    mean /= hi - lo + 1;

    const val = flux[f];
    if (val > mean * diffThresh && val > 0.35 && val >= flux[f - 1] && val > flux[f + 1]) {
      const sec = f * frameSec;
      if (sec - lastSec >= minGapSec && sec > 0.25) {
        onsets.push({ sec, strength: val / Math.max(1e-4, mean), cent: centroid[f], en: energy[f] });
        lastSec = sec;
      }
    }
  }

  // Autocorrelation BPM estimation over flux envelope (75..195 BPM)
  let bpm = opts.knownBpm && opts.knownBpm > 40 ? opts.knownBpm : 120;
  if (!opts.knownBpm && numFrames > 120) {
    const minLag = Math.max(4, Math.floor((60 / 195) / frameSec));
    const maxLag = Math.min(Math.floor(numFrames / 3), Math.ceil((60 / 75) / frameSec));
    let bestLag = Math.round((60 / 120) / frameSec);
    let bestScore = -1;
    for (let lag = minLag; lag <= maxLag; lag++) {
      let acc = 0;
      const limit = Math.min(numFrames - lag, 2400);
      for (let i = 0; i < limit; i++) {
        acc += flux[i] * flux[i + lag];
      }
      // Slight preference toward 110..150 BPM range
      const candBpm = 60 / (lag * frameSec);
      const bias = 1 - Math.abs(candBpm - 128) * 0.002;
      const score = acc * bias;
      if (score > bestScore) {
        bestScore = score;
        bestLag = lag;
      }
    }
    bpm = Math.round(60 / (bestLag * frameSec));
    if (bpm < 80) bpm *= 2;
    if (bpm > 195) bpm = Math.round(bpm / 2);
  }

  // Quantize onsets to 16th-note grid & assign alternating call-and-response sections
  const beatSec = 60 / bpm;
  const stepSec = beatSec / 4;
  const barSec = beatSec * 4;
  const durationSec = data.length / sr;
  const totalBars = Math.max(4, Math.ceil(durationSec / barSec));

  const sections: SectionInfo[] = [];
  for (let b = 0; b < totalBars; b++) {
    const pair = Math.floor(b / 2);
    sections.push({
      startMs: b * barSec * 1000,
      lengthSteps: 16,
      mustHitSection: pair % 2 === 1,
    });
  }

  // Compute centroid quartiles for lane mapping
  const sortedCent = onsets.map((o) => o.cent).sort((a, b) => a - b);
  const q1 = sortedCent[Math.floor(sortedCent.length * 0.25)] ?? 20;
  const q2 = sortedCent[Math.floor(sortedCent.length * 0.5)] ?? 45;
  const q3 = sortedCent[Math.floor(sortedCent.length * 0.75)] ?? 80;

  const notes: Note[] = [];
  const usedSlots = new Set<string>();
  let lastDir = 0;

  for (let i = 0; i < onsets.length; i++) {
    const o = onsets[i];
    const snappedStep = Math.round(o.sec / stepSec);
    const timeMs = snappedStep * stepSec * 1000;
    const barIdx = Math.min(sections.length - 1, Math.floor(timeMs / (barSec * 1000)));
    const side: 0 | 1 = sections[barIdx]?.mustHitSection ? 1 : 0;

    // Map spectral centroid to lane 0..3, avoiding repetitive jacks on Easy/Normal
    let dir = o.cent < q1 ? 0 : o.cent < q2 ? 1 : o.cent < q3 ? 2 : 3;
    if (dir === lastDir && opts.difficulty <= 1) {
      dir = (dir + 1) & 3;
    }
    lastDir = dir;

    const slotKey = `${side}:${snappedStep}:${dir}`;
    if (usedSlots.has(slotKey)) continue;
    usedSlots.add(slotKey);

    // Detect sustain holds when gap to next onset is wide and energy stays high
    let lengthMs = 0;
    const nextSec = i + 1 < onsets.length ? onsets[i + 1].sec : durationSec;
    const gapSec = nextSec - o.sec;
    if (gapSec >= beatSec * 0.85 && o.strength > 1.7) {
      lengthMs = Math.min(beatSec * 1.5, gapSec * 0.65) * 1000;
    }

    notes.push({ timeMs, dir, side, lengthMs });

    // Add jump chords on strong downbeats for Hard/Infernal
    if (opts.difficulty >= 2 && o.strength > 2.35 && snappedStep % 4 === 0) {
      const dir2 = (dir + 2) & 3;
      const key2 = `${side}:${snappedStep}:${dir2}`;
      if (!usedSlots.has(key2)) {
        usedSlots.add(key2);
        notes.push({ timeMs, dir: dir2, side, lengthMs: 0 });
      }
    }
  }

  notes.sort((a, b) => a.timeMs - b.timeMs || a.dir - b.dir);
  const baseSpeed = 2.4 + opts.difficulty * 0.28;

  return {
    title: opts.title,
    bpm,
    speed: +baseSpeed.toFixed(2),
    lanes: 4,
    notes,
    tempoChanges: [{ timeMs: 0, bpm }],
    sections,
    opponent: opts.opponent ?? 'shade',
    player: 'herald',
    stage: opts.stage ?? 3,
    accentHex: opts.accentHex ?? 0x36b5ff,
    difficulty: opts.difficulty,
  };
}
