// Core types, constants, and data structures for God's Hand of Principality and Demise

export const kLeft = 0;
export const kDown = 1;
export const kUp = 2;
export const kRight = 3;
export const kCenter = 4;

export interface RGBColor {
  r: number;
  g: number;
  b: number;
  a?: number;
}

export type CharId = 'herald' | 'principality' | 'hand' | 'demise' | 'shade';

export type Difficulty = 0 | 1 | 2 | 3; // 0 Easy, 1 Normal, 2 Hard, 3 Infernal

export const DIFFICULTIES: readonly string[] = ['Easy', 'Normal', 'Hard', 'Infernal'] as const;

export interface RatingInfo {
  name: string;
  windowMs: number;
  score: number;
  weight: number;
  healthDelta: number;
  splash: boolean;
  color: RGBColor;
}

export const RATINGS: readonly RatingInfo[] = [
  { name: 'SICK!', windowMs: 45, score: 350, weight: 1.0, healthDelta: 0.023, splash: true, color: { r: 255, g: 242, b: 140, a: 255 } },
  { name: 'GOOD', windowMs: 90, score: 200, weight: 0.75, healthDelta: 0.014, splash: false, color: { r: 120, g: 255, b: 170, a: 255 } },
  { name: 'BAD', windowMs: 135, score: 100, weight: 0.4, healthDelta: -0.02, splash: false, color: { r: 255, g: 185, b: 90, a: 255 } },
  { name: 'WACK', windowMs: 166, score: 50, weight: 0.1, healthDelta: -0.05, splash: false, color: { r: 255, g: 95, b: 115, a: 255 } },
] as const;

export const MISS_WINDOW_MS = 166;
export const MISS_PENALTY_SCORE = 10;
export const MISS_HEALTH_DELTA = 0.0475;
export const HOLD_HEALTH_PER_SEC = 0.06;

export interface Note {
  timeMs: number;
  dir: number; // 0..3
  side: 0 | 1; // 0 opponent, 1 player
  lengthMs: number;
}

export interface TempoPoint {
  timeMs: number;
  bpm: number;
}

export interface SectionInfo {
  startMs: number;
  lengthSteps: number;
  mustHitSection: boolean;
  bpm?: number;
}

export interface Chart {
  title: string;
  bpm: number;
  speed: number;
  lanes: number;
  notes: Note[];
  tempoChanges: TempoPoint[];
  sections: SectionInfo[];
  opponent: CharId;
  player: CharId;
  stage: number;
  accentHex: number;
  difficulty: Difficulty;
}

export interface SongDef {
  id: string;
  title: string;
  subtitle: string;
  artist: string;
  bpm: number;
  bars: number;
  stage: number;
  opponent: CharId;
  accentHex: number;
  seed: number;
  speed: number;
  keyRoot: number; // MIDI note
  minor: boolean;
  genre: 'rooftop' | 'club' | 'punk' | 'vigil' | 'menu';
  week: number;
}

export interface SongEntry {
  id: string;
  title: string;
  subtitle: string;
  artist: string;
  bpm: number;
  stage: number;
  opponent: CharId;
  accentHex: number;
  isBuiltin: boolean;
  isCustomAudio?: boolean;
  customAudioBuffer?: AudioBuffer;
  customChart?: Chart;
  difficulties: Difficulty[];
}

export type ScrollType = 'multiplicative' | 'constant';

export interface Modifiers {
  playbackRate: number; // 0.5 .. 2.0
  botplay: boolean;
  practice: boolean;
  mirror: boolean; // Play as Opponent
  scrollType: ScrollType;
  scrollValue: number; // multiplier (e.g. 1.0) or constant speed (e.g. 2.8)
}

export interface Settings {
  downscroll: boolean;
  middlescroll: boolean;
  ghostTapping: boolean;
  noteSplashes: boolean;
  cameraZooms: boolean;
  flashingLights: boolean;
  lowQuality: boolean;
  showVisualizer: boolean;
  showTimeBar: boolean;
  showFps: boolean;
  showWatermark: boolean;
  hideHud: boolean;
  hitSoundVol: number; // 0..1
  masterVol: number; // 0..1
  musicVol: number; // 0..1
  sfxVol: number; // 0..1
  muted: boolean;
  audioOffsetMs: number; // -200 .. +200
  keys4: [string, string, string, string];
  altKeys4: [string, string, string, string];
}

export interface ScoreEntry {
  score: number;
  accuracy: number;
  misses: number;
  rank: string;
  clearLabel: string;
}

export type ScoreMap = Record<string, ScoreEntry>;

export interface PlayRequest {
  song: SongEntry;
  difficulty: Difficulty;
  mods: Modifiers;
  autoChart: boolean;
  chartOverride?: Chart;
  storyQueue?: SongEntry[];
  storyIndex?: number;
  storyAccumScore?: number;
  fromScreen: ScreenId;
}

export interface HitErrorSample {
  timeMs: number;
  deltaMs: number; // negative = early, positive = late
  rating: number; // 0..3, -1 for miss
}

export interface ResultData {
  song: SongEntry;
  difficulty: Difficulty;
  score: number;
  accuracy: number;
  rank: string;
  clearLabel: string;
  maxCombo: number;
  misses: number;
  counts: [number, number, number, number];
  totalNotes: number;
  hitErrors: HitErrorSample[];
  meanMs: number;
  sdMs: number;
  isNewBest: boolean;
  botplay: boolean;
  practice: boolean;
  autoChart: boolean;
  storyQueue?: SongEntry[];
  storyIndex?: number;
  storyAccumScore?: number;
  fromScreen: ScreenId;
  mods: Modifiers;
}

export type ScreenId =
  | 'title'
  | 'main'
  | 'story'
  | 'freeplay'
  | 'autochart'
  | 'play'
  | 'editor'
  | 'options'
  | 'credits'
  | 'results';
