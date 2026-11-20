// Canvas 2D rendering primitives, offscreen note/receptor sprites, stars, glassy 3D cubes, and menu backdrop
// Ported from src/gfx.cpp & src/app.cpp
import { kCenter, kDown, kLeft, kRight, kUp, RGBColor } from './types';
import { Rng } from './synth';

export const CANVAS_W = 1280;
export const CANVAS_H = 720;

export const COLOR_GOLD: RGBColor = { r: 255, g: 222, b: 64, a: 255 };
export const COLOR_IVORY: RGBColor = { r: 242, g: 244, b: 255, a: 255 };
export const COLOR_WHITE: RGBColor = { r: 255, g: 255, b: 255, a: 255 };
export const COLOR_BLACK: RGBColor = { r: 0, g: 0, b: 0, a: 255 };

export function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

export function saturate(v: number): number {
  return clamp(v, 0, 1);
}

export function hexToRgb(hex: number, a = 255): RGBColor {
  return { r: (hex >> 16) & 255, g: (hex >> 8) & 255, b: hex & 255, a };
}

export function rgba(c: RGBColor, alphaScale = 1): string {
  const a = ((c.a ?? 255) / 255) * saturate(alphaScale);
  return `rgba(${Math.round(c.r)},${Math.round(c.g)},${Math.round(c.b)},${a.toFixed(3)})`;
}

export function fade(c: RGBColor, a: number): RGBColor {
  return { r: c.r, g: c.g, b: c.b, a: Math.round((c.a ?? 255) * saturate(a)) };
}

export function mix(a: RGBColor, b: RGBColor, t: number): RGBColor {
  const u = saturate(t);
  return {
    r: Math.round(a.r + (b.r - a.r) * u),
    g: Math.round(a.g + (b.g - a.g) * u),
    b: Math.round(a.b + (b.b - a.b) * u),
    a: Math.round((a.a ?? 255) + ((b.a ?? 255) - (a.a ?? 255)) * u),
  };
}

export function brighten(c: RGBColor, amount: number): RGBColor {
  return amount >= 0
    ? mix(c, { r: 255, g: 255, b: 255, a: c.a ?? 255 }, amount)
    : mix(c, { r: 0, g: 0, b: 0, a: c.a ?? 255 }, -amount);
}

export function dirColor(dir: number): RGBColor {
  switch (dir) {
    case kLeft:
      return { r: 255, g: 92, b: 138, a: 255 };
    case kDown:
      return { r: 54, g: 181, b: 255, a: 255 };
    case kUp:
      return { r: 255, g: 201, b: 60, a: 255 };
    case kRight:
      return { r: 60, g: 224, b: 160, a: 255 };
    default:
      return { r: 201, g: 166, b: 255, a: 255 };
  }
}

export function dirRotationRad(dir: number): number {
  switch (dir) {
    case kUp:
      return Math.PI * 0.5;
    case kRight:
      return Math.PI;
    case kDown:
      return Math.PI * 1.5;
    default:
      return 0;
  }
}

// --- Offscreen SDF Arrow / Receptor Sprites (matching src/gfx.cpp makeGlyph) ---
let noteSprites: HTMLCanvasElement[] | null = null;
let receptorSprite: HTMLCanvasElement | null = null;

function segDist(px: number, py: number, ax: number, ay: number, bx: number, by: number): number {
  const vx = bx - ax;
  const vy = by - ay;
  const wx = px - ax;
  const wy = py - ay;
  const l2 = vx * vx + vy * vy;
  const t = l2 > 0 ? saturate((wx * vx + wy * vy) / l2) : 0;
  return Math.hypot(wx - vx * t, wy - vy * t);
}

function glyphDist(u: number, v: number): number {
  let d = segDist(u, v, -0.5, 0, 0.6, 0);
  d = Math.min(d, segDist(u, v, -0.6, 0, -0.02, -0.58));
  d = Math.min(d, segDist(u, v, -0.6, 0, -0.02, 0.58));
  return d;
}

