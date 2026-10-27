"""Make the ambient sound loops (game/assets/enhanced/sounds/*.wav) with ElevenLabs' sound effects.

  python scripts/enhanced/ambient_sounds.py [name ...]   (no names: the missing ones)
  python scripts/enhanced/ambient_sounds.py --force rain_light

The key comes from ELEVENLABS_API_KEY. Each sound is asked for as a seamless loop and converted
with ffmpeg to 48 kHz stereo 16-bit WAV, the format the game's mixer plays (game/sound/989snd/
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


def convert(raw, name):
    OUT.mkdir(parents=True, exist_ok=True)
    wav = OUT / f"{name}.wav"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(raw), "-ar", "48000", "-ac", "2",
                    "-c:a", "pcm_s16le", str(wav)], check=True)
    return wav


def main():
    args = sys.argv[1:]
    force = "--force" in args
    names = [a for a in args if not a.startswith("--")] or list(SOUNDS)
    for name in names:
        prompt, seconds = SOUNDS[name]
        if (OUT / f"{name}.wav").exists() and not force:
            print(f"{name}: there already")
            continue
        raw = RAW / f"{name}.mp3"
        if force or not raw.exists():
            raw = generate(name, prompt, seconds)
        wav = convert(raw, name)
        print(f"{name}: {wav.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
