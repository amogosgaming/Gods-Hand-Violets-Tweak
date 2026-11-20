// Procedural song composer, built-in soundtrack, difficulty chart builder, and Psych Engine JSON parser/exporter
// Ported from src/composer.cpp & src/chart.cpp
import { Chart, CharId, Difficulty, Note, SectionInfo, SongDef, SongEntry } from './types';
import {
  addBass,
  addBell,
  addClap,
  addCrash,
  addHat,
  addKick,
  addPadNote,
  addPluck,
  addSnare,
  addVoiceNote,
  applyReverb,
  makeStereo,
  normalizeStereo,
  Rng,
  SR,
  StereoBuf,
  voiceProfileFor,
} from './synth';

export const BUILTIN_SONGS: readonly SongDef[] = [
  {
    id: 'vigil',
    title: 'Vigil',
    subtitle: 'Rooftop Warm-Up',
    artist: "God's Hand Synth",
    bpm: 112,
    bars: 28,
    stage: 0,
    opponent: 'principality',
    accentHex: 0xffa654,
    seed: 101,
    speed: 2.3,
    keyRoot: 60, // C
    minor: false,
    genre: 'vigil',
    week: 0,
  },
  {
    id: 'principality',
    title: 'Principality',
    subtitle: 'Week 1 — Stage 1',
    artist: "God's Hand Synth",
    bpm: 128,
    bars: 40,
    stage: 0,
    opponent: 'principality',
    accentHex: 0xa464ff,
    seed: 202,
    speed: 2.6,
    keyRoot: 57, // A minor
    minor: true,
    genre: 'rooftop',
    week: 1,
  },
  {
    id: 'godshand',
    title: 'Godshand',
    subtitle: 'Week 1 — Stage 2',
    artist: "God's Hand Synth",
    bpm: 146,
    bars: 44,
    stage: 1,
    opponent: 'hand',
    accentHex: 0x36b5ff,
    seed: 303,
    speed: 2.9,
    keyRoot: 62, // D minor
    minor: true,
    genre: 'club',
    week: 1,
  },
  {
    id: 'demise',
    title: 'Demise',
    subtitle: 'Week 1 — Finale',
    artist: "God's Hand Synth",
    bpm: 172,
    bars: 48,
    stage: 2,
    opponent: 'demise',
    accentHex: 0xff3054,
    seed: 404,
    speed: 3.3,
    keyRoot: 52, // E minor
    minor: true,
    genre: 'punk',
    week: 1,
  },
] as const;

export const MENU_THEME: SongDef = {
  id: 'rooftop_groove',
  title: 'Rooftop Groove',
  subtitle: 'Menu Theme',
  artist: "God's Hand Synth",
  bpm: 104,
  bars: 16,
  stage: 0,
  opponent: 'herald',
  accentHex: 0x48c6ff,
  seed: 777,
  speed: 2.2,
  keyRoot: 65, // F major
  minor: false,
  genre: 'menu',
  week: -1,
};

export function findBuiltinDef(id: string): SongDef | undefined {
  return BUILTIN_SONGS.find((s) => s.id === id);
}

export function builtinEntries(): SongEntry[] {
  return BUILTIN_SONGS.map((s) => ({
    id: s.id,
    title: s.title,
    subtitle: s.subtitle,
    artist: s.artist,
    bpm: s.bpm,
    stage: s.stage,
    opponent: s.opponent,
    accentHex: s.accentHex,
    isBuiltin: true,
    difficulties: [0, 1, 2, 3],
  }));
}

export interface VocalPhraseNote {
  step: number;
  durSteps: number;
  midi: number;
  dir: number;
  side: 0 | 1;
}

export interface ComposedSong {
  def: SongDef;
  durationSec: number;
  stepSec: number;
  vocalNotes: VocalPhraseNote[];
  sections: SectionInfo[];
  inst?: StereoBuf;
  oppVoice?: StereoBuf;
  playerVoice?: StereoBuf;
}

