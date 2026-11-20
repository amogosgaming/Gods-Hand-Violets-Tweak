// Web Audio API engine: sample-accurate multi-stem song playback, real-time FFT analyser, and synthesized SFX
// Ported from src/audio.cpp
import { SR, StereoBuf } from './synth';

export type SfxKind =
  | 'scroll'
  | 'confirm'
  | 'cancel'
  | 'count3'
  | 'count2'
  | 'count1'
  | 'countGo'
  | 'miss'
  | 'hitsound'
  | 'death'
  | 'unlock';

class AudioEngine {
  private ctx: AudioContext | null = null;
  private masterGain: GainNode | null = null;
  private musicGain: GainNode | null = null;
  private sfxGain: GainNode | null = null;
  private analyser: AnalyserNode | null = null;
  private fftData: Uint8Array<ArrayBuffer> = new Uint8Array(new ArrayBuffer(256));

  // Active song playback nodes
  private instBuf: AudioBuffer | null = null;
  private oppBuf: AudioBuffer | null = null;
  private plyBuf: AudioBuffer | null = null;

  private instSrc: AudioBufferSourceNode | null = null;
  private oppSrc: AudioBufferSourceNode | null = null;
  private plySrc: AudioBufferSourceNode | null = null;
  private plyGain: GainNode | null = null;

  private songPlaying = false;
  private songPaused = false;
  private songLoop = false;
  private songStartCtxTime = 0;
  private songOffsetSec = 0;
  private songRate = 1.0;
  private songDurationSec = 0;

  // Menu / preview tracking
  private activeTrackId = '';

  // Cached volumes
  private masterVol = 0.8;
  private musicVol = 0.85;
  private sfxVol = 0.8;
  private muted = false;

