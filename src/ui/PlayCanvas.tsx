// Full 1280x720 HTML5 Canvas gameplay screen
// Ported from src/state_play.cpp
import React, { useEffect, useRef, useState } from 'react';
import {
  Chart,
  PlayRequest,
  RATINGS,
  ResultData,
  RGBColor,
  Settings,
} from '../engine/types';
import {
  buildBuiltinChart,
  chartBeatAt,
  chartFocusAt,
  composeSong,
  findBuiltinDef,
} from '../engine/composer';
import { audioBufferToMono, autoChartFromMono } from '../engine/autochart';
import { audio } from '../engine/audio';
import { Session } from '../engine/session';
import { Stage } from '../engine/stage';
import { AudioVisualizer } from '../engine/visualizer';
import {
  charBeat,
  charColor,
  charDisplayName,
  charMiss,
  charSing,
  drawCharacter,
  drawIcon,
  makeCharState,
} from '../engine/characters';
import {
  CANVAS_H,
  CANVAS_W,
  clamp,
  COLOR_GOLD,
  COLOR_IVORY,
  COLOR_WHITE,
  dirColor,
  drawGlowText,
  drawNote,
  drawReceptor,
  drawSustain,
  drawText,
  hexToRgb,
  mix,
  ParticleSystem,
  rgba,
  roundRect,
  saturate,
} from '../engine/gfx';
import { fmtThousands, fmtTime, formatKeyCode } from '../engine/storage';

interface PlayCanvasProps {
  request: PlayRequest;
  settings: Settings;
  onFinish: (result: ResultData) => void;
  onExit: () => void;
  onOpenEditor: (chart: Chart, song: PlayRequest['song']) => void;
  onOpenOptions: () => void;
}

interface RatingPopup {
  text: string;
  sub: string;
  color: RGBColor;
  combo: number;
  born: number;
}

