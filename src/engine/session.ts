// Gameplay session state, judgement windows, sustains, accuracy, ranks, and botplay
// Ported from src/session.cpp
import {
  Chart,
  HitErrorSample,
  HOLD_HEALTH_PER_SEC,
  MISS_HEALTH_DELTA,
  MISS_PENALTY_SCORE,
  MISS_WINDOW_MS,
  Note,
  RATINGS,
} from './types';

export interface ActiveNote extends Note {
  index: number;
  hit: boolean;
  missed: boolean;
  holding: boolean;
  holdCompleted: boolean;
  holdProgressMs: number;
}

export interface SessionEvents {
  onPlayerHit?: (note: ActiveNote, rating: number, deltaMs: number) => void;
  onPlayerMiss?: (dir: number, isGhost: boolean, note?: ActiveNote) => void;
  onOpponentHit?: (note: ActiveNote) => void;
  onHoldTick?: (note: ActiveNote) => void;
}

export class Session {
  public notes: ActiveNote[] = [];
  public score = 0;
  public health = 1.0; // 0..2
  public combo = 0;
  public maxCombo = 0;
  public misses = 0;
  public counts: [number, number, number, number] = [0, 0, 0, 0];
  public weightSum = 0;
  public judgedCount = 0;
  public totalPlayerNotes = 0;
  public hitErrors: HitErrorSample[] = [];
  public dead = false;
  public playerVocalMuted = false;

  // NPS sliding window (timestamps in sec)
  private recentHits: number[] = [];
  public nps = 0;
  public peakNps = 0;

  // Modifiers
  public ghostTapping = true;
  public botplay = false;
  public practice = false;
  public playerSide: 0 | 1 = 1;

  // Lane hold state
  private laneHeld = [false, false, false, false];
  private lastSongMs = -10000;

  constructor(
    chart: Chart,
    opts: { ghostTapping: boolean; botplay: boolean; practice: boolean; mirror: boolean },
  ) {
    this.ghostTapping = opts.ghostTapping;
    this.botplay = opts.botplay;
    this.practice = opts.practice;
    this.playerSide = opts.mirror ? 0 : 1;

    this.notes = chart.notes
      .slice()
      .sort((a, b) => a.timeMs - b.timeMs || a.dir - b.dir)
      .map((n, index) => ({
        ...n,
        index,
        hit: false,
        missed: false,
        holding: false,
        holdCompleted: false,
        holdProgressMs: 0,
      }));

    this.totalPlayerNotes = this.notes.filter((n) => n.side === this.playerSide).length;
  }

  get accuracy(): number {
    if (this.judgedCount <= 0) return 100;
    return Math.max(0, Math.min(100, (this.weightSum / this.judgedCount) * 100));
  }

  get rank(): string {
    const a = this.accuracy;
    if (this.misses === 0 && a >= 99.5) return 'S+';
    if (a >= 95) return 'S';
    if (a >= 90) return 'A';
    if (a >= 80) return 'B';
    if (a >= 70) return 'C';
    return 'D';
  }

  get clearLabel(): string {
    if (this.misses === 0) {
      if (this.counts[1] === 0 && this.counts[2] === 0 && this.counts[3] === 0 && this.counts[0] > 0) return 'SFC';
      if (this.counts[2] === 0 && this.counts[3] === 0) return 'GFC';
      return 'FC';
    }
    if (this.misses < 10) return 'SDCB';
    return 'Clear';
  }

  getTimingStats(): { meanMs: number; sdMs: number } {
    const valid = this.hitErrors.filter((h) => h.rating >= 0);
    if (valid.length === 0) return { meanMs: 0, sdMs: 0 };
    let sum = 0;
    for (const h of valid) sum += h.deltaMs;
    const mean = sum / valid.length;
    let sq = 0;
    for (const h of valid) {
      const d = h.deltaMs - mean;
      sq += d * d;
    }
    return { meanMs: mean, sdMs: Math.sqrt(sq / valid.length) };
  }

  setLaneHeld(dir: number, held: boolean): void {
    if (dir >= 0 && dir < 4) this.laneHeld[dir] = held;
  }

  pressLane(dir: number, songMs: number, nowSec: number, events: SessionEvents): boolean {
    if (this.dead || this.botplay) return false;
    this.laneHeld[dir] = true;

    let best: ActiveNote | null = null;
    let bestAbs = MISS_WINDOW_MS + 1;
    for (const n of this.notes) {
      if (n.side !== this.playerSide || n.hit || n.missed || n.dir !== dir) continue;
      const diff = songMs - n.timeMs;
      if (diff < -MISS_WINDOW_MS) break; // sorted by timeMs
      const ad = Math.abs(diff);
      if (ad <= MISS_WINDOW_MS && ad < bestAbs) {
        bestAbs = ad;
        best = n;
      }
    }

    if (!best) {
      if (!this.ghostTapping) {
        this.registerMiss(dir, songMs, true, undefined, events);
      }
      return false;
    }

    const deltaMs = songMs - best.timeMs;
    const absMs = Math.abs(deltaMs);
    let rating = 3;
    for (let r = 0; r < RATINGS.length; r++) {
      if (absMs <= RATINGS[r].windowMs) {
        rating = r;
        break;
      }
    }
    this.registerHit(best, rating, deltaMs, nowSec, events);
    return true;
  }