const composedCache = new Map<string, ComposedSong>();

const MINOR_SCALE = [0, 2, 3, 5, 7, 8, 10, 12];
const MAJOR_SCALE = [0, 2, 4, 5, 7, 9, 11, 12];

const CHORD_PROGS: Record<string, number[][]> = {
  vigil: [
    [0, 2, 4],
    [4, 6, 8],
    [5, 7, 9],
    [3, 5, 7],
  ],
  rooftop: [
    [0, 2, 4],
    [5, 7, 9],
    [2, 4, 6],
    [4, 6, 8],
  ],
  club: [
    [0, 2, 4],
    [5, 7, 9],
    [3, 5, 7],
    [6, 8, 10],
  ],
  punk: [
    [0, 4, 7],
    [5, 9, 12],
    [3, 7, 10],
    [4, 8, 11],
  ],
  menu: [
    [0, 2, 4, 6],
    [5, 7, 9, 11],
    [1, 3, 5, 7],
    [4, 6, 8, 10],
  ],
};

function scaleMidi(root: number, minor: boolean, degree: number): number {
  const sc = minor ? MINOR_SCALE : MAJOR_SCALE;
  const oct = Math.floor(degree / 7);
  const idx = ((degree % 7) + 7) % 7;
  return root + oct * 12 + sc[idx];
}