function makeGlyphCanvas(base: RGBColor, receptor: boolean, size = 128): HTMLCanvasElement {
  const c = document.createElement('canvas');
  c.width = size;
  c.height = size;
  const ctx = c.getContext('2d')!;
  const img = ctx.createImageData(size, size);
  const d = img.data;
  const pw = 2.0 / size;
  const r = 0.16;
  const o = 0.075;
  const outline: RGBColor = receptor ? { r: 205, g: 210, b: 230, a: 255 } : { r: 22, g: 12, b: 38, a: 255 };
  const dark = receptor ? { r: 40, g: 38, b: 56, a: 255 } : mix(base, COLOR_BLACK, 0.45);
  const light = receptor ? { r: 118, g: 116, b: 140, a: 255 } : mix(base, COLOR_WHITE, 0.55);

  for (let y = 0; y < size; y++) {
    const v = (y + 0.5) * pw - 1.0;
    for (let x = 0; x < size; x++) {
      const u = (x + 0.5) * pw - 1.0;
      const dist = glyphDist(u, v);
      const aOuter = saturate((r + o - dist) / pw + 0.5);
      if (aOuter <= 0.002) continue;
      const aInner = saturate((r - dist) / pw + 0.5);
      const k = saturate(1.0 - dist / r);
      let fill = mix(dark, light, Math.pow(k, 0.8));
      fill = mix(fill, COLOR_WHITE, (receptor ? 0.05 : 0.18) * saturate(-v));
      const col = mix(outline, fill, aInner);
      const idx = (y * size + x) * 4;
      d[idx] = col.r;
      d[idx + 1] = col.g;
      d[idx + 2] = col.b;
      d[idx + 3] = Math.round(aOuter * (receptor ? 235 : 255));
    }
  }
  ctx.putImageData(img, 0, 0);
  return c;
}

function ensureNoteSprites(): void {
  if (noteSprites) return;
  noteSprites = [];
  for (let i = 0; i < 5; i++) {
    noteSprites.push(makeGlyphCanvas(dirColor(i), false, 128));
  }
  receptorSprite = makeGlyphCanvas(COLOR_WHITE, true, 128);
}

export function drawNote(
  ctx: CanvasRenderingContext2D,
  dir: number,
  cx: number,
  cy: number,
  size: number,
  alpha = 1,
  scale = 1,
): void {
  ensureNoteSprites();
  const s = size * scale;
  ctx.save();
  ctx.globalAlpha = saturate(alpha);
  ctx.translate(cx, cy);
  ctx.rotate(dirRotationRad(dir));
  ctx.drawImage(noteSprites![clamp(dir, 0, 4)], -s * 0.5, -s * 0.5, s, s);
  ctx.restore();
}

export function drawReceptor(
  ctx: CanvasRenderingContext2D,
  dir: number,
  cx: number,
  cy: number,
  size: number,
  alpha = 0.85,
  scale = 1,
  litColor?: RGBColor,
): void {
  ensureNoteSprites();
  const s = size * scale;
  ctx.save();
  ctx.globalAlpha = saturate(alpha);
  ctx.translate(cx, cy);
  ctx.rotate(dirRotationRad(dir));
  if (litColor) {
    ctx.drawImage(noteSprites![clamp(dir, 0, 4)], -s * 0.5, -s * 0.5, s, s);
  } else {
    ctx.drawImage(receptorSprite!, -s * 0.5, -s * 0.5, s, s);
  }
  ctx.restore();
}

