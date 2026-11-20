// Interactive Psych-style 8-lane step grid Chart Editor
// Ported from src/state_editor.cpp
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Chart, Note, SongEntry } from '../engine/types';
import { composeSong, exportPsychChart, findBuiltinDef, parsePsychChart } from '../engine/composer';
import { audio } from '../engine/audio';
import { dirColor, rgba } from '../engine/gfx';
import { Download, Play, Pause, Plus, Sparkles, Trash2, Upload, ArrowLeft } from 'lucide-react';

interface ChartEditorProps {
  initialChart: Chart;
  song: SongEntry;
  onPlaytest: (chart: Chart) => void;
  onExit: () => void;
}

const LANE_LABELS = ['L', 'D', 'U', 'R', 'L', 'D', 'U', 'R'];

export const ChartEditor: React.FC<ChartEditorProps> = ({ initialChart, song, onPlaytest, onExit }) => {
  const [chart, setChart] = useState<Chart>(() => ({
    ...initialChart,
    notes: initialChart.notes.map((n) => ({ ...n })),
    sections: initialChart.sections.map((s) => ({ ...s })),
  }));
  const [sectionIdx, setSectionIdx] = useState(0);
  const [selectedNoteIdx, setSelectedNoteIdx] = useState<number | null>(null);
  const [playing, setPlaying] = useState(false);
  const [curMs, setCurMs] = useState(0);
  const [statusMsg, setStatusMsg] = useState('Click grid to place/remove notes • Q/E sustain • Space play/pause • Enter playtest');
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const rafRef = useRef(0);

  const stepMs = (60000 / Math.max(30, chart.bpm)) / 4;
  const sectionMs = stepMs * 16;
  const totalSections = Math.max(
    16,
    chart.sections.length,
    Math.ceil((chart.notes.reduce((m, n) => Math.max(m, n.timeMs + n.lengthMs), 0) + sectionMs * 2) / sectionMs),
  );

  // Prepare audio stems for scrubbing & playback inside editor
  useEffect(() => {
    let active = true;
    const t = window.setTimeout(() => {
      if (!active) return;
      if (song.isBuiltin) {
        const def = findBuiltinDef(song.id);
        if (def) {
          const comp = composeSong(def, true);
          audio.loadStems(
            audio.stereoToAudioBuffer(comp.inst!),
            audio.stereoToAudioBuffer(comp.oppVoice!),
            audio.stereoToAudioBuffer(comp.playerVoice!),
            `editor:${song.id}`,
          );
        }
      } else if (song.customAudioBuffer) {
        audio.loadStems(song.customAudioBuffer, null, null, `editor:${song.id}`);
      }
    }, 30);
    return () => {
      active = false;
      window.clearTimeout(t);
      audio.stopSong();
    };
  }, [song]);

  // Playback cursor RAF
  useEffect(() => {
    if (!playing) return;
    const tick = () => {
      const ms = audio.getSongTimeSec() * 1000;
      setCurMs(ms);
      const sec = Math.max(0, Math.floor(ms / sectionMs));
      setSectionIdx(sec);
      if (ms >= audio.getDurationSec() * 1000 - 50) {
        audio.pauseSong();
        setPlaying(false);
        return;
      }
      rafRef.current = requestAnimationFrame(tick);
    };
    rafRef.current = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(rafRef.current);
  }, [playing, sectionMs]);

  const togglePlay = () => {
    if (playing) {
      audio.pauseSong();
      setPlaying(false);
    } else {
      const startSec = (sectionIdx * sectionMs) / 1000;
      audio.playSong(startSec, 1.0, false);
      setPlaying(true);
    }
  };

  // Keyboard shortcuts
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLInputElement) return;
      if (e.code === 'Space') {
        e.preventDefault();
        togglePlay();
      } else if (e.code === 'Enter') {
        e.preventDefault();
        audio.stopSong();
        onPlaytest(chart);
      } else if (e.code === 'Escape') {
        e.preventDefault();
        audio.stopSong();
        onExit();
      } else if (e.code === 'ArrowRight' || e.code === 'KeyD') {
        setSectionIdx((s) => Math.min(totalSections + 4, s + 1));
      } else if (e.code === 'ArrowLeft' || e.code === 'KeyA') {
        setSectionIdx((s) => Math.max(0, s - 1));
      } else if (e.code === 'KeyE' && selectedNoteIdx !== null) {
        adjustSustain(selectedNoteIdx, stepMs);
      } else if (e.code === 'KeyQ' && selectedNoteIdx !== null) {
        adjustSustain(selectedNoteIdx, -stepMs);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [playing, sectionIdx, chart, selectedNoteIdx, stepMs, totalSections]);

  const adjustSustain = (idx: number, deltaMs: number) => {
    setChart((prev) => {
      const notes = prev.notes.slice();
      if (!notes[idx]) return prev;
      notes[idx] = { ...notes[idx], lengthMs: Math.max(0, notes[idx].lengthMs + deltaMs) };
      return { ...prev, notes };
    });
  };

  const secStartMs = sectionIdx * sectionMs;
  const secEndMs = secStartMs + sectionMs;

  const sectionNotes = useMemo(() => {
    return chart.notes
      .map((n, idx) => ({ note: n, idx }))
      .filter(({ note }) => note.timeMs >= secStartMs - 10 && note.timeMs < secEndMs - 10);
  }, [chart.notes, secStartMs, secEndMs]);

  const mustHit = chart.sections[sectionIdx]?.mustHitSection ?? sectionIdx % 2 === 1;

  const toggleSectionFocus = () => {
    setChart((prev) => {
      const secs = prev.sections.slice();
      while (secs.length <= sectionIdx) {
        secs.push({
          startMs: secs.length * sectionMs,
          lengthSteps: 16,
          mustHitSection: secs.length % 2 === 1,
        });
      }
      secs[sectionIdx] = { ...secs[sectionIdx], mustHitSection: !secs[sectionIdx].mustHitSection };
      return { ...prev, sections: secs };
    });
    audio.playSfx('scroll');
  };

  const handleCellClick = (step: number, col: number, shiftKey: boolean) => {
    const side: 0 | 1 = col < 4 ? 0 : 1;
    const dir = col % 4;
    const targetMs = secStartMs + step * stepMs;

    const existingIdx = chart.notes.findIndex(
      (n) => n.side === side && n.dir === dir && Math.abs(n.timeMs - targetMs) < stepMs * 0.4,
    );

    if (existingIdx >= 0) {
      if (shiftKey) {
        setSelectedNoteIdx(existingIdx);
        adjustSustain(existingIdx, stepMs);
        return;
      }
      setChart((prev) => ({
        ...prev,
        notes: prev.notes.filter((_, i) => i !== existingIdx),
      }));
      if (selectedNoteIdx === existingIdx) setSelectedNoteIdx(null);
      audio.playSfx('cancel', 0.5);
    } else {
      const newNote: Note = { timeMs: targetMs, dir, side, lengthMs: 0 };
      setChart((prev) => {
        const next = [...prev.notes, newNote].sort((a, b) => a.timeMs - b.timeMs || a.dir - b.dir);
        const newIdx = next.indexOf(newNote);
        setSelectedNoteIdx(newIdx);
        return { ...prev, notes: next };
      });
      audio.playSfx('hitsound', 0.6);
    }
  };

  // EZ Spam / Pattern Generator for current section
  const applyPattern = (kind: 'stairs' | 'zigzag' | 'jacks' | 'chords' | 'random' | 'clear') => {
    setChart((prev) => {
      const kept = prev.notes.filter((n) => n.timeMs < secStartMs - 10 || n.timeMs >= secEndMs - 10);
      if (kind === 'clear') return { ...prev, notes: kept };

      const side: 0 | 1 = mustHit ? 1 : 0;
      const added: Note[] = [];
      for (let st = 0; st < 16; st++) {
        const t = secStartMs + st * stepMs;
        if (kind === 'stairs') {
          added.push({ timeMs: t, dir: st % 4, side, lengthMs: 0 });
        } else if (kind === 'zigzag') {
          const seq = [0, 1, 2, 3, 2, 1];
          added.push({ timeMs: t, dir: seq[st % seq.length], side, lengthMs: 0 });
        } else if (kind === 'jacks') {
          if (st % 2 === 0) added.push({ timeMs: t, dir: Math.floor(st / 4) % 4, side, lengthMs: 0 });
        } else if (kind === 'chords') {
          if (st % 4 === 0) {
            added.push({ timeMs: t, dir: 0, side, lengthMs: 0 });
            added.push({ timeMs: t, dir: 3, side, lengthMs: 0 });
          } else if (st % 4 === 2) {
            added.push({ timeMs: t, dir: 1, side, lengthMs: 0 });
            added.push({ timeMs: t, dir: 2, side, lengthMs: 0 });
          }
        } else if (kind === 'random') {
          if (st % 2 === 0 || Math.random() < 0.45) {
            added.push({ timeMs: t, dir: Math.floor(Math.random() * 4), side, lengthMs: 0 });
          }
        }
      }
      const next = [...kept, ...added].sort((a, b) => a.timeMs - b.timeMs || a.dir - b.dir);
      return { ...prev, notes: next };
    });
    setStatusMsg(`Applied ${kind.toUpperCase()} pattern to Section ${sectionIdx + 1}`);
    audio.playSfx('confirm', 0.7);
  };

  const handleExport = () => {
    const json = exportPsychChart(chart);
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `${chart.title.toLowerCase().replace(/[^a-z0-9]+/g, '_')}.json`;
    a.click();
    URL.revokeObjectURL(url);
    setStatusMsg('Exported Psych Engine .json chart!');
    audio.playSfx('unlock');
  };

  const handleImport = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    try {
      const txt = await file.text();
      const parsed = parsePsychChart(txt, file.name.replace(/\.json$/i, ''));
      setChart(parsed);
      setSectionIdx(0);
      setSelectedNoteIdx(null);
      setStatusMsg(`Imported ${parsed.title} (${parsed.notes.length} notes)`);
      audio.playSfx('unlock');
    } catch {
      setStatusMsg('Invalid Psych Engine JSON file.');
      audio.playSfx('cancel');
    }
    e.target.value = '';
  };

  const playheadInSec = playing && curMs >= secStartMs && curMs <= secEndMs ? (curMs - secStartMs) / sectionMs : -1;

  return (
    <div className="w-full h-full bg-[#090518] text-white flex flex-col overflow-hidden">
      {/* Top Toolbar */}
      <div className="h-14 px-4 bg-[#120a2c] border-b border-white/10 flex items-center justify-between gap-3 shrink-0">
        <div className="flex items-center gap-3">
          <button
            onClick={() => {
              audio.stopSong();
              onExit();
            }}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-white/10 hover:bg-white/20 text-sm cursor-pointer"
          >
            <ArrowLeft className="w-4 h-4" /> Back
          </button>
          <span className="font-title text-2xl text-[#ffde40] tracking-wide">CHART EDITOR</span>
          <span className="text-sm text-[#c7d2fe] hidden sm:inline">
            {chart.title} ({chart.notes.length} notes)
          </span>
        </div>

        <div className="flex items-center gap-2">
          <button
            onClick={togglePlay}
            className="flex items-center gap-1.5 px-3.5 py-1.5 rounded-lg bg-[#36b5ff] text-[#070414] font-bold text-sm hover:brightness-110 cursor-pointer"
          >
            {playing ? <Pause className="w-4 h-4" /> : <Play className="w-4 h-4" />}
            {playing ? 'Pause' : 'Play Section'}
          </button>
          <button
            onClick={() => {
              audio.stopSong();
              onPlaytest(chart);
            }}
            className="flex items-center gap-1.5 px-4 py-1.5 rounded-lg bg-gradient-to-r from-[#ff4fa3] to-[#a464ff] text-white font-bold text-sm hover:brightness-110 cursor-pointer"
          >
            <Play className="w-4 h-4" /> Playtest (Enter)
          </button>
          <button
            onClick={handleExport}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-white/10 hover:bg-white/20 text-sm cursor-pointer"
            title="Export Psych Engine JSON"
          >
            <Download className="w-4 h-4" /> Export
          </button>
          <button
            onClick={() => fileInputRef.current?.click()}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-white/10 hover:bg-white/20 text-sm cursor-pointer"
            title="Import Psych Engine JSON"
          >
            <Upload className="w-4 h-4" /> Import
          </button>
          <input ref={fileInputRef} type="file" accept=".json" className="hidden" onChange={handleImport} />
        </div>
      </div>

      {/* Main Split Body */}
      <div className="flex-1 flex min-h-0 overflow-hidden">
        {/* Left: 8-Lane Step Grid */}
        <div className="flex-1 flex flex-col items-center justify-center p-4 overflow-y-auto bg-[radial-gradient(circle_at_top,#1d1042,transparent_70%)]">
          {/* Section Nav */}
          <div className="flex items-center gap-3 mb-3">
            <button
              onClick={() => setSectionIdx((s) => Math.max(0, s - 1))}
              className="px-3 py-1 rounded bg-white/10 hover:bg-white/20 text-sm cursor-pointer"
            >
              Prev Section
            </button>
            <div className="font-title text-xl text-[#ffde40] min-w-36 text-center">
              SECTION {sectionIdx + 1} / {totalSections}
            </div>
            <button
              onClick={() => setSectionIdx((s) => s + 1)}
              className="px-3 py-1 rounded bg-white/10 hover:bg-white/20 text-sm cursor-pointer"
            >
              Next Section
            </button>
          </div>

          {/* Column Headers */}
          <div className="grid grid-cols-8 w-[448px] text-center text-xs font-bold tracking-wider mb-1">
            <div className="col-span-4 py-1 bg-[#a464ff]/25 text-[#d8b4fe] rounded-tl-lg border-r border-white/20">
              OPPONENT (4K)
            </div>
            <div className="col-span-4 py-1 bg-[#36b5ff]/25 text-[#bae6fd] rounded-tr-lg">PLAYER (4K)</div>
          </div>

          {/* 16 Steps x 8 Lanes */}
          <div className="relative w-[448px] border border-white/20 rounded-b-lg overflow-hidden shadow-2xl bg-[#100a24]">
            {Array.from({ length: 16 }).map((_, step) => (
              <div
                key={step}
                className={`grid grid-cols-8 h-9 ${step % 4 === 0 ? 'border-t-2 border-white/30' : 'border-t border-white/10'}`}
              >
                {Array.from({ length: 8 }).map((__, col) => {
                  const side: 0 | 1 = col < 4 ? 0 : 1;
                  const dir = col % 4;
                  const targetMs = secStartMs + step * stepMs;
                  const match = sectionNotes.find(
                    ({ note }) => note.side === side && note.dir === dir && Math.abs(note.timeMs - targetMs) < stepMs * 0.4,
                  );
                  const isDark = (step + col) % 2 === 0;
                  const colRgb = dirColor(dir);
                  const isSelected = match && match.idx === selectedNoteIdx;

                  return (
                    <button
                      key={col}
                      onClick={(e) => handleCellClick(step, col, e.shiftKey)}
                      className={`relative h-full flex items-center justify-center cursor-pointer transition hover:bg-white/15 ${
                        col === 3 ? 'border-r-2 border-white/35' : 'border-r border-white/10'
                      } ${isDark ? 'bg-white/[0.03]' : 'bg-transparent'}`}
                    >
                      {step === 0 && !match && (
                        <span className="text-[10px] text-white/25 font-bold">{LANE_LABELS[col]}</span>
                      )}
                      {match && (
                        <>
                          {match.note.lengthMs > 40 && (
                            <div
                              className="absolute top-1/2 w-3 rounded-b pointer-events-none opacity-75 z-10"
                              style={{
                                backgroundColor: rgba(colRgb, 0.85),
                                height: `${Math.min(16 - step, match.note.lengthMs / stepMs) * 36}px`,
                              }}
                            />
                          )}
                          <div
                            className={`w-7 h-7 rounded-lg flex items-center justify-center text-xs font-black text-[#090518] shadow-md z-20 ${
                              isSelected ? 'ring-2 ring-white scale-110' : ''
                            }`}
                            style={{ backgroundColor: rgba(colRgb) }}
                          >
                            {LANE_LABELS[col]}
                          </div>
                        </>
                      )}
                    </button>
                  );
                })}
              </div>
            ))}

            {/* Live Playhead */}
            {playheadInSec >= 0 && (
              <div
                className="absolute left-0 right-0 h-1 bg-[#ffde40] shadow-[0_0_12px_#ffde40] pointer-events-none z-30"
                style={{ top: `${playheadInSec * 100}%` }}
              />
            )}
          </div>
        </div>

        {/* Right: Properties & EZ Spam Tools */}
        <div className="w-80 bg-[#110927] border-l border-white/10 p-4 flex flex-col gap-4 overflow-y-auto shrink-0">
          <div>
            <h3 className="font-title text-xl text-[#ffde40] mb-2">SONG SETTINGS</h3>
            <div className="space-y-2.5 text-sm">
              <label className="block">
                <span className="text-xs text-white/60 uppercase">Title</span>
                <input
                  value={chart.title}
                  onChange={(e) => setChart((c) => ({ ...c, title: e.target.value }))}
                  className="w-full mt-0.5 px-2.5 py-1.5 rounded-lg bg-black/40 border border-white/15 text-white"
                />
              </label>
              <div className="grid grid-cols-2 gap-2">
                <label className="block">
                  <span className="text-xs text-white/60 uppercase">BPM</span>
                  <input
                    type="number"
                    min={40}
                    max={300}
                    value={chart.bpm}
                    onChange={(e) => setChart((c) => ({ ...c, bpm: Math.max(40, Number(e.target.value) || 120) }))}
                    className="w-full mt-0.5 px-2.5 py-1.5 rounded-lg bg-black/40 border border-white/15 text-white"
                  />
                </label>
                <label className="block">
                  <span className="text-xs text-white/60 uppercase">Scroll Speed</span>
                  <input
                    type="number"
                    step={0.1}
                    min={0.5}
                    max={6}
                    value={chart.speed}
                    onChange={(e) => setChart((c) => ({ ...c, speed: Math.max(0.5, Number(e.target.value) || 2.5) }))}
                    className="w-full mt-0.5 px-2.5 py-1.5 rounded-lg bg-black/40 border border-white/15 text-white"
                  />
                </label>
              </div>
            </div>
          </div>

          <div className="border-t border-white/10 pt-3">
            <h3 className="font-title text-xl text-[#36b5ff] mb-2">SECTION {sectionIdx + 1}</h3>
            <button
              onClick={toggleSectionFocus}
              className={`w-full py-2 px-3 rounded-lg font-bold text-sm border cursor-pointer transition ${
                mustHit
                  ? 'bg-[#36b5ff]/20 border-[#36b5ff] text-[#bae6fd]'
                  : 'bg-[#a464ff]/20 border-[#a464ff] text-[#e9d5ff]'
              }`}
            >
              Camera Focus: {mustHit ? 'PLAYER (Hype)' : 'OPPONENT'}
            </button>

            {selectedNoteIdx !== null && chart.notes[selectedNoteIdx] && (
              <div className="mt-3 p-2.5 rounded-lg bg-black/35 border border-white/10 text-xs space-y-2">
                <div className="flex justify-between text-white/80">
                  <span>Selected Sustain</span>
                  <span className="text-[#ffde40] font-bold">
                    {Math.round(chart.notes[selectedNoteIdx].lengthMs)} ms
                  </span>
                </div>
                <div className="flex gap-2">
                  <button
                    onClick={() => adjustSustain(selectedNoteIdx, -stepMs)}
                    className="flex-1 py-1 rounded bg-white/10 hover:bg-white/20 cursor-pointer"
                  >
                    -1 Step (Q)
                  </button>
                  <button
                    onClick={() => adjustSustain(selectedNoteIdx, stepMs)}
                    className="flex-1 py-1 rounded bg-white/10 hover:bg-white/20 cursor-pointer flex items-center justify-center gap-1"
                  >
                    <Plus className="w-3 h-3" /> 1 Step (E)
                  </button>
                </div>
              </div>
            )}
          </div>

          <div className="border-t border-white/10 pt-3">
            <h3 className="font-title text-xl text-[#ff4fa3] mb-2 flex items-center gap-1.5">
              <Sparkles className="w-4 h-4" /> EZ SPAM TOOLS
            </h3>
            <div className="grid grid-cols-2 gap-2 text-xs font-bold">
              <button
                onClick={() => applyPattern('stairs')}
                className="py-2 rounded-lg bg-white/10 hover:bg-[#ff4fa3]/30 border border-white/10 cursor-pointer"
              >
                Stairs
              </button>
              <button
                onClick={() => applyPattern('zigzag')}
                className="py-2 rounded-lg bg-white/10 hover:bg-[#36b5ff]/30 border border-white/10 cursor-pointer"
              >
                Zigzag
              </button>
              <button
                onClick={() => applyPattern('jacks')}
                className="py-2 rounded-lg bg-white/10 hover:bg-[#ffde40]/30 border border-white/10 cursor-pointer"
              >
                Jacks
              </button>
              <button
                onClick={() => applyPattern('chords')}
                className="py-2 rounded-lg bg-white/10 hover:bg-[#3ce0a0]/30 border border-white/10 cursor-pointer"
              >
                Jump Chords
              </button>
              <button
                onClick={() => applyPattern('random')}
                className="py-2 rounded-lg bg-white/10 hover:bg-[#a464ff]/30 border border-white/10 cursor-pointer"
              >
                Random Fill
              </button>
              <button
                onClick={() => applyPattern('clear')}
                className="py-2 rounded-lg bg-[#ff3054]/20 hover:bg-[#ff3054]/40 text-[#fda4af] border border-[#ff3054]/40 cursor-pointer flex items-center justify-center gap-1"
              >
                <Trash2 className="w-3.5 h-3.5" /> Clear Sec
              </button>
            </div>
          </div>

          <div className="mt-auto text-xs text-white/60 bg-black/30 p-2.5 rounded-lg border border-white/10">
            {statusMsg}
          </div>
        </div>
      </div>
    </div>
  );
};
