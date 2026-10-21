#!/usr/bin/env python3
"""
Generate a high-definition promo video for the YouKnow synthesizer.
Features:
- Sample-accurate audio assembly with equal-power crossfades from repository demos
- Real-time 36-band logarithmic spectrum analyzer with peak-hold decay
- Real-time stereo oscilloscope / Lissajous waveform visualizer
- Clean, stable camera framing: only zooms in to relevant modules during demos, zero wobble
- Big, prominent, high-legibility Roland Juno-106 themed typography and badges
- Direct FFmpeg piped encoding at 1080p60 (H.264 / AAC 320k)
"""

import os
import sys
import math
import time
import subprocess
import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Constants & Configuration
# ---------------------------------------------------------------------------
VIDEO_WIDTH = 1920
VIDEO_HEIGHT = 1080
FPS = 60
SAMPLE_RATE = 48000
TOTAL_DURATION = 54.0
TOTAL_FRAMES = int(TOTAL_DURATION * FPS) # 3240 frames
TOTAL_SAMPLES = int(TOTAL_DURATION * SAMPLE_RATE)

# Fonts
FONT_BOLD_PATH = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FONT_REGULAR_PATH = "/System/Library/Fonts/Supplemental/Arial.ttf"

# Color Palette (BGR for OpenCV, RGB for PIL)
BGR_BG_DARK = (16, 18, 20)
BGR_BG_PANEL = (25, 27, 31)
BGR_ORANGE = (53, 107, 255)       # Roland Orange #FF6B35
BGR_CYAN = (255, 229, 0)          # Roland Cyan #00E5FF
BGR_RED = (45, 45, 235)           # Red #EB2D2D
BGR_GREEN = (60, 220, 80)         # LED Green #50DC3C
BGR_YELLOW = (40, 220, 255)       # LED Yellow #FFDC28
BGR_WHITE = (250, 250, 250)
BGR_GRAY = (160, 165, 170)
BGR_MUTED = (85, 90, 95)
BGR_BORDER = (55, 60, 68)

RGB_ORANGE = (255, 107, 53)
RGB_CYAN = (0, 229, 255)
RGB_WHITE = (250, 250, 250)
RGB_GRAY = (205, 210, 215)
RGB_MUTED = (140, 145, 150)

# Paths
SCREENSHOT_PATH = "Docs/screenshots/youknow-standalone.png"
ICON_PATH = "Assets/youknow-icon.png"
TEXTURE_PATH = "Assets/used-charcoal-plastic.png"
OUTPUT_DIR = "out/promo"
AUDIO_MASTER_PATH = os.path.join(OUTPUT_DIR, "audio_master.wav")
VIDEO_OUTPUT_PATH = os.path.join(OUTPUT_DIR, "youknow_promo_video.mp4")

# ---------------------------------------------------------------------------
# Segment Definitions
# ---------------------------------------------------------------------------
SEGMENTS = [
    {
        "id": 0,
        "name": "Intro",
        "t_start": 0.0,
        "t_end": 8.0,
        "audio_file": "Docs/audio/showcase/05-pad-slow-horizon.wav",
        "audio_start": 0.0,
        "badge": "SYNTHESIZER SHOWCASE",
        "title": "YOUKNOW SYNTHESIZER",
        "subtitle": "Six-Voice Circuit-Modelled Architecture · Inspired by the 1984 Roland Juno-106",
        "preset": "PRESET: YP2 SLOW HORIZON · 96 kHz CAPTURE",
        "callout": "DISCRETE 6-VOICE POLYPHONY",
        "cam_target": (680, 359, 1.00), # Full overview, static
        "highlight_box": None,
    },
    {
        "id": 1,
        "name": "Chorus Strings",
        "t_start": 8.0,
        "t_end": 17.0,
        "audio_file": "Docs/audio/showcase/03-strings-silver-canopy.wav",
        "audio_start": 0.0,
        "badge": "DEMO 01 / 04 · CHORUS & STRINGS",
        "title": "DUAL BBD STEREO CHORUS",
        "subtitle": "Authentic MN3009 bucket-brigade delay lines · Modes I, II & I+II · Calibrated noise floor",
        "preset": "PRESET: B11 STRINGS · CHORUS MODE II",
        "callout": "VINTAGE STEREO SPATIALIZATION",
        "cam_target": (1150, 240, 1.80), # Zoom into Chorus & DCO right
        "highlight_box": (1080, 90, 1330, 440),
    },
    {
        "id": 2,
        "name": "Bass & Unison",
        "t_start": 17.0,
        "t_end": 26.0,
        "audio_file": "Docs/audio/showcase/01-bass-midnight-drive.wav",
        "audio_start": 0.0,
        "badge": "DEMO 02 / 04 · BASS & UNISON",
        "title": "16′ DCO & SUB-OSCILLATOR",
        "subtitle": "Six-voice unison mode · High-pass ladder bass boost · Discrete voice card tolerances",
        "preset": "PRESET: YB4 RUBBER BASS · 6-VOICE UNISON",
        "callout": "MASSIVE UNISON LOW-END WEIGHT",
        "cam_target": (440, 240, 1.80), # Zoom into DCO Range & Sub
        "highlight_box": (280, 90, 680, 440),
    },
    {
        "id": 3,
        "name": "Filter & Lead",
        "t_start": 26.0,
        "t_end": 35.0,
        "audio_file": "Docs/audio/showcase/02-lead-signal-fire.wav",
        "audio_start": 0.0,
        "badge": "DEMO 03 / 04 · FILTER & LEAD",
        "title": "IR3109 4-POLE OTA FILTER",
        "subtitle": "Self-oscillating nonlinear ladder · Implicit loop solver · True temperature drift & portamento",
        "preset": "PRESET: A53 LEAD III · MONO SOLO",
        "callout": "SELF-OSCILLATING RESONANCE",
        "cam_target": (800, 240, 1.80), # Zoom into VCF
        "highlight_box": (700, 90, 980, 440),
    },
    {
        "id": 4,
        "name": "Full Composition",
        "t_start": 35.0,
        "t_end": 47.0,
        "audio_file": "Docs/audio/composition/youknow-composition.wav",
        "audio_start": 16.8,
        "badge": "DEMO 04 / 04 · FULL ARRANGEMENT",
        "title": "100% YOUKNOW IN ACTION",
        "subtitle": "10 Overdubbed Parts · 144 Factory Presets · Drums, Bass, Leads & Pads · Zero external FX",
        "preset": "COMPOSITION: \"LOW SUN\" · D AEOLIAN / 114 BPM",
        "callout": "10 OVERDUBBED SYNTH TRACKS",
        "cam_target": (680, 359, 1.00), # Full overview, static
        "highlight_box": None,
    },
    {
        "id": 5,
        "name": "Outro",
        "t_start": 47.0,
        "t_end": 54.0,
        "audio_file": "Docs/audio/showcase/05-pad-slow-horizon.wav",
        "audio_start": 28.0,
        "badge": "AVAILABLE NOW",
        "title": "GET YOUKNOW TODAY",
        "subtitle": "Standalone & Plug-in for macOS, Windows and Linux · VST3 · AU · CLAP",
        "preset": "DOWNLOAD: PROTOCODUS.CZ",
        "callout": "FREE & OPEN SOURCE",
        "cam_target": (680, 359, 1.00),
        "highlight_box": None,
    }
]