export function drawSustain(
  ctx: CanvasRenderingContext2D,
  dir: number,
  x: number,
  y0: number,
  y1: number,
  width: number,
  alpha = 0.9,
  capAtY1 = true,
): void {
  if (Math.abs(y1 - y0) < 0.5) return;
  const base = dirColor(dir);
  const top = Math.min(y0, y1);
  const h = Math.abs(y1 - y0);
  ctx.fillStyle = rgba({ r: 22, g: 12, b: 38, a: 255 }, alpha * 0.85);
  ctx.fillRect(x - width * 0.5 - 3, top, width + 6, h);
  ctx.fillStyle = rgba(brighten(base, -0.15), alpha * 0.9);
  ctx.fillRect(x - width * 0.5, top, width, h);
  ctx.fillStyle = rgba(brighten(base, 0.45), alpha * 0.9);
  ctx.fillRect(x - width * 0.18, top, width * 0.36, h);
  if (capAtY1) {
    ctx.fillStyle = rgba({ r: 22, g: 12, b: 38, a: 255 }, alpha * 0.85);
    ctx.beginPath();
    ctx.arc(x, y1, width * 0.5 + 3, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = rgba(brighten(base, -0.15), alpha * 0.9);
    ctx.beginPath();
    ctx.arc(x, y1, width * 0.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = rgba(brighten(base, 0.45), alpha * 0.9);
    ctx.beginPath();
    ctx.arc(x, y1, width * 0.18, 0, Math.PI * 2);
    ctx.fill();
  }
}

// --- Text & Primitives ---
export function gradientRect(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  top: RGBColor,
  bottom: RGBColor,
): void {
  const g = ctx.createLinearGradient(0, y, 0, y + h);
  g.addColorStop(0, rgba(top));
  g.addColorStop(1, rgba(bottom));
  ctx.fillStyle = g;
  ctx.fillRect(x, y, w, h);
}

export function roundRect(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  r: number,
): void {
  const rad = Math.min(r, w * 0.5, h * 0.5);
  ctx.beginPath();
  ctx.moveTo(x + rad, y);
  ctx.arcTo(x + w, y, x + w, y + h, rad);
  ctx.arcTo(x + w, y + h, x, y + h, rad);
  ctx.arcTo(x, y + h, x, y, rad);
  ctx.arcTo(x, y, x + w, y, rad);
  ctx.closePath();
}

export function drawGlow(ctx: CanvasRenderingContext2D, cx: number, cy: number, radius: number, col: RGBColor): void {
  if (radius <= 1) return;
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, radius);
  g.addColorStop(0, rgba(col));
  g.addColorStop(0.5, rgba(col, 0.25));
  g.addColorStop(1, rgba(col, 0));
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(cx, cy, radius, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

export function drawSoftCircle(ctx: CanvasRenderingContext2D, cx: number, cy: number, radius: number, col: RGBColor): void {
  if (radius <= 1) return;
  const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, radius);
  g.addColorStop(0, rgba(col));
  g.addColorStop(1, rgba(col, 0));
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(cx, cy, radius, 0, Math.PI * 2);
  ctx.fill();
}

export function drawText(
  ctx: CanvasRenderingContext2D,
  font: 'title' | 'ui',
  text: string,
  x: number,
  y: number,
  size: number,
  col: RGBColor,
  align: CanvasTextAlign = 'left',
  outline = 0,
): void {
  ctx.save();
  ctx.font = font === 'title' ? `400 ${size}px "Luckiest Guy", cursive` : `700 ${size}px "Rajdhani", sans-serif`;
  ctx.textAlign = align;
  ctx.textBaseline = 'top';
  if (outline > 0) {
    ctx.lineWidth = outline * 2.2;
    ctx.lineJoin = 'round';
    ctx.strokeStyle = rgba({ r: 16, g: 12, b: 32, a: col.a ?? 255 });
    ctx.strokeText(text, x, y);
  }
  ctx.fillStyle = rgba(col);
  ctx.fillText(text, x, y);
  ctx.restore();
}

export function drawGlowText(
  ctx: CanvasRenderingContext2D,
  font: 'title' | 'ui',
  text: string,
  x: number,
  y: number,
  size: number,
  col: RGBColor,
  glowCol: RGBColor,
  align: CanvasTextAlign = 'center',
): void {
  ctx.save();
  ctx.font = font === 'title' ? `400 ${size}px "Luckiest Guy", cursive` : `700 ${size}px "Rajdhani", sans-serif`;
  ctx.textAlign = align;
  ctx.textBaseline = 'top';
  ctx.shadowColor = rgba(glowCol, 0.85);
  ctx.shadowBlur = size * 0.25;
  ctx.lineWidth = Math.max(2, size * 0.07);
  ctx.lineJoin = 'round';
  ctx.strokeStyle = rgba({ r: 16, g: 12, b: 32, a: col.a ?? 255 });
  ctx.strokeText(text, x, y);
  ctx.fillStyle = rgba(col);
  ctx.fillText(text, x, y);
  ctx.restore();
}

// --- Twinkling 4-ray Star ---
export function drawStar(ctx: CanvasRenderingContext2D, cx: number, cy: number, r: number, col: RGBColor): void {
  drawSoftCircle(ctx, cx, cy, r * 1.2, fade(col, 0.55));
  const w = Math.max(0.8, r * 0.16);
  ctx.fillStyle = rgba(col);
  for (let k = 0; k < 4; k++) {
    const a = k * Math.PI * 0.5;
    const dx = Math.cos(a);
    const dy = Math.sin(a);
    const nx = -dy;
    const ny = dx;
    ctx.beginPath();
    ctx.moveTo(cx + nx * w, cy + ny * w);
    ctx.lineTo(cx - nx * w, cy - ny * w);
    ctx.lineTo(cx + dx * r * 2.4, cy + dy * r * 2.4);
    ctx.closePath();
    ctx.fill();
    if (r > 5.5) {
      const b = a + Math.PI * 0.25;
      const ex = Math.cos(b);
      const ey = Math.sin(b);
      const mx = -ey;
      const my = ex;
      ctx.fillStyle = rgba(col, 0.7);
      ctx.beginPath();
      ctx.moveTo(cx + mx * w * 0.6, cy + my * w * 0.6);
      ctx.lineTo(cx - mx * w * 0.6, cy - my * w * 0.6);
      ctx.lineTo(cx + ex * r * 1.2, cy + ey * r * 1.2);
      ctx.closePath();
      ctx.fill();
      ctx.fillStyle = rgba(col);
    }
  }
  ctx.beginPath();
  ctx.arc(cx, cy, Math.max(1, r * 0.34), 0, Math.PI * 2);
  ctx.fill();
}

// --- 3D Rotating Glassy Cube (with floor reflection) ---
const CUBE_CORNERS = [
  [-1, -1, -1],
  [1, -1, -1],
  [1, 1, -1],
  [-1, 1, -1],
  [-1, -1, 1],
  [1, -1, 1],
  [1, 1, 1],
  [-1, 1, 1],
];
const CUBE_FACES = [
  [0, 1, 2, 3],
  [5, 4, 7, 6],
  [4, 0, 3, 7],
  [1, 5, 6, 2],
  [4, 5, 1, 0],
  [3, 2, 6, 7],
];
const CUBE_NORMALS = [
  [0, 0, -1],
  [0, 0, 1],
  [-1, 0, 0],
  [1, 0, 0],
  [0, -1, 0],
  [0, 1, 0],
];

export function drawCube(
  ctx: CanvasRenderingContext2D,
  axPos: number,
  ayPos: number,
  size: number,
  ax: number,
  ay: number,
  base: RGBColor,
  alpha: number,
  mirror: boolean,
  floorY: number,
): void {
  const cx = Math.cos(ax);
  const sx = Math.sin(ax);
  const cy = Math.cos(ay);
  const sy = Math.sin(ay);
  const rot = (v: number[]): [number, number, number] => {
    const x1 = v[0] * cy + v[2] * sy;
    const y1 = v[1];
    const z1 = -v[0] * sy + v[2] * cy;
    return [x1, y1 * cx - z1 * sx, y1 * sx + z1 * cx];
  };

  const p: [number, number][] = [];
  for (let i = 0; i < 8; i++) {
    const v = rot(CUBE_CORNERS[i]);
    const f = 4.0 / (4.0 + v[2]);
    let y = ayPos + v[1] * size * 0.5 * f;
    if (mirror) y = floorY + (floorY - y) * 0.55;
    p.push([axPos + v[0] * size * 0.5 * f, y]);
  }

  const lx = -0.45;
  const ly = -0.7;
  const lz = -0.55;
  const edgeCol = rgba(brighten(base, 0.45), alpha);

  for (let fi = 0; fi < 6; fi++) {
    const n = rot(CUBE_NORMALS[fi]);
    if (n[2] > -0.02) continue;
    const lit = Math.max(0, -(n[0] * lx + n[1] * ly + n[2] * lz));
    const shine = Math.pow(lit, 10);
    const faceCol = mix(mix({ r: 10, g: 8, b: 30, a: 255 }, base, 0.35 + 0.65 * lit), COLOR_WHITE, shine * 0.7);
    const q = CUBE_FACES[fi];
    ctx.beginPath();
    ctx.moveTo(p[q[0]][0], p[q[0]][1]);
    ctx.lineTo(p[q[1]][0], p[q[1]][1]);
    ctx.lineTo(p[q[2]][0], p[q[2]][1]);
    ctx.lineTo(p[q[3]][0], p[q[3]][1]);
    ctx.closePath();
    ctx.fillStyle = rgba(faceCol, alpha * (0.45 + 0.35 * lit));
    ctx.fill();
    ctx.lineWidth = mirror ? 1 : 2;
    ctx.strokeStyle = edgeCol;
    ctx.stroke();
  }
}

// --- Particle System ---
export interface Particle {
  x: number;
  y: number;
  vx: number;
  vy: number;
  c: RGBColor;
  size: number;
  life: number;
  maxLife: number;
  kind: 0 | 1 | 2; // 0 soft dot, 1 spark streak, 2 expanding ring
}

export class ParticleSystem {
  public list: Particle[] = [];

  add(p: Particle): void {
    if (this.list.length > 350) this.list.shift();
    this.list.push(p);
  }

  clear(): void {
    this.list = [];
  }

  update(dt: number, gravity = 0): void {
    let w = 0;
    for (let i = 0; i < this.list.length; i++) {
      const p = this.list[i];
      p.life -= dt;
      if (p.life <= 0) continue;
      p.vy += gravity * dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      this.list[w++] = p;
    }
    this.list.length = w;
  }

  draw(ctx: CanvasRenderingContext2D, additive = true): void {
    if (this.list.length === 0) return;
    ctx.save();
    if (additive) ctx.globalCompositeOperation = 'lighter';
    for (const p of this.list) {
      const t = saturate(p.life / p.maxLife);
      if (p.kind === 0) {
        const s = p.size * (0.5 + 0.5 * t);
        drawSoftCircle(ctx, p.x, p.y, s, fade(p.c, t));
      } else if (p.kind === 1) {
        const spd = Math.hypot(p.vx, p.vy);
        const len = spd * 0.04 + 2;
        const inv = 1 / Math.max(1e-3, spd);
        ctx.strokeStyle = rgba(p.c, t);
        ctx.lineWidth = p.size * t + 0.5;
        ctx.beginPath();
        ctx.moveTo(p.x, p.y);
        ctx.lineTo(p.x - p.vx * inv * len, p.y - p.vy * inv * len);
        ctx.stroke();
      } else {
        const r = p.size * (1 - t) + 4;
        ctx.strokeStyle = rgba(p.c, t);
        ctx.lineWidth = 3 * t + 1;
        ctx.beginPath();
        ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
        ctx.stroke();
      }
    }
    ctx.restore();
  }
}

// --- Retro Menu Backdrop (stars, striped sun, perspective grid, mirrored glassy cubes) ---
const MENU_STARS = (() => {
  const rng = new Rng(77);
  const arr: { x: number; y: number; r: number; phase: number; speed: number; tinted: boolean }[] = [];
  for (let i = 0; i < 44; i++) {
    const x = rng.uniform() * CANVAS_W;
    const y = rng.uniform() * (468 - 70);
    const u = rng.uniform();
    const r = 1.6 + u * u * 7.5;
    const phase = rng.uniform() * Math.PI * 2;
    const speed = 0.8 + rng.uniform() * 2.4;
    arr.push({ x, y, r, phase, speed, tinted: i % 5 === 0 });
  }
  return arr;
})();

const CUBE_COLORS: RGBColor[] = [
  { r: 255, g: 79, b: 163, a: 255 },
  { r: 40, g: 220, b: 255, a: 255 },
  { r: 130, g: 255, b: 90, a: 255 },
  { r: 170, g: 100, b: 255, a: 255 },
  { r: 255, g: 222, b: 64, a: 255 },
];

export function drawMenuBackground(ctx: CanvasRenderingContext2D, now: number, tintHex: number, pulse: number): void {
  const tint = hexToRgb(tintHex);
  const kHorizon = 468;
  const skyTop = mix({ r: 6, g: 4, b: 22, a: 255 }, tint, 0.1);
  const skyLow = mix({ r: 70, g: 18, b: 86, a: 255 }, tint, 0.4);
  gradientRect(ctx, 0, 0, CANVAS_W, kHorizon + 2, skyTop, skyLow);

  for (const s of MENU_STARS) {
    const tw = 0.5 + 0.5 * Math.sin(now * s.speed + s.phase);
    const c = s.tinted ? mix(COLOR_WHITE, tint, 0.45) : COLOR_WHITE;
    drawStar(ctx, s.x, s.y, s.r * (0.7 + 0.3 * tw + 0.25 * pulse), fade(c, 0.3 + 0.7 * tw));
  }

  // Striped sun
  const sunX = 640;
  const sunY = kHorizon - 34;
  const sunR = 150 + 6 * pulse;
  drawGlow(ctx, sunX, sunY, sunR * 1.9, fade(mix({ r: 255, g: 120, b: 170, a: 255 }, tint, 0.3), 0.35));
  const sg = ctx.createLinearGradient(0, sunY - sunR, 0, sunY + sunR);
  sg.addColorStop(0, rgba({ r: 255, g: 226, b: 90, a: 255 }, 0.9));
  sg.addColorStop(1, rgba(mix({ r: 255, g: 70, b: 150, a: 255 }, tint, 0.25), 0.9));
  ctx.fillStyle = sg;
  ctx.beginPath();
  ctx.arc(sunX, sunY, sunR, 0, Math.PI * 2);
  ctx.fill();
  for (let k = 0; k < 6; k++) {
    const y = sunY + 18 + k * 22 + ((now * 8) % 22);
    const h = 3 + k * 2.2;
    ctx.fillStyle = rgba(mix(skyTop, skyLow, saturate(y / kHorizon)));
    ctx.fillRect(sunX - sunR - 4, y, sunR * 2 + 8, h);
  }

  // Floor perspective grid
  gradientRect(ctx, 0, kHorizon, CANVAS_W, CANVAS_H - kHorizon, mix({ r: 24, g: 6, b: 36, a: 255 }, tint, 0.22), {
    r: 4,
    g: 2,
    b: 12,
    a: 255,
  });
  const line = mix({ r: 255, g: 80, b: 200, a: 255 }, tint, 0.35);
  ctx.save();
  ctx.globalCompositeOperation = 'lighter';
  const run = (now * 0.55) % 1.0;
  for (let i = 0; i < 16; i++) {
    const z = i + 1.0 - run;
    const y = kHorizon + 260.0 / z - 6.0;
    if (y < kHorizon || y > CANVAS_H + 4) continue;
    ctx.strokeStyle = rgba(line, Math.min(0.75, 0.12 + 0.9 / z) * (0.8 + 0.4 * pulse));
    ctx.lineWidth = 1.5 + 1.5 / z;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(CANVAS_W, y);
    ctx.stroke();
  }
  ctx.strokeStyle = rgba(line, 0.32 + 0.2 * pulse);
  ctx.lineWidth = 1.6;
  for (let i = -16; i <= 16; i++) {
    ctx.beginPath();
    ctx.moveTo(640 + i * 34, kHorizon);
    ctx.lineTo(640 + i * 300, CANVAS_H + 20);
    ctx.stroke();
  }
  ctx.strokeStyle = rgba(line, 0.9);
  ctx.lineWidth = 2.5;
  ctx.beginPath();
  ctx.moveTo(0, kHorizon);
  ctx.lineTo(CANVAS_W, kHorizon);
  ctx.stroke();
  ctx.restore();

  // Rotating glassy cubes + reflections
  for (let i = 0; i < 6; i++) {
    const x = ((90 + i * 223 + now * (9 + i * 2)) % (CANVAS_W + 200)) - 100;
    const y = 150 + ((i * 97) % 230) + Math.sin(now * 0.7 + i * 1.7) * 14;
    const size = (46 + ((i * 37) % 60)) * (1 + 0.06 * pulse);
    const ax = now * (0.35 + 0.08 * i) + i;
    const ay = now * (0.5 - 0.05 * i) + i * 2;
    const c = mix(CUBE_COLORS[i % 5], tint, 0.25);
    drawCube(ctx, x, y, size, ax, ay, c, 0.22, true, kHorizon + 8);
    drawCube(ctx, x, y, size, ax, ay, c, 0.85, false, kHorizon);
  }
}

export function drawVolumeTray(ctx: CanvasRenderingContext2D, masterVol: number, muted: boolean, ageSec: number): void {
  if (ageSec < 0 || ageSec > 1.8) return;
  const slide = ageSec < 1.5 ? 0 : (ageSec - 1.5) / 0.3;
  const w = 200;
  const h = 64;
  const x = CANVAS_W * 0.5 - w * 0.5;
  const y = 6 - slide * (h + 10);
  roundRect(ctx, x, y, w, h, 10);
  ctx.fillStyle = 'rgba(0,0,0,0.8)';
  ctx.fill();
  const level = muted ? 0 : Math.round(masterVol * 10);
  for (let i = 0; i < 10; i++) {
    const bh = 6 + i * 2.6;
    const bx = x + 22 + i * 16.5;
    const by = y + 38 - bh;
    ctx.fillStyle = i < level ? '#ffffff' : 'rgba(255,255,255,0.25)';
    ctx.fillRect(bx, by, 12, bh);
  }
  drawText(ctx, 'ui', muted ? 'MUTED' : `VOLUME ${level * 10}%`, CANVAS_W * 0.5, y + 42, 18, COLOR_WHITE, 'center', 1);
}