export const PlayCanvas: React.FC<PlayCanvasProps> = ({
  request,
  settings,
  onFinish,
  onExit,
  onOpenEditor,
  onOpenOptions,
}) => {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [statusText, setStatusText] = useState('Synthesizing track...');
  const [paused, setPaused] = useState(false);
  const [pauseIdx, setPauseIdx] = useState(0);
  const [gameOver, setGameOver] = useState(false);
  const [restartToken, setRestartToken] = useState(0);

  // Refs synced to RAF loop
  const pausedRef = useRef(false);
  const gameOverRef = useRef(false);
  const settingsRef = useRef(settings);
  const activeChartRef = useRef<Chart | null>(null);
  const finishedRef = useRef(false);

  useEffect(() => {
    settingsRef.current = settings;
  }, [settings]);

  useEffect(() => {
    pausedRef.current = paused;
  }, [paused]);

  useEffect(() => {
    gameOverRef.current = gameOver;
  }, [gameOver]);

  const pauseItems = ['Resume', 'Restart Song', 'Chart Editor', 'Options', 'Exit to Menu'];

  const handlePauseAction = (idx: number) => {
    audio.playSfx('confirm');
    if (idx === 0) {
      setPaused(false);
      pausedRef.current = false;
      audio.resumeSong();
    } else if (idx === 1) {
      setPaused(false);
      setGameOver(false);
      pausedRef.current = false;
      gameOverRef.current = false;
      audio.stopSong();
      setRestartToken((t) => t + 1);
    } else if (idx === 2) {
      audio.stopSong();
      if (activeChartRef.current) {
        onOpenEditor(activeChartRef.current, request.song);
      }
    } else if (idx === 3) {
      audio.stopSong();
      onOpenOptions();
    } else {
      audio.stopSong();
      onExit();
    }
  };

  useEffect(() => {
    let cancelled = false;
    let rafId = 0;
    finishedRef.current = false;
    setPaused(false);
    setGameOver(false);
    setPauseIdx(0);
    pausedRef.current = false;
    gameOverRef.current = false;
    setStatusText(request.autoChart ? 'Analyzing audio & generating AutoChart...' : 'Synthesizing track...');

    const initTimer = window.setTimeout(() => {
      if (cancelled) return;

      const { song, difficulty, mods } = request;
      let chart: Chart;
      let durationSec = 90;

      if (song.isBuiltin) {
        const def = findBuiltinDef(song.id)!;
        const comp = composeSong(def, true);
        const instAb = audio.stereoToAudioBuffer(comp.inst!);
        const oppAb = audio.stereoToAudioBuffer(comp.oppVoice!);
        const plyAb = audio.stereoToAudioBuffer(comp.playerVoice!);
        audio.loadStems(instAb, oppAb, plyAb, `play:${song.id}`);
        durationSec = comp.durationSec;

        if (request.chartOverride) {
          chart = request.chartOverride;
        } else if (request.autoChart) {
          const { mono, sampleRate } = audioBufferToMono(instAb);
          chart = autoChartFromMono(mono, sampleRate, {
            title: `${def.title} (AutoChart)`,
            difficulty,
            opponent: def.opponent,
            stage: def.stage,
            accentHex: def.accentHex,
            knownBpm: def.bpm,
          });
        } else {
          chart = buildBuiltinChart(def, comp, difficulty, mods.mirror);
        }
      } else {
        // Custom imported audio or Psych JSON
        if (song.customAudioBuffer) {
          audio.loadStems(song.customAudioBuffer, null, null, `custom:${song.id}`);
          durationSec = song.customAudioBuffer.duration;
        } else {
          // Fallback procedural backing track if JSON-only chart was imported without audio
          const def = findBuiltinDef('principality')!;
          const comp = composeSong(def, true);
          const instAb = audio.stereoToAudioBuffer(comp.inst!);
          audio.loadStems(instAb, null, null, `fallback:${song.id}`);
          durationSec = Math.max(
            comp.durationSec,
            (song.customChart?.notes.reduce((m, n) => Math.max(m, n.timeMs + n.lengthMs), 0) ?? 60000) / 1000 + 2,
          );
        }

        if (request.chartOverride) {
          chart = request.chartOverride;
        } else if (song.customChart && !request.autoChart) {
          chart = song.customChart;
        } else if (song.customAudioBuffer) {
          const { mono, sampleRate } = audioBufferToMono(song.customAudioBuffer);
          chart = autoChartFromMono(mono, sampleRate, {
            title: song.title,
            difficulty,
            opponent: song.opponent,
            stage: song.stage,
            accentHex: song.accentHex,
          });
        } else {
          const def = findBuiltinDef('vigil')!;
          const comp = composeSong(def, false);
          chart = buildBuiltinChart(def, comp, difficulty, mods.mirror);
        }
      }

      activeChartRef.current = chart;
      setStatusText('');

      // Setup runtime simulation objects
      const session = new Session(chart, {
        ghostTapping: settingsRef.current.ghostTapping,
        botplay: mods.botplay,
        practice: mods.practice,
        mirror: mods.mirror,
      });

      const stage = new Stage();
      stage.init(chart.stage, hexToRgb(chart.accentHex), settingsRef.current.lowQuality, settingsRef.current.flashingLights);

      const viz = new AudioVisualizer();
      const particles = new ParticleSystem();
      const oppChar = makeCharState(chart.opponent, charColor(chart.opponent));
      const plyChar = makeCharState(chart.player, charColor(chart.player));

      // Countdown state: 4 beats before songMs = 0
      const beatSec = 60 / Math.max(40, chart.bpm);
      const countdownTotal = beatSec * 4;
      let countdownElapsed = 0;
      let countdownStep = -1;
      let songStarted = false;
      let lastBeatInt = -1;

      // Camera & HUD juice
      let camX = 640;
      let camY = 390;
      let camZoom = 1.0;
      let hudZoom = 1.0;
      let iconBounce = 0;
      let lastRating: RatingPopup | null = null;
      const laneGlow = [0, 0, 0, 0];
      const oppGlow = [0, 0, 0, 0];
      const lanePress = [false, false, false, false];
      const activePointers = new Map<number, number>();
      let fpsVal = 60;
      let fpsFrames = 0;
      let fpsClock = performance.now();
      let prevTime = performance.now();

      // Layout helpers
      const getStrumLayout = (s: Settings) => {
        const noteSize = 104;
        const spacing = 112;
        const receptorY = s.downscroll ? 604 : 114;
        const plyCenterX = s.middlescroll ? 640 : 958;
        const oppCenterX = s.middlescroll ? 220 : 322;
        const laneX = (centerX: number, dir: number) => centerX + (dir - 1.5) * spacing;
        return { noteSize, spacing, receptorY, plyCenterX, oppCenterX, laneX };
      };

      const spawnSplash = (x: number, y: number, dir: number) => {
        if (!settingsRef.current.noteSplashes) return;
        const c = dirColor(dir);
        particles.add({ x, y, vx: 0, vy: 0, c, size: 78, life: 0.24, maxLife: 0.24, kind: 2 });
        for (let i = 0; i < 12; i++) {
          const a = (Math.PI * 2 * i) / 12 + Math.random() * 0.35;
          const spd = 160 + Math.random() * 280;
          particles.add({
            x,
            y,
            vx: Math.cos(a) * spd,
            vy: Math.sin(a) * spd,
            c: i % 2 === 0 ? c : COLOR_WHITE,
            size: 5 + Math.random() * 4,
            life: 0.26 + Math.random() * 0.14,
            maxLife: 0.4,
            kind: 1,
          });
        }
      };

      const sessionEvents = {
        onPlayerHit: (note: { dir: number; lengthMs: number }, rating: number, deltaMs: number) => {
          const now = performance.now() / 1000;
          const info = RATINGS[rating];
          laneGlow[note.dir] = 1.0;
          const activeChar = mods.mirror ? oppChar : plyChar;
          charSing(activeChar, note.dir, now, note.lengthMs / 1000);
          viz.hit(rating);
          audio.setPlayerVoiceVolume(1);
          if (settingsRef.current.hitSoundVol > 0.01) {
            audio.playSfx('hitsound', settingsRef.current.hitSoundVol);
          }
          if (info.splash) {
            const lay = getStrumLayout(settingsRef.current);
            spawnSplash(lay.laneX(lay.plyCenterX, note.dir), lay.receptorY, note.dir);
          }
          lastRating = {
            text: info.name,
            sub: mods.botplay ? 'BOT' : `${deltaMs >= 0 ? '+' : ''}${Math.round(deltaMs)}ms`,
            color: info.color,
            combo: session.combo,
            born: now,
          };
        },
        onPlayerMiss: (dir: number) => {
          const now = performance.now() / 1000;
          const activeChar = mods.mirror ? oppChar : plyChar;
          charMiss(activeChar, dir, now);
          viz.miss();
          audio.setPlayerVoiceVolume(0);
          audio.playSfx('miss', 0.8);
          lastRating = {
            text: 'MISS',
            sub: '',
            color: { r: 255, g: 75, b: 95, a: 255 },
            combo: 0,
            born: now,
          };
          if (session.dead && !gameOverRef.current) {
            gameOverRef.current = true;
            setGameOver(true);
            audio.stopSong();
            audio.playSfx('death');
          }
        },
        onOpponentHit: (note: { dir: number; lengthMs: number }) => {
          const now = performance.now() / 1000;
          oppGlow[note.dir] = 1.0;
          const otherChar = mods.mirror ? plyChar : oppChar;
          charSing(otherChar, note.dir, now, note.lengthMs / 1000);
          viz.opponentHit();
        },
      };

      const getCurrentSongMs = (): number => {
        if (!songStarted) {
          return (countdownElapsed - countdownTotal) * 1000;
        }
        return audio.getSongTimeSec() * 1000 - settingsRef.current.audioOffsetMs;
      };

      const keyToDir = (code: string): number => {
        const s = settingsRef.current;
        for (let i = 0; i < 4; i++) {
          if (s.keys4[i] === code || s.altKeys4[i] === code) return i;
        }
        return -1;
      };

      const onKeyDown = (e: KeyboardEvent) => {
        if (e.repeat) return;

        // Game Over controls
        if (gameOverRef.current) {
          if (e.code === 'Enter' || e.code === 'Space') {
            e.preventDefault();
            audio.playSfx('confirm');
            setRestartToken((t) => t + 1);
          } else if (e.code === 'Escape') {
            e.preventDefault();
            audio.playSfx('cancel');
            onExit();
          }
          return;
        }

        // Pause toggle
        if (e.code === 'Escape') {
          e.preventDefault();
          if (pausedRef.current) {
            setPaused(false);
            pausedRef.current = false;
            audio.resumeSong();
            audio.playSfx('confirm');
          } else {
            setPaused(true);
            pausedRef.current = true;
            setPauseIdx(0);
            audio.pauseSong();
            audio.playSfx('cancel');
          }
          return;
        }

        // Pause menu navigation
        if (pausedRef.current) {
          if (e.code === 'ArrowUp' || e.code === 'KeyW') {
            e.preventDefault();
            audio.playSfx('scroll');
            setPauseIdx((p) => (p + pauseItems.length - 1) % pauseItems.length);
          } else if (e.code === 'ArrowDown' || e.code === 'KeyS') {
            e.preventDefault();
            audio.playSfx('scroll');
            setPauseIdx((p) => (p + 1) % pauseItems.length);
          } else if (e.code === 'Enter') {
            e.preventDefault();
            setPauseIdx((cur) => {
              handlePauseAction(cur);
              return cur;
            });
          }
          return;
        }

        // 7 opens Chart Editor directly
        if (e.code === 'Digit7') {
          e.preventDefault();
          audio.stopSong();
          audio.playSfx('confirm');
          onOpenEditor(chart, song);
          return;
        }

        const dir = keyToDir(e.code);
        if (dir >= 0) {
          e.preventDefault();
          lanePress[dir] = true;
          const nowSec = performance.now() / 1000;
          session.pressLane(dir, getCurrentSongMs(), nowSec, sessionEvents);
        }
      };

      const onKeyUp = (e: KeyboardEvent) => {
        const dir = keyToDir(e.code);
        if (dir >= 0) {
          lanePress[dir] = false;
          session.releaseLane(dir);
        }
      };

      // Pointer / touch 4-lane support on the canvas
      const canvasEl = canvasRef.current;
      const pointerToDir = (ev: PointerEvent): number => {
        if (!canvasEl) return -1;
        const rect = canvasEl.getBoundingClientRect();
        const nx = (ev.clientX - rect.left) / Math.max(1, rect.width);
        return clamp(Math.floor(nx * 4), 0, 3);
      };

      const onPointerDown = (ev: PointerEvent) => {
        if (pausedRef.current || gameOverRef.current) return;
        const dir = pointerToDir(ev);
        if (dir < 0) return;
        activePointers.set(ev.pointerId, dir);
        lanePress[dir] = true;
        session.pressLane(dir, getCurrentSongMs(), performance.now() / 1000, sessionEvents);
      };

      const onPointerUp = (ev: PointerEvent) => {
        const dir = activePointers.get(ev.pointerId);
        if (dir !== undefined) {
          activePointers.delete(ev.pointerId);
          lanePress[dir] = false;
          session.releaseLane(dir);
        }
      };

      window.addEventListener('keydown', onKeyDown);
      window.addEventListener('keyup', onKeyUp);
      canvasEl?.addEventListener('pointerdown', onPointerDown);
      window.addEventListener('pointerup', onPointerUp);
      window.addEventListener('pointercancel', onPointerUp);

      const finishSong = () => {
        if (finishedRef.current) return;
        finishedRef.current = true;
        audio.stopSong();
        const stats = session.getTimingStats();
        onFinish({
          song,
          difficulty,
          score: session.score,
          accuracy: session.accuracy,
          rank: session.rank,
          clearLabel: session.clearLabel,
          maxCombo: session.maxCombo,
          misses: session.misses,
          counts: [...session.counts] as [number, number, number, number],
          totalNotes: session.totalPlayerNotes,
          hitErrors: session.hitErrors,
          meanMs: stats.meanMs,
          sdMs: stats.sdMs,
          isNewBest: false,
          botplay: mods.botplay,
          practice: mods.practice,
          autoChart: request.autoChart,
          storyQueue: request.storyQueue,
          storyIndex: request.storyIndex,
          storyAccumScore: (request.storyAccumScore ?? 0) + session.score,
          fromScreen: request.fromScreen,
          mods,
        });
      };

      const loop = (nowMs: number) => {
        if (cancelled) return;
        rafId = requestAnimationFrame(loop);

        const dt = Math.min(0.05, Math.max(0.001, (nowMs - prevTime) / 1000));
        prevTime = nowMs;
        const nowSec = nowMs / 1000;

        fpsFrames++;
        if (nowMs - fpsClock >= 500) {
          fpsVal = Math.round((fpsFrames * 1000) / (nowMs - fpsClock));
          fpsFrames = 0;
          fpsClock = nowMs;
        }

        const s = settingsRef.current;
        const isPaused = pausedRef.current;
        const isDead = gameOverRef.current;

        if (!isPaused && !isDead) {
          if (!songStarted) {
            countdownElapsed += dt * mods.playbackRate;
            const step = Math.floor(countdownElapsed / beatSec);
            if (step !== countdownStep && step >= 0 && step < 4) {
              countdownStep = step;
              const sfxList = ['count3', 'count2', 'count1', 'countGo'] as const;
              audio.playSfx(sfxList[step]);
              stage.onBeat(step);
              charBeat(oppChar, nowSec);
              charBeat(plyChar, nowSec);
              viz.beat();
            }
            if (countdownElapsed >= countdownTotal) {
              songStarted = true;
              audio.playSong(0, mods.playbackRate, false);
            }
          }

          const songMs = getCurrentSongMs();
          const beatF = chartBeatAt(chart, Math.max(0, songMs));
          const beatInt = Math.floor(beatF);
          if (songStarted && beatInt > lastBeatInt) {
            lastBeatInt = beatInt;
            stage.onBeat(beatInt);
            charBeat(oppChar, nowSec);
            charBeat(plyChar, nowSec);
            viz.beat();
            iconBounce = 1.0;
            if (s.cameraZooms && beatInt % 4 === 0) {
              camZoom = 1.045;
              hudZoom = 1.028;
            }
          }

          session.ghostTapping = s.ghostTapping;
          session.update(songMs, dt, nowSec, sessionEvents);
          if (session.dead && !gameOverRef.current) {
            gameOverRef.current = true;
            setGameOver(true);
            audio.stopSong();
            audio.playSfx('death');
          }

          stage.update(dt);
          particles.update(dt, 480);

          const focusSide = chartFocusAt(chart, Math.max(0, songMs));
          const targetCamX = focusSide === 0 ? 545 : 735;
          const targetCamY = focusSide === 0 ? 380 : 395;
          camX += (targetCamX - camX) * (1 - Math.exp(-dt * 4.5));
          camY += (targetCamY - camY) * (1 - Math.exp(-dt * 4.5));
          camZoom += (1.0 - camZoom) * (1 - Math.exp(-dt * 6.5));
          hudZoom += (1.0 - hudZoom) * (1 - Math.exp(-dt * 7.5));
          iconBounce *= Math.exp(-dt * 9);

          for (let d = 0; d < 4; d++) {
            laneGlow[d] = Math.max(0, laneGlow[d] - dt * 6);
            oppGlow[d] = Math.max(0, oppGlow[d] - dt * 6);
          }

          viz.update(
            dt,
            audio.getFrequencyData(),
            songStarted,
            focusSide,
            charColor(chart.opponent),
            charColor(chart.player),
            session.nps,
            session.health,
          );

          // Song completion check
          if (songStarted && songMs >= durationSec * 1000 - 150) {
            finishSong();
            return;
          }
        }

        // --- RENDER 1280x720 CANVAS ---
        const canvas = canvasRef.current;
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        if (!ctx) return;

        ctx.clearRect(0, 0, CANVAS_W, CANVAS_H);

        // 1. World space (Camera pan + beat zoom)
        ctx.save();
        ctx.translate(CANVAS_W * 0.5, CANVAS_H * 0.5);
        ctx.scale(camZoom, camZoom);
        ctx.translate(-camX, -camY);

        stage.drawBack(ctx, nowSec);
        drawCharacter(ctx, oppChar, 320, 628, 0.98, nowSec, false);
        drawCharacter(ctx, plyChar, 960, 636, 0.98, nowSec, true);
        stage.drawFront(ctx, nowSec);
        ctx.restore();

        stage.drawOverlay(ctx);

        // 2. HUD & Note Highway
        const songMs = getCurrentSongMs();
        const lay = getStrumLayout(s);
        const scrollSpeed =
          mods.scrollType === 'constant' ? mods.scrollValue : chart.speed * mods.scrollValue;
        const pxPerMs = 0.45 * scrollSpeed * (mods.playbackRate > 1 ? 1 / Math.sqrt(mods.playbackRate) : 1);
        const dirSign = s.downscroll ? -1 : 1;

        ctx.save();
        ctx.translate(CANVAS_W * 0.5, CANVAS_H * 0.5);
        ctx.scale(hudZoom, hudZoom);
        ctx.translate(-CANVAS_W * 0.5, -CANVAS_H * 0.5);

        // Subtle dark lane backing for player readability
        ctx.fillStyle = 'rgba(8, 6, 22, 0.34)';
        roundRect(ctx, lay.plyCenterX - lay.spacing * 2 - 14, 0, lay.spacing * 4 + 28, CANVAS_H, 0);
        ctx.fill();

        // Draw Opponent receptors & Player receptors
        const oppAlpha = s.middlescroll ? 0.32 : 0.85;
        for (let d = 0; d < 4; d++) {
          const ox = lay.laneX(lay.oppCenterX, d);
          const osc = 1 + oppGlow[d] * 0.14;
          drawReceptor(ctx, d, ox, lay.receptorY, lay.noteSize, oppAlpha, osc, oppGlow[d] > 0.12 ? dirColor(d) : undefined);

          const px = lay.laneX(lay.plyCenterX, d);
          const psc = lanePress[d] ? 0.92 : 1 + laneGlow[d] * 0.15;
          drawReceptor(ctx, d, px, lay.receptorY, lay.noteSize, 0.95, psc, laneGlow[d] > 0.12 || lanePress[d] ? dirColor(d) : undefined);

          // Keybind label hint during countdown
          if (!songStarted) {
            drawText(
              ctx,
              'ui',
              formatKeyCode(s.keys4[d]),
              px,
              lay.receptorY + (s.downscroll ? -74 : 56),
              20,
              COLOR_GOLD,
              'center',
              2,
            );
          }
        }

        // Draw Sustain Tails first, then Note Heads
        for (const n of session.notes) {
          if (n.holdCompleted) continue;
          const isPlayerNote = n.side === session.playerSide;
          const centerX = isPlayerNote ? lay.plyCenterX : lay.oppCenterX;
          const x = lay.laneX(centerX, n.dir);
          const noteAlpha = isPlayerNote ? (n.missed ? 0.35 : 1) : oppAlpha;

          if (n.lengthMs > 40) {
            const headMs = n.holding ? Math.max(n.timeMs, songMs) : n.timeMs;
            const tailMs = n.timeMs + n.lengthMs;
            const yHead = lay.receptorY + (headMs - songMs) * pxPerMs * dirSign;
            const yTail = lay.receptorY + (tailMs - songMs) * pxPerMs * dirSign;
            if (Math.min(yHead, yTail) < CANVAS_H + 120 && Math.max(yHead, yTail) > -120) {
              drawSustain(ctx, n.dir, x, yHead, yTail, 34, noteAlpha * 0.88, true);
            }
          }

          if (!n.hit) {
            const y = lay.receptorY + (n.timeMs - songMs) * pxPerMs * dirSign;
            if (y > -120 && y < CANVAS_H + 120) {
              drawNote(ctx, n.dir, x, y, lay.noteSize, noteAlpha, 1);
            }
          }
        }

        // Note splash particles
        particles.draw(ctx, true);

        // Rating & Combo popup
        if (lastRating && nowSec - lastRating.born < 0.72) {
          const age = nowSec - lastRating.born;
          const pop = 1 + Math.exp(-age * 16) * 0.35;
          const alpha = saturate(1 - Math.max(0, age - 0.36) / 0.36);
          const py = 278 - age * 46;
          ctx.save();
          ctx.translate(CANVAS_W * 0.5, py);
          ctx.scale(pop, pop);
          drawGlowText(ctx, 'title', lastRating.text, 0, -24, 54, { ...lastRating.color, a: Math.round(255 * alpha) }, lastRating.color, 'center');
          if (lastRating.sub) {
            drawText(ctx, 'ui', lastRating.sub, 0, 32, 22, { ...COLOR_IVORY, a: Math.round(230 * alpha) }, 'center', 2);
          }
          if (lastRating.combo >= 5) {
            drawText(ctx, 'title', `${lastRating.combo}x`, 0, 56, 34, { ...COLOR_GOLD, a: Math.round(255 * alpha) }, 'center', 3);
          }
          ctx.restore();
        }

        // Countdown big text
        if (!songStarted && countdownStep >= 0 && countdownStep < 4) {
          const labels = ['3', '2', '1', 'GO!'];
          const frac = (countdownElapsed % beatSec) / beatSec;
          const sc = 1 + (1 - frac) * 0.35;
          const alpha = saturate(1 - frac * 0.7);
          const col = countdownStep === 3 ? COLOR_GOLD : COLOR_WHITE;
          ctx.save();
          ctx.translate(CANVAS_W * 0.5, 310);
          ctx.scale(sc, sc);
          drawGlowText(ctx, 'title', labels[countdownStep], 0, -52, 118, { ...col, a: Math.round(255 * alpha) }, COLOR_GOLD, 'center');
          ctx.restore();
        }

        // HUD elements (unless hidden)
        if (!s.hideHud) {
          // Time Bar
          if (s.showTimeBar) {
            const tbW = 420;
            const tbH = 14;
            const tbX = CANVAS_W * 0.5 - tbW * 0.5;
            const tbY = s.downscroll ? 684 : 18;
            const prog = saturate(Math.max(0, songMs / 1000) / Math.max(1, durationSec));
            roundRect(ctx, tbX - 3, tbY - 3, tbW + 6, tbH + 6, 9);
            ctx.fillStyle = 'rgba(10, 6, 24, 0.85)';
            ctx.fill();
            roundRect(ctx, tbX, tbY, tbW * prog, tbH, 7);
            const tg = ctx.createLinearGradient(tbX, 0, tbX + tbW, 0);
            tg.addColorStop(0, '#ff4fa3');
            tg.addColorStop(1, '#36b5ff');
            ctx.fillStyle = tg;
            ctx.fill();
            drawText(
              ctx,
              'ui',
              `${chart.title}  •  ${fmtTime(Math.max(0, durationSec - Math.max(0, songMs / 1000)))}`,
              CANVAS_W * 0.5,
              tbY - 4,
              19,
              COLOR_WHITE,
              'center',
              2,
            );
          }

          // Audio Visualizer (top-right, Issue #31)
          if (s.showVisualizer) {
            viz.draw(ctx, CANVAS_W - 252, s.downscroll ? 24 : 20, 230, 74, 0.95);
          }

          // Health Bar & Bopping Character Icons
          const hbW = 580;
          const hbH = 20;
          const hbX = CANVAS_W * 0.5 - hbW * 0.5;
          const hbY = s.downscroll ? 58 : 640;
          const plyFrac = saturate(session.health / 2);
          const oppCol = charColor(chart.opponent);
          const plyCol = charColor(chart.player);

          roundRect(ctx, hbX - 4, hbY - 4, hbW + 8, hbH + 8, 10);
          ctx.fillStyle = 'rgba(12, 8, 28, 0.9)';
          ctx.fill();

          const splitX = hbX + hbW * (1 - plyFrac);
          ctx.fillStyle = rgba(oppCol);
          ctx.fillRect(hbX, hbY, Math.max(0, splitX - hbX), hbH);
          ctx.fillStyle = rgba(plyCol);
          ctx.fillRect(splitX, hbY, Math.max(0, hbX + hbW - splitX), hbH);

          // Gloss strip on health bar
          ctx.fillStyle = 'rgba(255,255,255,0.22)';
          ctx.fillRect(hbX, hbY, hbW, hbH * 0.42);

          const iconSize = 68 * (1 + iconBounce * 0.14);
          drawIcon(ctx, chart.opponent, oppCol, splitX - 36, hbY + hbH * 0.5, iconSize, session.health > 1.6, nowSec, false);
          drawIcon(ctx, chart.player, plyCol, splitX + 36, hbY + hbH * 0.5, iconSize, session.health < 0.4, nowSec, true);

          // Score / Misses / Accuracy / Rank / NPS bar
          const scoreY = s.downscroll ? 86 : 672;
          const botTag = mods.botplay ? '  [BOTPLAY]' : mods.practice ? '  [PRACTICE]' : '';
          const statsLine = `Score: ${fmtThousands(session.score)}   |   Misses: ${session.misses}   |   Acc: ${session.accuracy.toFixed(2)}% (${session.rank} • ${session.clearLabel})   |   NPS: ${session.nps}${botTag}`;
          drawText(ctx, 'ui', statsLine, CANVAS_W * 0.5, scoreY, 22, COLOR_WHITE, 'center', 2.4);

          // Watermark & FPS
          if (s.showWatermark) {
            drawText(
              ctx,
              'ui',
              `God's Hand Engine v2.0.0 Web  •  ${charDisplayName(chart.opponent)} vs ${charDisplayName(chart.player)}`,
              18,
              CANVAS_H - 28,
              16,
              mix(COLOR_IVORY, COLOR_WHITE, 0.2),
              'left',
              1.6,
            );
          }
          if (s.showFps) {
            drawText(ctx, 'ui', `${fpsVal} FPS`, 18, 16, 18, COLOR_GOLD, 'left', 2);
          }
        }

        ctx.restore();
      };

      rafId = requestAnimationFrame(loop);

      return () => {
        window.removeEventListener('keydown', onKeyDown);
        window.removeEventListener('keyup', onKeyUp);
        canvasEl?.removeEventListener('pointerdown', onPointerDown);
        window.removeEventListener('pointerup', onPointerUp);
        window.removeEventListener('pointercancel', onPointerUp);
      };
    }, 30);

    return () => {
      cancelled = true;
      window.clearTimeout(initTimer);
      cancelAnimationFrame(rafId);
      audio.stopSong();
    };
  }, [request, restartToken]);

  return (
    <div className="relative w-full h-full bg-[#05030d] flex items-center justify-center overflow-hidden">
      <canvas
        ref={canvasRef}
        width={CANVAS_W}
        height={CANVAS_H}
        className="max-w-full max-h-full aspect-video shadow-[0_0_60px_rgba(0,0,0,0.9)] touch-none"
      />

      {/* Loading / Synthesizing overlay */}
      {statusText && (
        <div className="absolute inset-0 bg-[#070414]/90 backdrop-blur-sm flex flex-col items-center justify-center z-30">
          <div className="w-12 h-12 rounded-full border-4 border-[#a464ff]/30 border-t-[#ffde40] animate-spin mb-5" />
          <div className="font-title text-3xl text-[#ffde40] tracking-wider drop-shadow-[0_3px_0_#100820]">
            {request.song.title.toUpperCase()}
          </div>
          <div className="font-ui text-lg text-[#c7d2fe] mt-2">{statusText}</div>
        </div>
      )}

      {/* Quick pause button in top-left for mouse/touch users */}
      {!statusText && !paused && !gameOver && (
        <button
          onClick={() => {
            setPaused(true);
            pausedRef.current = true;
            setPauseIdx(0);
            audio.pauseSong();
            audio.playSfx('cancel');
          }}
          className="absolute top-3 left-3 z-20 px-3 py-1.5 rounded-lg bg-[#0d0822]/75 border border-white/15 text-xs font-ui text-white/80 hover:text-[#ffde40] hover:border-[#ffde40]/60 transition cursor-pointer"
          title="Pause (Esc)"
        >
          ESC • PAUSE
        </button>
      )}

      {/* Pause Overlay */}
      {paused && (
        <div className="absolute inset-0 bg-[#060314]/82 backdrop-blur-md flex flex-col items-center justify-center z-40">
          <h2 className="font-title text-6xl text-[#ffde40] tracking-wide drop-shadow-[0_4px_0_#120826] mb-2">
            PAUSED
          </h2>
          <p className="text-sm text-[#c7d2fe]/80 mb-8">
            {request.song.title} • Press Enter to select or Esc to resume
          </p>
          <div className="flex flex-col gap-2.5 w-72">
            {pauseItems.map((label, idx) => (
              <button
                key={label}
                onMouseEnter={() => setPauseIdx(idx)}
                onClick={() => handlePauseAction(idx)}
                className={`py-3 px-6 rounded-xl font-title text-2xl tracking-wide transition cursor-pointer border-2 ${
                  pauseIdx === idx
                    ? 'bg-gradient-to-r from-[#ff4fa3] to-[#a464ff] text-white border-[#ffde40] scale-105 shadow-[0_0_25px_rgba(164,100,255,0.6)]'
                    : 'bg-[#120b2a]/85 text-[#e2e8f0] border-white/10 hover:border-white/30'
                }`}
              >
                {label}
              </button>
            ))}
          </div>
        </div>
      )}

      {/* Game Over Overlay */}
      {gameOver && (
        <div className="absolute inset-0 bg-[#0d020c]/90 backdrop-blur-md flex flex-col items-center justify-center z-40">
          <h2 className="font-title text-7xl text-[#ff3054] tracking-wider drop-shadow-[0_5px_0_#1a040e] mb-3">
            BLUEBALLED!
          </h2>
          <p className="text-lg text-[#fecdd3] mb-8">Your rhythm flatlined — ready for a rematch?</p>
          <div className="flex gap-4">
            <button
              onClick={() => {
                audio.playSfx('confirm');
                setRestartToken((t) => t + 1);
              }}
              className="px-8 py-3.5 rounded-xl font-title text-2xl bg-gradient-to-r from-[#36b5ff] to-[#6366f1] text-white border-2 border-[#ffde40] hover:scale-105 transition cursor-pointer shadow-[0_0_25px_rgba(54,181,255,0.5)]"
            >
              RETRY (ENTER)
            </button>
            <button
              onClick={() => {
                audio.playSfx('cancel');
                onExit();
              }}
              className="px-8 py-3.5 rounded-xl font-title text-2xl bg-[#180d32] text-white/90 border-2 border-white/20 hover:border-white/50 transition cursor-pointer"
            >
              EXIT (ESC)
            </button>
          </div>
        </div>
      )}
    </div>
  );
};
