// Procedural stages ported from src/stage.cpp
// 0: School Rooftop at Sunset, 1: The Club, 2: Punk Back Alley, 3: Synthwave Night
import { RGBColor } from './types';
import {
  CANVAS_H,
  CANVAS_W,
  COLOR_WHITE,
  drawCube,
  drawGlow,
  drawSoftCircle,
  drawStar,
  drawText,
  fade,
  gradientRect,
  mix,
  ParticleSystem,
  rgba,
  roundRect,
  saturate,
} from './gfx';
import { Rng } from './synth';

const K_INK: RGBColor = { r: 24, g: 16, b: 36, a: 255 };
const FULL_X = -320;
const FULL_Y = -260;
const FULL_W = 1920;
const FULL_H = 1240;

function hash3(a: number, b: number, c: number): number {
  let h = (Math.imul(a | 0, 73856093) ^ Math.imul(b | 0, 19349663) ^ Math.imul(c | 0, 83492791)) >>> 0;
  h ^= h >>> 13;
  h = Math.imul(h, 0x5bd1e995) >>> 0;
  h ^= h >>> 15;
  return h >>> 0;
}

function inkRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, c: RGBColor, line = 4): void {
  ctx.fillStyle = rgba(K_INK);
  ctx.fillRect(x - line, y - line, w + line * 2, h + line * 2);
  ctx.fillStyle = rgba(c);
  ctx.fillRect(x, y, w, h);
}

