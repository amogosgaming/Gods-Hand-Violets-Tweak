// Procedural glossy "Frutiger Aero" cartoon cast ported from src/characters.cpp
// Characters: Hype (herald), Principal P (principality), DJ Hand (hand), D-Mize (demise), Shade
import { CharId, RGBColor } from './types';
import { COLOR_BLACK, COLOR_WHITE, drawGlow, fade, mix, rgba } from './gfx';

const K_INK: RGBColor = { r: 18, g: 38, b: 76, a: 255 };
const K_MISS_TONE: RGBColor = { r: 96, g: 84, b: 170, a: 255 };
const K_LINE = 4.2;

export interface CharacterState {
  id: CharId;
  tint: RGBColor;
  singDir: number;
  singStart: number;
  singUntil: number;
  missUntil: number;
  beatAt: number;
}

export function makeCharState(id: CharId, tint?: RGBColor): CharacterState {
  return {
    id,
    tint: tint ?? charColor(id),
    singDir: -1,
    singStart: -10,
    singUntil: -10,
    missUntil: -10,
    beatAt: -10,
  };
}

export function charSing(s: CharacterState, dir: number, now: number, holdSec = 0): void {
  s.singDir = dir;
  s.singStart = now;
  s.singUntil = now + Math.max(0.32, holdSec + 0.18);
  s.missUntil = -10;
}

export function charMiss(s: CharacterState, dir: number, now: number): void {
  s.singDir = dir;
  s.singStart = now;
  s.singUntil = now + 0.35;
  s.missUntil = now + 0.35;
}

export function charBeat(s: CharacterState, now: number): void {
  if (now > s.singUntil) s.beatAt = now;
}

export function charDisplayName(id: CharId): string {
  switch (id) {
    case 'herald':
      return 'Hype';
    case 'principality':
      return 'Principal P';
    case 'hand':
      return 'DJ Hand';
    case 'demise':
      return 'D-Mize';
    default:
      return 'Shade';
  }
}

export function charColor(id: CharId): RGBColor {
  switch (id) {
    case 'herald':
      return { r: 48, g: 182, b: 240, a: 255 };
    case 'principality':
      return { r: 164, g: 100, b: 255, a: 255 };
    case 'hand':
      return { r: 70, g: 170, b: 255, a: 255 };
    case 'demise':
      return { r: 255, g: 48, b: 84, a: 255 };
    default:
      return { r: 180, g: 140, b: 210, a: 255 };
  }
}

interface Xf {
  ox: number;
  oy: number;
  s: number;
  sx: number;
  sy: number;
  rot: number;
  flip: boolean;
}

function xfPt(X: Xf, x: number, y: number): [number, number] {
  let px = X.flip ? -x : x;
  px *= X.sx * X.s;
  const py = y * X.sy * X.s;
  const c = Math.cos(X.rot);
  const n = Math.sin(X.rot);
  return [X.ox + px * c - py * n, X.oy + px * n + py * c];
}

function xfLen(X: Xf, v: number): number {
  return v * X.s * (X.sx + X.sy) * 0.5;
}

function hash01(x: number): number {
  const s = Math.sin(x) * 43758.5453;
  return s - Math.floor(s);
}