  releaseLane(dir: number): void {
    if (dir >= 0 && dir < 4) this.laneHeld[dir] = false;
  }

  private registerHit(note: ActiveNote, rating: number, deltaMs: number, nowSec: number, events: SessionEvents): void {
    const info = RATINGS[rating];
    note.hit = true;
    if (note.lengthMs > 40) {
      note.holding = true;
    } else {
      note.holdCompleted = true;
    }

    if (!this.botplay) {
      this.counts[rating]++;
      this.judgedCount++;
      this.weightSum += info.weight;
      this.score += info.score;
      this.combo++;
      if (this.combo > this.maxCombo) this.maxCombo = this.combo;
      this.health = Math.max(0, Math.min(2, this.health + info.healthDelta));
      this.hitErrors.push({ timeMs: note.timeMs, deltaMs, rating });
    } else {
      this.combo++;
      if (this.combo > this.maxCombo) this.maxCombo = this.combo;
    }

    this.playerVocalMuted = false;
    this.recentHits.push(nowSec);
    events.onPlayerHit?.(note, rating, deltaMs);
  }

  private registerMiss(
    dir: number,
    songMs: number,
    isGhost: boolean,
    note: ActiveNote | undefined,
    events: SessionEvents,
  ): void {
    this.misses++;
    this.judgedCount++;
    this.combo = 0;
    this.score = Math.max(0, this.score - MISS_PENALTY_SCORE);
    this.health = Math.max(0, this.health - MISS_HEALTH_DELTA);
    this.playerVocalMuted = true;
    this.hitErrors.push({ timeMs: note ? note.timeMs : songMs, deltaMs: MISS_WINDOW_MS, rating: -1 });
    if (this.health <= 0 && !this.practice && !this.botplay) {
      this.dead = true;
    }
    events.onPlayerMiss?.(dir, isGhost, note);
  }

  update(songMs: number, dtSec: number, nowSec: number, events: SessionEvents): void {
    if (this.dead) return;
    const stepMs = this.lastSongMs < -9000 ? 0 : Math.max(0, songMs - this.lastSongMs);
    this.lastSongMs = songMs;

    // Update NPS window (last 1 second)
    while (this.recentHits.length > 0 && nowSec - this.recentHits[0] > 1.0) {
      this.recentHits.shift();
    }
    this.nps = this.recentHits.length;
    if (this.nps > this.peakNps) this.peakNps = this.nps;

    for (const n of this.notes) {
      // Opponent notes (auto-hit at exact time)
      if (n.side !== this.playerSide) {
        if (!n.hit && songMs >= n.timeMs) {
          n.hit = true;
          if (n.lengthMs > 40) n.holding = true;
          else n.holdCompleted = true;
          events.onOpponentHit?.(n);
        }
        if (n.holding) {
          if (songMs >= n.timeMs + n.lengthMs) {
            n.holding = false;
            n.holdCompleted = true;
          }
        }
        continue;
      }

      // Player notes
      if (this.botplay && !n.hit && !n.missed && songMs >= n.timeMs) {
        this.registerHit(n, 0, 0, nowSec, events);
      }

      if (!n.hit && !n.missed && songMs - n.timeMs > MISS_WINDOW_MS) {
        n.missed = true;
        this.registerMiss(n.dir, songMs, false, n, events);
      }

      // Sustain hold handling
      if (n.holding) {
        const endMs = n.timeMs + n.lengthMs;
        if (songMs >= endMs - 35) {
          n.holding = false;
          n.holdCompleted = true;
          n.holdProgressMs = n.lengthMs;
        } else if (this.botplay || this.laneHeld[n.dir]) {
          n.holdProgressMs = Math.max(0, songMs - n.timeMs);
          if (!this.botplay && stepMs > 0) {
            this.health = Math.min(2, this.health + HOLD_HEALTH_PER_SEC * (stepMs / 1000));
            this.score += Math.round(stepMs * 0.12);
          }
          events.onHoldTick?.(n);
        } else {
          // Dropped hold early — small grace window (120ms)
          const heldUntil = n.timeMs + n.holdProgressMs;
          if (songMs - heldUntil > 120) {
            n.holding = false;
            n.missed = true;
            this.playerVocalMuted = true;
            this.health = Math.max(0, this.health - MISS_HEALTH_DELTA * 0.5);
            if (this.health <= 0 && !this.practice && !this.botplay) {
              this.dead = true;
            }
          }
        }
      }
    }
    void dtSec;
  }
}
