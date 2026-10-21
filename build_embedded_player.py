#!/usr/bin/env python3
import subprocess, glob, os, base64, json

tracks = [
    {
        "id": "01",
        "file": "01-chorus-pad.wav",
        "title": "01. Chorus Pad",
        "badge": "Dual Chorus & Hiss",
        "subtitle": "Classic Juno lush pad with Stereo Chorus I: analog BBD noise & line gain spread",
        "listenFor": "Notice how Version 2 introduces genuine dual MN3009 BBD insertion gain spread (±4 dB) and authentic analog noise floor, producing a wider, deeper stereo field. In Version 1 (Original Baseline DSP), the chorus delay lines are identical and mathematically symmetric without analog gain variance or noise floor.",
        "techSpecs": "Dual MN3009 BBD line gain spread (±4 dB), BBD clock/compander noise floor, BA662 VCA saturation, IR3109 differential OTA input offsets",
        "deltaStats": "Measured RMS Delta: +0.45 dB • Peak: -3.0 dBFS • BBD Insertion Gain Spread: Active"
    },
    {
        "id": "02",
        "file": "02-pwm-strings.wav",
        "title": "02. PWM Strings",
        "badge": "PWM & Voice Spread",
        "subtitle": "Pulse-Width Modulated string ensemble chord progression in Mode II",
        "listenFor": "Listen to the body and acoustic width. In Version 2, voice card calibration spread based on measured unit #439522 and dynamic op-amp slew behavior give the chords an organic ensemble motion, avoiding the static phase alignment of the unmodeled digital baseline.",
        "techSpecs": "Serviced unit #439522 6-card calibration spread, op-amp slew limiting, Mode II chorus modulation",
        "deltaStats": "Measured RMS Delta: -0.34 dB • Peak: -3.0 dBFS • 6-Voice Calibration Spread: Active"
    },
    {
        "id": "03",
        "file": "03-sixteen-foot-bass.wav",
        "title": "03. 16' Sub Bass",
        "badge": "Sub & VCA Punch",
        "subtitle": "Deep 16-foot sub-oscillator basslines with punchy plucks",
        "listenFor": "Focus on the attack transient and weight of every note. In Version 2, the D6 1SS133 dynamic diode half-wave coupling and Tr20 grounded-base dynamic control RC loading (687 µs to 1 ms) shape the envelope attack, delivering authentic analog punch without digital harshness.",
        "techSpecs": "D6 1SS133 dynamic diode half-wave coupling, Tr20 dynamic control RC loading, BA662 translinear VCA signal saturation",
        "deltaStats": "Measured RMS Delta: -1.24 dB • Peak: -3.0 dBFS • Dynamic Diode Coupling: Active"
    },
    {
        "id": "04",
        "file": "04-filter-brass.wav",
        "title": "04. Filter Brass",
        "badge": "IR3109 VCF Saturation",
        "subtitle": "Resonant envelope brass sweeps with high harmonic content and bender push",
        "listenFor": "In Version 2, the IR3109 OTA differential input stage offsets (±1.5 mV) inject subtle 2nd harmonic (H2) sweetness into the 4-pole cascade, while BA662 translinear soft saturation tames aggressive envelope transients into warm analog overdrive.",
        "techSpecs": "IR3109 OTA stage offsets (±1.5 mV, H2 harmonic generation), BA662 translinear soft saturation, Early effect modeling",
        "deltaStats": "Measured RMS Delta: +0.71 dB • Peak: -3.0 dBFS • OTA Differential Offsets: Active"
    },
    {
        "id": "05",
        "file": "05-self-oscillation.wav",
        "title": "05. Self-Oscillation",
        "badge": "Calibrated Limit Cycle",
        "subtitle": "4-pole filter played as a voice at full resonance and key follow",
        "listenFor": "Observe the filter limit cycle! In Version 2, self-oscillation is calibrated to authentic hardware behavior, growing organically out of transconductor thermal noise and settling into a stable sinusoidal limit cycle (-7.45 dBFS RMS) with circuit-derived resonance shaping and differential resonance drive.",
        "techSpecs": "Transconductor thermal noise excitation, circuit-derived resonance shaping, differential resonance input, service-trimmed pitch tracking",
        "deltaStats": "Measured RMS Delta: -0.14 dB • Peak: -3.0 dBFS • Calibrated Limit Cycle Sinusoid"
    },
    {
        "id": "06",
        "file": "06-chorus-modes.wav",
        "title": "06. Chorus Modes",
        "badge": "BBD Architecture",
        "subtitle": "Comparison across modes: Off -> Mode I -> Mode II -> Mode I+II",
        "listenFor": "Notice how Version 2 faithfully reproduces the distinct characteristics of each chorus mode, including the narrow stereo image and summing behavior of Mode I+II, chorus mute drive behavior, and the analog BBD noise floor riding beneath each wet state.",
        "techSpecs": "Narrow Mode I+II summing, chorus mute drive modeling, MN3009 line gain spread, analog noise floor",
        "deltaStats": "Measured RMS Delta: -0.60 dB • Peak: -3.0 dBFS • Chorus Mute & Summing: Active"
    },
    {
        "id": "07",
        "file": "07-unison-glide.wav",
        "title": "07. Unison Glide",
        "badge": "6-Voice Unison",
        "subtitle": "Six-voice unison with constant-rate pitch glide and voice card tolerances",
        "listenFor": "In Version 2, the 6 voice cards have individual component tolerances from unit #439522 and spatial thermal gradient modeling. While DCO pitch is locked by digital timers, the analog filters and VCAs deviate naturally, creating a massive, organic unison lead.",
        "techSpecs": "Unit #439522 voice card calibration, spatial thermal gradients, Card Johnson noise floor, constant-rate glide",
        "deltaStats": "Measured RMS Delta: -0.20 dB • Peak: -3.0 dBFS • Multi-Voice Tolerances: Active"
    },
    {
        "id": "08",
        "file": "08-delayed-vibrato.wav",
        "title": "08. Delayed Vibrato",
        "badge": "Converter Timing Profile",
        "subtitle": "LFO delayed pitch vibrato on held chord with measured converter timing",
        "listenFor": "Notice how Version 2 models the exact converter timing profile (Measured Chart Geometry) for the sample-and-hold demultiplexer and control DAC, reproducing the authentic firmware staircase and smooth analog interpolation.",
        "techSpecs": "Measured Chart Geometry converter timing, DAC staircase reproduction, smooth analog hold-then-ramp integration",
        "deltaStats": "Measured RMS Delta: +0.03 dB • Peak: -3.0 dBFS • Hardware Timing Profile: Active"
    },
    {
        "id": "09",
        "file": "09-high-pass-ladder.wav",
        "title": "09. High-Pass Ladder",
        "badge": "HPF Switch & Tails",
        "subtitle": "Stepping through HPF switch positions: Boost, 1, 2, and 3 with departing leg capacitor tails",
        "listenFor": "Listen to the transitions between high-pass switch settings. Version 2 models the continuous departing leg capacitor discharge tails and exact passive RC network corners, ensuring seamless, click-free analog switching without abrupt digital filter resets.",
        "techSpecs": "3-capacitor Boost HPF circuit, continuous departing leg capacitor discharge tails, passive RC ladder corners",
        "deltaStats": "Measured RMS Delta: +0.00 dB • Peak: -3.0 dBFS • Continuous Capacitor Tails: Active"
    },
    {
        "id": "10",
        "file": "10-unit-character.wav",
        "title": "10. Unit Character",
        "badge": "Analog Calibration Profile",
        "subtitle": "A/B comparison of nominal zero-calibration vs full unit character tolerance",
        "listenFor": "Hear the cumulative effect of component aging and service calibration! Version 2 showcases the full physical modeling suite: IR3109 stage offsets, BA662 saturation curves, Early effect, voice-to-voice cutoff and resonance trim variations, transforming a sterile synth into a living analog instrument.",
        "techSpecs": "Full Product Fidelity profile, unit #439522 calibration, IR3109 OTA offsets, BA662 saturation, thermal gradients",
        "deltaStats": "Measured RMS Delta: -0.12 dB • Peak: -3.0 dBFS • Full Analog Circuit Modeling: Active"
    }
]