function drawBubble(ctx: CanvasRenderingContext2D, cx: number, cy: number, r: number, a: number): void {
  if (a <= 0.01 || r < 0.5) return;
  ctx.save();
  ctx.fillStyle = rgba({ r: 170, g: 235, b: 255, a: 38 }, a);
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.fill();
  ctx.strokeStyle = rgba({ r: 255, g: 255, b: 255, a: 175 }, a);
  ctx.lineWidth = Math.max(0.8, r * 0.14);
  ctx.stroke();
  ctx.fillStyle = rgba(COLOR_WHITE, 0.85 * a);
  ctx.beginPath();
  ctx.arc(cx - r * 0.36, cy - r * 0.36, r * 0.22, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

function drawBubbles(
  ctx: CanvasRenderingContext2D,
  X: Xf,
  x: number,
  y: number,
  w: number,
  h: number,
  n: number,
  now: number,
  speed: number,
  seed: number,
): void {
  for (let i = 0; i < n; i++) {
    const s = seed + i * 7.31;
    const ph = (now * speed * (0.55 + 0.45 * hash01(s)) + hash01(s * 1.7)) % 1.0;
    const bx = x + (hash01(s * 2.3) - 0.5) * w + Math.sin(ph * 9 + s) * 6;
    const by = y - ph * h;
    const r = 4 + 8 * hash01(s * 3.1);
    const [px, py] = xfPt(X, bx, by);
    drawBubble(ctx, px, py, xfLen(X, r), Math.sin(ph * Math.PI));
  }
}

function applyXf(ctx: CanvasRenderingContext2D, X: Xf): void {
  ctx.translate(X.ox, X.oy);
  ctx.rotate(X.rot);
  ctx.scale((X.flip ? -1 : 1) * X.sx * X.s, X.sy * X.s);
}

function aeroGradient(ctx: CanvasRenderingContext2D, y0: number, y1: number, col: RGBColor): CanvasGradient {
  const g = ctx.createLinearGradient(0, y0, 0, y1);
  g.addColorStop(0, rgba(mix(col, COLOR_WHITE, 0.45)));
  g.addColorStop(0.5, rgba(col));
  g.addColorStop(1, rgba(mix(col, K_INK, 0.32)));
  return g;
}

// Grouped Ink shapes: draw all thick dark-blue outlines first, then glossy lit fills + highlights
type InkItem =
  | { kind: 'e'; cx: number; cy: number; rx: number; ry: number; col: RGBColor }
  | { kind: 'c'; ax: number; ay: number; bx: number; by: number; r: number; col: RGBColor }
  | { kind: 'p'; pts: [number, number][]; col: RGBColor };

class Ink {
  private items: InkItem[] = [];
  constructor(
    private ctx: CanvasRenderingContext2D,
    private X: Xf,
    private w = K_LINE,
    private gloss = true,
  ) {}

  e(cx: number, cy: number, rx: number, ry: number, col: RGBColor): this {
    this.items.push({ kind: 'e', cx, cy, rx, ry, col });
    return this;
  }
  c(ax: number, ay: number, bx: number, by: number, r: number, col: RGBColor): this {
    this.items.push({ kind: 'c', ax, ay, bx, by, r, col });
    return this;
  }
  p(pts: [number, number][], col: RGBColor): this {
    this.items.push({ kind: 'p', pts, col });
    return this;
  }
  limb(ax: number, ay: number, bx: number, by: number, r: number, bend: number, col: RGBColor): this {
    const dx = bx - ax;
    const dy = by - ay;
    const len = Math.max(0.001, Math.hypot(dx, dy));
    const jx = (ax + bx) * 0.5 - (dy / len) * bend;
    const jy = (ay + by) * 0.5 + (dx / len) * bend;
    this.c(ax, ay, jx, jy, r, col);
    this.c(jx, jy, bx, by, r * 0.9, col);
    return this;
  }

  draw(): void {
    const { ctx, X, w, gloss } = this;
    ctx.save();
    applyXf(ctx, X);
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';

    // Pass 1: Outlines
    ctx.fillStyle = rgba(K_INK);
    ctx.strokeStyle = rgba(K_INK);
    for (const it of this.items) {
      if (it.kind === 'e') {
        ctx.beginPath();
        ctx.ellipse(it.cx, it.cy, it.rx + w, it.ry + w, 0, 0, Math.PI * 2);
        ctx.fill();
      } else if (it.kind === 'c') {
        ctx.lineWidth = (it.r + w) * 2;
        ctx.beginPath();
        ctx.moveTo(it.ax, it.ay);
        ctx.lineTo(it.bx, it.by);
        ctx.stroke();
      } else if (it.pts.length >= 3) {
        ctx.beginPath();
        ctx.moveTo(it.pts[0][0], it.pts[0][1]);
        for (let i = 1; i < it.pts.length; i++) ctx.lineTo(it.pts[i][0], it.pts[i][1]);
        ctx.closePath();
        ctx.lineWidth = w * 2;
        ctx.stroke();
        ctx.fill();
      }
    }

    // Pass 2: Fills + Frutiger Aero glossy shines
    for (const it of this.items) {
      if (it.kind === 'e') {
        ctx.beginPath();
        ctx.ellipse(it.cx, it.cy, it.rx, it.ry, 0, 0, Math.PI * 2);
        ctx.fillStyle = gloss ? aeroGradient(ctx, it.cy - it.ry, it.cy + it.ry, it.col) : rgba(it.col);
        ctx.fill();
        if (gloss && it.rx >= 9 && it.ry >= 9) {
          const sx = it.cx - it.rx * 0.16;
          const sy = it.cy - it.ry * 0.42;
          const sg = ctx.createLinearGradient(0, sy - it.ry * 0.36, 0, sy + it.ry * 0.36);
          sg.addColorStop(0, 'rgba(255,255,255,0.62)');
          sg.addColorStop(1, 'rgba(255,255,255,0)');
          ctx.fillStyle = sg;
          ctx.beginPath();
          ctx.ellipse(sx, sy, it.rx * 0.62, it.ry * 0.36, 0, 0, Math.PI * 2);
          ctx.fill();
        }
      } else if (it.kind === 'c') {
        ctx.strokeStyle = rgba(it.col);
        ctx.lineWidth = it.r * 2;
        ctx.beginPath();
        ctx.moveTo(it.ax, it.ay);
        ctx.lineTo(it.bx, it.by);
        ctx.stroke();
        if (gloss && it.r >= 6) {
          const dx = it.bx - it.ax;
          const dy = it.by - it.ay;
          const len = Math.max(0.001, Math.hypot(dx, dy));
          let nx = -dy / len;
          let ny = dx / len;
          if (nx * -0.4 + ny * -0.9 < 0) {
            nx = -nx;
            ny = -ny;
          }
          const o = it.r * 0.42;
          ctx.strokeStyle = 'rgba(255,255,255,0.32)';
          ctx.lineWidth = it.r * 0.6;
          ctx.beginPath();
          ctx.moveTo(it.ax + nx * o, it.ay + ny * o);
          ctx.lineTo(it.bx + nx * o, it.by + ny * o);
          ctx.stroke();
        }
      } else if (it.pts.length >= 3) {
        let y0 = 1e9;
        let y1 = -1e9;
        for (const q of it.pts) {
          if (q[1] < y0) y0 = q[1];
          if (q[1] > y1) y1 = q[1];
        }
        ctx.beginPath();
        ctx.moveTo(it.pts[0][0], it.pts[0][1]);
        for (let i = 1; i < it.pts.length; i++) ctx.lineTo(it.pts[i][0], it.pts[i][1]);
        ctx.closePath();
        ctx.fillStyle = gloss ? aeroGradient(ctx, y0, y1, it.col) : rgba(it.col);
        ctx.fill();
        if (gloss && y1 - y0 >= 22) {
          const sg = ctx.createLinearGradient(0, y0, 0, y0 + (y1 - y0) * 0.55);
          sg.addColorStop(0, 'rgba(255,255,255,0.36)');
          sg.addColorStop(1, 'rgba(255,255,255,0)');
          ctx.fillStyle = sg;
          ctx.fill();
        }
      }
    }
    ctx.restore();
  }
}

function drawLine(ctx: CanvasRenderingContext2D, X: Xf, ax: number, ay: number, bx: number, by: number, w: number, col: RGBColor): void {
  ctx.save();
  applyXf(ctx, X);
  ctx.strokeStyle = rgba(col);
  ctx.lineWidth = w;
  ctx.lineCap = 'round';
  ctx.beginPath();
  ctx.moveTo(ax, ay);
  ctx.lineTo(bx, by);
  ctx.stroke();
  ctx.restore();
}

function drawEll(ctx: CanvasRenderingContext2D, X: Xf, cx: number, cy: number, rx: number, ry: number, col: RGBColor): void {
  ctx.save();
  applyXf(ctx, X);
  ctx.fillStyle = rgba(col);
  ctx.beginPath();
  ctx.ellipse(cx, cy, rx, ry, 0, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

function drawPoly(ctx: CanvasRenderingContext2D, X: Xf, pts: [number, number][], col: RGBColor): void {
  ctx.save();
  applyXf(ctx, X);
  ctx.fillStyle = rgba(col);
  ctx.beginPath();
  ctx.moveTo(pts[0][0], pts[0][1]);
  for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

interface Pose {
  dx: number;
  dy: number;
  sx: number;
  sy: number;
  rot: number;
  pop: number;
  dir: number;
  singing: boolean;
  missing: boolean;
  bop: number;
}

function computePose(s: CharacterState, now: number): Pose {
  const singing = now < s.singUntil && s.singDir >= 0;
  const missing = now < s.missUntil;
  const bop = Math.exp(-Math.max(0, now - s.beatAt) * 7.0);
  const p: Pose = { dx: 0, dy: 0, sx: 1, sy: 1, rot: 0, pop: 0, dir: -1, singing, missing, bop };
  if (singing) {
    const pop = Math.exp(-Math.max(0, now - s.singStart) * 12.0);
    p.pop = pop;
    p.dir = s.singDir;
    if (s.singDir === 0) {
      p.dx = -12 - 10 * pop;
      p.rot = -0.06;
    } else if (s.singDir === 3) {
      p.dx = 12 + 10 * pop;
      p.rot = 0.06;
    } else if (s.singDir === 2) {
      p.dy = -10 - 10 * pop;
      p.sy = 1.05 + 0.04 * pop;
      p.sx = 0.97;
    } else if (s.singDir === 1) {
      p.dy = 8 + 6 * pop;
      p.sy = 0.92 - 0.03 * pop;
      p.sx = 1.05;
    }
  } else {
    p.sy = 1 - 0.035 * bop;
    p.sx = 1 + 0.02 * bop;
  }
  return p;
}

// 0 idle, 1 up, 2 down, 3 front, 4 back
function slotOf(P: Pose, flip: boolean): number {
  if (!P.singing) return 0;
  if (P.dir === 0) return flip ? 3 : 4;
  if (P.dir === 3) return flip ? 4 : 3;
  if (P.dir === 2) return 1;
  if (P.dir === 1) return 2;
  return 0;
}

interface Face {
  open: number;
  lookX: number;
  lookY: number;
  blink: number;
  singing: boolean;
  miss: boolean;
  dir: number;
}

function faceFor(P: Pose, now: number, flip: boolean, phase: number): Face {
  const f: Face = {
    open: 0,
    lookX: 0.3,
    lookY: 0,
    blink: 1,
    singing: P.singing,
    miss: P.missing,
    dir: P.dir,
  };
  if (P.singing && !P.missing) {
    f.open = Math.min(1, 0.45 + 0.5 * P.pop + 0.15 * Math.abs(Math.sin(now * 15)));
    const m = flip ? -1 : 1;
    if (P.dir === 0) f.lookX = -m;
    if (P.dir === 3) f.lookX = m;
    if (P.dir === 2) f.lookY = -1;
    if (P.dir === 1) f.lookY = 1;
  } else if (P.missing) {
    f.open = 0.35;
  }
  if (!P.singing && (now + phase) % 3.9 < 0.12) f.blink = 0;
  return f;
}

function drawEye(ctx: CanvasRenderingContext2D, X: Xf, cx: number, cy: number, rx: number, ry: number, f: Face, white: RGBColor, side: number): void {
  if (f.miss) {
    drawLine(ctx, X, cx - side * rx, cy - ry * 0.5, cx + side * rx * 0.6, cy, 3.4, K_INK);
    drawLine(ctx, X, cx + side * rx * 0.6, cy, cx - side * rx, cy + ry * 0.5, 3.4, K_INK);
    return;
  }
  if (f.blink < 0.5) {
    drawLine(ctx, X, cx - rx, cy + 1, cx + rx, cy + 1, 3.2, K_INK);
    return;
  }
  new Ink(ctx, X, 3.0).e(cx, cy, rx, ry, white).draw();
  const px = cx + f.lookX * rx * 0.36;
  const py = cy + f.lookY * ry * 0.3;
  drawEll(ctx, X, px, py, rx * 0.48, ry * 0.5, K_INK);
  drawEll(ctx, X, px - rx * 0.18, py - ry * 0.2, rx * 0.17, rx * 0.17, COLOR_WHITE);
}

function drawMouth(ctx: CanvasRenderingContext2D, X: Xf, cx: number, cy: number, w: number, f: Face, lips: RGBColor | null, grin: boolean): void {
  const inside: RGBColor = { r: 70, g: 16, b: 40, a: 255 };
  if (f.open < 0.01) {
    if (grin) {
      const pts: [number, number][] = [];
      for (let i = 0; i <= 12; i++) {
        const a = (Math.PI * i) / 12;
        pts.push([cx + Math.cos(a) * w * 1.2, cy - 3 + Math.sin(a) * w * 0.75]);
      }
      new Ink(ctx, X, 3.2, false).p(pts, inside).draw();
      drawPoly(
        ctx,
        X,
        [
          [cx - w * 1.02, cy - 3],
          [cx + w * 1.02, cy - 3],
          [cx + w * 0.9, cy + w * 0.16],
          [cx - w * 0.9, cy + w * 0.16],
        ],
        COLOR_WHITE,
      );
    } else if (lips) {
      drawLine(ctx, X, cx - w, cy, cx + w * 0.8, cy - 1, 4.4, lips);
    } else {
      drawLine(ctx, X, cx - w, cy, cx + w * 0.55, cy, 3.4, K_INK);
      drawLine(ctx, X, cx + w * 0.55, cy, cx + w, cy - 4, 3.4, K_INK);
    }
    return;
  }
  const rx = w * (0.72 + 0.28 * f.open) * (f.dir === 2 ? 0.82 : 1);
  const ry = w * (0.28 + 0.62 * f.open) * (f.dir === 1 ? 0.75 : 1);
  if (lips) {
    drawEll(ctx, X, cx, cy, rx + 3.2, ry + 3.2, lips);
    drawEll(ctx, X, cx, cy, rx, ry, inside);
  } else {
    new Ink(ctx, X, 3.2, false).e(cx, cy, rx, ry, inside).draw();
  }
  drawEll(ctx, X, cx, cy - ry * 0.55, rx * 0.55, ry * 0.25, { r: 250, g: 250, b: 245, a: 255 });
  drawEll(ctx, X, cx + rx * 0.1, cy + ry * 0.55, rx * 0.55, ry * 0.32, { r: 255, g: 110, b: 130, a: 255 });
}

function drawSweat(ctx: CanvasRenderingContext2D, X: Xf, x: number, y: number): void {
  const drop: RGBColor = { r: 150, g: 220, b: 255, a: 255 };
  new Ink(ctx, X, 2.6)
    .e(x, y + 5, 5.5, 6.5, drop)
    .p(
      [
        [x - 4.6, y + 3],
        [x + 4.6, y + 3],
        [x, y - 9],
      ],
      drop,
    )
    .draw();
  drawEll(ctx, X, x - 1.8, y + 3, 1.6, 2.2, COLOR_WHITE);
}

function drawMic(ctx: CanvasRenderingContext2D, X: Xf, hx: number, hy: number, dx: number, dy: number, grille: RGBColor): void {
  const l = Math.max(0.001, Math.hypot(dx, dy));
  const ux = dx / l;
  const uy = dy / l;
  const gx = hx + ux * 36;
  const gy = hy + uy * 36;
  new Ink(ctx, X, 3.4)
    .c(hx - ux * 6, hy - uy * 6, hx + ux * 26, hy + uy * 26, 5.5, { r: 48, g: 46, b: 62, a: 255 })
    .e(gx, gy, 11.5, 11.5, grille)
    .draw();
  drawLine(ctx, X, gx - uy * 10, gy + ux * 10, gx + uy * 10, gy - ux * 10, 1.6, fade(K_INK, 0.45));
  drawEll(ctx, X, gx - 3.5, gy - 3.5, 3.2, 3.2, fade(COLOR_WHITE, 0.75));
}

// --- HYPE (herald) & SHADE ---
function hypePalette(shade: boolean, tint: RGBColor, missing: boolean) {
  const t = (c: RGBColor): RGBColor => {
    let out = c;
    if (shade) out = mix(mix(out, tint, 0.3), { r: 12, g: 8, b: 20, a: 255 }, 0.62);
    if (missing) out = mix(out, K_MISS_TONE, 0.38);
    return out;
  };
  const glow = mix(tint, COLOR_WHITE, 0.55);
  return {
    skin: t({ r: 186, g: 120, b: 78, a: 255 }),
    skinShade: t({ r: 150, g: 92, b: 60, a: 255 }),
    hair: t({ r: 46, g: 30, b: 36, a: 255 }),
    beanie: t({ r: 130, g: 214, b: 50, a: 255 }),
    beanieDark: t({ r: 80, g: 156, b: 28, a: 255 }),
    pom: t({ r: 242, g: 250, b: 255, a: 255 }),
    hoodie: t({ r: 48, g: 182, b: 240, a: 255 }),
    hoodieDark: t({ r: 26, g: 128, b: 196, a: 255 }),
    pants: t({ r: 46, g: 70, b: 128, a: 255 }),
    shoe: t({ r: 248, g: 250, b: 255, a: 255 }),
    sole: t({ r: 130, g: 214, b: 50, a: 255 }),
    phones: t({ r: 120, g: 220, b: 250, a: 255 }),
    eyeWhite: shade ? glow : COLOR_WHITE,
    shade,
    glow,
  };
}

function drawHypeHead(ctx: CanvasRenderingContext2D, H: Xf, f: Face, k: ReturnType<typeof hypePalette>): void {
  new Ink(ctx, H)
    .p(
      [
        [-50, -18],
        [-34, -22],
        [-42, 4],
      ],
      k.hair,
    )
    .p(
      [
        [-48, -2],
        [-34, -8],
        [-40, 18],
      ],
      k.hair,
    )
    .draw();
  new Ink(ctx, H).e(0, 2, 44, 42, k.skin).e(42, 16, 9, 8, k.skin).draw();
  new Ink(ctx, H, 3.0).e(-18, 10, 9, 11, k.skin).draw();
  drawEll(ctx, H, -18, 10, 4, 6, k.skinShade);

  const dome: [number, number][] = [];
  for (let i = 0; i <= 16; i++) {
    const a = Math.PI + (Math.PI * i) / 16;
    dome.push([-3 + Math.cos(a) * 49, -22 + Math.sin(a) * 44]);
  }
  new Ink(ctx, H).e(-12, -68, 14, 13, k.pom).p(dome, k.beanie).draw();
  for (let i = -2; i <= 2; i++) {
    drawLine(ctx, H, -3 + i * 15, -30, -3 + i * 9, -58 + Math.abs(i) * 6, 2.4, k.beanieDark);
  }
  new Ink(ctx, H)
    .p(
      [
        [-54, -32],
        [49, -32],
        [51, -15],
        [-55, -13],
      ],
      k.beanieDark,
    )
    .draw();
  for (let i = 0; i < 7; i++) {
    drawLine(ctx, H, -46 + i * 14, -29, -46 + i * 14, -17, 2.0, fade(k.beanie, 0.75));
  }

  if (f.miss) {
    drawLine(ctx, H, 5, -4, 21, -11, 3.8, k.hair);
    drawLine(ctx, H, 27, -11, 40, -6, 3.8, k.hair);
  } else {
    const up = f.singing ? -3 : 0;
    drawLine(ctx, H, 4, -7 + up, 21, -10 + up, 3.8, k.hair);
    drawLine(ctx, H, 27, -10 + up, 40, -7 + up, 3.8, k.hair);
  }

  if (k.shade) {
    for (let i = 0; i < 2; i++) {
      const x = i ? 32 : 13;
      const y = i ? 10 : 11;
      const r = i ? 7 : 8.5;
      drawEll(ctx, H, x, y, r, f.blink < 0.5 || f.miss ? 1.8 : r * 1.25, k.glow);
      const [gx, gy] = xfPt(H, x, y);
      drawGlow(ctx, gx, gy, xfLen(H, 26), fade(k.glow, 0.5));
    }
  } else {
    drawEye(ctx, H, 13, 11, 9.5, 12.5, f, k.eyeWhite, 1);
    drawEye(ctx, H, 32, 10, 8.0, 11.0, f, k.eyeWhite, -1);
  }
  drawMouth(ctx, H, 23, 32, 11, f, null, false);
  if (f.miss) drawSweat(ctx, H, -30, -4);
}

function drawHype(ctx: CanvasRenderingContext2D, s: CharacterState, P: Pose, fx: number, fy: number, sc: number, now: number, flip: boolean, shade: boolean): void {
  const k = hypePalette(shade, s.tint, P.missing);
  const base: Xf = { ox: fx, oy: fy, s: sc, sx: 1, sy: 1, rot: 0, flip: false };
  drawEll(ctx, base, 0, 0, 66, 11, fade(COLOR_BLACK, 0.3));
  if (shade) drawGlow(ctx, fx, fy - 150 * sc, 210 * sc, fade(s.tint, 0.28 + 0.2 * P.bop));
  drawBubbles(ctx, base, 0, -30, 220, 320, 6, now, 0.16, 1.0);

  const X: Xf = { ox: fx + P.dx * sc, oy: fy + P.dy * sc, s: sc, sx: P.sx, sy: P.sy, rot: P.rot, flip };
  const slot = slotOf(P, flip);
  const sway = P.singing ? 0 : Math.sin(now * 2) * 2;

  const kFar: [number, number][] = [
    [-30, -104],
    [-52, -232],
    [-36, -96],
    [-6, -128],
    [-84, -150],
  ];
  const kFarBend = [10, -10, 10, 8, -10];
  const fhX = kFar[slot][0];
  const fhY = kFar[slot][1] + sway;
  new Ink(ctx, X).limb(-22, -164, fhX, fhY, 12, kFarBend[slot], mix(k.hoodie, K_INK, 0.22)).draw();
  new Ink(ctx, X).e(fhX, fhY, 10, 9, k.skinShade).draw();

  new Ink(ctx, X).limb(-14, -90, -20, -26, 14.5, -4, k.pants).limb(12, -90, 20, -26, 15, 4, k.pants).draw();
  for (const x of [-16, 26]) {
    new Ink(ctx, X)
      .e(x, -15, 25, 13, k.shoe)
      .p(
        [
          [x - 25, -10],
          [x + 27, -10],
          [x + 27, 0],
          [x - 25, 0],
        ],
        k.sole,
      )
      .draw();
    drawLine(ctx, X, x - 14, -19, x + 10, -14, 2.6, k.sole);
  }

  new Ink(ctx, X)
    .e(-30, -182, 26, 16, k.hoodieDark)
    .p(
      [
        [-40, -178],
        [36, -178],
        [46, -88],
        [-44, -88],
      ],
      k.hoodie,
    )
    .e(-36, -166, 15, 15, k.hoodie)
    .e(32, -166, 15, 15, k.hoodie)
    .e(1, -90, 46, 10, k.hoodie)
    .draw();
  new Ink(ctx, X, 2.6)
    .p(
      [
        [-24, -130],
        [26, -130],
        [32, -102],
        [-30, -102],
      ],
      k.hoodieDark,
    )
    .draw();
  drawLine(ctx, X, -44, -96, 46, -96, 2.4, k.hoodieDark);

  // Headphones & neck
  new Ink(ctx, X, 2.0, false).c(-22, -186, 26, -188, 5.5, mix(k.phones, K_INK, 0.45)).draw();
  new Ink(ctx, X).c(2, -178, 4, -202, 12, k.skin).draw();
  new Ink(ctx, X).e(-22, -184, 11, 14, k.phones).e(28, -186, 12, 15, k.phones).draw();
  drawEll(ctx, X, -22, -184, 5, 8, mix(k.phones, K_INK, 0.4));
  drawEll(ctx, X, 28, -186, 5.5, 8.5, mix(k.phones, K_INK, 0.4));
  drawLine(ctx, X, -4, -170, -8, -140, 2.2, k.shoe);
  drawLine(ctx, X, 10, -170, 13, -142, 2.2, k.shoe);

  const [hx, hy] = xfPt(X, 6, -232 + 3 * P.bop);
  drawHypeHead(ctx, { ...X, ox: hx, oy: hy }, faceFor(P, now, flip, 0.7), k);

  const kNear: [number, number][] = [
    [36, -124],
    [80, -252],
    [44, -96],
    [102, -178],
    [-60, -164],
  ];
  const kMicDir: [number, number][] = [
    [0.25, -1],
    [0.3, -1],
    [0.35, 1],
    [1, -0.2],
    [-1, -0.3],
  ];
  const kNearBend = [12, 12, 12, 12, -12];
  const nhX = kNear[slot][0];
  const nhY = kNear[slot][1] + sway;
  new Ink(ctx, X).limb(16, -168, nhX, nhY, 12.5, kNearBend[slot], k.hoodie).draw();
  drawMic(ctx, X, nhX, nhY, kMicDir[slot][0], kMicDir[slot][1], { r: 206, g: 212, b: 228, a: 255 });
  new Ink(ctx, X).e(nhX, nhY, 11, 10, k.skin).draw();
  if (P.singing && !P.missing) {
    drawBubbles(ctx, X, nhX + kMicDir[slot][0] * 40, nhY + kMicDir[slot][1] * 30, 34, 110, 4, now, 0.9, 11.0);
  }
}

// --- PRINCIPAL P ---
function drawPrincipalHead(ctx: CanvasRenderingContext2D, H: Xf, f: Face, missing: boolean): void {
  const tone = (c: RGBColor) => (missing ? mix(c, K_MISS_TONE, 0.38) : c);
  const hair = tone({ r: 156, g: 96, b: 255, a: 255 });
  const hairDark = tone({ r: 96, g: 48, b: 188, a: 255 });
  const skin = tone({ r: 246, g: 206, b: 178, a: 255 });

  new Ink(ctx, H).e(-10, -52, 34, 42, hair).e(-6, -12, 40, 32, hair).e(-36, 10, 12, 16, hair).draw();
  new Ink(ctx, H)
    .e(4, 6, 31, 37, skin)
    .p(
      [
        [28, -2],
        [50, 16],
        [30, 20],
      ],
      skin,
    )
    .p(
      [
        [-6, 34],
        [32, 22],
        [30, 36],
        [14, 44],
      ],
      skin,
    )
    .draw();

  // Cat-eye glasses
  const lens: RGBColor = { r: 216, g: 246, b: 255, a: 255 };
  new Ink(ctx, H, 3.8).e(14, 6, 11.5, 8.5, lens).e(36, 5, 9.5, 7.5, lens).draw();
  drawPoly(
    ctx,
    H,
    [
      [0, 2],
      [6, -6],
      [-4, -8],
    ],
    K_INK,
  );
  if (f.blink < 0.5) {
    drawLine(ctx, H, 7, 7, 21, 7, 2.6, K_INK);
    drawLine(ctx, H, 30, 6, 42, 6, 2.6, K_INK);
  } else {
    const pr = f.miss ? 2.0 : 3.4;
    drawEll(ctx, H, 14 + f.lookX * 4, 7 + f.lookY * 2.5, pr, pr * 1.15, K_INK);
    drawEll(ctx, H, 36 + f.lookX * 3.5, 6 + f.lookY * 2.2, pr * 0.9, pr * 1.05, K_INK);
  }
  drawLine(ctx, H, 8, 3, 13, -1, 2.0, fade(COLOR_WHITE, 0.9));

  if (f.miss) {
    drawLine(ctx, H, 4, -10, 20, -15, 3.8, hairDark);
    drawLine(ctx, H, 30, -15, 44, -11, 3.8, hairDark);
  } else {
    const r = f.singing ? 2.5 : 0;
    drawLine(ctx, H, 3, -12 - r, 22, -6, 3.8, hairDark);
    drawLine(ctx, H, 29, -6, 45, -11 - r, 3.8, hairDark);
  }
  drawMouth(ctx, H, 23, 27, 9, f, tone({ r: 224, g: 40, b: 84, a: 255 }), false);
  if (f.miss) drawSweat(ctx, H, -22, -14);
}

function drawPrincipal(ctx: CanvasRenderingContext2D, P: Pose, fx: number, fy: number, sc: number, now: number, flip: boolean): void {
  const tone = (c: RGBColor) => (P.missing ? mix(c, K_MISS_TONE, 0.38) : c);
  const blazer = tone({ r: 228, g: 66, b: 148, a: 255 });
  const blazerDark = tone({ r: 170, g: 36, b: 106, a: 255 });
  const skirt = tone({ r: 98, g: 36, b: 94, a: 255 });
  const legs = tone({ r: 72, g: 52, b: 96, a: 255 });
  const shoes = tone({ r: 168, g: 28, b: 70, a: 255 });
  const shirt = tone({ r: 250, g: 250, b: 252, a: 255 });
  const tie = tone({ r: 255, g: 208, b: 44, a: 255 });
  const skin = tone({ r: 246, g: 206, b: 178, a: 255 });

  const base: Xf = { ox: fx, oy: fy, s: sc, sx: 1, sy: 1, rot: 0, flip: false };
  drawEll(ctx, base, 0, 0, 52, 9, fade(COLOR_BLACK, 0.3));
  drawBubbles(ctx, base, 0, -30, 200, 400, 5, now, 0.14, 2.0);
  const X: Xf = { ox: fx + P.dx * sc, oy: fy + P.dy * sc, s: sc, sx: P.sx, sy: P.sy, rot: P.rot, flip };
  const slot = slotOf(P, flip);

  const kFar: [number, number][] = [
    [-38, -168],
    [-46, -330],
    [-42, -150],
    [-8, -206],
    [-96, -240],
  ];
  const kFarBend = [26, -8, 20, 12, -10];
  new Ink(ctx, X).limb(-28, -246, kFar[slot][0], kFar[slot][1], 9.5, kFarBend[slot], blazerDark).draw();
  new Ink(ctx, X).e(kFar[slot][0], kFar[slot][1], 8, 7.5, skin).draw();

  new Ink(ctx, X).limb(-9, -120, -13, -22, 7, -3, legs).limb(9, -120, 13, -22, 7.5, 3, legs).draw();
  for (const x of [-13, 13]) {
    new Ink(ctx, X, 3.4)
      .p(
        [
          [x - 8, -26],
          [x + 12, -20],
          [x + 22, -6],
          [x - 8, -6],
        ],
        shoes,
      )
      .c(x - 6, -8, x - 7, 0, 2.6, shoes)
      .draw();
  }

  new Ink(ctx, X)
    .p(
      [
        [-30, -172],
        [30, -172],
        [26, -108],
        [-26, -108],
      ],
      skirt,
    )
    .draw();
  new Ink(ctx, X)
    .p(
      [
        [-36, -254],
        [34, -254],
        [32, -160],
        [-34, -160],
      ],
      blazer,
    )
    .e(-34, -246, 13, 10, blazer)
    .e(32, -246, 13, 10, blazer)
    .draw();
  drawPoly(
    ctx,
    X,
    [
      [-12, -254],
      [12, -254],
      [0, -216],
    ],
    shirt,
  );
  new Ink(ctx, X, 2.4)
    .p(
      [
        [-3.5, -250],
        [3.5, -250],
        [5, -220],
        [0, -212],
        [-5, -220],
      ],
      tie,
    )
    .draw();
  new Ink(ctx, X).c(2, -252, 4, -270, 8, skin).draw();

  const [hx, hy] = xfPt(X, 6, -298 + 3 * P.bop);
  drawPrincipalHead(ctx, { ...X, ox: hx, oy: hy }, faceFor(P, now, flip, 1.9), P.missing);

  // Near arm + megaphone
  const kFrom: [number, number][] = [
    [46, -190],
    [46, -284],
    [44, -254],
    [48, -268],
    [-60, -262],
  ];
  const kDir: [number, number][] = [
    [0.38, 0.92],
    [0.45, -0.9],
    [0.72, 0.7],
    [1, -0.08],
    [-1, -0.08],
  ];
  const kBend = [14, 10, 12, 12, -12];
  const from = kFrom[slot];
  const dLen = Math.hypot(kDir[slot][0], kDir[slot][1]);
  const dx = kDir[slot][0] / dLen;
  const dy = kDir[slot][1] / dLen;
  let sx = -dy;
  let sy = dx;
  if (sy < 0) {
    sx = -sx;
    sy = -sy;
  }
  const gripX = from[0] + dx * 16 + sx * 20;
  const gripY = from[1] + dy * 16 + sy * 20;
  new Ink(ctx, X).limb(22, -244, gripX, gripY, 9.5, kBend[slot], blazer).draw();

  const at = (t: number, sgn: number): [number, number] => {
    const hw = 7 + 18 * t;
    return [from[0] + dx * 58 * t + sx * hw * sgn, from[1] + dy * 58 * t + sy * hw * sgn];
  };
  new Ink(ctx, X, 3.2).c(from[0] + dx * 14 + sx * 6, from[1] + dy * 14 + sy * 6, gripX, gripY, 4.5, { r: 60, g: 58, b: 76, a: 255 }).draw();
  new Ink(ctx, X).p([at(0, -1), at(0, 1), at(1, 1), at(1, -1)], { r: 248, g: 248, b: 252, a: 255 }).draw();
  drawPoly(ctx, X, [at(0.6, -1), at(0.6, 1), at(0.78, 1), at(0.78, -1)], { r: 60, g: 200, b: 255, a: 255 });
  new Ink(ctx, X).e(gripX, gripY, 8, 7.5, skin).draw();
  if (P.singing && !P.missing) {
    drawBubbles(ctx, X, from[0] + dx * 70, from[1] + dy * 70, 40, 110, 4, now, 0.9, 12.0);
  }
}

// --- DJ HAND ---
function drawDjGlove(
  ctx: CanvasRenderingContext2D,
  G: Xf,
  f: Face,
  missing: boolean,
  spread: number,
  curl: number,
  point: boolean,
  now: number,
): void {
  const tone = (c: RGBColor) => (missing ? mix(c, K_MISS_TONE, 0.38) : c);
  const glove = tone({ r: 252, g: 252, b: 255, a: 255 });
  const cuff = tone({ r: 70, g: 170, b: 255, a: 255 });
  const shades = tone({ r: 22, g: 20, b: 32, a: 255 });

  new Ink(ctx, G).e(0, 76, 46, 15, cuff).draw();
  const k = new Ink(ctx, G)
    .p(
      [
        [-34, 28],
        [34, 28],
        [40, 68],
        [-40, 68],
      ],
      glove,
    )
    .e(0, 0, 58, 60, glove);

  const kLen = [50, 66, 70, 58];
  for (let i = 0; i < 4; i++) {
    const bx = -39 + i * 26;
    let len = kLen[i] * curl;
    if (point) len = i === 3 ? kLen[i] * 1.12 : kLen[i] * 0.42;
    const wig = Math.sin(now * 3 + i * 1.3) * 2.5;
    const tx = bx + (bx / 39) * 15 * spread + wig;
    const ty = -42 - len;
    k.c(bx, -38, tx, ty, 14.5, glove);
  }
  k.c(46, 8, 80, -20, 14.5, glove);
  k.draw();

  for (let i = -4; i <= 4; i++) {
    new Ink(ctx, G, 2.2).e(i * 9.5, 78 - Math.abs(i) * 0.6, 4.4, 3.6, { r: 255, g: 208, b: 60, a: 255 }).draw();
  }

  const fx = 6;
  new Ink(ctx, G, 3.4)
    .p(
      [
        [fx - 42, -20],
        [fx - 4, -20],
        [fx - 8, 2],
        [fx - 38, 0],
      ],
      shades,
    )
    .p(
      [
        [fx + 6, -20],
        [fx + 44, -20],
        [fx + 40, 0],
        [fx + 10, 2],
      ],
      shades,
    )
    .c(fx - 4, -16, fx + 6, -16, 3.2, shades)
    .draw();
  drawLine(ctx, G, fx - 32, -14, fx - 22, -5, 2.6, fade(COLOR_WHITE, 0.8));
  drawLine(ctx, G, fx + 16, -14, fx + 26, -5, 2.6, fade(COLOR_WHITE, 0.8));
  drawMouth(ctx, G, fx + 2, 28, 16, f, null, true);
  if (f.miss) drawSweat(ctx, G, fx + 46, -36);
}

function drawHand(ctx: CanvasRenderingContext2D, s: CharacterState, P: Pose, fx: number, fy: number, sc: number, now: number, flip: boolean): void {
  const cuff = P.missing ? mix({ r: 70, g: 170, b: 255, a: 255 }, K_MISS_TONE, 0.38) : { r: 70, g: 170, b: 255, a: 255 };
  const base: Xf = { ox: fx, oy: fy, s: sc, sx: 1, sy: 1, rot: 0, flip: false };
  drawEll(ctx, base, 0, 0, 96, 13, fade(COLOR_BLACK, 0.32));

  // DJ booth
  const B: Xf = { ox: fx, oy: fy, s: sc, sx: 1, sy: 1, rot: 0, flip };
  new Ink(ctx, B)
    .p(
      [
        [-84, -98],
        [84, -98],
        [76, 0],
        [-76, 0],
      ],
      { r: 36, g: 104, b: 156, a: 255 },
    )
    .draw();
  drawLine(ctx, B, -78, -74, 78, -74, 4, cuff);
  const [gx, gy] = xfPt(B, 0, -74);
  drawGlow(ctx, gx, gy, 110 * sc, fade(cuff, 0.25 + 0.25 * P.bop));
  for (const x of [-42, 42]) {
    const pump = 1 + 0.12 * P.bop;
    new Ink(ctx, B, 3.0).e(x, -36, 20 * pump, 20 * pump, { r: 196, g: 210, b: 230, a: 255 }).draw();
    drawEll(ctx, B, x, -36, 9 * pump, 9 * pump, { r: 20, g: 40, b: 70, a: 255 });
  }
  new Ink(ctx, B)
    .p(
      [
        [-92, -112],
        [92, -112],
        [86, -96],
        [-86, -96],
      ],
      { r: 200, g: 214, b: 232, a: 255 },
    )
    .draw();

  // Spinning record
  const spin = now * 4 + (P.singing ? Math.sin((now - s.singStart) * 22) * 1.4 : 0);
  new Ink(ctx, B, 3.0).e(-32, -118, 40, 10, { r: 20, g: 18, b: 28, a: 255 }).draw();
  drawEll(ctx, B, -32, -118, 12, 3.2, { r: 130, g: 214, b: 50, a: 255 });
  drawLine(
    ctx,
    B,
    -32 + Math.cos(spin) * 16,
    -118 + Math.sin(spin) * 4,
    -32 + Math.cos(spin) * 36,
    -118 + Math.sin(spin) * 9,
    2,
    fade(COLOR_WHITE, 0.4),
  );
  drawLine(ctx, B, 24, -124, -2, -118, 3, { r: 214, g: 218, b: 234, a: 255 });

  drawBubbles(ctx, B, 0, -70, 160, 330, 7, now, 0.22, 3.0);

  const hover = Math.sin(now * 1.8) * 8;
  let lift = 0;
  let spread = 1;
  let curl = 1;
  let tilt = 0;
  let point = false;
  if (P.singing) {
    if (P.dir === 0) {
      tilt = -0.32;
      point = true;
    }
    if (P.dir === 3) {
      tilt = 0.32;
      point = true;
    }
    if (P.dir === 2) {
      lift = -24;
      spread = 1.5;
    }
    if (P.dir === 1) {
      lift = 16;
      curl = 0.5;
    }
  }
  const G: Xf = {
    ox: fx + P.dx * sc * 0.6,
    oy: fy + (-236 + hover + lift + P.dy * 0.5) * sc,
    s: sc,
    sx: P.sx,
    sy: P.sy,
    rot: P.rot + tilt,
    flip,
  };
  drawGlow(ctx, G.ox, G.oy, 160 * sc, fade(cuff, 0.18 + 0.18 * P.bop));
  drawDjGlove(ctx, G, faceFor(P, now, flip, 2.6), P.missing, spread, curl, point, now);
}

// --- D-MIZE (Punk Skeleton) ---
function drawDmizeHead(ctx: CanvasRenderingContext2D, H: Xf, f: Face, missing: boolean, glowEyes: boolean): void {
  const tone = (c: RGBColor) => (missing ? mix(c, K_MISS_TONE, 0.38) : c);
  const bone = tone({ r: 242, g: 234, b: 212, a: 255 });
  const boneShade = tone({ r: 200, g: 190, b: 166, a: 255 });
  const mohawk = tone({ r: 255, g: 48, b: 84, a: 255 });
  const mohawkDark = tone({ r: 186, g: 22, b: 56, a: 255 });
  const socket: RGBColor = { r: 20, g: 10, b: 22, a: 255 };
  const eyes: RGBColor = { r: 255, g: 60, b: 80, a: 255 };
  const teeth = tone({ r: 252, g: 250, b: 240, a: 255 });

  // Mohawk spikes
  const kH = [30, 48, 60, 54, 38];
  const mk = new Ink(ctx, H);
  for (let i = 0; i < 5; i++) {
    const a = Math.PI * (1.12 + 0.19 * i);
    const ca = Math.cos(a);
    const sa = Math.sin(a);
    const bx = ca * 32;
    const by = sa * 32 - 4;
    mk.p(
      [
        [bx - sa * 13, by + ca * 13],
        [bx + sa * 13, by - ca * 13],
        [ca * (32 + kH[i]) - 6, sa * (32 + kH[i]) - 4],
      ],
      mohawk,
    );
  }
  mk.draw();
  for (let i = 0; i < 5; i++) {
    const a = Math.PI * (1.12 + 0.19 * i);
    drawLine(ctx, H, Math.cos(a) * 30, Math.sin(a) * 30 - 4, Math.cos(a) * (30 + kH[i] * 0.7) - 4, Math.sin(a) * (30 + kH[i] * 0.7) - 4, 4, mohawkDark);
  }

  new Ink(ctx, H)
    .e(0, 0, 42, 40, bone)
    .e(14, 18, 26, 16, bone)
    .p(
      [
        [0, 18],
        [44, 14],
        [46, 34],
        [2, 36],
      ],
      bone,
    )
    .draw();
  drawEll(ctx, H, -16, 10, 12, 16, boneShade);
  drawEll(ctx, H, 10, 2, 12, 14, socket);
  drawEll(ctx, H, 33, 1, 9.5, 12.5, socket);
  if (f.blink >= 0.5) {
    const ex = (f.lookX - 0.3) * 3;
    const ey = f.lookY * 3;
    const r = f.miss ? 2.4 : 4.4;
    drawEll(ctx, H, 11 + ex, 4 + ey, r, r * 1.1, eyes);
    drawEll(ctx, H, 34 + ex, 3 + ey, r * 0.85, r * 0.95, eyes);
    if (glowEyes) {
      const [g1x, g1y] = xfPt(H, 11 + ex, 4 + ey);
      const [g2x, g2y] = xfPt(H, 34 + ex, 3 + ey);
      drawGlow(ctx, g1x, g1y, xfLen(H, 22), fade(eyes, 0.55));
      drawGlow(ctx, g2x, g2y, xfLen(H, 18), fade(eyes, 0.5));
    }
  }
  drawPoly(
    ctx,
    H,
    [
      [22, 14],
      [30, 14],
      [26, 23],
    ],
    socket,
  );
  const o = f.open * 14;
  if (o > 0.5) {
    drawPoly(
      ctx,
      H,
      [
        [4, 31],
        [42, 29],
        [40, 37 + o],
        [6, 38 + o],
      ],
      { r: 60, g: 8, b: 24, a: 255 },
    );
  }
  drawPoly(
    ctx,
    H,
    [
      [4, 24],
      [42, 22],
      [42, 31],
      [4, 32],
    ],
    teeth,
  );
  new Ink(ctx, H)
    .p(
      [
        [-2, 37 + o],
        [44, 34 + o],
        [42, 49 + o],
        [10, 56 + o],
        [-4, 47 + o],
      ],
      bone,
    )
    .draw();
  drawPoly(
    ctx,
    H,
    [
      [4, 37 + o],
      [40, 35 + o],
      [40, 41 + o],
      [4, 43 + o],
    ],
    teeth,
  );
}

function drawBoneArm(ctx: CanvasRenderingContext2D, X: Xf, ax: number, ay: number, bx: number, by: number, bend: number, sleeve: RGBColor, bone: RGBColor): void {
  const dx = bx - ax;
  const dy = by - ay;
  const len = Math.max(0.001, Math.hypot(dx, dy));
  const jx = (ax + bx) * 0.5 - (dy / len) * bend;
  const jy = (ay + by) * 0.5 + (dx / len) * bend;
  new Ink(ctx, X, 3.4).c(jx, jy, bx, by, 5, bone).draw();
  new Ink(ctx, X).c(ax, ay, jx, jy, 13, sleeve).draw();
  const a0 = Math.atan2(by - jy, bx - jx);
  const hand = new Ink(ctx, X, 3.0);
  for (let i = -1; i <= 1; i++) {
    const a = a0 + i * 0.45;
    hand.c(bx, by, bx + Math.cos(a) * 15, by + Math.sin(a) * 15, 2.8, bone);
  }
  hand.e(bx, by, 7.5, 7, bone).draw();
}

function drawDemise(ctx: CanvasRenderingContext2D, P: Pose, fx: number, fy: number, sc: number, now: number, flip: boolean): void {
  const tone = (c: RGBColor) => (P.missing ? mix(c, K_MISS_TONE, 0.38) : c);
  const jacket = tone({ r: 42, g: 38, b: 54, a: 255 });
  const jacketHi = tone({ r: 92, g: 86, b: 112, a: 255 });
  const tee = tone({ r: 232, g: 38, b: 66, a: 255 });
  const jeans = tone({ r: 40, g: 50, b: 84, a: 255 });
  const boots = tone({ r: 30, g: 26, b: 38, a: 255 });
  const bone = tone({ r: 242, g: 234, b: 212, a: 255 });
  const boneShade = tone({ r: 200, g: 190, b: 166, a: 255 });
  const silver: RGBColor = { r: 208, g: 214, b: 230, a: 255 };
  const red: RGBColor = { r: 255, g: 48, b: 84, a: 255 };

  const base: Xf = { ox: fx, oy: fy, s: sc, sx: 1, sy: 1, rot: 0, flip: false };
  drawEll(ctx, base, 0, 0, 80, 12, fade(COLOR_BLACK, 0.35));
  drawBubbles(ctx, base, 0, -30, 240, 420, 5, now, 0.13, 4.0);
  const X: Xf = { ox: fx + P.dx * sc, oy: fy + P.dy * sc, s: sc, sx: P.sx, sy: P.sy, rot: P.rot, flip };
  const slot = slotOf(P, flip);

  const kFar: [number, number][] = [
    [-46, -150],
    [-64, -318],
    [-42, -132],
    [-14, -178],
    [-96, -214],
  ];
  const kFarBend = [12, -10, 12, 10, -12];
  drawBoneArm(ctx, X, -36, -234, kFar[slot][0], kFar[slot][1], kFarBend[slot], mix(jacket, K_INK, 0.3), boneShade);

  new Ink(ctx, X).limb(-16, -130, -22, -32, 13, -4, jeans).limb(14, -130, 18, -32, 13.5, 4, jeans).draw();
  for (const x of [-20, 22]) {
    new Ink(ctx, X)
      .e(x + 6, -20, 26, 19, boots)
      .p(
        [
          [x - 20, -10],
          [x + 32, -10],
          [x + 32, 0],
          [x - 20, 0],
        ],
        { r: 72, g: 68, b: 86, a: 255 },
      )
      .draw();
  }

  // Lightning tee & leather jacket
  new Ink(ctx, X)
    .p(
      [
        [-30, -238],
        [30, -238],
        [32, -130],
        [-32, -130],
      ],
      tee,
    )
    .draw();
  drawPoly(
    ctx,
    X,
    [
      [4, -214],
      [-10, -184],
      [0, -184],
    ],
    K_INK,
  );
  drawPoly(
    ctx,
    X,
    [
      [2, -190],
      [12, -190],
      [-4, -156],
    ],
    K_INK,
  );
  new Ink(ctx, X)
    .p(
      [
        [-46, -248],
        [-10, -248],
        [-16, -136],
        [-46, -136],
      ],
      jacket,
    )
    .p(
      [
        [8, -248],
        [44, -248],
        [44, -136],
        [16, -136],
      ],
      jacket,
    )
    .draw();
  drawPoly(
    ctx,
    X,
    [
      [-10, -248],
      [-24, -248],
      [-14, -222],
    ],
    jacketHi,
  );
  drawPoly(
    ctx,
    X,
    [
      [8, -248],
      [22, -248],
      [12, -222],
    ],
    jacketHi,
  );
  new Ink(ctx, X).c(2, -246, 4, -272, 8.5, bone).draw();
  new Ink(ctx, X, 3.2)
    .p(
      [
        [-12, -262],
        [18, -262],
        [18, -250],
        [-12, -250],
      ],
      boots,
    )
    .draw();
  for (let i = 0; i < 4; i++) {
    const sx = -7 + i * 8;
    drawPoly(
      ctx,
      X,
      [
        [sx - 3, -250],
        [sx + 3, -250],
        [sx, -242],
      ],
      silver,
    );
  }

  const [hx, hy] = xfPt(X, 8, -318 + 3 * P.bop);
  drawDmizeHead(ctx, { ...X, ox: hx, oy: hy }, faceFor(P, now, flip, 3.3), P.missing, true);

  const kNear: [number, number][] = [
    [46, -176],
    [60, -326],
    [50, -134],
    [118, -232],
    [-74, -230],
  ];
  const kMicDir: [number, number][] = [
    [0.2, -1],
    [0.2, -1],
    [0.3, 1],
    [1, -0.2],
    [-1, -0.25],
  ];
  const kNearBend = [14, 10, 14, 12, -12];
  drawMic(ctx, X, kNear[slot][0], kNear[slot][1], kMicDir[slot][0], kMicDir[slot][1], red);
  drawBoneArm(ctx, X, 32, -236, kNear[slot][0], kNear[slot][1], kNearBend[slot], jacket, bone);
  if (P.singing && !P.missing) {
    drawBubbles(ctx, X, kNear[slot][0] + kMicDir[slot][0] * 40, kNear[slot][1] + kMicDir[slot][1] * 30, 34, 110, 4, now, 0.9, 13.0);
  }
}

// --- Public Entry Points ---
export function drawCharacter(
  ctx: CanvasRenderingContext2D,
  s: CharacterState,
  feetX: number,
  feetY: number,
  scale: number,
  now: number,
  faceLeft: boolean,
): void {
  const P = computePose(s, now);
  switch (s.id) {
    case 'herald':
      drawHype(ctx, s, P, feetX, feetY, scale, now, faceLeft, false);
      break;
    case 'shade':
      drawHype(ctx, s, P, feetX, feetY, scale, now, faceLeft, true);
      break;
    case 'principality':
      drawPrincipal(ctx, P, feetX, feetY, scale, now, faceLeft);
      break;
    case 'hand':
      drawHand(ctx, s, P, feetX, feetY, scale, now, faceLeft);
      break;
    case 'demise':
      drawDemise(ctx, P, feetX, feetY, scale, now, faceLeft);
      break;
  }
}

export function drawIcon(
  ctx: CanvasRenderingContext2D,
  id: CharId,
  tint: RGBColor,
  cx: number,
  cy: number,
  size: number,
  losing: boolean,
  now: number,
  faceLeft = false,
): void {
  const f: Face = {
    open: losing ? 0.35 : 0,
    lookX: 0.3,
    lookY: 0,
    blink: 1,
    singing: false,
    miss: losing,
    dir: -1,
  };
  const u = size / 100;
  if (id === 'herald' || id === 'shade') {
    const hs = u * 0.74;
    drawHypeHead(ctx, { ox: cx, oy: cy + 16 * hs, s: hs, sx: 1, sy: 1, rot: 0, flip: faceLeft }, f, hypePalette(id === 'shade', tint, false));
  } else if (id === 'principality') {
    const hs = u * 0.68;
    drawPrincipalHead(ctx, { ox: cx, oy: cy + 24 * hs, s: hs, sx: 1, sy: 1, rot: 0, flip: faceLeft }, f, false);
  } else if (id === 'hand') {
    const hs = u * 0.44;
    drawDjGlove(ctx, { ox: cx, oy: cy + 12 * hs, s: hs, sx: 1, sy: 1, rot: 0, flip: faceLeft }, f, false, 1, 1, false, now);
  } else {
    const hs = u * 0.6;
    drawDmizeHead(ctx, { ox: cx, oy: cy + 26 * hs, s: hs, sx: 1, sy: 1, rot: 0, flip: faceLeft }, f, false, false);
  }
}