  ensureContext(): AudioContext {
    if (!this.ctx) {
      const AC = window.AudioContext || (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
      this.ctx = new AC();
      this.masterGain = this.ctx.createGain();
      this.musicGain = this.ctx.createGain();
      this.sfxGain = this.ctx.createGain();
      this.analyser = this.ctx.createAnalyser();
      this.analyser.fftSize = 512;
      this.analyser.smoothingTimeConstant = 0.65;
      this.fftData = new Uint8Array(new ArrayBuffer(this.analyser.frequencyBinCount));

      this.musicGain.connect(this.analyser);
      this.analyser.connect(this.masterGain);
      this.sfxGain.connect(this.masterGain);
      this.masterGain.connect(this.ctx.destination);
      this.applyVolumes();
    }
    if (this.ctx.state === 'suspended') {
      this.ctx.resume().catch(() => {});
    }
    return this.ctx;
  }

  setVolumes(master: number, music: number, sfx: number, muted: boolean): void {
    this.masterVol = master;
    this.musicVol = music;
    this.sfxVol = sfx;
    this.muted = muted;
    this.applyVolumes();
  }

  private applyVolumes(): void {
    if (!this.ctx || !this.masterGain || !this.musicGain || !this.sfxGain) return;
    const now = this.ctx.currentTime;
    this.masterGain.gain.setTargetAtTime(this.muted ? 0 : this.masterVol, now, 0.015);
    this.musicGain.gain.setTargetAtTime(this.musicVol, now, 0.015);
    this.sfxGain.gain.setTargetAtTime(this.sfxVol, now, 0.015);
  }

  stereoToAudioBuffer(buf: StereoBuf, sampleRate = SR): AudioBuffer {
    const ctx = this.ensureContext();
    const ab = ctx.createBuffer(2, buf.l.length, sampleRate);
    ab.getChannelData(0).set(buf.l);
    ab.getChannelData(1).set(buf.r);
    return ab;
  }

  async decodeFile(file: File): Promise<AudioBuffer> {
    const ctx = this.ensureContext();
    const arr = await file.arrayBuffer();
    return await ctx.decodeAudioData(arr);
  }

  loadStems(inst: AudioBuffer, opp: AudioBuffer | null, ply: AudioBuffer | null, trackId = ''): void {
    this.stopSong();
    this.instBuf = inst;
    this.oppBuf = opp;
    this.plyBuf = ply;
    this.songDurationSec = inst.duration;
    this.songOffsetSec = 0;
    this.activeTrackId = trackId;
  }

  getActiveTrackId(): string {
    return this.activeTrackId;
  }

  getDurationSec(): number {
    return this.songDurationSec;
  }

  isPlaying(): boolean {
    return this.songPlaying && !this.songPaused;
  }

  playSong(offsetSec = 0, rate = 1.0, loop = false): void {
    if (!this.instBuf) return;
    const ctx = this.ensureContext();
    this.stopSourcesOnly();

    this.songRate = Math.max(0.5, Math.min(2.0, rate));
    this.songLoop = loop;
    const safeOffset = Math.max(0, Math.min(this.songDurationSec - 0.01, offsetSec));
    this.songOffsetSec = safeOffset;
    this.songStartCtxTime = ctx.currentTime + 0.025;

    const startNode = (buf: AudioBuffer, dest: AudioNode): AudioBufferSourceNode => {
      const src = ctx.createBufferSource();
      src.buffer = buf;
      src.playbackRate.value = this.songRate;
      src.loop = loop;
      src.connect(dest);
      src.start(this.songStartCtxTime, safeOffset);
      return src;
    };

    this.instSrc = startNode(this.instBuf, this.musicGain!);
    if (this.oppBuf) {
      this.oppSrc = startNode(this.oppBuf, this.musicGain!);
    }
    if (this.plyBuf) {
      this.plyGain = ctx.createGain();
      this.plyGain.gain.value = 1;
      this.plyGain.connect(this.musicGain!);
      this.plySrc = startNode(this.plyBuf, this.plyGain);
    }

    this.songPlaying = true;
    this.songPaused = false;
  }

  pauseSong(): void {
    if (!this.songPlaying || this.songPaused) return;
    this.songOffsetSec = this.getSongTimeSec();
    this.stopSourcesOnly();
    this.songPaused = true;
  }

  resumeSong(): void {
    if (!this.songPaused || !this.instBuf) return;
    this.playSong(this.songOffsetSec, this.songRate, this.songLoop);
  }

  stopSong(): void {
    this.stopSourcesOnly();
    this.songPlaying = false;
    this.songPaused = false;
    this.songOffsetSec = 0;
    this.activeTrackId = '';
  }

  private stopSourcesOnly(): void {
    for (const s of [this.instSrc, this.oppSrc, this.plySrc]) {
      if (s) {
        try {
          s.stop();
          s.disconnect();
        } catch {
          // ignore
        }
      }
    }
    this.instSrc = null;
    this.oppSrc = null;
    this.plySrc = null;
    if (this.plyGain) {
      try {
        this.plyGain.disconnect();
      } catch {
        // ignore
      }
      this.plyGain = null;
    }
  }

  setPlayerVoiceVolume(vol: number): void {
    if (!this.ctx || !this.plyGain) return;
    this.plyGain.gain.setTargetAtTime(Math.max(0, Math.min(1, vol)), this.ctx.currentTime, 0.02);
  }

  getSongTimeSec(): number {
    if (!this.songPlaying) return this.songOffsetSec;
    if (this.songPaused || !this.ctx) return this.songOffsetSec;
    const elapsed = (this.ctx.currentTime - this.songStartCtxTime) * this.songRate;
    const t = this.songOffsetSec + elapsed;
    if (this.songLoop && this.songDurationSec > 0) {
      return ((t % this.songDurationSec) + this.songDurationSec) % this.songDurationSec;
    }
    return t;
  }

  getFrequencyData(): Uint8Array {
    if (!this.analyser || !this.songPlaying || this.songPaused) {
      this.fftData.fill(0);
      return this.fftData;
    }
    this.analyser.getByteFrequencyData(this.fftData);
    return this.fftData;
  }

  // Synthesized crisp UI & gameplay SFX (zero external asset downloads)
  playSfx(kind: SfxKind, gainScale = 1): void {
    const ctx = this.ensureContext();
    if (!this.sfxGain) return;
    const now = ctx.currentTime;

    const tone = (
      type: OscillatorType,
      f0: number,
      f1: number,
      dur: number,
      vol: number,
      delay = 0,
    ) => {
      const osc = ctx.createOscillator();
      const g = ctx.createGain();
      osc.type = type;
      osc.frequency.setValueAtTime(f0, now + delay);
      if (f1 !== f0) {
        osc.frequency.exponentialRampToValueAtTime(Math.max(20, f1), now + delay + dur);
      }
      g.gain.setValueAtTime(0.0001, now + delay);
      g.gain.linearRampToValueAtTime(vol * gainScale, now + delay + 0.008);
      g.gain.exponentialRampToValueAtTime(0.0001, now + delay + dur);
      osc.connect(g);
      g.connect(this.sfxGain!);
      osc.start(now + delay);
      osc.stop(now + delay + dur + 0.02);
    };

    switch (kind) {
      case 'scroll':
        tone('triangle', 520, 680, 0.06, 0.22);
        break;
      case 'confirm':
        tone('triangle', 523, 784, 0.09, 0.28, 0);
        tone('triangle', 784, 1046, 0.14, 0.26, 0.06);
        break;
      case 'cancel':
        tone('sawtooth', 420, 210, 0.12, 0.18);
        break;
      case 'count3':
        tone('triangle', 440, 440, 0.14, 0.34);
        break;
      case 'count2':
        tone('triangle', 523, 523, 0.14, 0.34);
        break;
      case 'count1':
        tone('triangle', 659, 659, 0.14, 0.34);
        break;
      case 'countGo':
        tone('square', 880, 1175, 0.22, 0.26);
        break;
      case 'miss':
        tone('sawtooth', 190, 95, 0.14, 0.24);
        break;
      case 'hitsound':
        tone('sine', 1200, 420, 0.035, 0.32);
        break;
      case 'death':
        tone('sawtooth', 320, 72, 0.55, 0.35);
        tone('triangle', 160, 48, 0.7, 0.3, 0.12);
        break;
      case 'unlock':
        tone('triangle', 587, 880, 0.12, 0.25, 0);
        tone('triangle', 880, 1175, 0.18, 0.25, 0.09);
        break;
    }
  }
}

export const audio = new AudioEngine();
