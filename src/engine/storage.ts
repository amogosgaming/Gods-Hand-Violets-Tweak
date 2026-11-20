// Persistent localStorage settings, high scores, and formatting helpers
import { Difficulty, Modifiers, ScoreEntry, ScoreMap, Settings } from './types';

const SETTINGS_KEY = 'godshand_settings_v2';
const SCORES_KEY = 'godshand_scores_v2';

export function defaultSettings(): Settings {
  return {
    downscroll: false,
    middlescroll: false,
    ghostTapping: true,
    noteSplashes: true,
    cameraZooms: true,
    flashingLights: true,
    lowQuality: false,
    showVisualizer: true,
    showTimeBar: true,
    showFps: false,
    showWatermark: true,
    hideHud: false,
    hitSoundVol: 0.0,
    masterVol: 0.8,
    musicVol: 0.85,
    sfxVol: 0.8,
    muted: false,
    audioOffsetMs: 0,
    keys4: ['KeyD', 'KeyF', 'KeyJ', 'KeyK'],
    altKeys4: ['ArrowLeft', 'ArrowDown', 'ArrowUp', 'ArrowRight'],
  };
}

export function defaultModifiers(): Modifiers {
  return {
    playbackRate: 1.0,
    botplay: false,
    practice: false,
    mirror: false,
    scrollType: 'multiplicative',
    scrollValue: 1.0,
  };
}

export function loadSettings(): Settings {
  const def = defaultSettings();
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return def;
    const parsed = JSON.parse(raw);
    return {
      ...def,
      ...parsed,
      keys4: Array.isArray(parsed.keys4) && parsed.keys4.length === 4 ? parsed.keys4 : def.keys4,
      altKeys4: Array.isArray(parsed.altKeys4) && parsed.altKeys4.length === 4 ? parsed.altKeys4 : def.altKeys4,
    };
  } catch {
    return def;
  }
}

export function saveSettings(s: Settings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(s));
  } catch {
    // ignore quota errors
  }
}

export function loadScores(): ScoreMap {
  try {
    const raw = localStorage.getItem(SCORES_KEY);
    if (!raw) return {};
    return JSON.parse(raw) as ScoreMap;
  } catch {
    return {};
  }
}

export function saveScores(map: ScoreMap): void {
  try {
    localStorage.setItem(SCORES_KEY, JSON.stringify(map));
  } catch {
    // ignore quota errors
  }
}

export function scoreKey(songId: string, diff: Difficulty, autoChart = false): string {
  return `${songId}:${diff}${autoChart ? ':ac' : ''}`;
}

export function submitScore(
  map: ScoreMap,
  songId: string,
  diff: Difficulty,
  entry: ScoreEntry,
  autoChart = false,
): { map: ScoreMap; isNewBest: boolean } {
  const key = scoreKey(songId, diff, autoChart);
  const prev = map[key];
  const isNewBest = !prev || entry.score > prev.score;
  if (!isNewBest) return { map, isNewBest: false };
  const next = { ...map, [key]: entry };
  saveScores(next);
  return { map: next, isNewBest: true };
}

export function formatKeyCode(code: string): string {
  if (code.startsWith('Key')) return code.slice(3);
  if (code.startsWith('Digit')) return code.slice(5);
  switch (code) {
    case 'ArrowLeft':
      return 'LEFT';
    case 'ArrowDown':
      return 'DOWN';
    case 'ArrowUp':
      return 'UP';
    case 'ArrowRight':
      return 'RIGHT';
    case 'Space':
      return 'SPACE';
    case 'Semicolon':
      return ';';
    case 'Quote':
      return "'";
    case 'Comma':
      return ',';
    case 'Period':
      return '.';
    case 'Slash':
      return '/';
    case 'BracketLeft':
      return '[';
    case 'BracketRight':
      return ']';
    default:
      return code.toUpperCase();
  }
}

export function fmtThousands(n: number): string {
  return Math.round(n).toLocaleString('en-US');
}

export function fmtTime(sec: number): string {
  const s = Math.max(0, Math.floor(sec));
  const m = Math.floor(s / 60);
  const r = s % 60;
  return `${m}:${r < 10 ? '0' : ''}${r}`;
}
