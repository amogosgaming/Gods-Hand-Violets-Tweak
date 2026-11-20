// Root application: Menus, Story Mode, Freeplay, AutoChart, Options, Credits, Results,
// animated Frutiger Aero Canvas backdrop, Rooftop Groove menu music, and global volume tray.
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  Chart,
  CharId,
  DIFFICULTIES,
  Difficulty,
  Modifiers,
  PlayRequest,
  RATINGS,
  ResultData,
  ScoreMap,
  ScreenId,
  Settings,
  SongEntry,
} from './engine/types';
import {
  buildBuiltinChart,
  builtinEntries,
  composeSong,
  findBuiltinDef,
  MENU_THEME,
  parsePsychChart,
} from './engine/composer';
import { audio } from './engine/audio';
import {
  defaultModifiers,
  defaultSettings,
  fmtThousands,
  formatKeyCode,
  loadScores,
  loadSettings,
  saveSettings,
  scoreKey,
  submitScore,
} from './engine/storage';
import {
  charBeat,
  charColor,
  charDisplayName,
  drawCharacter,
  drawIcon,
  makeCharState,
} from './engine/characters';
import {
  CANVAS_H,
  CANVAS_W,
  drawMenuBackground,
  drawVolumeTray,
} from './engine/gfx';
import { PlayCanvas } from './ui/PlayCanvas';
import { ChartEditor } from './ui/ChartEditor';
import {
  ArrowLeft,
  AudioWaveform,
  FileJson,
  Music,
  Play,
  Search,
  Settings2,
  Sliders,
  Sparkles,
  Trophy,
  Upload,
  Volume2,
  VolumeX,
} from 'lucide-react';

// Small live Canvas portrait for character head icons (Freeplay / Story)
const CharIconCanvas: React.FC<{ id: CharId; size?: number; losing?: boolean }> = ({
  id,
  size = 64,
  losing = false,
}) => {
  const ref = useRef<HTMLCanvasElement | null>(null);
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    const ctx = c.getContext('2d');
    if (!ctx) return;
    ctx.clearRect(0, 0, c.width, c.height);
    drawIcon(ctx, id, charColor(id), c.width * 0.5, c.height * 0.5, c.width * 0.88, losing, performance.now() / 1000, false);
  }, [id, losing, size]);
  return <canvas ref={ref} width={size} height={size} className="shrink-0" />;
};

// Timing scatter plot on Results screen (early vs late ms across song progress)
const HitErrorGraph: React.FC<{ result: ResultData }> = ({ result }) => {
  const ref = useRef<HTMLCanvasElement | null>(null);
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    const ctx = c.getContext('2d');
    if (!ctx) return;
    const w = c.width;
    const h = c.height;
    ctx.clearRect(0, 0, w, h);

    // Background & judgement bands
    ctx.fillStyle = 'rgba(10, 6, 26, 0.78)';
    ctx.fillRect(0, 0, w, h);

    const maxMs = 175;
    const yFor = (ms: number) => h * 0.5 + (ms / maxMs) * (h * 0.44);

    // SICK band (+/-45ms) & GOOD band (+/-90ms)
    ctx.fillStyle = 'rgba(120, 255, 170, 0.08)';
    ctx.fillRect(0, yFor(-90), w, yFor(90) - yFor(-90));
    ctx.fillStyle = 'rgba(255, 242, 140, 0.12)';
    ctx.fillRect(0, yFor(-45), w, yFor(45) - yFor(-45));

    // Center 0ms line
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.35)';
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(0, h * 0.5);
    ctx.lineTo(w, h * 0.5);
    ctx.stroke();

    const maxTime = result.hitErrors.reduce((m, e) => Math.max(m, e.timeMs), 1000);
    for (const sample of result.hitErrors) {
      const x = 10 + (sample.timeMs / Math.max(1, maxTime)) * (w - 20);
      const y = sample.rating < 0 ? h - 8 : Math.max(6, Math.min(h - 6, yFor(sample.deltaMs)));
      if (sample.rating < 0) {
        ctx.fillStyle = '#ff3054';
      } else {
        const col = RATINGS[sample.rating].color;
        ctx.fillStyle = `rgb(${col.r},${col.g},${col.b})`;
      }
      ctx.beginPath();
      ctx.arc(x, y, sample.rating < 0 ? 3.2 : 2.6, 0, Math.PI * 2);
      ctx.fill();
    }

    ctx.fillStyle = 'rgba(255,255,255,0.55)';
    ctx.font = '700 11px "Rajdhani", sans-serif';
    ctx.fillText('EARLY (-166ms)', 8, 13);
    ctx.fillText('LATE (+166ms)', 8, h - 6);
  }, [result]);

  return (
    <canvas
      ref={ref}
      width={460}
      height={140}
      className="w-full h-[140px] rounded-xl border border-white/15"
    />
  );
};

const MAIN_MENU_ITEMS: { id: ScreenId; label: string; desc: string; tint: number }[] = [
  { id: 'story', label: 'STORY MODE', desc: 'Week 1: The Big Show — 3-song gauntlet', tint: 0xa464ff },
  { id: 'freeplay', label: 'FREEPLAY', desc: 'Play any track, modifiers, & custom charts', tint: 0x36b5ff },
  { id: 'autochart', label: 'AUTOCHART', desc: 'Drop any MP3/WAV/OGG to auto-generate a 4K chart', tint: 0xff4fa3 },
  { id: 'editor', label: 'CHART EDITOR', desc: 'Psych-style 8-lane step grid & EZ Spam tools', tint: 0xffc93c },
  { id: 'options', label: 'OPTIONS', desc: 'Scroll direction, visual effects, audio, & 4K keys', tint: 0x3ce0a0 },
  { id: 'credits', label: 'CREDITS', desc: 'Original creator, bundled fonts, & web engine', tint: 0x8b5cf6 },
];