function inkCircle(ctx: CanvasRenderingContext2D, cx: number, cy: number, r: number, c: RGBColor, line = 4): void {
  ctx.fillStyle = rgba(K_INK);
  ctx.beginPath();
  ctx.arc(cx, cy, r + line, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = rgba(c);
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.fill();
}

function drawSpeaker(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, pump: number): void {
  inkRect(ctx, x, y, w, h, { r: 40, g: 36, b: 58, a: 255 });
  const cx = x + w * 0.5;
  const big = Math.min(w, h * 0.5) * 0.38;
  for (let i = 0; i < 2; i++) {
    const cy = y + h * (0.27 + 0.48 * i);
    const rr = big * (i === 0 ? 0.72 : 1.0) * (1 + 0.08 * pump);
    inkCircle(ctx, cx, cy, rr, { r: 72, g: 68, b: 94, a: 255 }, 3);
    ctx.fillStyle = 'rgb(24,20,36)';
    ctx.beginPath();
    ctx.arc(cx, cy, rr * 0.45, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = 'rgba(255,255,255,0.35)';
    ctx.beginPath();
    ctx.arc(cx - rr * 0.22, cy - rr * 0.22, rr * 0.12, 0, Math.PI * 2);
    ctx.fill();
  }
}

function drawAmp(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number): void {
  inkRect(ctx, x, y, w, h, { r: 30, g: 26, b: 36, a: 255 });
  ctx.fillStyle = 'rgb(56,50,62)';
  ctx.fillRect(x + 10, y + 34, w - 20, h - 44);
  ctx.fillStyle = 'rgb(206,186,124)';
  ctx.fillRect(x + 10, y + 8, w - 20, 18);
  ctx.fillStyle = rgba(K_INK);
  for (let k = 0; k < 5; k++) {
    ctx.beginPath();
    ctx.arc(x + 24 + (k * (w - 48)) / 4, y + 17, 5, 0, Math.PI * 2);
    ctx.fill();
  }
}

function drawCloud(ctx: CanvasRenderingContext2D, x: number, y: number, s: number, col: RGBColor): void {
  ctx.fillStyle = rgba(col);
  ctx.beginPath();
  ctx.arc(x, y, 34 * s, 0, Math.PI * 2);
  ctx.arc(x + 42 * s, y - 16 * s, 46 * s, 0, Math.PI * 2);
  ctx.arc(x + 92 * s, y, 32 * s, 0, Math.PI * 2);
  ctx.fill();
  roundRect(ctx, x - 30 * s, y - 2 * s, 152 * s, 34 * s, 17 * s);
  ctx.fill();
}

export class Stage {
  private kind = 0;
  private accent: RGBColor = { r: 255, g: 160, b: 64, a: 255 };
  private low = false;
  private flashing = true;
  private pulse = 0;
  private flash = 0;
  private beat = 0;
  private spawn = 0;
  private motes = new ParticleSystem();
  private stars: { x: number; y: number; s: number; ph: number }[] = [];
  private far: { x: number; w: number; h: number }[] = [];
  private near: { x: number; w: number; h: number }[] = [];

  init(kind: number, accent: RGBColor, lowDetail: boolean, flashing: boolean): void {
    this.kind = kind;
    this.accent = accent;
    this.low = lowDetail;
    this.flashing = flashing;
    this.pulse = 0;
    this.flash = 0;
    this.beat = 0;
    this.motes.clear();
    this.stars = [];
    this.far = [];
    this.near = [];

    const rng = new Rng(0x51a7 + kind);
    for (let i = 0; i < 160; i++) {
      const u = rng.uniform();
      this.stars.push({
        x: rng.uniform() * FULL_W + FULL_X,
        y: rng.uniform() * 780 + FULL_Y,
        s: 1.4 + u * u * 6.0,
        ph: rng.uniform() * Math.PI * 2,
      });
    }
    for (let x = FULL_X; x < FULL_X + FULL_W; ) {
      const w = 70 + rng.uniform() * 90;
      this.far.push({ x, w, h: 150 + rng.uniform() * 190 });
      x += w + rng.uniform() * 14;
    }
    for (let x = FULL_X; x < FULL_X + FULL_W; ) {
      const w = 90 + rng.uniform() * 120;
      this.near.push({ x, w, h: 60 + rng.uniform() * 120 });
      x += w + 10 + rng.uniform() * 60;
    }
  }

  onBeat(b: number): void {
    this.pulse = 1;
    this.beat = b;
    if (this.kind === 2 && this.flashing && b % 16 === 8) this.flash = 0.45;
  }

  update(dt: number): void {
    this.flash = Math.max(0, this.flash - dt * 2.5);
    this.pulse = Math.max(0, this.pulse - dt * 3.0);
    this.motes.update(dt, this.kind === 2 ? -20 : 0);
    if (this.low) return;
    this.spawn += dt * (this.kind === 2 ? 22 : 10);
    while (this.spawn >= 1) {
      this.spawn -= 1;
      if (this.kind === 0) {
        this.motes.add({
          x: -100 + Math.random() * 1480,
          y: 200 + Math.random() * 500,
          vx: -14 + Math.random() * 28,
          vy: -20 + Math.random() * 14,
          c: { r: 255, g: 222, b: 170, a: 145 },
          size: 3 + Math.random() * 4,
          life: 4 + Math.random() * 3,
          maxLife: 7,
          kind: 0,
        });
      } else if (this.kind === 1) {
        this.motes.add({
          x: -100 + Math.random() * 1480,
          y: -60 + Math.random() * 680,
          vx: -10 + Math.random() * 20,
          vy: -10 + Math.random() * 20,
          c: Math.random() > 0.5 ? { r: 120, g: 240, b: 255, a: 210 } : { r: 255, g: 130, b: 230, a: 210 },
          size: 2 + Math.random() * 3,
          life: 0.6 + Math.random(),
          maxLife: 1.6,
          kind: 0,
        });
      } else if (this.kind === 2) {
        this.motes.add({
          x: 1190 - 30 + Math.random() * 60,
          y: 482,
          vx: -24 + Math.random() * 48,
          vy: -150 + Math.random() * 90,
          c: Math.random() > 0.33 ? { r: 255, g: 110, b: 40, a: 230 } : { r: 255, g: 200, b: 90, a: 230 },
          size: 2 + Math.random() * 3,
          life: 1.2 + Math.random() * 1.4,
          maxLife: 2.6,
          kind: 0,
        });
      } else {
        this.motes.add({
          x: -100 + Math.random() * 1480,
          y: 560 + Math.random() * 220,
          vx: -10 + Math.random() * 20,
          vy: -35 + Math.random() * 25,
          c: fade(this.accent, 0.8),
          size: 3 + Math.random() * 5,
          life: 3 + Math.random() * 3,
          maxLife: 6,
          kind: 0,
        });
      }
    }
  }

  private drawRooftop(ctx: CanvasRenderingContext2D, t: number): void {
    const skyTop: RGBColor = { r: 64, g: 34, b: 120, a: 255 };
    const skyMid: RGBColor = { r: 238, g: 96, b: 140, a: 255 };
    const skyLow: RGBColor = { r: 255, g: 176, b: 96, a: 255 };
    gradientRect(ctx, FULL_X, FULL_Y, FULL_W, 210 - FULL_Y, skyTop, skyMid);
    gradientRect(ctx, FULL_X, 210, FULL_W, 362, skyMid, skyLow);

    // Striped sunset sun
    const sunX = 640;
    const sunY = 410;
    const sr = 200 + 6 * this.pulse;
    drawGlow(ctx, sunX, sunY, sr * 1.8, { r: 255, g: 150, b: 120, a: 110 });
    const sg = ctx.createLinearGradient(0, sunY - sr, 0, sunY + sr);
    sg.addColorStop(0, 'rgb(255,240,140)');
    sg.addColorStop(1, 'rgb(255,110,120)');
    ctx.fillStyle = sg;
    ctx.beginPath();
    ctx.arc(sunX, sunY, sr, 0, Math.PI * 2);
    ctx.fill();
    for (let k = 0; k < 7; k++) {
      const y = sunY - 50 + k * 26;
      const h = 3 + k * 2.4;
      ctx.fillStyle = rgba(mix(skyMid, skyLow, saturate((y - 210) / 362)));
      ctx.fillRect(sunX - sr - 4, y, sr * 2 + 8, h);
    }

    for (let i = 0; i < 5; i++) {
      const x = ((i * 560 + t * (8 + i * 3)) % 2400) - 560;
      drawCloud(ctx, x, 30 + (i % 3) * 64, 0.8 + 0.2 * (i % 2), { r: 255, g: 196, b: 214, a: 255 });
    }

    // Far city
    for (let i = 0; i < this.far.length; i++) {
      const b = this.far[i];
      const top = 540 - b.h;
      ctx.fillStyle = 'rgb(112,56,134)';
      ctx.fillRect(b.x, top, b.w, b.h);
      if (!this.low) {
        ctx.fillStyle = 'rgba(255,214,140,0.82)';
        for (let wy = top + 14; wy < 520; wy += 24) {
          for (let wx = b.x + 10; wx < b.x + b.w - 14; wx += 20) {
            const h = hash3(i, Math.floor(wx), Math.floor(wy));
            const lit = (h % 3 === 0) !== (h % 13 === 0 && Math.floor(this.beat / 2) % 2 === 0);
            if (lit) ctx.fillRect(wx, wy, 8, 11);
          }
        }
      }
    }

    // Near city
    ctx.fillStyle = 'rgb(70,32,96)';
    for (const b of this.near) {
      const top = 548 - b.h;
      ctx.fillRect(b.x, top, b.w, b.h);
    }

    // Water tower
    const wx = 70;
    const base = 520;
    ctx.strokeStyle = rgba(K_INK);
    ctx.lineWidth = 7;
    for (let l = 0; l < 4; l++) {
      ctx.beginPath();
      ctx.moveTo(wx - 52 + l * 34, base);
      ctx.lineTo(wx - 40 + l * 26, base - 124);
      ctx.stroke();
    }
    inkRect(ctx, wx - 64, base - 232, 128, 110, { r: 110, g: 66, b: 132, a: 255 });
    ctx.fillStyle = rgba(K_INK);
    ctx.beginPath();
    ctx.moveTo(wx - 78, base - 230);
    ctx.lineTo(wx + 78, base - 230);
    ctx.lineTo(wx, base - 292);
    ctx.closePath();
    ctx.fill();
    ctx.fillStyle = 'rgb(140,80,150)';
    ctx.beginPath();
    ctx.moveTo(wx - 68, base - 234);
    ctx.lineTo(wx + 68, base - 234);
    ctx.lineTo(wx, base - 284);
    ctx.closePath();
    ctx.fill();

    // Chain-link fence & roof wall
    ctx.strokeStyle = 'rgba(60,40,80,0.45)';
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    for (let x = FULL_X; x < FULL_X + FULL_W + 130; x += 26) {
      ctx.moveTo(x, 400);
      ctx.lineTo(x - 120, 520);
      ctx.moveTo(x - 120, 400);
      ctx.lineTo(x, 520);
    }
    ctx.stroke();
    ctx.fillStyle = 'rgb(70,46,92)';
    for (let x = FULL_X + 60; x < FULL_X + FULL_W; x += 220) ctx.fillRect(x - 5, 384, 10, 136);
    ctx.fillRect(FULL_X, 398, FULL_W, 5);
    ctx.fillStyle = 'rgb(176,138,168)';
    ctx.fillRect(FULL_X, 512, FULL_W, 52);
    ctx.fillStyle = 'rgb(206,170,196)';
    ctx.fillRect(FULL_X, 512, FULL_W, 10);

    // AC unit with spinning fan
    inkRect(ctx, 1090, 440, 150, 72, { r: 190, g: 180, b: 204, a: 255 });
    inkCircle(ctx, 1130, 476, 26, { r: 92, g: 86, b: 112, a: 255 }, 3);
    ctx.strokeStyle = 'rgb(204,200,218)';
    ctx.lineWidth = 6;
    for (let b = 0; b < 3; b++) {
      const a = t * 9 + b * 2.094;
      ctx.beginPath();
      ctx.moveTo(1130, 476);
      ctx.lineTo(1130 + Math.cos(a) * 22, 476 + Math.sin(a) * 22);
      ctx.stroke();
    }

    // String lights
    const bulbs: RGBColor[] = [
      { r: 255, g: 230, b: 90, a: 255 },
      { r: 255, g: 110, b: 190, a: 255 },
      { r: 90, g: 230, b: 255, a: 255 },
      { r: 150, g: 255, b: 120, a: 255 },
    ];
    ctx.strokeStyle = rgba(K_INK);
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i <= 32; i++) {
      const u = i / 32;
      const px = -40 + u * 1360;
      const py = 206 + Math.sin(u * Math.PI) * 92;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.stroke();
    for (let i = 0; i <= 32; i++) {
      const u = i / 32;
      const px = -40 + u * 1360;
      const py = 206 + Math.sin(u * Math.PI) * 92 + 8;
      const on = (i + this.beat) % 2 === 0;
      const bc = bulbs[i % 4];
      ctx.fillStyle = rgba(on ? bc : mix(bc, K_INK, 0.6));
      ctx.beginPath();
      ctx.arc(px, py, 6, 0, Math.PI * 2);
      ctx.fill();
      if (on && !this.low) drawGlow(ctx, px, py, 26 + 8 * this.pulse, fade(bc, 0.45));
    }

    // Roof floor
    gradientRect(ctx, FULL_X, 566, FULL_W, FULL_Y + FULL_H - 566, { r: 150, g: 116, b: 150, a: 255 }, { r: 66, g: 46, b: 80, a: 255 });
  }

  private drawClub(ctx: CanvasRenderingContext2D, t: number): void {
    gradientRect(ctx, FULL_X, FULL_Y, FULL_W, FULL_H, { r: 10, g: 6, b: 26, a: 255 }, { r: 40, g: 14, b: 66, a: 255 });
    const wx = 170;
    const wy = 30;
    const ww = 940;
    const wh = 430;
    inkRect(ctx, wx - 10, wy - 10, ww + 20, wh + 20, { r: 24, g: 20, b: 40, a: 255 }, 5);

    const cols = 24;
    const rows = 11;
    const cw = ww / cols;
    const ch = wh / rows;
    for (let c = 0; c < cols; c++) {
      const level =
        0.22 +
        0.36 * (0.5 + 0.5 * Math.sin(t * 2.3 + c * 0.55)) +
        0.4 * this.pulse * (0.5 + 0.5 * Math.sin(c * 1.7 + this.beat));
      const lit = Math.floor(level * rows);
      for (let r = 0; r < rows; r++) {
        const up = rows - 1 - r;
        const u = up / (rows - 1);
        const col =
          u < 0.5
            ? mix({ r: 40, g: 220, b: 255, a: 255 }, { r: 255, g: 60, b: 200, a: 255 }, u * 2)
            : mix({ r: 255, g: 60, b: 200, a: 255 }, { r: 255, g: 230, b: 80, a: 255 }, (u - 0.5) * 2);
        ctx.fillStyle = rgba(col, up < lit ? 1 : 0.1);
        ctx.fillRect(wx + c * cw + 2, wy + r * ch + 2, cw - 4, ch - 4);
      }
    }

    // Mirror ball
    const bx = 640;
    const by = -40;
    inkCircle(ctx, bx, by, 68, { r: 150, g: 156, b: 186, a: 255 });
    for (let i = 0; i < 6; i++) {
      const lat = -1.1 + i * 0.44;
      const y = by + Math.sin(lat) * 60;
      const rr = Math.cos(lat) * 62;
      for (let k = 0; k < 12; k++) {
        const a = t * 0.9 + k * 0.52;
        const z = Math.cos(a);
        if (z <= 0.05) continue;
        const x = bx + Math.sin(a) * rr;
        const s = 9 * z * Math.cos(lat) + 2;
        const shine = (hash3(i, k, 0) % 100) / 100;
        ctx.fillStyle = rgba(mix({ r: 120, g: 126, b: 160, a: 255 }, COLOR_WHITE, shine * z));
        ctx.fillRect(x - s * 0.5, y - 4, s, 8);
      }
    }
    drawGlow(ctx, bx, by, 150 + 40 * this.pulse, { r: 200, g: 220, b: 255, a: 70 });

    // Lasers
    if (!this.low) {
      ctx.save();
      ctx.globalCompositeOperation = 'lighter';
      for (let i = 0; i < 8; i++) {
        const left = i < 4;
        const ox = left ? -120 : 1400;
        const oy = -160;
        const a = (left ? 0.9 : 2.24) + Math.sin(t * 1.4 + i * 0.8) * 0.35;
        const ex = ox + Math.cos(a) * 1600;
        const ey = oy + Math.sin(a) * 1600;
        const lc: RGBColor = i % 2 ? { r: 60, g: 255, b: 120, a: 255 } : { r: 255, g: 60, b: 160, a: 255 };
        ctx.strokeStyle = rgba(lc, 0.45 + 0.4 * this.pulse);
        ctx.lineWidth = 3;
        ctx.beginPath();
        ctx.moveTo(ox, oy);
        ctx.lineTo(ex, ey);
        ctx.stroke();
        ctx.strokeStyle = rgba(lc, 0.12);
        ctx.lineWidth = 10;
        ctx.stroke();
      }
      ctx.restore();
    }

    drawSpeaker(ctx, -80, 230, 170, 160, this.pulse);
    drawSpeaker(ctx, -80, 400, 170, 160, this.pulse);
    drawSpeaker(ctx, 1190, 230, 170, 160, this.pulse);
    drawSpeaker(ctx, 1190, 400, 170, 160, this.pulse);

    // Dance floor
    const fy = 560;
    gradientRect(ctx, FULL_X, fy, FULL_W, FULL_Y + FULL_H - fy, { r: 24, g: 10, b: 40, a: 255 }, { r: 8, g: 4, b: 14, a: 255 });
    const kTile: RGBColor[] = [
      { r: 255, g: 60, b: 200, a: 255 },
      { r: 40, g: 220, b: 255, a: 255 },
      { r: 255, g: 230, b: 80, a: 255 },
    ];
    let y0 = fy;
    for (let r = 0; r < 6; r++) {
      const y1 = y0 + 28 + r * 24;
      const s0 = (y0 - 300) / (fy - 300);
      const s1 = (y1 - 300) / (fy - 300);
      for (let c = -10; c < 10; c++) {
        const pick = (r + c + 30 + this.beat) % 4;
        ctx.fillStyle =
          pick < 3 ? rgba(kTile[pick], 0.22 + 0.45 * this.pulse) : 'rgb(20,12,34)';
        ctx.beginPath();
        ctx.moveTo(640 + c * 130 * s0, y0);
        ctx.lineTo(640 + (c + 1) * 130 * s0, y0);
        ctx.lineTo(640 + (c + 1) * 130 * s1, y1);
        ctx.lineTo(640 + c * 130 * s1, y1);
        ctx.closePath();
        ctx.fill();
      }
      y0 = y1;
    }
  }

  private drawAlley(ctx: CanvasRenderingContext2D, t: number): void {
    gradientRect(ctx, FULL_X, FULL_Y, FULL_W, 580 - FULL_Y, { r: 8, g: 4, b: 18, a: 255 }, { r: 86, g: 14, b: 44, a: 255 });
    for (const s of this.stars) {
      if (s.y < 260) {
        ctx.fillStyle = rgba(COLOR_WHITE, 0.25 + 0.35 * (0.5 + 0.5 * Math.sin(t * 2 + s.ph)));
        ctx.beginPath();
        ctx.arc(s.x, s.y, s.s * 0.4, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // Red moon
    drawGlow(ctx, 640, 110, 300, { r: 255, g: 50, b: 70, a: Math.round(90 + 60 * this.pulse) });
    ctx.fillStyle = 'rgb(226,52,72)';
    ctx.beginPath();
    ctx.arc(640, 110, 110, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = 'rgb(196,38,60)';
    ctx.beginPath();
    ctx.arc(610, 80, 22, 0, Math.PI * 2);
    ctx.arc(676, 132, 30, 0, Math.PI * 2);
    ctx.arc(662, 66, 12, 0, Math.PI * 2);
    ctx.fill();

    // Brick walls
    ctx.fillStyle = 'rgb(98,35,51)';
    ctx.fillRect(160, 250, 960, 330);
    ctx.fillStyle = 'rgb(62,25,47)';
    ctx.fillRect(FULL_X, -60, 170 - FULL_X, 640);
    ctx.fillRect(1110, -60, FULL_X + FULL_W - 1110, 640);

    // Graffiti squiggles
    const spray: RGBColor[] = [
      { r: 90, g: 255, b: 120, a: 255 },
      { r: 255, g: 220, b: 60, a: 255 },
      { r: 60, g: 200, b: 255, a: 255 },
    ];
    const tagX = [200, 790, 960];
    ctx.lineCap = 'round';
    for (let g = 0; g < 3; g++) {
      ctx.strokeStyle = rgba(spray[g]);
      ctx.lineWidth = 6;
      ctx.beginPath();
      for (let s = 0; s <= 28; s++) {
        const u = s / 28;
        const a = u * Math.PI * 4 + g;
        const qx = tagX[g] + u * 120 + Math.cos(a) * 14;
        const qy = 520 + Math.sin(a) * 18 - u * 10;
        if (s === 0) ctx.moveTo(qx, qy);
        else ctx.lineTo(qx, qy);
      }
      ctx.stroke();
    }

    // Flickering neon LIVE sign
    const on = !this.flashing || hash3(Math.floor(t * 8), 3, 3) % 11 !== 0;
    const neon: RGBColor = { r: 255, g: 70, b: 150, a: 255 };
    inkRect(ctx, 530, 290, 220, 84, { r: 30, g: 14, b: 30, a: 255 });
    if (on) drawGlow(ctx, 640, 332, 170, fade(neon, 0.35 + 0.25 * this.pulse));
    drawText(ctx, 'title', 'LIVE', 640, 300, 58, on ? neon : mix(neon, K_INK, 0.7), 'center', 4);

    // Dumpster & flaming fire barrel
    inkRect(ctx, 24, 476, 170, 100, { r: 44, g: 120, b: 80, a: 255 });
    inkRect(ctx, 16, 462, 186, 16, { r: 34, g: 96, b: 64, a: 255 });

    const bx = 1190;
    const by = 580;
    drawGlow(ctx, bx, by - 130, 170 + 24 * this.pulse, { r: 255, g: 120, b: 40, a: 120 });
    for (let k = 0; k < 3; k++) {
      const h = 70 - k * 18 + Math.sin(t * 9 + k * 2) * 10 + 12 * this.pulse;
      const w = 36 - k * 9;
      ctx.fillStyle = k === 0 ? 'rgb(255,90,40)' : k === 1 ? 'rgb(255,170,50)' : 'rgb(255,240,140)';
      const top = by - 96;
      ctx.beginPath();
      ctx.moveTo(bx - w, top);
      ctx.lineTo(bx + w, top);
      ctx.lineTo(bx + Math.sin(t * 7 + k) * 8, top - h);
      ctx.closePath();
      ctx.fill();
    }
    inkRect(ctx, bx - 40, by - 96, 80, 96, { r: 126, g: 72, b: 50, a: 255 });

    // Wet asphalt & red moon puddles
    gradientRect(ctx, FULL_X, 578, FULL_W, FULL_Y + FULL_H - 578, { r: 48, g: 32, b: 54, a: 255 }, { r: 12, g: 8, b: 18, a: 255 });
    for (const [px, py] of [
      [150, 690],
      [640, 660],
      [1170, 700],
    ]) {
      ctx.fillStyle = 'rgb(30,20,44)';
      ctx.beginPath();
      ctx.ellipse(px, py, 120, 20, 0, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = rgba({ r: 226, g: 52, b: 72, a: 255 }, 0.35 + 0.2 * this.pulse);
      ctx.beginPath();
      ctx.ellipse(px + 10, py, 46, 8, 0, 0, Math.PI * 2);
      ctx.fill();
    }
  }

  private drawSynthwave(ctx: CanvasRenderingContext2D, t: number): void {
    const tint = this.accent;
    const horizon = 560;
    const skyTop = mix({ r: 6, g: 4, b: 22, a: 255 }, tint, 0.1);
    const skyLow = mix({ r: 70, g: 18, b: 86, a: 255 }, tint, 0.4);
    gradientRect(ctx, FULL_X, FULL_Y, FULL_W, horizon - FULL_Y + 2, skyTop, skyLow);

    for (const s of this.stars) {
      if (s.y > horizon - 90) continue;
      const tw = 0.5 + 0.5 * Math.sin(t * (1 + s.s * 0.25) + s.ph);
      drawStar(ctx, s.x, s.y, s.s * (0.7 + 0.3 * tw + 0.25 * this.pulse), fade(COLOR_WHITE, 0.3 + 0.7 * tw));
    }

    const sunX = 640;
    const sunY = horizon - 40;
    const sunR = 190 + 8 * this.pulse;
    drawGlow(ctx, sunX, sunY, sunR * 1.9, fade(mix({ r: 255, g: 120, b: 170, a: 255 }, tint, 0.3), 0.35));
    const sg = ctx.createLinearGradient(0, sunY - sunR, 0, sunY + sunR);
    sg.addColorStop(0, 'rgba(255,226,90,0.9)');
    sg.addColorStop(1, rgba(mix({ r: 255, g: 70, b: 150, a: 255 }, tint, 0.25), 0.9));
    ctx.fillStyle = sg;
    ctx.beginPath();
    ctx.arc(sunX, sunY, sunR, 0, Math.PI * 2);
    ctx.fill();

    gradientRect(ctx, FULL_X, horizon, FULL_W, FULL_Y + FULL_H - horizon, mix({ r: 24, g: 6, b: 36, a: 255 }, tint, 0.22), {
      r: 4,
      g: 2,
      b: 12,
      a: 255,
    });
    const line = mix({ r: 255, g: 80, b: 200, a: 255 }, tint, 0.35);
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    const run = (t * 0.55) % 1.0;
    for (let i = 0; i < 14; i++) {
      const z = i + 1 - run;
      const y = horizon + 320 / z - 8;
      if (y < horizon || y > FULL_Y + FULL_H) continue;
      ctx.strokeStyle = rgba(line, Math.min(0.75, 0.12 + 0.9 / z) * (0.8 + 0.4 * this.pulse));
      ctx.lineWidth = 1.5 + 1.5 / z;
      ctx.beginPath();
      ctx.moveTo(FULL_X, y);
      ctx.lineTo(FULL_X + FULL_W, y);
      ctx.stroke();
    }
    ctx.strokeStyle = rgba(line, 0.35 + 0.2 * this.pulse);
    ctx.lineWidth = 1.6;
    for (let i = -24; i <= 24; i++) {
      ctx.beginPath();
      ctx.moveTo(640 + i * 40, horizon);
      ctx.lineTo(640 + i * 330, FULL_Y + FULL_H);
      ctx.stroke();
    }
    ctx.restore();

    const colors: RGBColor[] = [
      { r: 255, g: 79, b: 163, a: 255 },
      { r: 40, g: 220, b: 255, a: 255 },
      { r: 130, g: 255, b: 90, a: 255 },
      { r: 170, g: 100, b: 255, a: 255 },
      { r: 255, g: 222, b: 64, a: 255 },
    ];
    for (let i = 0; i < 6; i++) {
      const x = ((i * 360 + t * (9 + i * 2)) % 2000) - 360;
      const y = 90 + ((i * 97) % 240) + Math.sin(t * 0.7 + i * 1.7) * 14;
      const sz = (50 + ((i * 37) % 55)) * (1 + 0.06 * this.pulse);
      const ax = t * (0.35 + 0.08 * i) + i;
      const ay = t * (0.5 - 0.05 * i) + i * 2;
      const c = mix(colors[i % 5], tint, 0.25);
      drawCube(ctx, x, y, sz, ax, ay, c, 0.22, true, horizon + 8);
      drawCube(ctx, x, y, sz, ax, ay, c, 0.85, false, horizon);
    }
  }

  drawBack(ctx: CanvasRenderingContext2D, now: number): void {
    if (this.kind === 0) this.drawRooftop(ctx, now);
    else if (this.kind === 1) this.drawClub(ctx, now);
    else if (this.kind === 2) this.drawAlley(ctx, now);
    else this.drawSynthwave(ctx, now);
  }

  drawFront(ctx: CanvasRenderingContext2D, now: number): void {
    this.motes.draw(ctx, true);
    if (this.kind === 0) {
      drawSpeaker(ctx, -150, 560, 170, 200, this.pulse);
      drawSpeaker(ctx, 1260, 560, 170, 200, this.pulse);
    } else if (this.kind === 1) {
      for (let i = 0; i < 15; i++) {
        const x = -180 + i * 112 + (i % 2) * 30;
        const y = 716 + (i % 3) * 12 - 14 * this.pulse * ((i + this.beat) % 2 ? 1 : 0.35);
        ctx.fillStyle = 'rgb(14,8,26)';
        roundRect(ctx, x - 46, y + 28, 92, 140, 36);
        ctx.fill();
        ctx.beginPath();
        ctx.arc(x, y, 34, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = i % 2 ? 'rgba(255,80,200,0.6)' : 'rgba(60,220,255,0.6)';
        ctx.lineWidth = 4;
        ctx.beginPath();
        ctx.arc(x, y, 33, Math.PI * 1.1, Math.PI * 1.9);
        ctx.stroke();
      }
    } else if (this.kind === 2) {
      for (let i = 0; i < 7; i++) {
        const x = ((i * 230 + now * 20) % 1800) - 260;
        drawSoftCircle(ctx, x, 750, 210, { r: 40, g: 8, b: 20, a: 110 });
      }
      drawAmp(ctx, -110, 560, 170, 150);
      drawAmp(ctx, 1220, 560, 170, 150);
    }
  }

  drawOverlay(ctx: CanvasRenderingContext2D): void {
    if (this.flash > 0 && this.flashing) {
      ctx.fillStyle = `rgba(255,220,220,${(this.flash * 0.6).toFixed(3)})`;
      ctx.fillRect(0, 0, CANVAS_W, CANVAS_H);
    }
  }
}