# ---------------------------------------------------------------------------
# Audio Processing Functions
# ---------------------------------------------------------------------------
def load_audio_slice(path, start_s, dur_s, sr=SAMPLE_RATE):
    """Load a slice of audio resampled to target sample rate using ffmpeg pipe."""
    cmd = [
        'ffmpeg', '-y',
        '-ss', f'{start_s:.3f}',
        '-t', f'{dur_s:.3f}',
        '-i', path,
        '-ar', str(sr),
        '-ac', '2',
        '-f', 'f32le',
        'pipe:1'
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    raw, _ = p.communicate()
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)


def assemble_audio():
    """Build the master soundtrack with seamless crossfades and normalize."""
    print("-> Assembling audio tracks...")
    master = np.zeros((TOTAL_SAMPLES, 2), dtype=np.float32)
    crossfade_s = 0.40
    cf_samples = int(crossfade_s * SAMPLE_RATE)

    for i, seg in enumerate(SEGMENTS):
        dur = seg["t_end"] - seg["t_start"] + (crossfade_s if i < len(SEGMENTS)-1 else 0.0)
        chunk = load_audio_slice(seg["audio_file"], seg["audio_start"], dur).copy()
        n = len(chunk)

        # Crossfade in
        if i == 0:
            fi_n = int(0.5 * SAMPLE_RATE)
            if fi_n < n:
                chunk[:fi_n] *= np.sin(np.linspace(0, np.pi/2, fi_n))[:, None]
        else:
            if cf_samples < n:
                chunk[:cf_samples] *= np.sin(np.linspace(0, np.pi/2, cf_samples))[:, None]

        # Crossfade out
        if i == len(SEGMENTS) - 1:
            fo_n = int(1.2 * SAMPLE_RATE)
            if fo_n < n:
                chunk[-fo_n:] *= np.cos(np.linspace(0, np.pi/2, fo_n))[:, None]
        else:
            if cf_samples < n:
                chunk[-cf_samples:] *= np.cos(np.linspace(0, np.pi/2, cf_samples))[:, None]

        dest_start = int(seg["t_start"] * SAMPLE_RATE)
        dest_end = min(TOTAL_SAMPLES, dest_start + n)
        master[dest_start:dest_end] += chunk[:dest_end - dest_start]

    # Normalize peak to -1.5 dBFS
    peak = np.max(np.abs(master))
    if peak > 0:
        target_peak = 10 ** (-1.5 / 20.0)
        master = master * (target_peak / peak)
    print(f"-> Audio master peak normalized to: {np.max(np.abs(master)):.3f}")

    # Write WAV file
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    raw16 = np.clip(master * 32767.0, -32768, 32767).astype(np.int16)
    import wave
    with wave.open(AUDIO_MASTER_PATH, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(raw16.tobytes())
    print(f"-> Saved master audio: {AUDIO_MASTER_PATH}")
    return master


# ---------------------------------------------------------------------------
# Audio Analytics: FFT Spectrum & Oscilloscope
# ---------------------------------------------------------------------------
class AudioAnalytics:
    def __init__(self, audio_data, num_bands=36, fft_size=2048):
        self.audio = audio_data
        self.num_bands = num_bands
        self.fft_size = fft_size
        self.window = np.hanning(fft_size).astype(np.float32)

        # Logarithmic frequency band boundaries from 35 Hz to 16 kHz
        self.freq_bins = np.logspace(np.log10(35), np.log10(16000), num_bands + 1)
        freq_axis = np.fft.rfftfreq(fft_size, d=1.0 / SAMPLE_RATE)

        self.band_masks = []
        for b in range(num_bands):
            f_low = self.freq_bins[b]
            f_high = self.freq_bins[b + 1]
            mask = (freq_axis >= f_low) & (freq_axis < f_high)
            if not np.any(mask):
                idx = np.searchsorted(freq_axis, (f_low + f_high) / 2.0)
                idx = np.clip(idx, 0, len(freq_axis) - 1)
                mask = np.zeros_like(freq_axis, dtype=bool)
                mask[idx] = True
            self.band_masks.append(mask)

        # State tracking for smoothing & peak holds
        self.smooth_levels = np.zeros(num_bands, dtype=np.float32)
        self.peak_holds = np.zeros(num_bands, dtype=np.float32)

    def get_frame_analytics(self, frame_idx):
        t_center = frame_idx / FPS
        sample_center = int(t_center * SAMPLE_RATE)

        # 1. Oscilloscope chunk (1024 samples)
        osc_len = 1024
        osc_start = sample_center - osc_len // 2
        osc_end = osc_start + osc_len
        if osc_start < 0:
            osc_buf = np.zeros((osc_len, 2), dtype=np.float32)
            valid = max(0, osc_end)
            if valid > 0:
                osc_buf[-valid:] = self.audio[:valid]
        elif osc_end > len(self.audio):
            osc_buf = np.zeros((osc_len, 2), dtype=np.float32)
            valid = max(0, len(self.audio) - osc_start)
            if valid > 0:
                osc_buf[:valid] = self.audio[osc_start:osc_start + valid]
        else:
            osc_buf = self.audio[osc_start:osc_end]

        # 2. FFT Spectrum (fft_size samples)
        fft_start = sample_center - self.fft_size // 2
        fft_end = fft_start + self.fft_size
        if fft_start < 0 or fft_end > len(self.audio):
            fft_buf = np.zeros((self.fft_size, 2), dtype=np.float32)
            s_clamp = max(0, fft_start)
            e_clamp = min(len(self.audio), fft_end)
            if e_clamp > s_clamp:
                dest_s = s_clamp - fft_start
                dest_e = dest_s + (e_clamp - s_clamp)
                fft_buf[dest_s:dest_e] = self.audio[s_clamp:e_clamp]
        else:
            fft_buf = self.audio[fft_start:fft_end]

        mono = np.mean(fft_buf, axis=1) * self.window
        spec = np.abs(np.fft.rfft(mono)) / (self.fft_size / 2)

        band_energies = np.zeros(self.num_bands, dtype=np.float32)
        for b, mask in enumerate(self.band_masks):
            e = np.max(spec[mask]) if np.any(mask) else 0.0
            db = 20.0 * np.log10(max(e, 1e-5))
            val = np.clip((db + 65.0) / 60.0, 0.0, 1.0)
            band_energies[b] = val

        attack = 0.70
        decay = 0.18
        self.smooth_levels = np.where(band_energies > self.smooth_levels,
                                      self.smooth_levels + attack * (band_energies - self.smooth_levels),
                                      self.smooth_levels - decay * (self.smooth_levels - band_energies))
        self.smooth_levels = np.clip(self.smooth_levels, 0.0, 1.0)
        self.peak_holds = np.maximum(self.smooth_levels, self.peak_holds - 0.012)

        rms_l = float(np.sqrt(np.mean(osc_buf[:, 0]**2)))
        rms_r = float(np.sqrt(np.mean(osc_buf[:, 1]**2)))
        vu_l = np.clip((20.0 * np.log10(max(rms_l, 1e-4)) + 48.0) / 45.0, 0.0, 1.0)
        vu_r = np.clip((20.0 * np.log10(max(rms_r, 1e-4)) + 48.0) / 45.0, 0.0, 1.0)

        return osc_buf, self.smooth_levels, self.peak_holds, vu_l, vu_r


# ---------------------------------------------------------------------------
# Visual Compositing Engine
# ---------------------------------------------------------------------------
class VideoCompositor:
    def __init__(self):
        print("-> Initializing visual compositor...")
        self.screenshot = cv2.imread(SCREENSHOT_PATH)
        if self.screenshot is None:
            raise FileNotFoundError(f"Missing {SCREENSHOT_PATH}")
        self.ss_h, self.ss_w = self.screenshot.shape[:2]

        self.icon = cv2.imread(ICON_PATH, cv2.IMREAD_UNCHANGED)

        # Load fonts - substantially larger for high readability
        self.font_title = ImageFont.truetype(FONT_BOLD_PATH, 50)       # Big prominent title (was 34)
        self.font_sub = ImageFont.truetype(FONT_REGULAR_PATH, 24)      # Clear descriptive sub (was 19)
        self.font_badge = ImageFont.truetype(FONT_BOLD_PATH, 20)      # Readable section badge (was 14)
        self.font_preset = ImageFont.truetype(FONT_BOLD_PATH, 22)     # Distinct preset readout (was 15)
        self.font_callout = ImageFont.truetype(FONT_BOLD_PATH, 24)    # Prominent callout pill
        self.font_header_title = ImageFont.truetype(FONT_BOLD_PATH, 42)
        self.font_header_sub = ImageFont.truetype(FONT_BOLD_PATH, 18)
        self.font_header_pill = ImageFont.truetype(FONT_BOLD_PATH, 19)
        self.font_outro_title = ImageFont.truetype(FONT_BOLD_PATH, 70) # Huge outro title (was 52)
        self.font_outro_sub = ImageFont.truetype(FONT_BOLD_PATH, 34)
        self.font_outro_body = ImageFont.truetype(FONT_BOLD_PATH, 28)
        self.font_outro_small = ImageFont.truetype(FONT_REGULAR_PATH, 23)
        self.font_outro_url = ImageFont.truetype(FONT_BOLD_PATH, 46)

        # Vignette mask
        self.vignette = self._build_vignette()

        # Pre-render text overlays for each segment
        self.segment_overlays = self._prerender_segment_text()
        self.callout_overlays = self._prerender_callouts()

        # Icons
        self.icon_small = self._prepare_icon(64)
        self.icon_outro = self._prepare_icon(220)

    def _build_vignette(self):
        y, x = np.ogrid[:VIDEO_HEIGHT, :VIDEO_WIDTH]
        cx, cy = VIDEO_WIDTH / 2.0, VIDEO_HEIGHT / 2.0
        dist = np.sqrt((x - cx)**2 + (y - cy)**2)
        max_dist = np.sqrt(cx**2 + cy**2)
        vig = 1.0 - 0.45 * (dist / max_dist)**1.6
        vig = np.clip(vig, 0.45, 1.0).astype(np.float32)
        return vig[:, :, None]

    def _prepare_icon(self, size):
        if self.icon is None:
            return None
        return cv2.resize(self.icon, (size, size), interpolation=cv2.INTER_AREA)

    def _prerender_segment_text(self):
        overlays = []
        for seg in SEGMENTS:
            banner = Image.new("RGBA", (1220, 235), (0, 0, 0, 0))
            draw = ImageDraw.Draw(banner)

            # Badge pill
            badge_text = seg["badge"]
            bbox = draw.textbbox((0, 0), badge_text, font=self.font_badge)
            bw = bbox[2] - bbox[0] + 28
            bh = 34
            draw.rounded_rectangle([0, 0, bw, bh], radius=6, fill=(30, 42, 54, 245), outline=RGB_CYAN, width=2)
            draw.text((14, 5), badge_text, font=self.font_badge, fill=RGB_CYAN)

            # Title - Big & Bold
            draw.text((0, 46), seg["title"], font=self.font_title, fill=RGB_WHITE)

            # Subtitle - Crisp & Clear
            draw.text((0, 114), seg["subtitle"], font=self.font_sub, fill=RGB_GRAY)

            # Preset / Info
            draw.text((0, 160), seg["preset"], font=self.font_preset, fill=RGB_ORANGE)

            overlays.append(np.array(banner))
        return overlays

    def _prerender_callouts(self):
        overlays = []
        for seg in SEGMENTS:
            callout_text = seg["callout"]
            # Create pill image
            dummy = Image.new("RGBA", (10, 10))
            ddraw = ImageDraw.Draw(dummy)
            bbox = ddraw.textbbox((0, 0), callout_text, font=self.font_callout)
            tw = bbox[2] - bbox[0]
            th = bbox[3] - bbox[1]

            pill_w = tw + 52
            pill_h = 44
            pill = Image.new("RGBA", (pill_w, pill_h), (0, 0, 0, 0))
            pdraw = ImageDraw.Draw(pill)

            pdraw.rounded_rectangle([0, 0, pill_w, pill_h], radius=8, fill=(20, 24, 30, 240), outline=RGB_ORANGE, width=2)
            # Glowing dot
            pdraw.ellipse([14, pill_h // 2 - 6, 26, pill_h // 2 + 6], fill=RGB_CYAN)
            pdraw.text((36, 7), callout_text, font=self.font_callout, fill=RGB_WHITE)

            overlays.append(np.array(pill))
        return overlays

    def _get_camera(self, t, current_seg_idx):
        """
        Rock-solid camera controller:
        Smooth 0.45s ease when transitioning to a zoomed-in section,
        then stays COMPLETELY STATIC throughout the demo. Zero wobble or breathing.
        """
        current_seg = SEGMENTS[current_seg_idx]
        tgt = current_seg["cam_target"]
        if current_seg_idx == 0:
            return tgt

        prev_tgt = SEGMENTS[current_seg_idx - 1]["cam_target"]
        trans_dur = 0.45
        t_start = current_seg["t_start"]

        if t < t_start + trans_dur:
            prog = (t - t_start) / trans_dur
            ease = 0.5 - 0.5 * math.cos(math.pi * prog)
            cx = prev_tgt[0] + (tgt[0] - prev_tgt[0]) * ease
            cy = prev_tgt[1] + (tgt[1] - prev_tgt[1]) * ease
            zoom = prev_tgt[2] + (tgt[2] - prev_tgt[2]) * ease
            return (cx, cy, zoom)
        else:
            return tgt

    def render_frame(self, frame_idx, osc_buf, spectrum, peak_holds, vu_l, vu_r):
        t = frame_idx / FPS

        # Identify current segment
        current_seg = SEGMENTS[-1]
        current_seg_idx = len(SEGMENTS) - 1
        for i, seg in enumerate(SEGMENTS):
            if seg["t_start"] <= t < seg["t_end"]:
                current_seg = seg
                current_seg_idx = i
                break

        # Base frame: dark slate
        frame = np.full((VIDEO_HEIGHT, VIDEO_WIDTH, 3), BGR_BG_DARK, dtype=np.uint8)

        # -------------------------------------------------------------------
        # 1. Center Stage (y = 90 to 800, height = 710)
        # -------------------------------------------------------------------
        stage_y = 90
        stage_h = 710
        stage_w = 1920

        if current_seg_idx < 5:
            # Camera viewport: solid and stable
            cx, cy, zoom = self._get_camera(t, current_seg_idx)

            view_h = int(720 / zoom)
            view_w = int(view_h * (1920.0 / 710.0))

            x1 = int(cx - view_w / 2)
            y1 = int(cy - view_h / 2)
            x2 = x1 + view_w
            y2 = y1 + view_h

            pad_left = max(0, -x1)
            pad_top = max(0, -y1)
            pad_right = max(0, x2 - self.ss_w)
            pad_bottom = max(0, y2 - self.ss_h)

            src_x1 = max(0, x1)
            src_y1 = max(0, y1)
            src_x2 = min(self.ss_w, x2)
            src_y2 = min(self.ss_h, y2)

            crop = self.screenshot[src_y1:src_y2, src_x1:src_x2]
            if pad_left > 0 or pad_top > 0 or pad_right > 0 or pad_bottom > 0:
                crop = cv2.copyMakeBorder(crop, pad_top, pad_bottom, pad_left, pad_right,
                                          cv2.BORDER_CONSTANT, value=BGR_BG_DARK)

            synth_stage = cv2.resize(crop, (stage_w, stage_h), interpolation=cv2.INTER_LINEAR)

            # Gentle edge shading
            fade_top = np.linspace(0.0, 1.0, 50)[:, None, None]
            synth_stage[:50] = (synth_stage[:50] * fade_top).astype(np.uint8)
            fade_bottom = np.linspace(1.0, 0.0, 50)[:, None, None]
            synth_stage[-50:] = (synth_stage[-50:] * fade_bottom).astype(np.uint8)

            frame[stage_y:stage_y + stage_h, 0:stage_w] = synth_stage

            # Section highlight brackets
            box = current_seg["highlight_box"]
            if box is not None:
                bx1, by1, bx2, by2 = box
                sx1 = int((bx1 - x1) * (stage_w / view_w))
                sy1 = int((by1 - y1) * (stage_h / view_h)) + stage_y
                sx2 = int((bx2 - x1) * (stage_w / view_w))
                sy2 = int((by2 - y1) * (stage_h / view_h)) + stage_y

                sx1 = max(0, min(VIDEO_WIDTH-1, sx1))
                sx2 = max(0, min(VIDEO_WIDTH-1, sx2))
                sy1 = max(stage_y, min(stage_y + stage_h, sy1))
                sy2 = max(stage_y, min(stage_y + stage_h, sy2))

                if sx2 > sx1 and sy2 > sy1:
                    pulse = 0.70 + 0.30 * math.sin(t * 4.0)
                    glow_color = (int(53 * pulse), int(107 * pulse), int(255 * pulse))
                    bracket_len = 28
                    # Top-left
                    cv2.line(frame, (sx1, sy1), (sx1 + bracket_len, sy1), glow_color, 3, cv2.LINE_AA)
                    cv2.line(frame, (sx1, sy1), (sx1, sy1 + bracket_len), glow_color, 3, cv2.LINE_AA)
                    # Top-right
                    cv2.line(frame, (sx2, sy1), (sx2 - bracket_len, sy1), glow_color, 3, cv2.LINE_AA)
                    cv2.line(frame, (sx2, sy1), (sx2, sy1 + bracket_len), glow_color, 3, cv2.LINE_AA)
                    # Bottom-left
                    cv2.line(frame, (sx1, sy2), (sx1 + bracket_len, sy2), glow_color, 3, cv2.LINE_AA)
                    cv2.line(frame, (sx1, sy2), (sx1, sy2 - bracket_len), glow_color, 3, cv2.LINE_AA)
                    # Bottom-right
                    cv2.line(frame, (sx2, sy2), (sx2 - bracket_len, sy2), glow_color, 3, cv2.LINE_AA)
                    cv2.line(frame, (sx2, sy2), (sx2, sy2 - bracket_len), glow_color, 3, cv2.LINE_AA)

            # Prominent Callout Pill on top right of the stage
            pill = self.callout_overlays[current_seg_idx]
            ph, pw = pill.shape[:2]
            px = VIDEO_WIDTH - pw - 40
            py = stage_y + 24
            alpha = pill[:, :, 3] / 255.0
            for c in range(3):
                frame[py:py+ph, px:px+pw, c] = (
                    pill[:, :, 2 - c] * alpha +
                    frame[py:py+ph, px:px+pw, c] * (1.0 - alpha)
                ).astype(np.uint8)

            # In segment 4 (multitrack composition), show active voice cards
            if current_seg_idx == 4:
                self._draw_voice_leds(frame, t, spectrum)

        else:
            # Outro card (Segment 5) - static, clean, bold
            self._draw_outro_card(frame)

        # Apply dark vignette over the stage
        frame = (frame.astype(np.float32) * self.vignette).astype(np.uint8)

        # -------------------------------------------------------------------
        # 2. Top Header Bar (y: 0 to 90)
        # -------------------------------------------------------------------
        self._draw_header(frame, osc_buf, vu_l, vu_r)

        # -------------------------------------------------------------------
        # 3. Bottom HUD & Visualizers (y: 800 to 1055)
        # -------------------------------------------------------------------
        self._draw_hud(frame, current_seg_idx, spectrum, peak_holds)

        # -------------------------------------------------------------------
        # 4. Progress Timeline Bar (y: 1060 to 1080)
        # -------------------------------------------------------------------
        self._draw_progress_bar(frame, t)

        return frame

    def _draw_header(self, frame, osc_buf, vu_l, vu_r):
        cv2.rectangle(frame, (0, 0), (VIDEO_WIDTH, 90), (14, 16, 18), -1)
        cv2.line(frame, (0, 90), (VIDEO_WIDTH, 90), (45, 50, 58), 1)

        # Synth Icon on left
        if self.icon_small is not None:
            h, w = self.icon_small.shape[:2]
            y_off = 13
            x_off = 40
            if self.icon_small.shape[2] == 4:
                alpha = self.icon_small[:, :, 3] / 255.0
                for c in range(3):
                    frame[y_off:y_off+h, x_off:x_off+w, c] = (
                        self.icon_small[:, :, c] * alpha +
                        frame[y_off:y_off+h, x_off:x_off+w, c] * (1.0 - alpha)
                    ).astype(np.uint8)
            else:
                frame[y_off:y_off+h, x_off:x_off+w] = self.icon_small[:, :, :3]

        # Branding text via PIL for sharp typography
        pil_header = Image.new("RGBA", (700, 80), (0, 0, 0, 0))
        hdraw = ImageDraw.Draw(pil_header)
        hdraw.text((0, 4), "YOUKNOW", font=self.font_header_title, fill=RGB_WHITE)
        hdraw.text((230, 22), "CIRCUIT-MODELLED 6-VOICE SYNTHESIZER", font=self.font_header_sub, fill=RGB_CYAN)

        # Blit branding text
        hdr_arr = np.array(pil_header)
        hh, hw = hdr_arr.shape[:2]
        ha = hdr_arr[:, :, 3] / 255.0
        for c in range(3):
            frame[5:5+hh, 118:118+hw, c] = (
                hdr_arr[:, :, 2 - c] * ha +
                frame[5:5+hh, 118:118+hw, c] * (1.0 - ha)
            ).astype(np.uint8)

        # Stereo Oscilloscope on the right
        osc_x = 1370
        osc_y = 15
        osc_w = 330
        osc_h = 60
        cv2.rectangle(frame, (osc_x, osc_y), (osc_x + osc_w, osc_y + osc_h), (20, 23, 28), -1)
        cv2.rectangle(frame, (osc_x, osc_y), (osc_x + osc_w, osc_y + osc_h), (48, 54, 62), 1)
        cv2.line(frame, (osc_x, osc_y + osc_h // 2), (osc_x + osc_w, osc_y + osc_h // 2), (32, 36, 42), 1)

        step = len(osc_buf) / float(osc_w)
        pts_l = []
        pts_r = []
        for i in range(osc_w):
            idx = int(i * step)
            val_l = osc_buf[idx, 0]
            val_r = osc_buf[idx, 1]
            yl = int(osc_y + osc_h // 2 - val_l * (osc_h * 0.45))
            yr = int(osc_y + osc_h // 2 - val_r * (osc_h * 0.45))
            yl = max(osc_y + 2, min(osc_y + osc_h - 2, yl))
            yr = max(osc_y + 2, min(osc_y + osc_h - 2, yr))
            pts_l.append((osc_x + i, yl))
            pts_r.append((osc_x + i, yr))

        if len(pts_l) > 1:
            cv2.polylines(frame, [np.array(pts_l, dtype=np.int32)], False, BGR_CYAN, 1, cv2.LINE_AA)
            cv2.polylines(frame, [np.array(pts_r, dtype=np.int32)], False, BGR_ORANGE, 1, cv2.LINE_AA)

        cv2.putText(frame, "STEREO OSCILLOSCOPE", (osc_x + 8, osc_y + 14), cv2.FONT_HERSHEY_SIMPLEX, 0.34, BGR_GRAY, 1, cv2.LINE_AA)

        # Mini Stereo VU Meters
        vu_x = 1730
        vu_y = 20
        vu_w = 140
        vu_h = 20
        self._draw_vu_bar(frame, vu_x, vu_y, vu_w, vu_h, vu_l, "L")
        self._draw_vu_bar(frame, vu_x, vu_y + 28, vu_w, vu_h, vu_r, "R")

    def _draw_vu_bar(self, frame, x, y, w, h, level, label):
        cv2.putText(frame, label, (x - 18, y + 15), cv2.FONT_HERSHEY_SIMPLEX, 0.42, BGR_GRAY, 1, cv2.LINE_AA)
        cv2.rectangle(frame, (x, y), (x + w, y + h), (20, 22, 26), -1)
        cv2.rectangle(frame, (x, y), (x + w, y + h), (45, 50, 58), 1)

        num_leds = 18
        led_gap = 2
        led_w = (w - (num_leds + 1) * led_gap) / float(num_leds)
        active_count = int(level * num_leds)

        for i in range(num_leds):
            lx1 = int(x + led_gap + i * (led_w + led_gap))
            lx2 = int(lx1 + led_w)
            if i < active_count:
                if i < 12:
                    color = BGR_GREEN
                elif i < 15:
                    color = BGR_YELLOW
                else:
                    color = BGR_RED
            else:
                color = (30, 34, 38)
            cv2.rectangle(frame, (lx1, y + 2), (lx2, y + h - 2), color, -1)

    def _draw_hud(self, frame, seg_idx, spectrum, peak_holds):
        hud_y = 800
        hud_h = 255
        cv2.rectangle(frame, (0, hud_y), (VIDEO_WIDTH, hud_y + hud_h), (13, 15, 17), -1)
        cv2.line(frame, (0, hud_y), (VIDEO_WIDTH, hud_y), (48, 54, 64), 1)

        # Blit enlarged pre-rendered segment text
        overlay = self.segment_overlays[seg_idx]
        oh, ow = overlay.shape[:2]
        dest_x = 50
        dest_y = hud_y + 16

        alpha = overlay[:, :, 3] / 255.0
        for c in range(3):
            frame[dest_y:dest_y+oh, dest_x:dest_x+ow, c] = (
                overlay[:, :, 2 - c] * alpha +
                frame[dest_y:dest_y+oh, dest_x:dest_x+ow, c] * (1.0 - alpha)
            ).astype(np.uint8)

        # 36-Band Logarithmic Spectrum Analyzer on the right
        spec_x = 1270
        spec_y = hud_y + 20
        spec_w = 600
        spec_h = 200

        cv2.rectangle(frame, (spec_x, spec_y), (spec_x + spec_w, spec_y + spec_h), (18, 20, 24), -1)
        cv2.rectangle(frame, (spec_x, spec_y), (spec_x + spec_w, spec_y + spec_h), (48, 54, 64), 1)

        for db_line in [0.25, 0.50, 0.75]:
            gy = int(spec_y + spec_h * (1.0 - db_line))
            cv2.line(frame, (spec_x, gy), (spec_x + spec_w, gy), (28, 32, 38), 1)

        cv2.putText(frame, "36-BAND SPECTRUM ANALYZER", (spec_x + 14, spec_y + 22),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.42, BGR_GRAY, 1, cv2.LINE_AA)
        cv2.putText(frame, "35 Hz", (spec_x + 14, spec_y + spec_h - 8),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.36, BGR_MUTED, 1, cv2.LINE_AA)
        cv2.putText(frame, "16 kHz", (spec_x + spec_w - 55, spec_y + spec_h - 8),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.36, BGR_MUTED, 1, cv2.LINE_AA)

        num_bars = len(spectrum)
        bar_gap = 3
        bar_w = (spec_w - 24 - (num_bars - 1) * bar_gap) / float(num_bars)
        inner_bottom = spec_y + spec_h - 24
        max_bar_h = spec_h - 52

        for b in range(num_bars):
            bx1 = int(spec_x + 12 + b * (bar_w + bar_gap))
            bx2 = int(bx1 + bar_w)
            bh = int(spectrum[b] * max_bar_h)
            by1 = inner_bottom - bh

            if bh > 0:
                t_col = b / float(num_bars)
                if t_col < 0.45:
                    c = BGR_CYAN
                elif t_col < 0.80:
                    c = BGR_ORANGE
                else:
                    c = (80, 80, 255)
                cv2.rectangle(frame, (bx1, by1), (bx2, inner_bottom), c, -1)

            ph = int(peak_holds[b] * max_bar_h)
            py_cap = inner_bottom - ph
            if ph > 2:
                cv2.line(frame, (bx1, py_cap), (bx2, py_cap), BGR_WHITE, 2, cv2.LINE_AA)

    def _draw_progress_bar(self, frame, t):
        bar_y = 1060
        bar_h = 10
        progress = np.clip(t / TOTAL_DURATION, 0.0, 1.0)

        cv2.rectangle(frame, (0, bar_y), (VIDEO_WIDTH, bar_y + bar_h), (25, 28, 32), -1)

        for seg in SEGMENTS:
            nx = int((seg["t_start"] / TOTAL_DURATION) * VIDEO_WIDTH)
            cv2.line(frame, (nx, bar_y - 2), (nx, bar_y + bar_h + 2), (65, 72, 82), 1)

        fill_w = int(progress * VIDEO_WIDTH)
        if fill_w > 0:
            cv2.rectangle(frame, (0, bar_y), (fill_w, bar_y + bar_h), BGR_ORANGE, -1)
            cv2.circle(frame, (fill_w, bar_y + bar_h // 2), 6, BGR_WHITE, -1, cv2.LINE_AA)

        mins = int(t) // 60
        secs = int(t) % 60
        total_m = int(TOTAL_DURATION) // 60
        total_s = int(TOTAL_DURATION) % 60
        tc = f"{mins:02d}:{secs:02d} / {total_m:02d}:{total_s:02d}"
        cv2.putText(frame, tc, (VIDEO_WIDTH - 120, bar_y - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.40, BGR_GRAY, 1, cv2.LINE_AA)

    def _draw_voice_leds(self, frame, t, spectrum):
        center_x = VIDEO_WIDTH // 2
        y = 730
        led_w = 48
        gap = 16
        total_w = 6 * led_w + 5 * gap
        start_x = center_x - total_w // 2

        cv2.rectangle(frame, (start_x - 18, y - 24), (start_x + total_w + 18, y + 28), (18, 22, 26), -1)
        cv2.rectangle(frame, (start_x - 18, y - 24), (start_x + total_w + 18, y + 28), (48, 56, 66), 1)
        cv2.putText(frame, "ACTIVE VOICES (DISCRETE 6-VOICE CARDS)", (start_x, y - 9), cv2.FONT_HERSHEY_SIMPLEX, 0.40, BGR_GRAY, 1, cv2.LINE_AA)

        for v in range(6):
            lx = start_x + v * (led_w + gap)
            energy = spectrum[(v * 5) % len(spectrum)]
            is_active = energy > 0.32
            color = BGR_ORANGE if is_active else (38, 42, 48)
            cv2.rectangle(frame, (lx, y), (lx + led_w, y + 16), color, -1)
            label = f"V{v+1}"
            cv2.putText(frame, label, (lx + 11, y + 13), cv2.FONT_HERSHEY_SIMPLEX, 0.40, BGR_WHITE if is_active else BGR_MUTED, 1, cv2.LINE_AA)

    def _draw_outro_card(self, frame):
        """Clean, bold, rock-solid outro card."""
        center_x = VIDEO_WIDTH // 2
        center_y = 440

        # Centered Icon - big and sharp
        if self.icon_outro is not None:
            h, w = self.icon_outro.shape[:2]
            ix = center_x - w // 2
            iy = center_y - 290
            if self.icon_outro.shape[2] == 4:
                alpha = self.icon_outro[:, :, 3] / 255.0
                for c in range(3):
                    frame[iy:iy+h, ix:ix+w, c] = (
                        self.icon_outro[:, :, c] * alpha +
                        frame[iy:iy+h, ix:ix+w, c] * (1.0 - alpha)
                    ).astype(np.uint8)

        # PIL overlay for large, crisp typography
        pil_frame = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
        draw = ImageDraw.Draw(pil_frame)

        # Title - 70pt bold!
        t_title = "YOUKNOW SYNTHESIZER"
        bbox = draw.textbbox((0, 0), t_title, font=self.font_outro_title)
        tw = bbox[2] - bbox[0]
        draw.text((center_x - tw // 2, center_y - 50), t_title, font=self.font_outro_title, fill=RGB_WHITE)

        # Subtitle - 34pt bold orange
        t_sub = "Circuit-Modelled 1984 Polyphonic Icon"
        bbox = draw.textbbox((0, 0), t_sub, font=self.font_outro_sub)
        tw = bbox[2] - bbox[0]
        draw.text((center_x - tw // 2, center_y + 30), t_sub, font=self.font_outro_sub, fill=RGB_ORANGE)

        # Platform Badges - 28pt bold white
        platforms = ["VST3", "Audio Unit (AU)", "CLAP", "Standalone"]
        p_str = "   ·   ".join(platforms)
        bbox = draw.textbbox((0, 0), p_str, font=self.font_outro_body)
        tw = bbox[2] - bbox[0]
        draw.text((center_x - tw // 2, center_y + 90), p_str, font=self.font_outro_body, fill=RGB_WHITE)

        # OS support - 23pt gray
        os_str = "macOS (Apple Silicon & Intel)   |   Windows 10/11 x64   |   Linux x64"
        bbox = draw.textbbox((0, 0), os_str, font=self.font_outro_small)
        tw = bbox[2] - bbox[0]
        draw.text((center_x - tw // 2, center_y + 135), os_str, font=self.font_outro_small, fill=RGB_GRAY)

        # Open source notice
        lic_str = "100% Free & Open Source   ·   MIT License   ·   Zero Telemetry"
        bbox = draw.textbbox((0, 0), lic_str, font=self.font_outro_small)
        tw = bbox[2] - bbox[0]
        draw.text((center_x - tw // 2, center_y + 175), lic_str, font=self.font_outro_small, fill=RGB_MUTED)

        # Website link - 46pt bold cyan
        site_str = "protocodus.cz"
        bbox = draw.textbbox((0, 0), site_str, font=self.font_outro_url)
        tw = bbox[2] - bbox[0]
        draw.text((center_x - tw // 2, center_y + 225), site_str, font=self.font_outro_url, fill=RGB_CYAN)

        frame[:] = cv2.cvtColor(np.array(pil_frame), cv2.COLOR_RGB2BGR)


# ---------------------------------------------------------------------------
# Main Video Rendering Pipeline
# ---------------------------------------------------------------------------
def render_promo_video():
    print(f"============================================================")
    print(f" Generating YouKnow Promo Video (1080p60 · {TOTAL_DURATION}s)")
    print(f"============================================================")

    # 1. Assemble audio
    audio_data = assemble_audio()

    # 2. Initialize analytics and compositor
    analytics = AudioAnalytics(audio_data)
    compositor = VideoCompositor()

    # 3. Setup FFmpeg encoding pipe
    ffmpeg_cmd = [
        'ffmpeg', '-y',
        '-f', 'rawvideo',
        '-vcodec', 'rawvideo',
        '-s', f'{VIDEO_WIDTH}x{VIDEO_HEIGHT}',
        '-pix_fmt', 'bgr24',
        '-r', str(FPS),
        '-i', '-',
        '-i', AUDIO_MASTER_PATH,
        '-c:v', 'libx264',
        '-preset', 'medium',
        '-crf', '18',
        '-pix_fmt', 'yuv420p',
        '-c:a', 'aac',
        '-b:a', '320k',
        '-shortest',
        VIDEO_OUTPUT_PATH
    ]

    print(f"-> Starting FFmpeg piped encoder...")
    proc = subprocess.Popen(ffmpeg_cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    t0 = time.time()
    last_print = t0

    # 4. Render loop
    for f in range(TOTAL_FRAMES):
        osc_buf, spec, peak_holds, vu_l, vu_r = analytics.get_frame_analytics(f)
        frame = compositor.render_frame(f, osc_buf, spec, peak_holds, vu_l, vu_r)

        proc.stdin.write(frame.tobytes())

        # Progress reporting
        now = time.time()
        if now - last_print > 2.0 or f == TOTAL_FRAMES - 1:
            fps_actual = (f + 1) / (now - t0)
            percent = (f + 1) / TOTAL_FRAMES * 100.0
            eta = (TOTAL_FRAMES - f - 1) / max(1.0, fps_actual)
            print(f"   [Frame {f+1:4d}/{TOTAL_FRAMES}] ({percent:5.1f}%) - {fps_actual:5.1f} fps - ETA: {eta:4.1f}s")
            last_print = now

    proc.stdin.close()
    proc.wait()

    total_time = time.time() - t0
    print(f"-> Video encoding complete in {total_time:.1f}s ({TOTAL_FRAMES/total_time:.1f} avg fps)")
    print(f"-> Saved: {VIDEO_OUTPUT_PATH}")

    # Check file size
    if os.path.exists(VIDEO_OUTPUT_PATH):
        size_mb = os.path.getsize(VIDEO_OUTPUT_PATH) / (1024 * 1024)
        print(f"-> Output file size: {size_mb:.2f} MB")


if __name__ == "__main__":
    render_promo_video()
