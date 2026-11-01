# make_ui_sounds.py — the menus' sound (the Quick menu), built into src/ui_sounds_data.h:
#   Romain Raynal's Modern UI Vol.1 (CC0): UI_Confirm_Blips_Smooth_001 (title/sounds/src).
# Every menu sound (ui_sound.h's ids) is this one. 48 kHz mono 16-bit, the silence at its end
# trimmed. Only needed when you change the sound: the made file is in git.
# Run (WSL, ffmpeg): python3 title/make_ui_sounds.py
import array
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "sounds", "src", "Romain_Raynal_UI_Confirm_Blips_Smooth_001.wav")
OUT = os.path.join(os.path.dirname(HERE), "src", "ui_sounds_data.h")
IDS = 8  # ui_sound.h: SND_CLICK ... SND_STEP


def main():
    chain = "aresample=48000,areverse,silenceremove=start_periods=1:start_threshold=-55dB,areverse,volume=0.9"
    pcm = subprocess.run(["ffmpeg", "-v", "error", "-i", SRC, "-af", chain, "-ac", "1", "-ar", "48000",
                          "-f", "s16le", "-"], capture_output=True, check=True).stdout
    a = array.array("h", pcm)
    fade = min(len(a), 1440)  # the last 30 ms fade out
    for i in range(fade):
        j = len(a) - fade + i
        a[j] = int(a[j] * (1 - i / fade))
    with open(OUT, "w", newline="\n") as f:
        f.write("// ui_sounds_data.h - the menus' sound: Romain Raynal's Modern UI Vol.1, Confirm_Blips_Smooth_001\n"
                "// (CC0), 48 kHz mono 16-bit (title/make_ui_sounds.py). Every menu sound is this one.\n"
                "#include <stdint.h>\n")
        f.write(f"static const int16_t snd_open[{len(a)}] = {{\n")
        for i in range(0, len(a), 24):
            f.write("    " + ",".join(str(v) for v in a[i:i + 24]) + ",\n")
        f.write("};\n")
        f.write("static const struct { const int16_t *pcm; int frames; } ui_sounds[] = {\n")
        for _ in range(IDS):
            f.write(f"    {{ snd_open, {len(a)} }},\n")
        f.write("};\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