// Generate deterministic call-and-response vocal phrases and stereo audio stems
export function composeSong(def: SongDef, renderAudio: boolean): ComposedSong {
  const cached = composedCache.get(def.id);
  if (cached && (!renderAudio || cached.inst)) {
    return cached;
  }

  const rng = new Rng(def.seed);
  const beatSec = 60 / def.bpm;
  const stepSec = beatSec / 4; // 16th note
  const barSec = beatSec * 4;
  const leadInSec = def.genre === 'menu' ? 0 : 0.25;
  const totalSec = leadInSec + def.bars * barSec + (def.genre === 'menu' ? 0 : 1.5);
  const totalSamples = Math.ceil(totalSec * SR);

  const vocalNotes: VocalPhraseNote[] = [];
  const sections: SectionInfo[] = [];

  // Plan call-and-response sections (2 bars = 32 steps per turn, switching between opp & player)
  const prog = CHORD_PROGS[def.genre] ?? CHORD_PROGS.rooftop;

  // For each 2-bar pair, opponent sings a catchy motif, then player answers (with variations in finale)
  const numBars = def.bars;
  if (def.genre !== 'menu') {
    for (let bar = 0; bar < numBars; bar++) {
      const pairIdx = Math.floor(bar / 2);
      const isFinale = bar >= numBars - 8;
      // Bars 0-1: intro (no vocals or light), then alternating opp (even pairs) and player (odd pairs)
      let mustHit = pairIdx % 2 === 1;
      if (bar < 2) mustHit = false;
      if (isFinale) mustHit = bar % 2 === 1; // rapid 1-bar trading in finale!
      sections.push({
        startMs: (leadInSec + bar * barSec) * 1000,
        lengthSteps: 16,
        mustHitSection: mustHit,
      });
    }

    // Build musical call-and-response melodies
    const storedMotifs: { relStep: number; dur: number; deg: number; dir: number }[][] = [];
    for (let bar = 2; bar < numBars; bar++) {
      const sec = sections[bar];
      const side: 0 | 1 = sec.mustHitSection ? 1 : 0;
      const isFinale = bar >= numBars - 8;
      const chord = prog[bar % prog.length];
      const density =
        def.genre === 'vigil'
          ? 0.35
          : def.genre === 'rooftop'
            ? 0.48
            : def.genre === 'club'
              ? 0.6
              : 0.72;

      const motifKey = isFinale ? Math.floor((bar - 2) / 2) : Math.floor((bar - 2) / 4);
      const isAnswer =
        (!isFinale && side === 1 && bar - 2 >= 2 && storedMotifs[bar - 2]) ||
        (isFinale && side === 1 && storedMotifs[bar - 1]);

      if (isAnswer) {
        const srcBar = isFinale ? bar - 1 : bar - 2;
        const src = storedMotifs[srcBar] ?? [];
        for (const m of src) {
          const deg = m.deg + (rng.uniform() < 0.2 ? 2 : 0);
          const dir = (m.dir + (rng.uniform() < 0.15 ? 1 : 0)) & 3;
          vocalNotes.push({
            step: bar * 16 + m.relStep,
            durSteps: m.dur,
            midi: scaleMidi(def.keyRoot + 12, def.minor, deg),
            dir,
            side: 1,
          });
        }
      } else {
        const motif: { relStep: number; dur: number; deg: number; dir: number }[] = [];
        let lastDir = rng.int(0, 3);
        let s = 0;
        while (s < 16) {
          const onBeat = s % 4 === 0;
          const onEighth = s % 2 === 0;
          const chance = onBeat ? density + 0.32 : onEighth ? density : density * 0.48;
          if (rng.uniform() < chance) {
            const deg = chord[rng.int(0, chord.length - 1)] + (rng.uniform() < 0.3 ? rng.int(-1, 2) : 0);
            const dur = rng.uniform() < 0.22 && s <= 12 ? rng.int(2, 4) : 1;
            const dir = rng.uniform() < 0.75 ? (lastDir + rng.int(1, 3)) & 3 : lastDir;
            lastDir = dir;
            motif.push({ relStep: s, dur, deg, dir });
            vocalNotes.push({
              step: bar * 16 + s,
              durSteps: dur,
              midi: scaleMidi(def.keyRoot + (side === 1 ? 12 : 7), def.minor, deg),
              dir,
              side,
            });
            s += Math.max(1, dur);
          } else {
            s += 1;
          }
        }
        storedMotifs[bar] = motif;
        void motifKey;
      }
    }
  }

  let inst: StereoBuf | undefined;
  let oppVoice: StereoBuf | undefined;
  let playerVoice: StereoBuf | undefined;

  if (renderAudio) {
    inst = makeStereo(totalSamples);
    oppVoice = makeStereo(totalSamples);
    playerVoice = makeStereo(totalSamples);
    const audioRng = new Rng(def.seed ^ 0x55aa);

    for (let bar = 0; bar < numBars; bar++) {
      const barStart = leadInSec + bar * barSec;
      const chord = prog[bar % prog.length];
      const energy = def.genre === 'menu' ? 0.7 : bar < 2 ? 0.55 : bar >= numBars - 8 ? 1.0 : 0.85;

      // Crash on section changes
      if (bar % 8 === 0 && bar > 0) {
        addCrash(inst, barStart, 0.26 * energy, audioRng);
      }

      // Warm synth pad chords
      for (let ci = 0; ci < chord.length; ci++) {
        const midi = scaleMidi(def.keyRoot, def.minor, chord[ci]);
        addPadNote(inst, barStart, barSec * 0.96, midi, 0.11 * energy, (ci - 1) * 0.35);
      }

      // Drums, Bass & Arps per 16th step
      for (let st = 0; st < 16; st++) {
        const t = barStart + st * stepSec;
        const rootMidi = scaleMidi(def.keyRoot - 24, def.minor, chord[0]);

        // Drum patterns tailored to each track's genre
        if (def.genre === 'club') {
          if (st % 4 === 0) addKick(inst, t, 0.85 * energy, 165, 42);
          if (st === 4 || st === 12) addClap(inst, t, 0.55 * energy, audioRng);
          if (st % 2 === 1) addHat(inst, t, st === 6 || st === 14, 0.26 * energy, audioRng, 0.22);
        } else if (def.genre === 'punk') {
          if (st === 0 || st === 6 || st === 8 || st === 14) addKick(inst, t, 0.9 * energy, 175, 46);
          if (st === 4 || st === 12 || (bar % 4 === 3 && st === 15)) addSnare(inst, t, 0.72 * energy, audioRng, 1.25);
          if (st % 2 === 0) addHat(inst, t, st % 8 === 4, 0.24 * energy, audioRng, -0.18);
        } else if (def.genre === 'menu') {
          if (st === 0 || st === 7 || st === 10) addKick(inst, t, 0.68, 145, 46);
          if (st === 4 || st === 12) addSnare(inst, t, 0.45, audioRng, 0.9);
          if (st % 2 === 0) addHat(inst, t, st === 14, 0.16, audioRng, 0.25);
        } else {
          // Rooftop / Vigil groove
          if (st === 0 || st === 6 || st === 10) addKick(inst, t, 0.82 * energy, 155, 44);
          if (st === 4 || st === 12) {
            addSnare(inst, t, 0.62 * energy, audioRng, 1.0);
            if (def.genre === 'rooftop') addClap(inst, t, 0.35 * energy, audioRng);
          }
          if (st % 2 === 0) addHat(inst, t, st === 14, 0.2 * energy, audioRng, 0.15);
        }

        // Bassline
        const bassHit =
          def.genre === 'punk'
            ? st % 2 === 0
            : def.genre === 'club'
              ? st % 4 === 2 || st === 0
              : st === 0 || st === 3 || st === 6 || st === 10 || st === 12;
        if (bassHit) {
          const octJump = st === 10 || st === 14 ? 12 : 0;
          addBass(
            inst,
            t,
            stepSec * (def.genre === 'punk' ? 1.4 : 1.9),
            rootMidi + octJump,
            0.52 * energy,
            def.genre === 'punk' ? 1.85 : 1.2,
          );
        }

        // Arpeggio / Pluck shimmer
        const arpStep = def.genre === 'menu' ? st % 2 === 0 : st % 2 === 0 || (def.genre === 'club' && st % 4 === 3);
        if (arpStep && (bar >= 2 || def.genre === 'menu')) {
          const noteDeg = chord[(st >> 1) % chord.length] + (st >= 8 ? 7 : 0);
          const arpMidi = scaleMidi(def.keyRoot + 12, def.minor, noteDeg);
          if (def.genre === 'menu' && st % 4 === 0) {
            addBell(inst, t, stepSec * 3, arpMidi, 0.16, -0.3);
          } else {
            addPluck(
              inst,
              t,
              stepSec * 1.4,
              arpMidi,
              (def.genre === 'menu' ? 0.16 : 0.2) * energy,
              st % 4 === 0 ? -0.35 : 0.35,
              def.genre === 'punk' ? 'saw' : 'pulse',
            );
          }
        }
      }
    }

    // Synthesize vocals into separate opponent & player stems
    const oppProf = voiceProfileFor(def.opponent, false);
    const plyProf = voiceProfileFor('herald', true);
    for (const vn of vocalNotes) {
      const startSec = leadInSec + vn.step * stepSec;
      const durSec = Math.max(0.09, vn.durSteps * stepSec * 0.92);
      if (vn.side === 0) {
        addVoiceNote(oppVoice, startSec, durSec, vn.midi, oppProf, vn.dir, 0.48);
      } else {
        addVoiceNote(playerVoice, startSec, durSec, vn.midi, plyProf, vn.dir, 0.48);
      }
    }

    applyReverb(inst, 0.12);
    applyReverb(oppVoice, 0.1);
    applyReverb(playerVoice, 0.1);
    normalizeStereo(inst, 0.88);
    normalizeStereo(oppVoice, 0.85);
    normalizeStereo(playerVoice, 0.85);
  }

  const result: ComposedSong = {
    def,
    durationSec: totalSec,
    stepSec,
    vocalNotes,
    sections,
    inst,
    oppVoice,
    playerVoice,
  };
  composedCache.set(def.id, result);
  return result;
}