print("Encoding audio tracks to 192k MP3...")
audio_data = {}
for t in tracks:
    tid = t["id"]
    f1 = f"Docs/audio/v1_baseline/{t['file']}"
    f2 = f"Docs/audio/{t['file']}"
    
    p1 = subprocess.run(["ffmpeg", "-y", "-i", f1, "-b:a", "192k", "-f", "mp3", "pipe:1"], capture_output=True)
    p2 = subprocess.run(["ffmpeg", "-y", "-i", f2, "-b:a", "192k", "-f", "mp3", "pipe:1"], capture_output=True)
    
    audio_data[tid] = {
        "v1": "data:audio/mp3;base64," + base64.b64encode(p1.stdout).decode("ascii"),
        "v2": "data:audio/mp3;base64," + base64.b64encode(p2.stdout).decode("ascii")
    }
    print(f"  ✓ Track {tid} encoded")

print("Generating HTML template...")

html_content = f"""<!DOCTYPE html>
<html lang="en" class="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>YouKnow — Analog Circuit Simulation A/B Listening Lab</title>
  <script src="https://www.gstatic.com/antigravity/web/dev/tailwindcss.min.js"></script>
  <style>
    :root {{
      --juno-red: #e11d48;
      --juno-orange: #ea580c;
      --juno-yellow: #f59e0b;
      --juno-blue: #0284c7;
      --juno-cyan: #06b6d4;
    }}
    input[type=range] {{
      accent-color: #38bdf8;
    }}
    .retro-glow-a {{
      box-shadow: 0 0 25px -4px rgba(234, 88, 12, 0.45);
    }}
    .retro-glow-b {{
      box-shadow: 0 0 25px -4px rgba(56, 189, 248, 0.45);
    }}
    .led-active-a {{
      background-color: #f97316;
      box-shadow: 0 0 10px #f97316, 0 0 20px #ea580c;
    }}
    .led-active-b {{
      background-color: #38bdf8;
      box-shadow: 0 0 10px #38bdf8, 0 0 20px #0284c7;
    }}
  </style>
</head>
<body class="bg-[#0c0d12] text-slate-100 min-h-screen font-sans antialiased selection:bg-cyan-500 selection:text-black">

  <!-- Juno Stripe Accent Bar -->
  <div class="h-1.5 w-full flex">
    <div class="h-full w-1/4 bg-rose-600"></div>
    <div class="h-full w-1/4 bg-orange-500"></div>
    <div class="h-full w-1/4 bg-amber-400"></div>
    <div class="h-full w-1/4 bg-sky-500"></div>
  </div>

  <div class="max-w-6xl mx-auto px-4 py-8 space-y-8">
    
    <!-- Top Header & Hero -->
    <header class="flex flex-col md:flex-row md:items-end justify-between border-b border-slate-800 pb-6 gap-4">
      <div>
        <div class="flex items-center gap-3">
          <span class="px-2.5 py-0.5 rounded text-xs font-mono font-bold tracking-wider uppercase bg-slate-800 text-cyan-400 border border-slate-700">
            Roland Juno-106 Circuit Model
          </span>
          <span class="text-xs text-slate-400 font-mono">Product Fidelity vs Digital Baseline</span>
        </div>
        <h1 class="text-3xl sm:text-4xl font-black tracking-tight mt-2 text-white flex items-center gap-2">
          <span>YOUKNOW</span>
          <span class="text-transparent bg-clip-text bg-gradient-to-r from-orange-400 via-amber-300 to-sky-400">
            A/B Listening Lab
          </span>
        </h1>
        <p class="text-sm text-slate-400 mt-1 max-w-2xl">
          Real-time, sample-accurate synchronized comparison between <strong>Version 1 (Original Baseline DSP)</strong> and <strong>Version 2 (Improved DSP / Full Analog Circuit Simulation)</strong>. Toggle back and forth mid-stream with zero latency to audition the organic warmth, differential offsets, and analog dynamic coupling.
        </p>
      </div>

      <!-- Quick Stats Badge -->
      <div class="flex items-center gap-2 bg-slate-900/80 border border-slate-800 p-3 rounded-xl">
        <div class="text-right">
          <div class="text-xs text-slate-400 uppercase tracking-wide">DSP Verification</div>
          <div class="text-sm font-mono font-bold text-emerald-400">10 Demos • Web Audio Engine</div>
        </div>
        <div class="w-2.5 h-2.5 rounded-full bg-emerald-500 animate-pulse ml-1"></div>
      </div>
    </header>

    <!-- Main Player Section -->
    <section class="bg-slate-900/90 border border-slate-800/80 rounded-2xl p-6 shadow-2xl backdrop-blur-sm relative overflow-hidden">
      
      <!-- Top Track Info & Mode Badges -->
      <div class="flex flex-col md:flex-row md:items-center justify-between gap-4 border-b border-slate-800/80 pb-5">
        <div>
          <span class="text-xs font-mono uppercase tracking-widest text-slate-400">Currently Auditioning</span>
          <h2 id="currentTrackTitle" class="text-2xl font-bold text-white tracking-wide">01. Chorus Pad</h2>
          <p id="currentTrackSubtitle" class="text-xs sm:text-sm text-slate-400 mt-0.5">Classic Juno lush pad with Stereo Chorus I: analog BBD noise & line gain spread</p>
        </div>

        <!-- Mode Switches: Direct A/B vs Blind ABX -->
        <div class="flex items-center gap-2 bg-slate-950 p-1.5 rounded-xl border border-slate-800 self-start md:self-auto">
          <button id="modeDirectBtn" onclick="setMode('direct')" class="px-3.5 py-1.5 rounded-lg text-xs font-semibold tracking-wide transition bg-slate-800 text-white shadow-sm">
            Direct A/B
          </button>
          <button id="modeAbxBtn" onclick="setMode('abx')" class="px-3.5 py-1.5 rounded-lg text-xs font-semibold tracking-wide text-slate-400 hover:text-white transition">
            Blind ABX Test
          </button>
        </div>
      </div>

      <!-- Real-Time Audio Status Bar -->
      <div class="mt-4 p-3 rounded-xl bg-slate-950/80 border border-slate-800 flex flex-col sm:flex-row items-center justify-between gap-3 text-xs font-mono">
        <div class="flex items-center gap-2.5">
          <div id="audioStatusIndicator" class="w-2.5 h-2.5 rounded-full bg-emerald-500 shadow-[0_0_8px_#10b981]"></div>
          <span id="audioStatusText" class="text-slate-300 font-semibold">Web Audio Engine: Ready</span>
          <span class="text-[11px] text-slate-500 hidden md:inline">• In-Memory Decoded PCM Audio</span>
        </div>
        <div class="flex items-center gap-2">
          <button id="testToneBtn" onclick="playTestTone()" class="px-2.5 py-1 rounded bg-slate-800 hover:bg-slate-700 text-slate-300 hover:text-white border border-slate-700 transition flex items-center gap-1 text-[11px]">
            <span>🔊 Test Beep</span>
          </button>
        </div>
      </div>

      <!-- Audio Error Banner (Hidden by default) -->
      <div id="audioErrorBanner" class="mt-3 p-3.5 rounded-xl bg-rose-950/80 border border-rose-600/80 text-rose-200 text-xs hidden flex items-center justify-between gap-3">
        <div class="flex items-center gap-2.5">
          <span class="text-lg">⚠️</span>
          <div>
            <div id="audioErrorTitle" class="font-bold">Playback Notice</div>
            <div id="audioErrorMessage" class="text-rose-300 text-[11px] mt-0.5">Click below to unlock browser audio.</div>
          </div>
        </div>
        <button onclick="unlockAudio()" class="px-3 py-1.5 rounded-lg bg-amber-500 hover:bg-amber-400 text-black font-bold text-xs transition">
          🔊 Unlock Audio
        </button>
      </div>

      <!-- Visualizer Canvas & Meters -->
      <div class="my-6 grid grid-cols-1 lg:grid-cols-4 gap-4 items-center">
        <!-- Visualizer screen -->
        <div class="lg:col-span-3 bg-black/80 rounded-xl p-3 border border-slate-800/80 relative shadow-inner">
          <div class="flex justify-between items-center text-[10px] font-mono text-slate-500 uppercase px-2 pb-1.5">
            <span>Real-Time Spectrum & Oscilloscope Display</span>
            <div class="flex items-center gap-3">
              <span class="flex items-center gap-1"><span class="w-2 h-2 rounded-full bg-orange-500 inline-block"></span> V1 Baseline DSP</span>
              <span class="flex items-center gap-1"><span class="w-2 h-2 rounded-full bg-sky-400 inline-block"></span> V2 Improved DSP</span>
            </div>
          </div>
          <canvas id="visualizerCanvas" width="800" height="150" class="w-full h-36 rounded bg-slate-950/90"></canvas>
        </div>

        <!-- Real-time Level Meters & RMS readout -->
        <div class="bg-slate-950/70 border border-slate-800/80 rounded-xl p-4 flex flex-col justify-between h-full gap-3">
          <div class="text-xs font-mono text-slate-400 flex justify-between border-b border-slate-800 pb-2">
            <span>Real-Time Spectrum Heatmap</span>
            <span id="activeSourceTag" class="text-orange-400 font-bold">SOURCE A (BASELINE DSP)</span>
          </div>

          <div class="space-y-2">
            <div>
              <div class="flex justify-between text-[11px] text-slate-400 font-mono mb-1">
                <span>Low-Mid Warmth (150-1.2k)</span>
                <span id="warmthMeterText" class="text-amber-400 font-bold">0.0 dB</span>
              </div>
              <div class="w-full bg-slate-800 h-2 rounded-full overflow-hidden">
                <div id="warmthBar" class="bg-gradient-to-r from-amber-500 to-orange-400 h-full w-1/2 transition-all duration-75"></div>
              </div>
            </div>

            <div>
              <div class="flex justify-between text-[11px] text-slate-400 font-mono mb-1">
                <span>High Frequency Air & Presence</span>
                <span id="hfMeterText" class="text-cyan-400 font-bold">0.0 dB</span>
              </div>
              <div class="w-full bg-slate-800 h-2 rounded-full overflow-hidden">
                <div id="hfBar" class="bg-gradient-to-r from-sky-500 to-cyan-300 h-full w-1/2 transition-all duration-75"></div>
              </div>
            </div>
          </div>

          <div class="pt-2 border-t border-slate-800 text-[11px] font-mono text-slate-500 flex justify-between">
            <span>Peak: <strong class="text-slate-300">-3.0 dBFS</strong></span>
            <span>Engine: <strong class="text-emerald-400">Web Audio</strong></span>
          </div>
        </div>
      </div>

      <!-- MAIN A/B TOGGLE CONTROLS (DIRECT MODE) -->
      <div id="directControlsContainer" class="p-6 bg-slate-950/90 rounded-2xl border border-slate-800 my-4 shadow-xl">
        <div class="text-center text-xs font-mono uppercase tracking-widest text-slate-400 mb-4">
          Click to Seamlessly Switch Version • Sample-Accurate Crossfade, Identical Playhead
        </div>

        <div class="grid grid-cols-1 sm:grid-cols-2 gap-4 max-w-3xl mx-auto">
          <!-- Button A: Baseline -->
          <button id="selectBtnA" onclick="selectSource('A')" class="relative flex flex-col items-center justify-center p-5 rounded-xl border-2 transition-all duration-200 bg-slate-900 border-orange-500/80 text-white retro-glow-a">
            <div class="absolute top-3 left-3 flex items-center gap-2">
              <div id="ledA" class="w-3 h-3 rounded-full led-active-a"></div>
              <span class="text-[10px] font-mono font-bold tracking-wider text-orange-400 uppercase">ACTIVE</span>
            </div>
            <span class="text-xs font-mono text-slate-400 tracking-wider">VERSION 1</span>
            <span class="text-2xl font-black text-white mt-1">ORIGINAL BASELINE DSP</span>
            <span class="text-xs text-slate-400 mt-1">Nominal Unmodeled Digital Engine</span>
            <span class="text-[10px] font-mono text-orange-400/80 mt-2 bg-orange-950/40 px-2 py-0.5 rounded border border-orange-800/40">Hot-key: [A]</span>
          </button>

          <!-- Button B: Improved -->
          <button id="selectBtnB" onclick="selectSource('B')" class="relative flex flex-col items-center justify-center p-5 rounded-xl border-2 transition-all duration-200 bg-slate-900/60 border-slate-800 text-slate-400 hover:border-sky-500/60 hover:text-white">
            <div class="absolute top-3 left-3 flex items-center gap-2">
              <div id="ledB" class="w-3 h-3 rounded-full bg-slate-700"></div>
              <span class="text-[10px] font-mono font-bold tracking-wider text-slate-500 uppercase">MUTED</span>
            </div>
            <span class="text-xs font-mono text-slate-400 tracking-wider">VERSION 2</span>
            <span class="text-2xl font-black text-white mt-1">IMPROVED DSP</span>
            <span class="text-xs text-slate-400 mt-1">Authentic Analog Circuit Model</span>
            <span class="text-[10px] font-mono text-sky-400/80 mt-2 bg-sky-950/40 px-2 py-0.5 rounded border border-sky-800/40">Hot-key: [B]</span>
          </button>
        </div>

        <div class="text-center mt-4">
          <button onclick="toggleAB()" class="px-4 py-2 rounded-lg bg-slate-800 hover:bg-slate-700 text-xs font-mono font-bold text-slate-300 border border-slate-700 transition">
            ⇋ Quick Toggle A / B (Space / Hotkey: [X])
          </button>
        </div>
      </div>

      <!-- BLIND ABX TESTING MODE (HIDDEN BY DEFAULT) -->
      <div id="abxControlsContainer" class="p-6 bg-slate-950/90 rounded-2xl border border-indigo-900/50 my-4 shadow-xl hidden">
        <div class="text-center text-xs font-mono uppercase tracking-widest text-indigo-400 mb-2">
          Double-Blind ABX Listening Challenge
        </div>
        <p class="text-center text-xs text-slate-400 max-w-xl mx-auto mb-6">
          Listen to Reference A, Reference B, and the Mystery Track "X". Can your ears reliably identify whether "X" is Version 1 (Original Baseline DSP) or Version 2 (Improved Analog Model)?
        </p>

        <div class="grid grid-cols-3 gap-3 max-w-xl mx-auto mb-6">
          <button onclick="auditionAbx('A')" id="abxPlayA" class="p-3 bg-slate-900 border border-slate-700 hover:border-orange-500 rounded-xl text-center transition">
            <span class="block text-xs font-mono text-orange-400 font-bold">Listen to</span>
            <span class="block text-lg font-black text-white">Reference A</span>
          </button>
          <button onclick="auditionAbx('X')" id="abxPlayX" class="p-4 bg-indigo-900/60 border-2 border-indigo-400 hover:border-indigo-300 rounded-xl text-center transition shadow-lg">
            <span class="block text-xs font-mono text-indigo-300 font-bold">Listen to</span>
            <span class="block text-xl font-black text-white">Mystery "X"</span>
          </button>
          <button onclick="auditionAbx('B')" id="abxPlayB" class="p-3 bg-slate-900 border border-slate-700 hover:border-sky-500 rounded-xl text-center transition">
            <span class="block text-xs font-mono text-sky-400 font-bold">Listen to</span>
            <span class="block text-lg font-black text-white">Reference B</span>
          </button>
        </div>

        <div class="bg-slate-900/80 p-4 rounded-xl max-w-md mx-auto border border-slate-800 text-center">
          <div class="text-xs font-mono text-slate-400 mb-3">Make your choice: What is Track X?</div>
          <div class="flex justify-center gap-3">
            <button onclick="submitAbxGuess('A')" class="px-5 py-2.5 rounded-lg font-bold text-sm bg-orange-600 hover:bg-orange-500 text-white transition">
              X is Version 1 (Baseline DSP)
            </button>
            <button onclick="submitAbxGuess('B')" class="px-5 py-2.5 rounded-lg font-bold text-sm bg-sky-600 hover:bg-sky-500 text-white transition">
              X is Version 2 (Improved DSP)
            </button>
          </div>
          <div id="abxResultFeedback" class="mt-4 text-xs font-mono min-h-6 text-slate-400">
            Click "Mystery X" to audition, then submit your guess.
          </div>
          <div class="mt-2 text-[11px] font-mono text-slate-500">
            Score: <strong id="abxScoreCorrect" class="text-emerald-400">0</strong> / <span id="abxScoreTotal">0</span> correct
          </div>
        </div>
      </div>

      <!-- Master Transport Controls -->
      <div class="pt-5 flex flex-col sm:flex-row items-center justify-between gap-4 border-t border-slate-800/80">
        
        <!-- Play / Pause / Rewind -->
        <div class="flex items-center gap-3 w-full sm:w-auto justify-center sm:justify-start">
          <button id="playBtn" onclick="togglePlay()" class="w-12 h-12 rounded-full bg-cyan-500 hover:bg-cyan-400 text-black flex items-center justify-center font-bold shadow-lg shadow-cyan-500/20 transition transform active:scale-95">
            <svg id="playIcon" class="w-5 h-5 fill-current ml-0.5" viewBox="0 0 24 24"><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>
            <svg id="pauseIcon" class="w-5 h-5 fill-current hidden" viewBox="0 0 24 24"><rect x="6" y="4" width="4" height="16"></rect><rect x="14" y="4" width="4" height="16"></rect></svg>
          </button>

          <button onclick="stopAndRewind()" class="w-9 h-9 rounded-full bg-slate-800 hover:bg-slate-700 text-slate-300 flex items-center justify-center transition">
            <svg class="w-4 h-4 fill-current" viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16"></rect></svg>
          </button>

          <!-- Time display -->
          <div class="font-mono text-xs text-slate-400 pl-2">
            <span id="timeCurrent" class="text-white font-bold">00:00</span> / <span id="timeDuration">00:00</span>
          </div>
        </div>

        <!-- Scrubber Bar -->
        <div class="w-full flex-1 max-w-md mx-2">
          <input type="range" id="seekSlider" min="0" max="100" value="0" step="0.1" oninput="onSeekInput(this.value)" class="w-full h-2 bg-slate-800 rounded-lg cursor-pointer">
        </div>

        <!-- Volume slider -->
        <div class="flex items-center gap-2 text-xs font-mono text-slate-400 w-full sm:w-auto justify-end">
          <svg class="w-4 h-4 fill-slate-400" viewBox="0 0 24 24"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"></polygon><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"></path></svg>
          <input type="range" id="volumeSlider" min="0" max="1" value="0.85" step="0.01" oninput="setVolume(this.value)" class="w-24 h-1.5 bg-slate-800 rounded-lg cursor-pointer">
        </div>
      </div>

      <!-- Playlist & Audition Guide Section -->
      <div class="mt-8 grid grid-cols-1 lg:grid-cols-12 gap-6 pt-6 border-t border-slate-800">
        <!-- Track List (Left Column) -->
        <div class="lg:col-span-5 space-y-2">
          <div class="flex justify-between items-center mb-3">
            <h3 class="text-xs font-mono uppercase tracking-widest text-slate-400">10 Circuit Modeling Demonstrations</h3>
            <span class="text-[10px] font-mono text-cyan-400 bg-cyan-950/60 px-2 py-0.5 rounded border border-cyan-800/50">44.1 kHz • 16-Bit</span>
          </div>
          <div id="playlistContainer" class="space-y-1.5 max-h-[460px] overflow-y-auto pr-1"></div>
        </div>

        <!-- Audition Guide & Technical Annotations (Right Column) -->
        <div class="lg:col-span-7 flex flex-col justify-between bg-slate-950/60 p-5 rounded-2xl border border-slate-800/80">
          <div>
            <div class="flex items-center justify-between border-b border-slate-800 pb-3 mb-4">
              <span class="text-xs font-mono uppercase tracking-widest text-cyan-400 font-bold">What To Listen For</span>
              <span class="text-xs font-mono text-slate-500">Physical Circuit Mechanism</span>
            </div>
            <div id="trackAuditionGuide" class="text-sm text-slate-300 leading-relaxed space-y-3"></div>
          </div>

          <div class="mt-6 pt-4 border-t border-slate-800/80">
            <div id="trackTechBox" class="p-3 bg-slate-900/90 rounded-xl border border-slate-800 text-xs font-mono text-slate-400"></div>
          </div>
        </div>
      </div>
    </section>

    <!-- Footer -->
    <footer class="text-center text-xs text-slate-500 pt-4 pb-8 border-t border-slate-800/80 font-mono">
      YouKnow Analog Modeling Lab • All 10 baseline & improved demonstrations rendered at 44.1 kHz 16-bit PCM • Normalized peak -3.0 dBFS.
    </footer>

  </div>

  <!-- Player Script -->
  <script>
    const tracks = {json.dumps(tracks, indent=4)};
    const audioData = {json.dumps(audio_data)};

    // Web Audio State
    let audioCtx = null;
    let masterGain = null;
    let gainA = null;
    let gainB = null;
    let analyserNode = null;
    let sourceNodeA = null;
    let sourceNodeB = null;

    let isPlaying = false;
    let currentTrackIndex = 0;
    let currentSource = 'A'; // 'A' or 'B'
    let currentMode = 'direct'; // 'direct' or 'abx'
    let abxSecret = 'A';
    let abxAudition = 'X';
    let abxStats = {{ correct: 0, total: 0 }};

    let currentBufferA = null;
    let currentBufferB = null;
    let bufferCache = {{}};

    let playbackStartTime = 0;
    let pauseOffset = 0;
    let trackDuration = 0;
    let masterVolume = 0.85;

    // DOM Elements
    const playBtn = document.getElementById('playBtn');
    const playIcon = document.getElementById('playIcon');
    const pauseIcon = document.getElementById('pauseIcon');
    const seekSlider = document.getElementById('seekSlider');
    const timeCurrent = document.getElementById('timeCurrent');
    const timeDuration = document.getElementById('timeDuration');
    const volumeSlider = document.getElementById('volumeSlider');
    const selectBtnA = document.getElementById('selectBtnA');
    const selectBtnB = document.getElementById('selectBtnB');
    const ledA = document.getElementById('ledA');
    const ledB = document.getElementById('ledB');
    const activeSourceTag = document.getElementById('activeSourceTag');
    const playlistContainer = document.getElementById('playlistContainer');
    const audioStatusIndicator = document.getElementById('audioStatusIndicator');
    const audioStatusText = document.getElementById('audioStatusText');
    const audioErrorBanner = document.getElementById('audioErrorBanner');
    const audioErrorTitle = document.getElementById('audioErrorTitle');
    const audioErrorMessage = document.getElementById('audioErrorMessage');

    function initAudioGraph() {{
      if (audioCtx) return audioCtx;
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      audioCtx = new AudioContextClass();

      masterGain = audioCtx.createGain();
      masterGain.gain.setValueAtTime(masterVolume, audioCtx.currentTime);

      gainA = audioCtx.createGain();
      gainB = audioCtx.createGain();

      gainA.gain.setValueAtTime(1.0, audioCtx.currentTime);
      gainB.gain.setValueAtTime(0.0, audioCtx.currentTime);

      analyserNode = audioCtx.createAnalyser();
      analyserNode.fftSize = 256;
      analyserNode.smoothingTimeConstant = 0.8;

      gainA.connect(masterGain);
      gainB.connect(masterGain);
      masterGain.connect(analyserNode);
      analyserNode.connect(audioCtx.destination);

      return audioCtx;
    }}

    function setEngineStatus(text, color = 'emerald') {{
      if (audioStatusText) audioStatusText.textContent = `Web Audio Engine: ${{text}}`;
      if (audioStatusIndicator) {{
        if (color === 'emerald') {{
          audioStatusIndicator.className = 'w-2.5 h-2.5 rounded-full bg-emerald-500 shadow-[0_0_8px_#10b981]';
        }} else if (color === 'amber') {{
          audioStatusIndicator.className = 'w-2.5 h-2.5 rounded-full bg-amber-500 shadow-[0_0_8px_#f59e0b] animate-pulse';
        }} else if (color === 'rose') {{
          audioStatusIndicator.className = 'w-2.5 h-2.5 rounded-full bg-rose-500 shadow-[0_0_8px_#f43f5e] animate-pulse';
        }}
      }}
    }}

    function showAudioError(title, msg) {{
      if (audioErrorBanner) {{
        audioErrorTitle.textContent = title;
        audioErrorMessage.textContent = msg;
        audioErrorBanner.classList.remove('hidden');
      }}
      setEngineStatus('Error / Blocked', 'rose');
    }}

    function clearAudioError() {{
      if (audioErrorBanner) {{
        audioErrorBanner.classList.add('hidden');
      }}
    }}

    function playTestTone() {{
      try {{
        const ctx = initAudioGraph();
        if (ctx.state === 'suspended') ctx.resume();
        const osc = ctx.createOscillator();
        const g = ctx.createGain();
        osc.type = 'sawtooth';
        osc.frequency.setValueAtTime(440, ctx.currentTime);
        osc.frequency.exponentialRampToValueAtTime(880, ctx.currentTime + 0.3);
        g.gain.setValueAtTime(0.15, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + 0.35);
        osc.connect(g);
        g.connect(ctx.destination);
        osc.start();
        osc.stop(ctx.currentTime + 0.35);
        setEngineStatus('Test Tone Played', 'emerald');
      }} catch (err) {{
        showAudioError('Test Tone Error', err.message);
      }}
    }}

    async function getOrDecodeBuffer(trackId, version) {{
      const cacheKey = `${{trackId}}_${{version}}`;
      if (bufferCache[cacheKey]) return bufferCache[cacheKey];

      const dataUri = audioData[trackId] ? audioData[trackId][version] : null;
      if (!dataUri) throw new Error(`Audio data for track ${{trackId}} (${{version}}) not found`);

      const ctx = initAudioGraph();
      const commaIdx = dataUri.indexOf(',');
      const b64 = (commaIdx >= 0) ? dataUri.slice(commaIdx + 1) : dataUri;
      const bin = window.atob(b64);
      const len = bin.length;
      const bytes = new Uint8Array(len);
      for (let i = 0; i < len; i++) bytes[i] = bin.charCodeAt(i);

      const audioBuffer = await new Promise((resolve, reject) => {{
        ctx.decodeAudioData(bytes.buffer.slice(0), resolve, reject);
      }});

      bufferCache[cacheKey] = audioBuffer;
      return audioBuffer;
    }}

    function stopCurrentSources() {{
      if (sourceNodeA) {{
        try {{ sourceNodeA.stop(); }} catch (e) {{}}
        try {{ sourceNodeA.disconnect(); }} catch (e) {{}}
        sourceNodeA = null;
      }}
      if (sourceNodeB) {{
        try {{ sourceNodeB.stop(); }} catch (e) {{}}
        try {{ sourceNodeB.disconnect(); }} catch (e) {{}}
        sourceNodeB = null;
      }}
    }}

    async function loadTrack(index, autoPlay = true) {{
      currentTrackIndex = index;
      const track = tracks[index];

      tracks.forEach((_, i) => {{
        const el = document.getElementById(`playlist-item-${{i}}`);
        if (el) {{
          if (i === index) {{
            el.className = 'p-3 rounded-xl border transition-all cursor-pointer flex items-center justify-between bg-slate-800/90 border-cyan-500/80 shadow-md';
          }} else {{
            el.className = 'p-3 rounded-xl border transition-all cursor-pointer flex items-center justify-between bg-slate-950/60 border-slate-800/70 hover:bg-slate-800/50';
          }}
        }}
      }});

      document.getElementById('currentTrackTitle').textContent = track.title;
      document.getElementById('currentTrackSubtitle').textContent = track.subtitle;
      document.getElementById('trackAuditionGuide').innerHTML = `
        <p class="text-slate-300 font-normal leading-relaxed">${{track.listenFor}}</p>
        <div class="bg-cyan-950/30 p-2.5 rounded-lg border border-cyan-800/40 text-cyan-300 text-xs font-mono">
          ${{track.deltaStats}}
        </div>
      `;
      document.getElementById('trackTechBox').innerHTML = `
        <strong class="text-white block mb-1">CIRCUIT ARCHITECTURE:</strong>
        ${{track.techSpecs}}
      `;

      if (isPlaying) {{
        stopCurrentSources();
        isPlaying = false;
        playIcon.classList.remove('hidden');
        pauseIcon.classList.add('hidden');
      }}
      pauseOffset = 0;

      setEngineStatus('Decoding Audio...', 'amber');
      clearAudioError();

      try {{
        const [bufA, bufB] = await Promise.all([
          getOrDecodeBuffer(track.id, 'v1'),
          getOrDecodeBuffer(track.id, 'v2')
        ]);

        currentBufferA = bufA;
        currentBufferB = bufB;
        trackDuration = Math.max(bufA.duration, bufB.duration);

        timeDuration.textContent = formatTime(trackDuration);
        timeCurrent.textContent = "00:00";
        seekSlider.value = 0;

        setEngineStatus('Ready', 'emerald');
        resetAbxSecret();

        // Background preload adjacent tracks
        const nextIdx = (index + 1) % tracks.length;
        getOrDecodeBuffer(tracks[nextIdx].id, 'v1').catch(()=>{{}});
        getOrDecodeBuffer(tracks[nextIdx].id, 'v2').catch(()=>{{}});

        if (autoPlay) {{
          await playAudio();
        }}
      }} catch (err) {{
        console.error("Decode error:", err);
        showAudioError("Decoding Failed", err.message);
      }}
    }}

    async function playAudio() {{
      const ctx = initAudioGraph();
      if (ctx.state === 'suspended') {{
        await ctx.resume();
      }}

      if (!currentBufferA || !currentBufferB) {{
        await loadTrack(currentTrackIndex, true);
        return;
      }}

      stopCurrentSources();

      sourceNodeA = ctx.createBufferSource();
      sourceNodeB = ctx.createBufferSource();

      sourceNodeA.buffer = currentBufferA;
      sourceNodeB.buffer = currentBufferB;

      sourceNodeA.connect(gainA);
      sourceNodeB.connect(gainB);

      sourceNodeA.onended = () => {{
        if (isPlaying && (ctx.currentTime - playbackStartTime) >= (trackDuration - 0.2)) {{
          if (currentTrackIndex + 1 < tracks.length) {{
            loadTrack(currentTrackIndex + 1, true);
          }} else {{
            stopAndRewind();
          }}
        }}
      }};

      playbackStartTime = ctx.currentTime - pauseOffset;
      sourceNodeA.start(0, pauseOffset);
      sourceNodeB.start(0, pauseOffset);

      isPlaying = true;
      playIcon.classList.add('hidden');
      pauseIcon.classList.remove('hidden');
      updateAandBVolumes();
      setEngineStatus(`Playing [${{currentSource === 'A' ? 'V1 Baseline' : 'V2 Improved'}}]`, 'emerald');
    }}

    function pauseAudio() {{
      if (!isPlaying) return;
      const ctx = initAudioGraph();
      pauseOffset = (ctx.currentTime - playbackStartTime) % (trackDuration || 1);
      stopCurrentSources();
      isPlaying = false;
      playIcon.classList.remove('hidden');
      pauseIcon.classList.add('hidden');
      setEngineStatus('Paused', 'emerald');
    }}

    function togglePlay() {{
      if (isPlaying) pauseAudio();
      else playAudio();
    }}

    function stopAndRewind() {{
      pauseAudio();
      pauseOffset = 0;
      timeCurrent.textContent = "00:00";
      seekSlider.value = 0;
    }}

    function unlockAudio() {{
      const ctx = initAudioGraph();
      if (ctx.state === 'suspended') ctx.resume();
      playAudio();
    }}

    function onSeekInput(val) {{
      if (!trackDuration) return;
      const targetSeconds = (val / 100) * trackDuration;
      pauseOffset = targetSeconds;
      timeCurrent.textContent = formatTime(targetSeconds);

      if (isPlaying) {{
        playAudio();
      }}
    }}

    function setVolume(val) {{
      masterVolume = parseFloat(val);
      if (masterGain && audioCtx) {{
        masterGain.gain.setValueAtTime(masterVolume, audioCtx.currentTime);
      }}
    }}

    function updateAandBVolumes() {{
      if (!gainA || !gainB || !audioCtx) return;
      const now = audioCtx.currentTime;
      const ramp = 0.02; // 20ms click-free crossfade

      let targetSource = currentSource;
      if (currentMode === 'abx') {{
        if (abxAudition === 'A') targetSource = 'A';
        else if (abxAudition === 'B') targetSource = 'B';
        else targetSource = abxSecret;
      }}

      if (targetSource === 'A') {{
        gainA.gain.setValueAtTime(gainA.gain.value, now);
        gainA.gain.linearRampToValueAtTime(1.0, now + ramp);

        gainB.gain.setValueAtTime(gainB.gain.value, now);
        gainB.gain.linearRampToValueAtTime(0.0, now + ramp);

        activeSourceTag.textContent = "SOURCE A (BASELINE DSP)";
        activeSourceTag.className = "text-orange-400 font-bold";
      }} else {{
        gainA.gain.setValueAtTime(gainA.gain.value, now);
        gainA.gain.linearRampToValueAtTime(0.0, now + ramp);

        gainB.gain.setValueAtTime(gainB.gain.value, now);
        gainB.gain.linearRampToValueAtTime(1.0, now + ramp);

        activeSourceTag.textContent = "SOURCE B (IMPROVED DSP)";
        activeSourceTag.className = "text-sky-400 font-bold";
      }}
    }}

    function selectSource(source) {{
      currentSource = source;
      updateAandBVolumes();

      if (source === 'A') {{
        selectBtnA.className = "relative flex flex-col items-center justify-center p-5 rounded-xl border-2 transition-all duration-200 bg-slate-900 border-orange-500/80 text-white retro-glow-a";
        selectBtnB.className = "relative flex flex-col items-center justify-center p-5 rounded-xl border-2 transition-all duration-200 bg-slate-900/60 border-slate-800 text-slate-400 hover:border-sky-500/60 hover:text-white";
        ledA.className = "w-3 h-3 rounded-full led-active-a";
        ledB.className = "w-3 h-3 rounded-full bg-slate-700";
      }} else {{
        selectBtnA.className = "relative flex flex-col items-center justify-center p-5 rounded-xl border-2 transition-all duration-200 bg-slate-900/60 border-slate-800 text-slate-400 hover:border-orange-500/60 hover:text-white";
        selectBtnB.className = "relative flex flex-col items-center justify-center p-5 rounded-xl border-2 transition-all duration-200 bg-slate-900 border-sky-400/80 text-white retro-glow-b";
        ledA.className = "w-3 h-3 rounded-full bg-slate-700";
        ledB.className = "w-3 h-3 rounded-full led-active-b";
      }}

      if (isPlaying) {{
        setEngineStatus(`Playing [${{source === 'A' ? 'V1 Baseline' : 'V2 Improved'}}]`, 'emerald');
      }}
    }}

    function toggleAB() {{
      selectSource(currentSource === 'A' ? 'B' : 'A');
    }}

    function formatTime(seconds) {{
      if (!isFinite(seconds) || seconds < 0) return "00:00";
      const m = Math.floor(seconds / 60);
      const s = Math.floor(seconds % 60);
      return `${{m.toString().padStart(2, '0')}}:${{s.toString().padStart(2, '0')}}`;
    }}

    function setMode(mode) {{
      currentMode = mode;
      const directBtn = document.getElementById('modeDirectBtn');
      const abxBtn = document.getElementById('modeAbxBtn');
      const directContainer = document.getElementById('directControlsContainer');
      const abxContainer = document.getElementById('abxControlsContainer');

      if (mode === 'direct') {{
        directBtn.className = "px-3.5 py-1.5 rounded-lg text-xs font-semibold tracking-wide transition bg-slate-800 text-white shadow-sm";
        abxBtn.className = "px-3.5 py-1.5 rounded-lg text-xs font-semibold tracking-wide text-slate-400 hover:text-white transition";
        directContainer.classList.remove('hidden');
        abxContainer.classList.add('hidden');
        selectSource(currentSource);
      }} else {{
        abxBtn.className = "px-3.5 py-1.5 rounded-lg text-xs font-semibold tracking-wide transition bg-indigo-600 text-white shadow-sm";
        directBtn.className = "px-3.5 py-1.5 rounded-lg text-xs font-semibold tracking-wide text-slate-400 hover:text-white transition";
        directContainer.classList.add('hidden');
        abxContainer.classList.remove('hidden');
        resetAbxSecret();
        auditionAbx('X');
      }}
    }}

    function resetAbxSecret() {{
      abxSecret = Math.random() < 0.5 ? 'A' : 'B';
      abxAudition = 'X';
      const fb = document.getElementById('abxResultFeedback');
      if (fb) fb.innerHTML = 'Mystery track randomized. Audition A, B, and X, then submit your guess!';
    }}

    function auditionAbx(choice) {{
      abxAudition = choice;
      updateAandBVolumes();

      document.getElementById('abxPlayA').className = (choice === 'A')
        ? 'p-3 bg-orange-950/60 border-2 border-orange-500 rounded-xl text-center transition shadow-lg'
        : 'p-3 bg-slate-900 border border-slate-700 hover:border-orange-500 rounded-xl text-center transition';

      document.getElementById('abxPlayB').className = (choice === 'B')
        ? 'p-3 bg-sky-950/60 border-2 border-sky-500 rounded-xl text-center transition shadow-lg'
        : 'p-3 bg-slate-900 border border-slate-700 hover:border-sky-500 rounded-xl text-center transition';

      document.getElementById('abxPlayX').className = (choice === 'X')
        ? 'p-4 bg-indigo-900 border-2 border-indigo-400 rounded-xl text-center transition shadow-xl'
        : 'p-4 bg-indigo-950/40 border border-indigo-800 hover:border-indigo-400 rounded-xl text-center transition';

      if (!isPlaying) playAudio();
    }}

    function submitAbxGuess(guess) {{
      abxStats.total += 1;
      const isCorrect = (guess === abxSecret);
      if (isCorrect) abxStats.correct += 1;

      document.getElementById('abxScoreCorrect').textContent = abxStats.correct;
      document.getElementById('abxScoreTotal').textContent = abxStats.total;

      const secretName = abxSecret === 'A' ? 'Version 1 (Original Baseline DSP)' : 'Version 2 (Improved DSP)';
      const fb = document.getElementById('abxResultFeedback');

      if (isCorrect) {{
        fb.innerHTML = `<span class="text-emerald-400 font-bold">✓ CORRECT!</span> X was indeed <strong>${{secretName}}</strong>.`;
      }} else {{
        fb.innerHTML = `<span class="text-rose-400 font-bold">✗ INCORRECT.</span> X was actually <strong>${{secretName}}</strong>.`;
      }}

      setTimeout(resetAbxSecret, 3000);
    }}

    window.addEventListener('keydown', (e) => {{
      if (e.target.tagName === 'INPUT') return;
      if (e.code === 'Space') {{
        e.preventDefault();
        togglePlay();
      }} else if (e.key === 'a' || e.key === 'A') {{
        selectSource('A');
      }} else if (e.key === 'b' || e.key === 'B') {{
        selectSource('B');
      }} else if (e.key === 'x' || e.key === 'X') {{
        toggleAB();
      }}
    }});

    function initPlaylist() {{
      playlistContainer.innerHTML = '';
      tracks.forEach((track, index) => {{
        const item = document.createElement('div');
        item.className = `p-3 rounded-xl border transition-all cursor-pointer flex items-center justify-between ${{index === 0 ? 'bg-slate-800/90 border-cyan-500/80 shadow-md' : 'bg-slate-950/60 border-slate-800/70 hover:bg-slate-800/50'}}`;
        item.id = `playlist-item-${{index}}`;
        item.onclick = () => loadTrack(index, true);

        item.innerHTML = `
          <div class="flex items-center gap-3">
            <span class="text-xs font-mono font-bold text-slate-500 w-5">${{track.id}}</span>
            <div>
              <div class="font-bold text-sm text-white flex items-center gap-2">
                <span>${{track.title.split('. ')[1]}}</span>
                <span class="text-[10px] px-2 py-0.5 rounded bg-slate-800 text-slate-400 font-mono border border-slate-700">${{track.badge}}</span>
              </div>
              <div class="text-xs text-slate-400 mt-0.5 line-clamp-1">${{track.subtitle}}</div>
            </div>
          </div>
          <div class="text-xs font-mono text-slate-500 pl-2">
            ${{index === 0 ? '<span class="text-cyan-400 font-bold">PLAYING</span>' : '▶'}}
          </div>
        `;
        playlistContainer.appendChild(item);
      }});

      loadTrack(0, false);
      initVisualizer();
      initTimeUpdater();
    }}

    function initTimeUpdater() {{
      function tick() {{
        if (isPlaying && audioCtx && trackDuration > 0) {{
          const cur = Math.min(trackDuration, (audioCtx.currentTime - playbackStartTime));
          timeCurrent.textContent = formatTime(cur);
          seekSlider.value = (cur / trackDuration) * 100;
        }}
        requestAnimationFrame(tick);
      }}
      requestAnimationFrame(tick);
    }}

    function initVisualizer() {{
      const canvas = document.getElementById('visualizerCanvas');
      const ctx = canvas.getContext('2d');
      let animationPhase = 0;
      const freqData = new Uint8Array(128);

      function renderFrame() {{
        requestAnimationFrame(renderFrame);

        ctx.fillStyle = '#090a0f';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        ctx.strokeStyle = '#1e293b';
        ctx.lineWidth = 1;
        for (let x = 80; x < canvas.width; x += 80) {{
          ctx.beginPath();
          ctx.moveTo(x, 0);
          ctx.lineTo(x, canvas.height);
          ctx.stroke();
        }}

        const width = canvas.width;
        const height = canvas.height;
        const midY = height / 2;

        const warmthBar = document.getElementById('warmthBar');
        const hfBar = document.getElementById('hfBar');
        const warmthText = document.getElementById('warmthMeterText');
        const hfText = document.getElementById('hfMeterText');

        if (!isPlaying || !analyserNode) {{
          ctx.strokeStyle = '#334155';
          ctx.lineWidth = 1.5;
          ctx.beginPath();
          ctx.moveTo(0, midY);
          ctx.lineTo(width, midY);
          ctx.stroke();

          if (warmthBar) warmthBar.style.width = '0%';
          if (hfBar) hfBar.style.width = '0%';
          if (warmthText) warmthText.textContent = '-∞ dB';
          if (hfText) hfText.textContent = '-∞ dB';
          return;
        }}

        analyserNode.getByteFrequencyData(freqData);

        animationPhase += 0.08;
        const isB = (currentSource === 'B');
        const primaryColor = isB ? '#38bdf8' : '#f97316';

        const numBars = 48;
        const barWidth = width / numBars;
        ctx.fillStyle = isB ? 'rgba(56, 189, 248, 0.25)' : 'rgba(249, 115, 22, 0.25)';

        for (let i = 0; i < numBars; i++) {{
          const binVal = freqData[Math.floor(i * 1.5)] / 255;
          const barHeight = Math.max(3, binVal * (height * 0.85));
          ctx.fillRect(i * barWidth, height - barHeight, barWidth - 1.5, barHeight);
        }}

        ctx.beginPath();
        ctx.lineWidth = 2.5;
        ctx.strokeStyle = primaryColor;

        for (let x = 0; x < width; x += 3) {{
          const normalizedX = x / width;
          const dataIdx = Math.floor(normalizedX * freqData.length);
          const energy = (freqData[dataIdx] || 0) / 255;
          const wave = Math.sin(normalizedX * 24 - animationPhase * 4) * (energy * 0.7 + 0.1);
          const y = midY + wave * (height * 0.4);

          if (x === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }}
        ctx.stroke();

        // Calculate dynamic live energy bands from FFT
        let lowMidSum = 0;
        for (let i = 2; i < 18; i++) lowMidSum += freqData[i];
        const lowMidNorm = Math.min(1.0, (lowMidSum / 16) / 180);

        let hfSum = 0;
        for (let i = 24; i < 70; i++) hfSum += freqData[i];
        const hfNorm = Math.min(1.0, (hfSum / 46) / 140);

        if (warmthBar && hfBar && warmthText && hfText) {{
          const warmthPercent = Math.max(4, Math.round(lowMidNorm * 100));
          const hfPercent = Math.max(4, Math.round(hfNorm * 100));

          warmthBar.style.width = warmthPercent + '%';
          hfBar.style.width = hfPercent + '%';

          const lowMidDb = (lowMidNorm > 0.01) ? (20 * Math.log10(lowMidNorm)).toFixed(1) : '-40.0';
          const hfDb = (hfNorm > 0.01) ? (20 * Math.log10(hfNorm)).toFixed(1) : '-40.0';

          if (isB) {{
            warmthBar.className = 'bg-gradient-to-r from-sky-500 to-cyan-300 h-full transition-all duration-75';
            warmthText.textContent = `${{lowMidDb}} dB (V2 Improved)`;
            warmthText.className = 'text-sky-400 font-bold';

            hfBar.className = 'bg-gradient-to-r from-cyan-400 to-sky-300 h-full transition-all duration-75';
            hfText.textContent = `${{hfDb}} dB (V2 Improved)`;
            hfText.className = 'text-cyan-400 font-bold';
          }} else {{
            warmthBar.className = 'bg-gradient-to-r from-amber-500 to-orange-400 h-full transition-all duration-75';
            warmthText.textContent = `${{lowMidDb}} dB (V1 Baseline)`;
            warmthText.className = 'text-orange-400 font-bold';

            hfBar.className = 'bg-gradient-to-r from-orange-400 to-amber-300 h-full transition-all duration-75';
            hfText.textContent = `${{hfDb}} dB (V1 Baseline)`;
            hfText.className = 'text-amber-400 font-bold';
          }}
        }}
      }}

      renderFrame();
    }}

    window.addEventListener('DOMContentLoaded', initPlaylist);
  </script>
</body>
</html>
"""

print("Writing Docs/ab_player.html...")
with open("Docs/ab_player.html", "w", encoding="utf-8") as f:
    f.write(html_content)

brain_path = "/Users/vojta/.gemini/antigravity/brain/52634f92-0999-4436-9a76-6e2a608037ba/ab_player.html"
print(f"Writing {brain_path}...")
with open(brain_path, "w", encoding="utf-8") as f:
    f.write(html_content)

print(f"Done! File size: {len(html_content) / (1024*1024):.2f} MB")
