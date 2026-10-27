"""Make the ambient sound loops (game/assets/enhanced/sounds/*.wav) with ElevenLabs' sound effects.

  python scripts/enhanced/ambient_sounds.py [name ...]   (no names: the missing ones)
  python scripts/enhanced/ambient_sounds.py --force rain_light   (asks again)
  python scripts/enhanced/ambient_sounds.py --convert             (the downloads again, no request)

The key comes from ELEVENLABS_API_KEY. Each sound is asked for as a seamless loop and converted
with ffmpeg to 48 kHz stereo 16-bit WAV, the format the game's mixer plays (game/sound/common/
enhanced_ambience.cpp). The raw downloads stay in local/ambience (not in git).
"""
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "game" / "assets" / "enhanced" / "sounds"
RAW = ROOT / "local" / "ambience"

# name: (prompt, seconds). Kept free of music and of single loud events, so they loop unnoticed.
# The loops come back at very different levels; each is brought to its own loudness (LUFS), the
# mix between them (heavy rain over light, crickets well under the game)
LOUDNESS = {"rain_light": -25, "rain_heavy": -19, "drips": -28, "wind_night": -26, "wind_storm": -20,
            "crickets": -28}
SOUNDS = {
    "rain_light": ("Gentle steady rain falling on grass, leaves and stone ground, soft constant "
                   "patter, calm ambience, no thunder, no music", 20),
    "rain_heavy": ("Heavy downpour, rain drumming on rooftops and splashing into water and puddles, "
                   "dense constant roar, ambience, no thunder, no music", 20),
    "drips": ("Water dripping after the rain has stopped: sparse drops falling from roofs and leaves "
              "into puddles, quiet and calm, no rain, no music", 20),
    "wind_night": ("Cold night wind blowing across open desert dunes, low soft howl and sand hiss, "
                   "slow gusts, ambience, no music", 20),
    "wind_storm": ("Strong gusty wind storm, loud howling gusts and blowing sand and debris, "
                   "ambience, no thunder, no music", 20),
    "crickets": ("Crickets chirping at night in a meadow, calm warm summer night ambience, a few "
                 "distant, no birds, no music", 20),
}


def generate(name, prompt, seconds):
    key = os.environ.get("ELEVENLABS_API_KEY")
    if not key:
        sys.exit("ELEVENLABS_API_KEY is not set")
    body = json.dumps({
        "text": prompt,
        "duration_seconds": seconds,
        "prompt_influence": 0.5,
        "loop": True,
        "model_id": "eleven_text_to_sound_v2",
    }).encode()
    req = urllib.request.Request(
        "https://api.elevenlabs.io/v1/sound-generation?output_format=mp3_44100_192",
        data=body, headers={"xi-api-key": key, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        sys.exit(f"{name}: ElevenLabs said {e.code}: {e.read().decode(errors='replace')[:400]}")
    RAW.mkdir(parents=True, exist_ok=True)
    raw = RAW / f"{name}.mp3"
    raw.write_bytes(data)
    return raw


def loudness(path):
    """Integrated loudness (LUFS)."""
    r = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(path), "-af", "ebur128=framelog=quiet",
                        "-f", "null", "-"], capture_output=True, text=True)
    lines = [l for l in r.stderr.splitlines() if l.strip().startswith("I:")]
    return float(lines[-1].split()[1])


def convert(raw, name):
    OUT.mkdir(parents=True, exist_ok=True)
    wav = OUT / f"{name}.wav"
    gain = LOUDNESS[name] - loudness(raw)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(raw), "-af",
                    f"volume={gain:.1f}dB,alimiter=limit=0.9:level=false", "-ar", "48000", "-ac", "2",
                    "-c:a", "pcm_s16le", str(wav)], check=True)
    return wav


def main():
    args = sys.argv[1:]
    force = "--force" in args
    names = [a for a in args if not a.startswith("--")] or list(SOUNDS)
    for name in names:
        prompt, seconds = SOUNDS[name]
        if (OUT / f"{name}.wav").exists() and not force and "--convert" not in args:
            print(f"{name}: there already")
            continue
        raw = RAW / f"{name}.mp3"
        if force or not raw.exists():
            raw = generate(name, prompt, seconds)
        wav = convert(raw, name)
        print(f"{name}: {wav.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