export function buildBuiltinChart(def: SongDef, comp: ComposedSong, diff: Difficulty, _mirror = false): Chart {
  const leadInMs = def.genre === 'menu' ? 0 : 250;
  const stepMs = comp.stepSec * 1000;
  const rng = new Rng(def.seed + diff * 997);
  const notes: Note[] = [];

  for (let i = 0; i < comp.vocalNotes.length; i++) {
    const vn = comp.vocalNotes[i];
    const timeMs = leadInMs + vn.step * stepMs;
    const holdMs = vn.durSteps >= 3 ? (vn.durSteps - 0.5) * stepMs : 0;

    // Easy trims fast offbeat notes
    if (diff === 0 && vn.step % 2 === 1 && rng.uniform() < 0.65) {
      continue;
    }
    // Normal trims dense 16th-note runs slightly
    if (diff === 1 && vn.step % 2 === 1 && rng.uniform() < 0.25) {
      continue;
    }

    notes.push({
      timeMs,
      dir: vn.dir,
      side: vn.side,
      lengthMs: holdMs,
    });

    // Hard adds occasional jump chords on strong beats
    if (diff >= 2 && vn.step % 8 === 0 && rng.uniform() < (diff === 3 ? 0.55 : 0.25)) {
      notes.push({
        timeMs,
        dir: (vn.dir + 2) & 3,
        side: vn.side,
        lengthMs: 0,
      });
    }

    // Infernal adds rapid stair fills between gaps
    if (diff === 3 && i + 1 < comp.vocalNotes.length) {
      const next = comp.vocalNotes[i + 1];
      if (next.side === vn.side && next.step - vn.step === 2 && rng.uniform() < 0.55) {
        notes.push({
          timeMs: leadInMs + (vn.step + 1) * stepMs,
          dir: (vn.dir + 1) & 3,
          side: vn.side,
          lengthMs: 0,
        });
      }
    }
  }

  notes.sort((a, b) => a.timeMs - b.timeMs || a.dir - b.dir);
  const speedScale = diff === 0 ? 0.82 : diff === 1 ? 0.94 : diff === 2 ? 1.05 : 1.22;

  return {
    title: def.title,
    bpm: def.bpm,
    speed: +(def.speed * speedScale).toFixed(2),
    lanes: 4,
    notes,
    tempoChanges: [{ timeMs: 0, bpm: def.bpm }],
    sections: comp.sections.map((s) => ({ ...s })),
    opponent: def.opponent,
    player: 'herald',
    stage: def.stage,
    accentHex: def.accentHex,
    difficulty: diff,
  };
}