export function App() {
  const [screen, setScreen] = useState<ScreenId>('title');
  const [settings, setSettings] = useState<Settings>(() => loadSettings());
  const [scores, setScores] = useState<ScoreMap>(() => loadScores());
  const [customSongs, setCustomSongs] = useState<SongEntry[]>([]);
  const [audioUnlocked, setAudioUnlocked] = useState(false);

  // Navigation & selections
  const [mainIdx, setMainIdx] = useState(0);
  const [storyDiff, setStoryDiff] = useState<Difficulty>(1);
  const [freeIdx, setFreeIdx] = useState(0);
  const [freeDiff, setFreeDiff] = useState<Difficulty>(1);
  const [searchQuery, setSearchQuery] = useState('');
  const [modsOpen, setModsOpen] = useState(false);
  const [mods, setMods] = useState<Modifiers>(() => defaultModifiers());
  const [optionsTab, setOptionsTab] = useState<'gameplay' | 'visuals' | 'audio' | 'keys'>('gameplay');
  const [rebindingLane, setRebindingLane] = useState<number | null>(null);
  const [optionsReturnTo, setOptionsReturnTo] = useState<ScreenId>('main');
  const [toastMsg, setToastMsg] = useState<string>('');

  // Gameplay / Editor / Results state
  const [playRequest, setPlayRequest] = useState<PlayRequest | null>(null);
  const [lastResult, setLastResult] = useState<ResultData | null>(null);
  const [editorTarget, setEditorTarget] = useState<{ chart: Chart; song: SongEntry; returnTo: ScreenId } | null>(null);

  // Volume tray timing ref
  const volTrayStampRef = useRef(-10);
  const bgCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const audioUploadRef = useRef<HTMLInputElement | null>(null);
  const chartUploadRef = useRef<HTMLInputElement | null>(null);

  const allSongs = useMemo<SongEntry[]>(() => [...builtinEntries(), ...customSongs], [customSongs]);

  const filteredSongs = useMemo(() => {
    const q = searchQuery.trim().toLowerCase();
    if (!q) return allSongs;
    return allSongs.filter(
      (s) =>
        s.title.toLowerCase().includes(q) ||
        s.subtitle.toLowerCase().includes(q) ||
        charDisplayName(s.opponent).toLowerCase().includes(q),
    );
  }, [allSongs, searchQuery]);

  const selectedSong = filteredSongs[Math.min(freeIdx, Math.max(0, filteredSongs.length - 1))] ?? allSongs[0];

  const showToast = useCallback((msg: string) => {
    setToastMsg(msg);
    window.setTimeout(() => {
      setToastMsg((cur) => (cur === msg ? '' : cur));
    }, 3200);
  }, []);

  // Persist & apply volumes whenever settings change
  useEffect(() => {
    saveSettings(settings);
    audio.setVolumes(settings.masterVol, settings.musicVol, settings.sfxVol, settings.muted);
  }, [settings]);

  // Menu theme ("Rooftop Groove") looping when unlocked and not in play/editor/freeplay-preview
  const startMenuMusic = useCallback(() => {
    if (audio.getActiveTrackId() === 'menu:rooftop_groove' && audio.isPlaying()) return;
    const comp = composeSong(MENU_THEME, true);
    const ab = audio.stereoToAudioBuffer(comp.inst!);
    audio.loadStems(ab, null, null, 'menu:rooftop_groove');
    audio.playSong(0, 1.0, true);
  }, []);

  useEffect(() => {
    if (!audioUnlocked) return;
    if (screen === 'play' || screen === 'editor') return;
    if (screen === 'freeplay' || screen === 'autochart') {
      // Preview selected song smoothly after brief debounce
      const song = selectedSong;
      if (!song) return;
      const timer = window.setTimeout(() => {
        if (song.isBuiltin) {
          const def = findBuiltinDef(song.id);
          if (def && audio.getActiveTrackId() !== `preview:${def.id}`) {
            const comp = composeSong(def, true);
            audio.loadStems(
              audio.stereoToAudioBuffer(comp.inst!),
              audio.stereoToAudioBuffer(comp.oppVoice!),
              audio.stereoToAudioBuffer(comp.playerVoice!),
              `preview:${def.id}`,
            );
            audio.playSong(0, 1.0, true);
          }
        } else if (song.customAudioBuffer && audio.getActiveTrackId() !== `preview:${song.id}`) {
          audio.loadStems(song.customAudioBuffer, null, null, `preview:${song.id}`);
          audio.playSong(0, 1.0, true);
        }
      }, 180);
      return () => window.clearTimeout(timer);
    } else {
      startMenuMusic();
    }
  }, [audioUnlocked, screen, selectedSong, startMenuMusic]);

  // Global volume shortcuts (-, =, 0)
  useEffect(() => {
    const onGlobalKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement || rebindingLane !== null) return;
      if (e.code === 'Minus') {
        setSettings((s) => ({ ...s, muted: false, masterVol: Math.max(0, +(s.masterVol - 0.1).toFixed(2)) }));
        volTrayStampRef.current = performance.now() / 1000;
        audio.playSfx('scroll', 0.6);
      } else if (e.code === 'Equal') {
        setSettings((s) => ({ ...s, muted: false, masterVol: Math.min(1, +(s.masterVol + 0.1).toFixed(2)) }));
        volTrayStampRef.current = performance.now() / 1000;
        audio.playSfx('scroll', 0.6);
      } else if (e.code === 'Digit0') {
        setSettings((s) => ({ ...s, muted: !s.muted }));
        volTrayStampRef.current = performance.now() / 1000;
      }
    };
    window.addEventListener('keydown', onGlobalKey);
    return () => window.removeEventListener('keydown', onGlobalKey);
  }, [rebindingLane]);

  // Open Chart Editor helper
  const openEditorForSong = useCallback(
    (song: SongEntry, diff: Difficulty, returnTo: ScreenId, chartOverride?: Chart) => {
      let chart: Chart;
      if (chartOverride) {
        chart = chartOverride;
      } else if (song.isBuiltin) {
        const def = findBuiltinDef(song.id)!;
        const comp = composeSong(def, false);
        chart = buildBuiltinChart(def, comp, diff, false);
      } else if (song.customChart) {
        chart = song.customChart;
      } else {
        const def = findBuiltinDef('vigil')!;
        const comp = composeSong(def, false);
        chart = buildBuiltinChart(def, comp, diff, false);
      }
      audio.stopSong();
      setEditorTarget({ chart, song, returnTo });
      setScreen('editor');
    },
    [],
  );

  // Launch PlayCanvas helper
  const launchSong = useCallback(
    (req: PlayRequest) => {
      audio.stopSong();
      setPlayRequest(req);
      setScreen('play');
    },
    [],
  );

  const startStoryMode = useCallback(() => {
    const weekSongs = builtinEntries().filter((s) => s.id !== 'vigil');
    audio.playSfx('confirm');
    launchSong({
      song: weekSongs[0],
      difficulty: storyDiff,
      mods: defaultModifiers(),
      autoChart: false,
      storyQueue: weekSongs,
      storyIndex: 0,
      storyAccumScore: 0,
      fromScreen: 'story',
    });
  }, [storyDiff, launchSong]);

  const unlockAndEnterMain = useCallback(() => {
    audio.ensureContext();
    setAudioUnlocked(true);
    audio.playSfx('confirm');
    setScreen('main');
  }, []);

  // Screen-specific keyboard navigation
  useEffect(() => {
    if (screen === 'play' || screen === 'editor') return;

    // Key rebinding capture
    if (screen === 'options' && rebindingLane !== null) {
      const onCapture = (e: KeyboardEvent) => {
        e.preventDefault();
        if (e.code === 'Escape') {
          setRebindingLane(null);
          audio.playSfx('cancel');
          return;
        }
        setSettings((prev) => {
          const next = [...prev.keys4] as [string, string, string, string];
          next[rebindingLane] = e.code;
          return { ...prev, keys4: next };
        });
        setRebindingLane(null);
        audio.playSfx('confirm');
      };
      window.addEventListener('keydown', onCapture);
      return () => window.removeEventListener('keydown', onCapture);
    }

    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement) {
        if (e.code === 'Escape') (e.target as HTMLInputElement).blur();
        return;
      }

      if (screen === 'title') {
        if (e.code === 'Enter' || e.code === 'Space') {
          e.preventDefault();
          unlockAndEnterMain();
        }
        return;
      }

      if (screen === 'main') {
        if (e.code === 'ArrowUp' || e.code === 'KeyW') {
          e.preventDefault();
          audio.playSfx('scroll');
          setMainIdx((i) => (i + MAIN_MENU_ITEMS.length - 1) % MAIN_MENU_ITEMS.length);
        } else if (e.code === 'ArrowDown' || e.code === 'KeyS') {
          e.preventDefault();
          audio.playSfx('scroll');
          setMainIdx((i) => (i + 1) % MAIN_MENU_ITEMS.length);
        } else if (e.code === 'Enter') {
          e.preventDefault();
          audio.playSfx('confirm');
          const target = MAIN_MENU_ITEMS[mainIdx].id;
          if (target === 'editor') {
            openEditorForSong(allSongs[1] ?? allSongs[0], 1, 'main');
          } else {
            if (target === 'options') setOptionsReturnTo('main');
            setScreen(target);
          }
        } else if (e.code === 'Digit7') {
          e.preventDefault();
          audio.playSfx('confirm');
          openEditorForSong(allSongs[1] ?? allSongs[0], 1, 'main');
        } else if (e.code === 'Escape') {
          e.preventDefault();
          audio.playSfx('cancel');
          setScreen('title');
        }
        return;
      }

      if (screen === 'story') {
        if (e.code === 'ArrowLeft' || e.code === 'KeyA') {
          audio.playSfx('scroll');
          setStoryDiff((d) => (((d + 2) % 3) as Difficulty));
        } else if (e.code === 'ArrowRight' || e.code === 'KeyD') {
          audio.playSfx('scroll');
          setStoryDiff((d) => (((d + 1) % 3) as Difficulty));
        } else if (e.code === 'Enter') {
          e.preventDefault();
          startStoryMode();
        } else if (e.code === 'Escape') {
          audio.playSfx('cancel');
          setScreen('main');
        }
        return;
      }

      if (screen === 'freeplay' || screen === 'autochart') {
        if (e.code === 'Tab') {
          e.preventDefault();
          audio.playSfx('scroll');
          setModsOpen((v) => !v);
          return;
        }
        if (modsOpen) {
          if (e.code === 'Escape') {
            setModsOpen(false);
            audio.playSfx('cancel');
          }
          return;
        }
        if (e.code === 'ArrowUp' || e.code === 'KeyW') {
          e.preventDefault();
          audio.playSfx('scroll');
          setFreeIdx((i) => (i + Math.max(1, filteredSongs.length) - 1) % Math.max(1, filteredSongs.length));
        } else if (e.code === 'ArrowDown' || e.code === 'KeyS') {
          e.preventDefault();
          audio.playSfx('scroll');
          setFreeIdx((i) => (i + 1) % Math.max(1, filteredSongs.length));
        } else if (e.code === 'ArrowLeft' || e.code === 'KeyA') {
          audio.playSfx('scroll');
          setFreeDiff((d) => (((d + 3) % 4) as Difficulty));
        } else if (e.code === 'ArrowRight' || e.code === 'KeyD') {
          audio.playSfx('scroll');
          setFreeDiff((d) => (((d + 1) % 4) as Difficulty));
        } else if (e.code === 'Enter' && selectedSong) {
          e.preventDefault();
          audio.playSfx('confirm');
          launchSong({
            song: selectedSong,
            difficulty: freeDiff,
            mods,
            autoChart: screen === 'autochart',
            fromScreen: screen,
          });
        } else if (e.code === 'Digit7' && selectedSong) {
          e.preventDefault();
          audio.playSfx('confirm');
          openEditorForSong(selectedSong, freeDiff, screen);
        } else if (e.code === 'Escape') {
          audio.playSfx('cancel');
          setScreen('main');
        }
        return;
      }

      if (screen === 'options' || screen === 'credits') {
        if (e.code === 'Escape') {
          audio.playSfx('cancel');
          setScreen(screen === 'options' ? optionsReturnTo : 'main');
        }
        return;
      }

      if (screen === 'results' && lastResult) {
        if (e.code === 'Enter' || e.code === 'Escape') {
          e.preventDefault();
          handleResultsContinue();
        }
      }
    };

    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [
    screen,
    mainIdx,
    storyDiff,
    freeIdx,
    freeDiff,
    filteredSongs,
    selectedSong,
    mods,
    modsOpen,
    allSongs,
    rebindingLane,
    optionsReturnTo,
    lastResult,
    unlockAndEnterMain,
    startStoryMode,
    launchSong,
    openEditorForSong,
  ]);

  // Animated 1280x720 Canvas menu backdrop (drawMenuBackground + bopping Frutiger Aero cast + volume tray)
  useEffect(() => {
    if (screen === 'play' || screen === 'editor') return;

    const hype = makeCharState('herald');
    const principal = makeCharState('principality');
    const hand = makeCharState('hand');
    const demise = makeCharState('demise');
    let lastBeat = -1;
    let rafId = 0;

    const render = (nowMs: number) => {
      rafId = requestAnimationFrame(render);
      const canvas = bgCanvasRef.current;
      if (!canvas) return;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;

      const now = nowMs / 1000;
      const bpm = screen === 'freeplay' || screen === 'autochart' ? selectedSong?.bpm ?? 104 : MENU_THEME.bpm;
      const beatF = now * (bpm / 60);
      const beatInt = Math.floor(beatF);
      const pulse = Math.exp(-(beatF - beatInt) * 5.0);

      if (beatInt > lastBeat) {
        lastBeat = beatInt;
        charBeat(hype, now);
        charBeat(principal, now);
        charBeat(hand, now);
        charBeat(demise, now);
      }

      let tint = 0xa464ff;
      if (screen === 'main') tint = MAIN_MENU_ITEMS[mainIdx]?.tint ?? 0xa464ff;
      else if (screen === 'freeplay' || screen === 'autochart') tint = selectedSong?.accentHex ?? 0x36b5ff;
      else if (screen === 'story') tint = 0xff4fa3;
      else if (screen === 'results' && lastResult) tint = lastResult.song.accentHex;
      else if (screen === 'options') tint = 0x3ce0a0;

      ctx.clearRect(0, 0, CANVAS_W, CANVAS_H);
      drawMenuBackground(ctx, now, tint, pulse);

      // Draw animated bopping characters on Title, Main Menu, and Story Mode
      if (screen === 'title') {
        drawCharacter(ctx, principal, 260, 668, 0.96, now, false);
        drawCharacter(ctx, hype, 1020, 668, 1.0, now, true);
      } else if (screen === 'main') {
        drawCharacter(ctx, hype, 1010, 678, 0.96, now, true);
      } else if (screen === 'story') {
        drawCharacter(ctx, principal, 825, 650, 0.72, now, true);
        drawCharacter(ctx, hand, 1000, 665, 0.72, now, true);
        drawCharacter(ctx, demise, 1170, 655, 0.72, now, true);
      }

      // Global Flixel-style volume tray
      const trayAge = now - volTrayStampRef.current;
      if (trayAge >= 0 && trayAge <= 1.8) {
        drawVolumeTray(ctx, settings.masterVol, settings.muted, trayAge);
      }
    };

    rafId = requestAnimationFrame(render);
    return () => cancelAnimationFrame(rafId);
  }, [screen, mainIdx, selectedSong, lastResult, settings.masterVol, settings.muted]);

  // Custom audio file upload (AutoChart)
  const handleAudioUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    try {
      showToast(`Decoding ${file.name}...`);
      const buf = await audio.decodeFile(file);
      const cleanName = file.name.replace(/\.[^.]+$/, '');
      const entry: SongEntry = {
        id: `custom_audio_${Date.now()}`,
        title: cleanName,
        subtitle: `Imported Audio • ${Math.round(buf.duration)}s`,
        artist: 'User Audio',
        bpm: 120,
        stage: 3,
        opponent: 'shade',
        accentHex: 0xff4fa3,
        isBuiltin: false,
        isCustomAudio: true,
        customAudioBuffer: buf,
        difficulties: [0, 1, 2, 3],
      };
      setCustomSongs((prev) => [...prev, entry]);
      setFreeIdx(allSongs.length);
      audio.playSfx('unlock');
      showToast(`Ready for AutoChart: ${cleanName}!`);
    } catch {
      audio.playSfx('cancel');
      showToast('Could not decode audio file. Try MP3, WAV, or OGG.');
    }
    e.target.value = '';
  };

  // Custom Psych JSON chart upload
  const handleChartUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    try {
      const txt = await file.text();
      const parsed = parsePsychChart(txt, file.name.replace(/\.json$/i, ''));
      const entry: SongEntry = {
        id: `custom_json_${Date.now()}`,
        title: parsed.title,
        subtitle: `Psych Chart • ${parsed.notes.length} notes`,
        artist: 'Imported JSON',
        bpm: parsed.bpm,
        stage: parsed.stage,
        opponent: parsed.opponent,
        accentHex: parsed.accentHex,
        isBuiltin: false,
        customChart: parsed,
        difficulties: [2],
      };
      setCustomSongs((prev) => [...prev, entry]);
      setFreeIdx(allSongs.length);
      audio.playSfx('unlock');
      showToast(`Imported chart: ${parsed.title} (${parsed.notes.length} notes)`);
    } catch {
      audio.playSfx('cancel');
      showToast('Invalid Psych Engine .json file.');
    }
    e.target.value = '';
  };

  const handlePlayFinish = useCallback(
    (res: ResultData) => {
      let isNewBest = false;
      if (!res.botplay && !res.practice) {
        const out = submitScore(
          scores,
          res.song.id,
          res.difficulty,
          {
            score: res.score,
            accuracy: res.accuracy,
            misses: res.misses,
            rank: res.rank,
            clearLabel: res.clearLabel,
          },
          res.autoChart,
        );
        setScores(out.map);
        isNewBest = out.isNewBest;
      }
      setLastResult({ ...res, isNewBest });
      setScreen('results');
      audio.playSfx(isNewBest ? 'unlock' : 'confirm');
    },
    [scores],
  );

  const handleResultsContinue = useCallback(() => {
    if (!lastResult) {
      setScreen('main');
      return;
    }
    audio.playSfx('confirm');
    if (
      lastResult.storyQueue &&
      lastResult.storyIndex !== undefined &&
      lastResult.storyIndex + 1 < lastResult.storyQueue.length
    ) {
      const nextIdx = lastResult.storyIndex + 1;
      launchSong({
        song: lastResult.storyQueue[nextIdx],
        difficulty: lastResult.difficulty,
        mods: lastResult.mods,
        autoChart: false,
        storyQueue: lastResult.storyQueue,
        storyIndex: nextIdx,
        storyAccumScore: lastResult.storyAccumScore,
        fromScreen: 'story',
      });
    } else {
      setScreen(lastResult.fromScreen === 'play' ? 'main' : lastResult.fromScreen);
    }
  }, [lastResult, launchSong]);

  // --- FULL-SCREEN PLAY & EDITOR VIEWS ---
  if (screen === 'play' && playRequest) {
    return (
      <PlayCanvas
        request={playRequest}
        settings={settings}
        onFinish={handlePlayFinish}
        onExit={() => setScreen(playRequest.fromScreen === 'play' ? 'main' : playRequest.fromScreen)}
        onOpenEditor={(chart, song) => openEditorForSong(song, playRequest.difficulty, playRequest.fromScreen, chart)}
        onOpenOptions={() => {
          setOptionsReturnTo(playRequest.fromScreen === 'play' ? 'main' : playRequest.fromScreen);
          setScreen('options');
        }}
      />
    );
  }

  if (screen === 'editor' && editorTarget) {
    return (
      <ChartEditor
        initialChart={editorTarget.chart}
        song={editorTarget.song}
        onPlaytest={(editedChart) =>
          launchSong({
            song: editorTarget.song,
            difficulty: editedChart.difficulty,
            mods,
            autoChart: false,
            chartOverride: editedChart,
            fromScreen: editorTarget.returnTo,
          })
        }
        onExit={() => setScreen(editorTarget.returnTo)}
      />
    );
  }

  // Week 1 combined score for Story Mode
  const weekScore = ['principality', 'godshand', 'demise'].reduce(
    (sum, id) => sum + (scores[scoreKey(id, storyDiff)]?.score ?? 0),
    0,
  );

  const selectedBest = selectedSong
    ? scores[scoreKey(selectedSong.id, freeDiff, screen === 'autochart')]
    : undefined;

  return (
    <div className="relative w-full h-full bg-[#060312] overflow-hidden select-none">
      {/* Animated 1280x720 Synthwave & Frutiger Aero Canvas Backdrop */}
      <canvas
        ref={bgCanvasRef}
        width={CANVAS_W}
        height={CANVAS_H}
        className="absolute inset-0 w-full h-full object-cover pointer-events-none"
      />

      {/* Subtle dark vignette for crisp UI readability */}
      <div className="absolute inset-0 bg-gradient-to-b from-[#070416]/45 via-transparent to-[#050210]/70 pointer-events-none" />

      {/* Quick Audio Mute Toggle (top-right) */}
      {screen !== 'title' && (
        <button
          onClick={() => {
            setSettings((s) => ({ ...s, muted: !s.muted }));
            volTrayStampRef.current = performance.now() / 1000;
          }}
          className="absolute top-4 right-4 z-30 flex items-center gap-2 px-3 py-1.5 rounded-xl bg-[#0e0826]/75 border border-white/15 text-xs font-bold text-white/85 hover:border-[#ffde40]/60 transition cursor-pointer backdrop-blur-md"
          title="Volume: - / = / 0"
        >
          {settings.muted ? <VolumeX className="w-4 h-4 text-[#ff4fa3]" /> : <Volume2 className="w-4 h-4 text-[#36b5ff]" />}
          <span>{settings.muted ? 'MUTED' : `${Math.round(settings.masterVol * 100)}%`}</span>
        </button>
      )}

      {/* Toast Notification */}
      {toastMsg && (
        <div className="absolute bottom-6 left-1/2 -translate-x-1/2 z-50 px-5 py-2.5 rounded-xl bg-[#12092c]/95 border border-[#ffde40]/60 text-sm font-bold text-[#ffde40] shadow-[0_0_30px_rgba(0,0,0,0.75)]">
          {toastMsg}
        </div>
      )}

      {/* Hidden File Inputs */}
      <input
        ref={audioUploadRef}
        type="file"
        accept="audio/*,.mp3,.wav,.ogg,.flac,.m4a"
        className="hidden"
        onChange={handleAudioUpload}
      />
      <input
        ref={chartUploadRef}
        type="file"
        accept=".json"
        className="hidden"
        onChange={handleChartUpload}
      />

      {/* ==================== TITLE SCREEN ==================== */}
      {screen === 'title' && (
        <div
          onClick={unlockAndEnterMain}
          className="relative z-10 w-full h-full flex flex-col items-center justify-between py-12 px-6 cursor-pointer"
        >
          <div className="text-center mt-4">
            <div className="inline-block px-4 py-1 rounded-full bg-[#120a2c]/70 border border-[#ffde40]/40 text-xs font-bold tracking-[0.25em] text-[#ffde40] uppercase mb-4 backdrop-blur-sm">
              God&apos;s Hand Engine v2.0.0 Web
            </div>
            <h1 className="font-title text-6xl sm:text-8xl tracking-wide text-transparent bg-clip-text bg-gradient-to-b from-[#fff7cc] via-[#ffde40] to-[#ff7b3b] drop-shadow-[0_6px_0_#190a32]">
              GOD&apos;S HAND
            </h1>
            <p className="font-title text-2xl sm:text-4xl text-[#f2f4ff] tracking-wider drop-shadow-[0_4px_0_#14092a] -mt-1">
              OF PRINCIPALITY AND DEMISE
            </p>
          </div>

          <div className="flex flex-col items-center gap-3 mb-4">
            <div className="px-8 py-3.5 rounded-2xl bg-gradient-to-r from-[#ff4fa3]/90 via-[#a464ff]/90 to-[#36b5ff]/90 border-2 border-[#ffde40] shadow-[0_0_40px_rgba(255,79,163,0.55)] animate-pulse">
              <span className="font-title text-2xl sm:text-3xl tracking-wider text-white drop-shadow-[0_2px_0_#140826]">
                PRESS ENTER OR CLICK TO BEGIN
              </span>
            </div>
            <p className="text-xs sm:text-sm text-white/75 tracking-wider uppercase font-bold drop-shadow">
              4-Key Rhythm • Procedural Synth Soundtrack • AutoChart • Chart Editor
            </p>
          </div>
        </div>
      )}

      {/* ==================== MAIN MENU ==================== */}
      {screen === 'main' && (
        <div className="relative z-10 w-full h-full flex flex-col justify-between p-6 sm:p-10">
          <div>
            <h1 className="font-title text-4xl sm:text-5xl text-[#ffde40] drop-shadow-[0_4px_0_#15082a] tracking-wide">
              GOD&apos;S HAND
            </h1>
            <p className="text-sm text-[#e2e8f0]/80 font-bold tracking-widest uppercase">
              of Principality and Demise
            </p>
          </div>

          <div className="flex flex-col gap-2.5 max-w-md my-auto">
            {MAIN_MENU_ITEMS.map((item, idx) => {
              const active = idx === mainIdx;
              return (
                <button
                  key={item.id}
                  onMouseEnter={() => {
                    if (mainIdx !== idx) {
                      setMainIdx(idx);
                      audio.playSfx('scroll', 0.65);
                    }
                  }}
                  onClick={() => {
                    audio.playSfx('confirm');
                    if (item.id === 'editor') {
                      openEditorForSong(allSongs[1] ?? allSongs[0], 1, 'main');
                    } else {
                      if (item.id === 'options') setOptionsReturnTo('main');
                      setScreen(item.id);
                    }
                  }}
                  className={`text-left px-6 py-3.5 rounded-2xl border-2 transition-all cursor-pointer backdrop-blur-md ${
                    active
                      ? 'bg-gradient-to-r from-[#ff4fa3]/85 via-[#a464ff]/85 to-[#36b5ff]/70 border-[#ffde40] translate-x-3 shadow-[0_0_32px_rgba(164,100,255,0.55)]'
                      : 'bg-[#100828]/70 border-white/15 hover:border-white/35 text-white/85'
                  }`}
                >
                  <div className="font-title text-3xl tracking-wide leading-none drop-shadow-[0_2px_0_#120624]">
                    {item.label}
                  </div>
                  {active && <div className="text-xs text-white/95 font-bold mt-1">{item.desc}</div>}
                </button>
              );
            })}
          </div>

          <div className="flex flex-wrap items-center justify-between gap-2 text-xs text-white/75 font-bold bg-[#0c0720]/70 px-4 py-2 rounded-xl border border-white/10 backdrop-blur-sm">
            <span>ARROWS / WASD: Navigate • ENTER: Select • 7: Chart Editor • - / = / 0: Volume</span>
            <span className="text-[#ffde40]">♪ Rooftop Groove</span>
          </div>
        </div>
      )}

      {/* ==================== STORY MODE ==================== */}
      {screen === 'story' && (
        <div className="relative z-10 w-full h-full flex flex-col justify-between p-6 sm:p-10">
          <div className="flex items-center gap-4">
            <button
              onClick={() => {
                audio.playSfx('cancel');
                setScreen('main');
              }}
              className="flex items-center gap-1.5 px-3.5 py-2 rounded-xl bg-[#120a2c]/80 border border-white/15 hover:border-[#ffde40] text-sm font-bold cursor-pointer backdrop-blur-md"
            >
              <ArrowLeft className="w-4 h-4" /> BACK
            </button>
            <div>
              <h2 className="font-title text-4xl sm:text-5xl text-[#ffde40] drop-shadow-[0_4px_0_#140728]">
                STORY MODE
              </h2>
              <p className="text-xs uppercase tracking-widest text-white/75 font-bold">
                Full 3-stage campaign across Rooftop, Club, and Back Alley
              </p>
            </div>
          </div>

          <div className="max-w-lg bg-[#0d0724]/82 border-2 border-[#a464ff]/60 rounded-3xl p-6 backdrop-blur-md shadow-2xl my-auto">
            <div className="flex items-center justify-between mb-3">
              <span className="px-3 py-0.5 rounded-full bg-[#ff4fa3]/25 border border-[#ff4fa3] text-xs font-bold text-[#fbcfe8]">
                WEEK 1
              </span>
              <span className="text-sm font-bold text-[#ffde40] flex items-center gap-1.5">
                <Trophy className="w-4 h-4" /> WEEK SCORE: {fmtThousands(weekScore)}
              </span>
            </div>
            <h3 className="font-title text-4xl text-white tracking-wide drop-shadow-[0_3px_0_#120622]">
              THE BIG SHOW
            </h3>
            <p className="text-sm text-[#c7d2fe] mt-1 mb-5">
              Face Principal P on the Sunset Rooftop, DJ Hand in the Neon Club, and D-Mize in the Punk Back Alley.
            </p>

            <div className="space-y-2 mb-6">
              {[
                { name: '1. Principality', vs: 'Principal P', bpm: 128, id: 'principality' as CharId },
                { name: '2. Godshand', vs: 'DJ Hand', bpm: 146, id: 'hand' as CharId },
                { name: '3. Demise', vs: 'D-Mize', bpm: 172, id: 'demise' as CharId },
              ].map((t) => (
                <div
                  key={t.name}
                  className="flex items-center justify-between px-3.5 py-2 rounded-xl bg-white/[0.06] border border-white/10"
                >
                  <div className="flex items-center gap-3">
                    <CharIconCanvas id={t.id} size={40} />
                    <div>
                      <div className="font-title text-xl leading-none text-white">{t.name}</div>
                      <div className="text-xs text-white/65 font-bold">vs {t.vs}</div>
                    </div>
                  </div>
                  <span className="text-xs font-bold text-[#36b5ff]">{t.bpm} BPM</span>
                </div>
              ))}
            </div>

            {/* Difficulty selector */}
            <div className="flex items-center justify-between gap-2 mb-5">
              {([0, 1, 2] as Difficulty[]).map((d) => (
                <button
                  key={d}
                  onClick={() => {
                    audio.playSfx('scroll');
                    setStoryDiff(d);
                  }}
                  className={`flex-1 py-2 rounded-xl font-title text-xl tracking-wide border-2 transition cursor-pointer ${
                    storyDiff === d
                      ? d === 0
                        ? 'bg-[#3ce0a0] text-[#070414] border-white'
                        : d === 1
                          ? 'bg-[#ffde40] text-[#070414] border-white'
                          : 'bg-[#ff3054] text-white border-[#ffde40]'
                      : 'bg-black/40 text-white/70 border-white/15 hover:border-white/35'
                  }`}
                >
                  {DIFFICULTIES[d].toUpperCase()}
                </button>
              ))}
            </div>

            <button
              onClick={startStoryMode}
              className="w-full py-3.5 rounded-2xl font-title text-3xl tracking-wider bg-gradient-to-r from-[#ff4fa3] via-[#a464ff] to-[#36b5ff] text-white border-2 border-[#ffde40] hover:scale-[1.02] transition cursor-pointer shadow-[0_0_30px_rgba(164,100,255,0.55)]"
            >
              START WEEK 1 (ENTER)
            </button>
          </div>

          <div className="text-xs text-white/70 font-bold">
            LEFT / RIGHT: Difficulty • ENTER: Launch Week • ESC: Back
          </div>
        </div>
      )}

      {/* ==================== FREEPLAY & AUTOCHART ==================== */}
      {(screen === 'freeplay' || screen === 'autochart') && (
        <div className="relative z-10 w-full h-full flex flex-col p-5 sm:p-8 gap-4 overflow-hidden">
          {/* Header */}
          <div className="flex flex-wrap items-center justify-between gap-3 pr-24">
            <div className="flex items-center gap-3">
              <button
                onClick={() => {
                  audio.playSfx('cancel');
                  setScreen('main');
                }}
                className="flex items-center gap-1.5 px-3.5 py-2 rounded-xl bg-[#120a2c]/80 border border-white/15 hover:border-[#ffde40] text-sm font-bold cursor-pointer backdrop-blur-md"
              >
                <ArrowLeft className="w-4 h-4" /> BACK
              </button>
              <div>
                <h2 className="font-title text-4xl text-[#ffde40] drop-shadow-[0_3px_0_#140728] leading-none">
                  {screen === 'autochart' ? 'AUTOCHART STUDIO' : 'FREEPLAY'}
                </h2>
                <p className="text-xs text-white/75 font-bold uppercase tracking-wider">
                  {screen === 'autochart'
                    ? 'FFT spectral-flux onset chart generator — play built-in tracks or import your own audio'
                    : 'Select any track, customize modifiers, or import audio / Psych JSON'}
                </p>
              </div>
            </div>

            <div className="flex flex-wrap items-center gap-2">
              <button
                onClick={() => audioUploadRef.current?.click()}
                className="flex items-center gap-1.5 px-3.5 py-2 rounded-xl bg-[#ff4fa3]/85 hover:bg-[#ff4fa3] text-white font-bold text-xs border border-white/25 cursor-pointer shadow"
              >
                <Upload className="w-4 h-4" /> Import Audio (MP3/WAV/OGG)
              </button>
              <button
                onClick={() => chartUploadRef.current?.click()}
                className="flex items-center gap-1.5 px-3.5 py-2 rounded-xl bg-[#36b5ff]/85 hover:bg-[#36b5ff] text-[#070414] font-bold text-xs border border-white/25 cursor-pointer shadow"
              >
                <FileJson className="w-4 h-4" /> Import Psych .JSON
              </button>
              <button
                onClick={() => {
                  audio.playSfx('scroll');
                  setModsOpen((v) => !v);
                }}
                className={`flex items-center gap-1.5 px-3.5 py-2 rounded-xl font-bold text-xs border cursor-pointer ${
                  modsOpen
                    ? 'bg-[#ffde40] text-[#070414] border-white'
                    : 'bg-[#120a2c]/85 text-white border-white/20 hover:border-[#ffde40]'
                }`}
              >
                <Sliders className="w-4 h-4" /> Modifiers (TAB)
              </button>
            </div>
          </div>

          {/* Main 2-column browser */}
          <div className="flex-1 grid grid-cols-1 lg:grid-cols-12 gap-5 min-h-0">
            {/* Left Song List */}
            <div className="lg:col-span-7 flex flex-col gap-3 min-h-0">
              <div className="relative">
                <Search className="w-4 h-4 text-white/50 absolute left-3.5 top-1/2 -translate-y-1/2" />
                <input
                  value={searchQuery}
                  onChange={(e) => {
                    setSearchQuery(e.target.value);
                    setFreeIdx(0);
                  }}
                  placeholder="Search songs or opponents..."
                  className="w-full pl-10 pr-4 py-2.5 rounded-xl bg-[#0e0824]/85 border border-white/15 text-sm text-white placeholder-white/40 focus:outline-none focus:border-[#ffde40] backdrop-blur-md"
                />
              </div>

              <div className="flex-1 overflow-y-auto space-y-2 pr-1">
                {filteredSongs.map((s, idx) => {
                  const active = selectedSong?.id === s.id;
                  const best = scores[scoreKey(s.id, freeDiff, screen === 'autochart')];
                  return (
                    <button
                      key={s.id}
                      onClick={() => {
                        if (active) {
                          audio.playSfx('confirm');
                          launchSong({
                            song: s,
                            difficulty: freeDiff,
                            mods,
                            autoChart: screen === 'autochart',
                            fromScreen: screen,
                          });
                        } else {
                          audio.playSfx('scroll', 0.7);
                          setFreeIdx(idx);
                        }
                      }}
                      className={`w-full flex items-center justify-between px-4 py-3 rounded-2xl border-2 transition-all cursor-pointer backdrop-blur-md ${
                        active
                          ? 'bg-gradient-to-r from-[#1c1046]/95 to-[#2c1460]/90 border-[#ffde40] shadow-[0_0_24px_rgba(164,100,255,0.45)]'
                          : 'bg-[#0e0824]/72 border-white/10 hover:border-white/30'
                      }`}
                    >
                      <div className="flex items-center gap-3.5 text-left">
                        <CharIconCanvas id={s.opponent} size={52} />
                        <div>
                          <div className="font-title text-2xl tracking-wide text-white leading-none">
                            {s.title}
                          </div>
                          <div className="text-xs text-[#c7d2fe] font-bold mt-1">
                            {s.subtitle} • {s.bpm} BPM • vs {charDisplayName(s.opponent)}
                          </div>
                        </div>
                      </div>
                      <div className="text-right">
                        {best ? (
                          <>
                            <div className="font-title text-2xl text-[#ffde40] leading-none">{best.rank}</div>
                            <div className="text-[11px] text-white/75 font-bold">
                              {fmtThousands(best.score)} • {best.accuracy.toFixed(1)}%
                            </div>
                          </>
                        ) : (
                          <span className="text-xs text-white/40 font-bold uppercase">Unplayed</span>
                        )}
                      </div>
                    </button>
                  );
                })}
              </div>
            </div>

            {/* Right Song Detail & Launch Card */}
            {selectedSong && (
              <div className="lg:col-span-5 bg-[#0e0824]/85 border-2 border-white/15 rounded-3xl p-5 backdrop-blur-md flex flex-col justify-between shadow-2xl">
                <div>
                  <div className="flex items-center gap-4 mb-4">
                    <div className="p-2 rounded-2xl bg-black/35 border border-white/10">
                      <CharIconCanvas id={selectedSong.opponent} size={82} />
                    </div>
                    <div>
                      <div className="text-xs font-bold uppercase tracking-widest text-[#36b5ff]">
                        {screen === 'autochart' ? 'Spectral AutoChart Mode' : selectedSong.isBuiltin ? 'Built-in Synth Track' : 'Custom Track'}
                      </div>
                      <h3 className="font-title text-4xl text-white leading-tight drop-shadow">
                        {selectedSong.title}
                      </h3>
                      <div className="text-xs text-white/75 font-bold">
                        {selectedSong.artist} • {selectedSong.bpm} BPM • Stage{' '}
                        {['Sunset Rooftop', 'The Club', 'Punk Back Alley', 'Synthwave Night'][selectedSong.stage] ?? 'Synthwave'}
                      </div>
                    </div>
                  </div>

                  {/* Difficulty Picker */}
                  <div className="mb-4">
                    <div className="text-xs font-bold uppercase text-white/60 mb-1.5">Difficulty (Left / Right)</div>
                    <div className="grid grid-cols-4 gap-1.5">
                      {([0, 1, 2, 3] as Difficulty[]).map((d) => (
                        <button
                          key={d}
                          onClick={() => {
                            audio.playSfx('scroll');
                            setFreeDiff(d);
                          }}
                          className={`py-2 rounded-xl font-title text-base tracking-wide border transition cursor-pointer ${
                            freeDiff === d
                              ? d === 0
                                ? 'bg-[#3ce0a0] text-[#070414] border-white'
                                : d === 1
                                  ? 'bg-[#ffde40] text-[#070414] border-white'
                                  : d === 2
                                    ? 'bg-[#ff4fa3] text-white border-[#ffde40]'
                                    : 'bg-[#ff3054] text-white border-[#ffde40]'
                              : 'bg-black/40 text-white/70 border-white/15 hover:border-white/35'
                          }`}
                        >
                          {DIFFICULTIES[d]}
                        </button>
                      ))}
                    </div>
                  </div>

                  {/* Personal Best Readout */}
                  <div className="p-3.5 rounded-2xl bg-black/35 border border-white/10 mb-4">
                    <div className="flex items-center justify-between text-xs uppercase text-white/55 font-bold mb-1">
                      <span>Personal Best ({DIFFICULTIES[freeDiff]})</span>
                      {selectedBest && (
                        <span className="text-[#3ce0a0]">{selectedBest.clearLabel}</span>
                      )}
                    </div>
                    {selectedBest ? (
                      <div className="flex items-baseline justify-between">
                        <span className="font-title text-3xl text-[#ffde40]">
                          {fmtThousands(selectedBest.score)}
                        </span>
                        <span className="text-sm font-bold text-white/90">
                          {selectedBest.accuracy.toFixed(2)}% • {selectedBest.misses} miss • Rank{' '}
                          <span className="text-[#ffde40]">{selectedBest.rank}</span>
                        </span>
                      </div>
                    ) : (
                      <div className="text-sm text-white/50 font-bold">No record yet — set the first score!</div>
                    )}
                  </div>

                  {/* Active Modifier Summary */}
                  <div className="flex flex-wrap gap-1.5 text-[11px] font-bold mb-4">
                    <span className="px-2.5 py-1 rounded-lg bg-white/10 text-white/85">
                      Rate {mods.playbackRate.toFixed(2)}x
                    </span>
                    <span className="px-2.5 py-1 rounded-lg bg-white/10 text-white/85">
                      Scroll {mods.scrollType === 'constant' ? `Const ${mods.scrollValue.toFixed(1)}` : `${mods.scrollValue.toFixed(2)}x`}
                    </span>
                    {mods.botplay && <span className="px-2.5 py-1 rounded-lg bg-[#36b5ff]/25 text-[#7dd3fc]">BOTPLAY</span>}
                    {mods.practice && <span className="px-2.5 py-1 rounded-lg bg-[#3ce0a0]/25 text-[#86efac]">PRACTICE</span>}
                    {mods.mirror && <span className="px-2.5 py-1 rounded-lg bg-[#ff4fa3]/25 text-[#f9a8d4]">OPPONENT SIDE</span>}
                  </div>
                </div>

                <div className="space-y-2">
                  <button
                    onClick={() => {
                      audio.playSfx('confirm');
                      launchSong({
                        song: selectedSong,
                        difficulty: freeDiff,
                        mods,
                        autoChart: screen === 'autochart',
                        fromScreen: screen,
                      });
                    }}
                    className="w-full py-3.5 rounded-2xl font-title text-3xl tracking-wider bg-gradient-to-r from-[#ff4fa3] via-[#a464ff] to-[#36b5ff] text-white border-2 border-[#ffde40] hover:scale-[1.02] transition cursor-pointer shadow-[0_0_28px_rgba(164,100,255,0.5)] flex items-center justify-center gap-2"
                  >
                    {screen === 'autochart' ? <AudioWaveform className="w-6 h-6" /> : <Play className="w-6 h-6" />}
                    {screen === 'autochart' ? 'AUTOCHART & PLAY' : 'PLAY TRACK (ENTER)'}
                  </button>
                  <button
                    onClick={() => {
                      audio.playSfx('confirm');
                      openEditorForSong(selectedSong, freeDiff, screen);
                    }}
                    className="w-full py-2 rounded-xl font-bold text-xs uppercase tracking-wider bg-white/10 hover:bg-white/20 text-white/90 border border-white/15 cursor-pointer"
                  >
                    Open in Chart Editor (Press 7)
                  </button>
                </div>
              </div>
            )}
          </div>

          {/* Modifiers Modal Overlay */}
          {modsOpen && (
            <div className="absolute inset-0 bg-black/70 backdrop-blur-sm z-40 flex items-center justify-center p-4">
              <div className="w-full max-w-md bg-[#12092c] border-2 border-[#ffde40]/70 rounded-3xl p-6 shadow-2xl space-y-4">
                <div className="flex items-center justify-between">
                  <h3 className="font-title text-3xl text-[#ffde40]">GAMEPLAY MODIFIERS</h3>
                  <button
                    onClick={() => setModsOpen(false)}
                    className="px-3 py-1 rounded-lg bg-white/10 hover:bg-white/20 text-xs font-bold cursor-pointer"
                  >
                    CLOSE (TAB)
                  </button>
                </div>

                <div className="space-y-3 text-sm">
                  <label className="block">
                    <div className="flex justify-between font-bold mb-1">
                      <span>Playback Rate</span>
                      <span className="text-[#36b5ff]">{mods.playbackRate.toFixed(2)}x</span>
                    </div>
                    <input
                      type="range"
                      min={0.5}
                      max={2.0}
                      step={0.05}
                      value={mods.playbackRate}
                      onChange={(e) => setMods((m) => ({ ...m, playbackRate: Number(e.target.value) }))}
                      className="w-full accent-[#ff4fa3]"
                    />
                  </label>

                  <div className="flex items-center justify-between py-1">
                    <span className="font-bold">Scroll Speed Type</span>
                    <button
                      onClick={() =>
                        setMods((m) => ({
                          ...m,
                          scrollType: m.scrollType === 'multiplicative' ? 'constant' : 'multiplicative',
                          scrollValue: m.scrollType === 'multiplicative' ? 2.8 : 1.0,
                        }))
                      }
                      className="px-3 py-1 rounded-lg bg-white/10 hover:bg-white/20 font-bold text-xs uppercase cursor-pointer"
                    >
                      {mods.scrollType}
                    </button>
                  </div>

                  <label className="block">
                    <div className="flex justify-between font-bold mb-1">
                      <span>{mods.scrollType === 'constant' ? 'Constant Scroll Speed' : 'Scroll Speed Multiplier'}</span>
                      <span className="text-[#ffde40]">{mods.scrollValue.toFixed(2)}</span>
                    </div>
                    <input
                      type="range"
                      min={mods.scrollType === 'constant' ? 1.0 : 0.5}
                      max={mods.scrollType === 'constant' ? 5.0 : 2.5}
                      step={0.1}
                      value={mods.scrollValue}
                      onChange={(e) => setMods((m) => ({ ...m, scrollValue: Number(e.target.value) }))}
                      className="w-full accent-[#ffde40]"
                    />
                  </label>

                  {(
                    [
                      ['botplay', 'Botplay (Auto-Hit, no score saved)'],
                      ['practice', 'Practice Mode (Cannot die, no score saved)'],
                      ['mirror', 'Play as Opponent (Swap sides)'],
                    ] as const
                  ).map(([key, label]) => (
                    <label key={key} className="flex items-center justify-between py-1.5 cursor-pointer">
                      <span className="font-bold">{label}</span>
                      <input
                        type="checkbox"
                        checked={mods[key]}
                        onChange={(e) => setMods((m) => ({ ...m, [key]: e.target.checked }))}
                        className="w-5 h-5 accent-[#a464ff] cursor-pointer"
                      />
                    </label>
                  ))}
                </div>

                <div className="flex gap-2 pt-2">
                  <button
                    onClick={() => setMods(defaultModifiers())}
                    className="flex-1 py-2 rounded-xl bg-white/10 hover:bg-white/20 text-xs font-bold cursor-pointer"
                  >
                    Reset Modifiers
                  </button>
                  <button
                    onClick={() => setModsOpen(false)}
                    className="flex-1 py-2 rounded-xl bg-[#ffde40] text-[#070414] text-xs font-black cursor-pointer"
                  >
                    Done
                  </button>
                </div>
              </div>
            </div>
          )}
        </div>
      )}

      {/* ==================== RESULTS SCREEN ==================== */}
      {screen === 'results' && lastResult && (
        <div className="relative z-10 w-full h-full flex items-center justify-center p-5 sm:p-8 overflow-y-auto">
          <div className="w-full max-w-4xl bg-[#0e0726]/88 border-2 border-[#a464ff]/65 rounded-3xl p-6 sm:p-8 backdrop-blur-md shadow-2xl">
            <div className="flex flex-wrap items-center justify-between gap-4 border-b border-white/10 pb-5 mb-5">
              <div>
                <div className="flex items-center gap-2 mb-1">
                  <span className="px-2.5 py-0.5 rounded-full bg-[#36b5ff]/25 text-[#7dd3fc] text-xs font-bold uppercase">
                    {DIFFICULTIES[lastResult.difficulty]}
                  </span>
                  <span className="px-2.5 py-0.5 rounded-full bg-[#3ce0a0]/25 text-[#86efac] text-xs font-bold uppercase">
                    {lastResult.clearLabel}
                  </span>
                  {lastResult.isNewBest && (
                    <span className="px-2.5 py-0.5 rounded-full bg-[#ffde40] text-[#070414] text-xs font-black uppercase flex items-center gap-1">
                      <Sparkles className="w-3 h-3" /> NEW BEST!
                    </span>
                  )}
                  {lastResult.botplay && (
                    <span className="px-2.5 py-0.5 rounded-full bg-white/15 text-white text-xs font-bold">BOTPLAY</span>
                  )}
                </div>
                <h2 className="font-title text-4xl sm:text-5xl text-white tracking-wide leading-none">
                  {lastResult.song.title}
                </h2>
                <p className="text-sm text-[#c7d2fe] font-bold mt-1">
                  Score: <span className="text-[#ffde40]">{fmtThousands(lastResult.score)}</span> • Accuracy:{' '}
                  <span className="text-white">{lastResult.accuracy.toFixed(2)}%</span> • Max Combo:{' '}
                  <span className="text-[#36b5ff]">{lastResult.maxCombo}x</span>
                </p>
              </div>

              <div className="text-center px-6 py-2 rounded-2xl bg-black/40 border-2 border-[#ffde40]">
                <div className="text-[10px] font-bold uppercase tracking-widest text-white/60">RANK</div>
                <div className="font-title text-7xl text-[#ffde40] leading-none drop-shadow-[0_4px_0_#1a082c]">
                  {lastResult.rank}
                </div>
              </div>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 gap-6 mb-6">
              {/* Judgement Breakdown */}
              <div className="space-y-2">
                {RATINGS.map((r, i) => (
                  <div
                    key={r.name}
                    className="flex items-center justify-between px-4 py-2 rounded-xl bg-white/[0.06] border border-white/10"
                  >
                    <span
                      className="font-title text-2xl tracking-wide"
                      style={{ color: `rgb(${r.color.r},${r.color.g},${r.color.b})` }}
                    >
                      {r.name}
                    </span>
                    <span className="font-bold text-xl text-white">{lastResult.counts[i]}</span>
                  </div>
                ))}
                <div className="flex items-center justify-between px-4 py-2 rounded-xl bg-[#ff3054]/15 border border-[#ff3054]/35">
                  <span className="font-title text-2xl tracking-wide text-[#ff4d6d]">MISSES</span>
                  <span className="font-bold text-xl text-[#fda4af]">{lastResult.misses}</span>
                </div>
              </div>

              {/* Timing Scatter Plot */}
              <div className="flex flex-col justify-between">
                <div>
                  <div className="flex items-center justify-between text-xs font-bold uppercase text-white/70 mb-2">
                    <span>Hit Timing Distribution</span>
                    <span>
                      Mean: {lastResult.meanMs >= 0 ? '+' : ''}
                      {lastResult.meanMs.toFixed(1)}ms • SD: {lastResult.sdMs.toFixed(1)}ms
                    </span>
                  </div>
                  <HitErrorGraph result={lastResult} />
                </div>

                <div className="mt-4 flex gap-3">
                  <button
                    onClick={() =>
                      launchSong({
                        song: lastResult.song,
                        difficulty: lastResult.difficulty,
                        mods: lastResult.mods,
                        autoChart: lastResult.autoChart,
                        storyQueue: lastResult.storyQueue,
                        storyIndex: lastResult.storyIndex,
                        storyAccumScore: (lastResult.storyAccumScore ?? 0) - lastResult.score,
                        fromScreen: lastResult.fromScreen,
                      })
                    }
                    className="flex-1 py-3 rounded-2xl bg-white/10 hover:bg-white/20 border border-white/20 font-title text-2xl tracking-wide cursor-pointer"
                  >
                    RETRY
                  </button>
                  <button
                    onClick={handleResultsContinue}
                    className="flex-[1.5] py-3 rounded-2xl bg-gradient-to-r from-[#ff4fa3] via-[#a464ff] to-[#36b5ff] border-2 border-[#ffde40] font-title text-2xl tracking-wide cursor-pointer shadow-[0_0_24px_rgba(164,100,255,0.5)]"
                  >
                    {lastResult.storyQueue &&
                    lastResult.storyIndex !== undefined &&
                    lastResult.storyIndex + 1 < lastResult.storyQueue.length
                      ? 'NEXT STORY SONG (ENTER)'
                      : 'CONTINUE (ENTER)'}
                  </button>
                </div>
              </div>
            </div>
          </div>
        </div>
      )}

      {/* ==================== OPTIONS SCREEN ==================== */}
      {screen === 'options' && (
        <div className="relative z-10 w-full h-full flex flex-col p-5 sm:p-8 max-w-4xl mx-auto">
          <div className="flex items-center justify-between gap-4 mb-5 pr-20">
            <div className="flex items-center gap-3">
              <button
                onClick={() => {
                  audio.playSfx('cancel');
                  setScreen(optionsReturnTo);
                }}
                className="flex items-center gap-1.5 px-3.5 py-2 rounded-xl bg-[#120a2c]/80 border border-white/15 hover:border-[#ffde40] text-sm font-bold cursor-pointer"
              >
                <ArrowLeft className="w-4 h-4" /> BACK
              </button>
              <h2 className="font-title text-4xl text-[#ffde40] drop-shadow-[0_3px_0_#140728]">OPTIONS</h2>
            </div>

            <button
              onClick={() => {
                setSettings(defaultSettings());
                audio.playSfx('confirm');
                showToast('Settings reset to defaults');
              }}
              className="px-3.5 py-1.5 rounded-xl bg-white/10 hover:bg-white/20 text-xs font-bold cursor-pointer"
            >
              Reset Defaults
            </button>
          </div>

          {/* Category Tabs */}
          <div className="flex flex-wrap gap-2 mb-4">
            {(
              [
                ['gameplay', 'Gameplay'],
                ['visuals', 'Visuals & HUD'],
                ['audio', 'Audio'],
                ['keys', 'Key Bindings'],
              ] as const
            ).map(([id, label]) => (
              <button
                key={id}
                onClick={() => {
                  audio.playSfx('scroll');
                  setOptionsTab(id);
                }}
                className={`px-5 py-2 rounded-xl font-title text-xl tracking-wide border-2 transition cursor-pointer ${
                  optionsTab === id
                    ? 'bg-[#ffde40] text-[#070414] border-white'
                    : 'bg-[#100828]/80 text-white/80 border-white/15 hover:border-white/35'
                }`}
              >
                {label}
              </button>
            ))}
          </div>

          <div className="flex-1 bg-[#0e0824]/88 border-2 border-white/15 rounded-3xl p-6 backdrop-blur-md overflow-y-auto space-y-3">
            {optionsTab === 'gameplay' && (
              <>
                {(
                  [
                    ['downscroll', 'Downscroll', 'Notes fall downward instead of rising upward'],
                    ['middlescroll', 'Middlescroll', 'Center your 4 note lanes and dim opponent lanes'],
                    ['ghostTapping', 'Ghost Tapping', 'Pressing empty lanes will not penalize your combo'],
                  ] as const
                ).map(([key, title, desc]) => (
                  <label
                    key={key}
                    className="flex items-center justify-between p-3.5 rounded-2xl bg-white/[0.05] border border-white/10 cursor-pointer"
                  >
                    <div>
                      <div className="font-bold text-lg text-white">{title}</div>
                      <div className="text-xs text-white/60">{desc}</div>
                    </div>
                    <input
                      type="checkbox"
                      checked={settings[key]}
                      onChange={(e) => {
                        audio.playSfx('scroll');
                        setSettings((s) => ({ ...s, [key]: e.target.checked }));
                      }}
                      className="w-6 h-6 accent-[#3ce0a0] cursor-pointer"
                    />
                  </label>
                ))}

                <div className="p-3.5 rounded-2xl bg-white/[0.05] border border-white/10">
                  <div className="flex justify-between font-bold mb-1">
                    <span>Audio Offset Calibration</span>
                    <span className="text-[#ffde40]">
                      {settings.audioOffsetMs >= 0 ? '+' : ''}
                      {settings.audioOffsetMs} ms
                    </span>
                  </div>
                  <input
                    type="range"
                    min={-150}
                    max={150}
                    step={5}
                    value={settings.audioOffsetMs}
                    onChange={(e) => setSettings((s) => ({ ...s, audioOffsetMs: Number(e.target.value) }))}
                    className="w-full accent-[#ffde40]"
                  />
                </div>
              </>
            )}

            {optionsTab === 'visuals' &&
              (
                [
                  ['showVisualizer', 'Audio Visualizer', 'Modern tinted-glass 32-bar FFT visualizer in gameplay'],
                  ['noteSplashes', 'Note Splashes', 'Glowing burst particles on SICK! hits'],
                  ['cameraZooms', 'Camera Beat Zooms', 'Pulse stage and HUD zoom every 4 beats'],
                  ['flashingLights', 'Flashing Lights', 'Enable strobe lightning & neon flicker'],
                  ['lowQuality', 'Low Quality Stage', 'Reduce background dust motes & club lasers'],
                  ['showTimeBar', 'Song Time Bar', 'Show remaining time progress bar'],
                  ['showFps', 'FPS Counter', 'Show live frame rate in top-left'],
                  ['showWatermark', 'Engine Watermark', 'Show song opponent & version label'],
                  ['hideHud', 'Hide HUD', 'Minimalist mode hiding score, health bar, and time bar'],
                ] as const
              ).map(([key, title, desc]) => (
                <label
                  key={key}
                  className="flex items-center justify-between p-3 rounded-2xl bg-white/[0.05] border border-white/10 cursor-pointer"
                >
                  <div>
                    <div className="font-bold text-base text-white">{title}</div>
                    <div className="text-xs text-white/60">{desc}</div>
                  </div>
                  <input
                    type="checkbox"
                    checked={settings[key]}
                    onChange={(e) => {
                      audio.playSfx('scroll');
                      setSettings((s) => ({ ...s, [key]: e.target.checked }));
                    }}
                    className="w-5 h-5 accent-[#36b5ff] cursor-pointer"
                  />
                </label>
              ))}

            {optionsTab === 'audio' &&
              (
                [
                  ['masterVol', 'Master Volume'],
                  ['musicVol', 'Music Volume'],
                  ['sfxVol', 'SFX Volume'],
                  ['hitSoundVol', 'Note Hit Sound Volume'],
                ] as const
              ).map(([key, label]) => (
                <div key={key} className="p-4 rounded-2xl bg-white/[0.05] border border-white/10">
                  <div className="flex justify-between font-bold mb-1.5">
                    <span>{label}</span>
                    <span className="text-[#36b5ff]">{Math.round(settings[key] * 100)}%</span>
                  </div>
                  <input
                    type="range"
                    min={0}
                    max={1}
                    step={0.05}
                    value={settings[key]}
                    onChange={(e) => setSettings((s) => ({ ...s, [key]: Number(e.target.value) }))}
                    className="w-full accent-[#ff4fa3]"
                  />
                </div>
              ))}

            {optionsTab === 'keys' && (
              <div className="space-y-3">
                <p className="text-xs text-white/70">
                  Click any lane below and press a new key to rebind. Arrow keys also work automatically as alternate bindings.
                </p>
                <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
                  {(['LEFT', 'DOWN', 'UP', 'RIGHT'] as const).map((laneName, idx) => (
                    <button
                      key={laneName}
                      onClick={() => {
                        audio.playSfx('scroll');
                        setRebindingLane(idx);
                      }}
                      className={`p-4 rounded-2xl border-2 text-center cursor-pointer transition ${
                        rebindingLane === idx
                          ? 'bg-[#ff4fa3]/30 border-[#ffde40] animate-pulse'
                          : 'bg-black/40 border-white/15 hover:border-[#36b5ff]'
                      }`}
                    >
                      <div className="text-xs font-bold text-white/60 mb-1">{laneName} LANE</div>
                      <div className="font-title text-3xl text-[#ffde40]">
                        {rebindingLane === idx ? 'PRESS KEY...' : formatKeyCode(settings.keys4[idx])}
                      </div>
                      <div className="text-[11px] text-white/45 mt-1">
                        Alt: {formatKeyCode(settings.altKeys4[idx])}
                      </div>
                    </button>
                  ))}
                </div>
              </div>
            )}
          </div>
        </div>
      )}

      {/* ==================== CREDITS SCREEN ==================== */}
      {screen === 'credits' && (
        <div className="relative z-10 w-full h-full flex items-center justify-center p-6">
          <div className="w-full max-w-2xl bg-[#0e0824]/88 border-2 border-[#a464ff]/60 rounded-3xl p-8 backdrop-blur-md shadow-2xl space-y-5">
            <div className="flex items-center justify-between">
              <h2 className="font-title text-5xl text-[#ffde40] drop-shadow-[0_3px_0_#140728]">CREDITS</h2>
              <button
                onClick={() => {
                  audio.playSfx('cancel');
                  setScreen('main');
                }}
                className="flex items-center gap-1.5 px-3.5 py-2 rounded-xl bg-white/10 hover:bg-white/20 text-sm font-bold cursor-pointer"
              >
                <ArrowLeft className="w-4 h-4" /> BACK
              </button>
            </div>

            <div className="space-y-3 text-sm">
              <div className="p-4 rounded-2xl bg-white/[0.06] border border-white/10">
                <div className="font-title text-2xl text-[#ff4fa3]">ORIGINAL GAME &amp; ENGINE</div>
                <div className="text-white/90 font-bold">
                  God&apos;s Hand of Principality and Demise — created by deatuamae67-crypto
                </div>
                <div className="text-xs text-white/60 mt-1">
                  Original C++20 / raylib rhythm game featuring procedural synth composition, Frutiger Aero cast, AutoChart, and Psych-compatible Chart Editor.
                </div>
              </div>

              <div className="p-4 rounded-2xl bg-white/[0.06] border border-white/10">
                <div className="font-title text-2xl text-[#36b5ff]">WEB ENGINE PORT</div>
                <div className="text-white/90 font-bold">
                  HTML5 Canvas 2D + Web Audio API Procedural Synthesizer + Radix-2 FFT AutoChart
                </div>
                <div className="text-xs text-white/60 mt-1">
                  Runs natively in the browser with sample-accurate multi-stem vocals, real-time 32-bar FFT visualizer, and full keyboard/touch support.
                </div>
              </div>

              <div className="p-4 rounded-2xl bg-white/[0.06] border border-white/10">
                <div className="font-title text-2xl text-[#3ce0a0]">TYPOGRAPHY</div>
                <div className="text-white/85 font-bold">
                  Luckiest Guy (Apache 2.0) &amp; Rajdhani Bold (SIL OFL)
                </div>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default App;
