// Modern tinted-glass FFT Audio Visualizer ported from src/visualizer.cpp (Issue #31)
import { RGBColor } from './types';
import { clamp, COLOR_WHITE, gradientRect, mix, rgba, roundRect, saturate } from './gfx';

function rgbToHsv(c: RGBColor): [number, number, number] {
  const r = c.r / 255;
  const g = c.g / 255;
  const b = c.b / 255;
  const mx = Math.max(r, g, b);
  const mn = Math.min(r, g, b);
  const d = mx - mn;
  let h = 0;
  if (d > 1e-5) {
    if (mx === r) h = 60 * (((g - b) / d) % 6);
    else if (mx === g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
  }
  if (h < 0) h += 360;
  return [h, mx > 1e-5 ? d / mx : 0, mx];
}

function hsvToRgb(h: number, s: number, v: number): RGBColor {
  const hh = ((h % 360) + 360) % 360;
  const c = v * s;
  const x = c * (1 - Math.abs(((hh / 60) % 2) - 1));
  const m = v - c;
  let r = 0;
  let g = 0;
  let b = 0;
  if (hh < 60) [r, g, b] = [c, x, 0];
  else if (hh < 120) [r, g, b] = [x, c, 0];
  else if (hh < 180) [r, g, b] = [0, c, x];
  else if (hh < 240) [r, g, b] = [0, x, c];
  else if (hh < 300) [r, g, b] = [x, 0, c];
  else [r, g, b] = [c, 0, x];
  return { r: Math.round((r + m) * 255), g: Math.round((g + m) * 255), b: Math.round((b + m) * 255), a: 255 };
}

function ease(now: number, target: number, dt: number, rate: number): number {
  return now + (target - now) * (1 - Math.exp(-dt * rate));
}

export class AudioVisualizer {
  private readonly bars = 32;
  private levels = new Float32Array(32);
  private peaks = new Float32Array(32);
  private peakHold = new Float32Array(32);
  private bassVal = 0;
  private t = 0;
  private baseRgb: [number, number, number] = [0.2, 0.72, 0.94];
  private sideCols: [RGBColor, RGBColor] = [
    { r: 164, g: 100, b: 255, a: 255 },
    { r: 48, g: 182, b: 240, a: 255 },
  ];
  private heat = 0;
  private spam = 0;
  private danger = 0;
  private hitPulse = 0;
  private oppPulse = 0;
  private missPulse = 0;
  private beatPulse = 0;
  private hitColor: RGBColor = { r: 255, g: 246, b: 200, a: 255 };

  hit(rating: number): void {
    const kRating: RGBColor[] = [
      { r: 255, g: 246, b: 200, a: 255 },
      { r: 150, g: 255, b: 180, a: 255 },
      { r: 255, g: 196, b: 110, a: 255 },
      { r: 255, g: 130, b: 130, a: 255 },
    ];
    this.hitColor = kRating[clamp(rating, 0, 3)];
    this.hitPulse = 1;
  }

  opponentHit(): void {
    this.oppPulse = Math.max(this.oppPulse, 0.7);
  }

  miss(): void {
    this.missPulse = 1;
  }

  beat(): void {
    this.beatPulse = 1;
  }

  update(
    dt: number,
    fft: Uint8Array,
    playing: boolean,
    focusSide: number,
    side0: RGBColor,
    side1: RGBColor,
    nps: number,
    health: number,
  ): void {
    this.t += dt;
    this.sideCols = [side0, side1];

    // Map FFT bins logarithmically across 32 bars
    let bassAcc = 0;
    const maxBin = fft.length - 1;
    for (let b = 0; b < this.bars; b++) {
      let target = 0;
      if (playing && maxBin > 0) {
        const f0 = Math.pow(b / this.bars, 1.85);
        const f1 = Math.pow((b + 1) / this.bars, 1.85);
        const i0 = Math.max(1, Math.floor(f0 * maxBin * 0.75));
        const i1 = Math.max(i0 + 1, Math.floor(f1 * maxBin * 0.75));
        let sum = 0;
        for (let i = i0; i < i1 && i <= maxBin; i++) sum += fft[i] / 255;
        target = saturate((sum / Math.max(1, i1 - i0)) * 1.18);
      }
      if (target > this.levels[b]) {
        this.levels[b] = ease(this.levels[b], target, dt, 26);
      } else {
        this.levels[b] = Math.max(0, this.levels[b] - dt * 1.65);
      }
      if (this.levels[b] >= this.peaks[b]) {
        this.peaks[b] = this.levels[b];
        this.peakHold[b] = 0.28;
      } else if (this.peakHold[b] > 0) {
        this.peakHold[b] -= dt;
      } else {
        this.peaks[b] = Math.max(0, this.peaks[b] - dt * 0.85);
      }
      if (b < 4) bassAcc += this.levels[b];
    }
    this.bassVal = ease(this.bassVal, bassAcc / 4, dt, 14);

    const active = focusSide === 1 ? side1 : side0;
    const [h, s, v] = rgbToHsv(active);
    const bright = hsvToRgb(h, Math.max(s, 0.45), Math.max(v, 0.85));
    this.baseRgb[0] = ease(this.baseRgb[0], bright.r / 255, dt, 5);
    this.baseRgb[1] = ease(this.baseRgb[1], bright.g / 255, dt, 5);
    this.baseRgb[2] = ease(this.baseRgb[2], bright.b / 255, dt, 5);

    this.heat = ease(this.heat, saturate(nps / 24), dt, 3);
    this.spam = ease(this.spam, saturate((nps - 40) / 60), dt, 2);
    this.danger = ease(this.danger, saturate((0.5 - health) / 0.5), dt, 4);
    this.hitPulse *= Math.exp(-dt * 10);
    this.oppPulse *= Math.exp(-dt * 8);
    this.missPulse *= Math.exp(-dt * 5);
    this.beatPulse *= Math.exp(-dt * 6);
  }

  draw(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, alpha = 1): void {
    if (alpha <= 0.01 || w < 8 || h < 8) return;
    const base: RGBColor = {
      r: Math.round(this.baseRgb[0] * 255),
      g: Math.round(this.baseRgb[1] * 255),
      b: Math.round(this.baseRgb[2] * 255),
      a: 255,
    };
    const [hh, ss] = rgbToHsv(base);
    const top = hsvToRgb(hh + 25 + 70 * this.heat, Math.min(1, ss * 0.7 + 0.25), 1);
    const bottom = hsvToRgb(hh, Math.min(1, ss + 0.1), 0.5 + 0.25 * this.heat);
    const danger = this.danger * (0.45 + 0.55 * this.beatPulse);
    const bass = saturate(this.bassVal * 1.4);

    // Tinted glass panel
    const px = x - 9;
    const py = y - 6;
    const pw = w + 18;
    const ph = h + 12;
    roundRect(ctx, px, py, pw, ph, 12);
    ctx.fillStyle = rgba(mix({ r: 8, g: 12, b: 34, a: 255 }, base, 0.1 + 0.12 * this.beatPulse), (0.48 + 0.06 * this.beatPulse) * alpha);
    ctx.fill();
    // Top glass highlight
    gradientRect(ctx, px + 8, py + 1.5, pw - 16, ph * 0.42, { r: 255, g: 255, b: 255, a: Math.round(35 * alpha) }, { r: 255, g: 255, b: 255, a: 0 });
    // Rim lit by bass
    roundRect(ctx, px, py, pw, ph, 12);
    ctx.lineWidth = 1.4;
    ctx.strokeStyle = rgba(mix(mix(COLOR_WHITE, base, 0.55), { r: 255, g: 60, b: 80, a: 255 }, danger), (0.3 + 0.45 * bass + 0.15 * this.beatPulse) * alpha);
    ctx.stroke();

    const floorY = y + h * 0.8;
    const room = floorY - y;
    const below = y + h - floorY;
    const gap = 2;
    const bw = Math.max(2, (w - gap * (this.bars - 1)) / this.bars);

    const tipPts: [number, number][] = [];
    for (let b = 0; b < this.bars; b++) {
      let ct = top;
      let cb = bottom;
      if (this.spam > 0.01) {
        const hue = (this.t * 140 + b * 9) % 360;
        ct = mix(ct, hsvToRgb(hue, 0.7, 1), this.spam);
        cb = mix(cb, hsvToRgb(hue + 40, 0.8, 0.8), this.spam * 0.8);
      }
      ct = mix(ct, this.hitColor, 0.55 * this.hitPulse);
      cb = mix(cb, this.sideCols[0], 0.4 * this.oppPulse);
      ct = mix(ct, { r: 255, g: 50, b: 70, a: 255 }, 0.75 * this.missPulse);
      cb = mix(cb, { r: 110, g: 0, b: 20, a: 255 }, 0.6 * this.missPulse);
      ct = mix(ct, { r: 255, g: 70, b: 70, a: 255 }, 0.7 * danger);
      cb = mix(cb, { r: 120, g: 0, b: 24, a: 255 }, danger);

      const bx = x + b * (bw + gap) + this.missPulse * 3 * Math.sin(b * 2.3 + this.t * 70);
      const bh = Math.max(bw, this.levels[b] * room);
      const barTop = floorY - bh;

      // Rounded pill bar
      const g = ctx.createLinearGradient(0, barTop, 0, floorY);
      g.addColorStop(0, rgba(ct, 0.96 * alpha));
      g.addColorStop(1, rgba(cb, 0.88 * alpha));
      roundRect(ctx, bx, barTop, bw, bh, bw * 0.5);
      ctx.fillStyle = g;
      ctx.fill();

      // Gloss strip
      if (bh > bw * 2) {
        ctx.fillStyle = rgba(COLOR_WHITE, 0.22 * alpha);
        ctx.fillRect(bx + bw * 0.22, barTop + bw * 0.5, Math.max(1, bw * 0.2), bh - bw);
      }

      // Reflection below floor
      const rh = Math.min(bh * 0.45, below - 2);
      if (rh > 1) {
        gradientRect(ctx, bx, floorY + 2, bw, rh, { ...cb, a: Math.round(78 * alpha) }, { ...cb, a: 0 });
      }

      tipPts.push([bx + bw * 0.5, barTop]);

      // Floating peak dot
      const pk = this.peaks[b];
      if (pk > 0.03 && pk * room > bh + bw) {
        ctx.fillStyle = rgba(mix(ct, COLOR_WHITE, 0.4), 0.9 * alpha);
        ctx.beginPath();
        ctx.arc(bx + bw * 0.5, floorY - pk * room - bw * 0.5, bw * 0.36, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // Smooth glow curve over bar tips
    if (tipPts.length >= 4) {
      const glowC = mix(top, COLOR_WHITE, 0.35);
      ctx.save();
      ctx.beginPath();
      ctx.moveTo(tipPts[0][0], tipPts[0][1]);
      for (let i = 1; i < tipPts.length - 1; i++) {
        const mx = (tipPts[i][0] + tipPts[i + 1][0]) * 0.5;
        const my = (tipPts[i][1] + tipPts[i + 1][1]) * 0.5;
        ctx.quadraticCurveTo(tipPts[i][0], tipPts[i][1], mx, my);
      }
      ctx.strokeStyle = rgba(glowC, 0.55 * alpha);
      ctx.lineWidth = 1.6;
      ctx.stroke();
      ctx.restore();
    }

    // Glowing floor line
    const floorC = mix(mix(base, COLOR_WHITE, 0.35 + 0.3 * this.beatPulse), { r: 255, g: 70, b: 70, a: 255 }, danger);
    const fa = (0.55 + 0.45 * bass) * alpha;
    const fg = ctx.createLinearGradient(x, 0, x + w, 0);
    fg.addColorStop(0, rgba(floorC, 0));
    fg.addColorStop(0.5, rgba(floorC, fa));
    fg.addColorStop(1, rgba(floorC, 0));
    ctx.fillStyle = fg;
    ctx.fillRect(x, floorY, w, 2);
  }
}