export function chartBeatAt(chart: Chart, ms: number): number {
  const bpm = Math.max(30, chart.bpm || 120);
  return (ms / 1000) * (bpm / 60);
}

export function chartFocusAt(chart: Chart, ms: number): 0 | 1 {
  if (chart.sections.length === 0) return 1;
  let focus: 0 | 1 = chart.sections[0].mustHitSection ? 1 : 0;
  for (const s of chart.sections) {
    if (ms >= s.startMs) {
      focus = s.mustHitSection ? 1 : 0;
    } else {
      break;
    }
  }
  return focus;
}

// Parse Psych Engine / FNF chart JSON
export function parsePsychChart(rawJson: string, fallbackTitle = 'Custom Chart'): Chart {
  const parsed = JSON.parse(rawJson);
  const song = parsed.song && typeof parsed.song === 'object' ? parsed.song : parsed;
  const title = typeof song.song === 'string' && song.song.trim() ? song.song.trim() : fallbackTitle;
  const bpm = Number(song.bpm) > 20 ? Number(song.bpm) : 120;
  const speed = Number(song.speed) > 0.2 ? Number(song.speed) : 2.6;
  const notes: Note[] = [];
  const sections: SectionInfo[] = [];

  const rawSections = Array.isArray(song.notes) ? song.notes : [];
  let curBpm = bpm;
  let curMs = 0;

  for (const sec of rawSections) {
    const mustHit = Boolean(sec.mustHitSection);
    const lengthSteps = Number(sec.lengthInSteps) || (Number(sec.sectionBeats) ? Number(sec.sectionBeats) * 4 : 16);
    if (sec.changeBPM && Number(sec.bpm) > 20) {
      curBpm = Number(sec.bpm);
    }
    sections.push({
      startMs: curMs,
      lengthSteps,
      mustHitSection: mustHit,
      bpm: curBpm,
    });

    const secNotes = Array.isArray(sec.sectionNotes) ? sec.sectionNotes : [];
    for (const sn of secNotes) {
      if (!Array.isArray(sn) || sn.length < 2) continue;
      const timeMs = Number(sn[0]);
      const rawLane = Number(sn[1]);
      const lengthMs = Number(sn[2]) || 0;
      if (!Number.isFinite(timeMs) || !Number.isFinite(rawLane) || rawLane < 0 || rawLane > 7) continue;

      const isSecondGroup = rawLane >= 4;
      const dir = rawLane % 4;
      // Psych Engine convention: in mustHitSection, 0..3 = player (side 1), 4..7 = opponent (side 0);
      // when !mustHitSection, 0..3 = opponent (side 0), 4..7 = player (side 1).
      const side: 0 | 1 = mustHit ? (isSecondGroup ? 0 : 1) : isSecondGroup ? 1 : 0;
      notes.push({ timeMs, dir, side, lengthMs: Math.max(0, lengthMs) });
    }

    const stepMs = (60000 / curBpm) / 4;
    curMs += lengthSteps * stepMs;
  }

  notes.sort((a, b) => a.timeMs - b.timeMs || a.dir - b.dir);

  const oppName = String(song.player2 || '').toLowerCase();
  let opponent: CharId = 'principality';
  if (oppName.includes('hand') || oppName.includes('dj')) opponent = 'hand';
  else if (oppName.includes('demise') || oppName.includes('pico') || oppName.includes('skid')) opponent = 'demise';
  else if (oppName.includes('shade') || oppName.includes('bf')) opponent = 'shade';

  return {
    title,
    bpm,
    speed,
    lanes: 4,
    notes,
    tempoChanges: [{ timeMs: 0, bpm }],
    sections,
    opponent,
    player: 'herald',
    stage: 3,
    accentHex: 0xa464ff,
    difficulty: 2,
  };
}

// Export chart to standard Psych Engine JSON
export function exportPsychChart(chart: Chart): string {
  const stepMs = (60000 / Math.max(30, chart.bpm)) / 4;
  const maxTimeMs = chart.notes.reduce((m, n) => Math.max(m, n.timeMs + n.lengthMs), 0);
  const numSections = Math.max(
    chart.sections.length,
    Math.ceil((maxTimeMs + stepMs * 16) / (stepMs * 16)),
  );

  const outSections = [];
  for (let i = 0; i < numSections; i++) {
    const startMs = i * 16 * stepMs;
    const endMs = (i + 1) * 16 * stepMs;
    const mustHit = chart.sections[i]?.mustHitSection ?? i % 2 === 1;
    const sectionNotes: [number, number, number][] = [];
    for (const n of chart.notes) {
      if (n.timeMs >= startMs - 0.5 && n.timeMs < endMs - 0.5) {
        const lane = mustHit ? (n.side === 1 ? n.dir : n.dir + 4) : n.side === 0 ? n.dir : n.dir + 4;
        sectionNotes.push([Math.round(n.timeMs * 10) / 10, lane, Math.round(n.lengthMs * 10) / 10]);
      }
    }
    outSections.push({
      sectionNotes,
      lengthInSteps: 16,
      mustHitSection: mustHit,
      bpm: chart.bpm,
      changeBPM: false,
      altAnim: false,
    });
  }

  return JSON.stringify(
    {
      song: {
        song: chart.title,
        bpm: chart.bpm,
        speed: chart.speed,
        needsVoices: true,
        player1: 'bf',
        player2: chart.opponent,
        stage: chart.stage === 0 ? 'rooftop' : chart.stage === 1 ? 'club' : chart.stage === 2 ? 'alley' : 'synthwave',
        notes: outSections,
      },
    },
    null,
    2,
  );
}
