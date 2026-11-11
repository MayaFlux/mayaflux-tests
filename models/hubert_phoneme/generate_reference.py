"""Regenerates the HuBERT phoneme reference fixture.

Runs the original network (transformers' HubertForCTC with the weights of
hubert_base_phoneme_en.safetensors) on a fixed synthetic signal and writes the
signal and the probability the network gives every class on every frame, frame
after frame, as raw little endian float32.

Usage, from the project root:
    python3 tests/models/hubert_phoneme/generate_reference.py data/tensors/hubert_phoneme tests/models/hubert_phoneme [speech.f32]

The first argument is the directory fetch_assets.py fills: it holds the
weights and config.json. Needs numpy, torch and transformers. The signal is
synthetic: pulse trains through resonators with a 4 Hz amplitude swell, noise
and silence. It contains no recorded speech, so every frame is the blank
class; the comparison is numerical, not a test of recognition.

With a third argument, raw mono float32 speech at 16 kHz, the first three
seconds of it are written as a second fixture with their probabilities, and
the phonemes the network recognises are printed. See README.md for the clip
the tracked speech fixture was cut from.
"""

import sys
import tempfile
import warnings
from pathlib import Path

import numpy as np
import torch

warnings.filterwarnings('ignore')

RATE = 16000
SECONDS = 2
WEIGHTS = 'hubert_base_phoneme_en.safetensors'


def synthetic_signal():
    rng = np.random.default_rng(20261111)
    count = RATE * SECONDS
    t = np.arange(count) / RATE
    out = np.zeros(count)
    for start, stop, pitch, formant in ((0.05, 0.55, 110.0, 700.0), (0.7, 1.1, 180.0, 1200.0), (1.4, 1.9, 140.0, 500.0)):
        a, b = int(start * RATE), int(stop * RATE)
        pulses = np.zeros(b - a)
        pulses[::int(RATE / pitch)] = 1.0
        radius = 0.97
        theta = 2.0 * np.pi * formant / RATE
        voiced = np.zeros(b - a)
        y1 = y2 = 0.0
        for n in range(b - a):
            y = pulses[n] + 2.0 * radius * np.cos(theta) * y1 - radius * radius * y2
            y2, y1 = y1, y
            voiced[n] = y
        voiced /= np.abs(voiced).max()
        out[a:b] += 0.5 * voiced * (0.6 + 0.4 * np.sin(2.0 * np.pi * 4.0 * t[a:b]))
    a, b = int(1.1 * RATE), int(1.35 * RATE)
    out[a:b] += 0.2 * rng.standard_normal(b - a)
    out += 0.002 * rng.standard_normal(count)
    return out.astype(np.float32)


LABELS = ['AA', 'AE', 'AH', 'AO', 'AW', 'AY', 'B', 'CH', 'D', 'DH', 'EH', 'ER', 'EY', 'F', 'G', 'HH', 'IH', 'IY', 'JH', 'K',
          'L', 'M', 'N', 'NG', 'OW', 'OY', 'P', 'R', 'S', 'SH', 'T', 'TH', 'UH', 'UW', 'V', 'W', 'Y', 'Z', 'ZH']
SPEECH_SECONDS = 3


def write_fixture(model, audio, out_dir, stem):
    x = torch.from_numpy(audio).double()
    x = ((x - x.mean()) / torch.sqrt(x.var(unbiased=False) + 1e-7)).float()
    with torch.no_grad():
        posteriors = torch.softmax(model(x[None, :]).logits[0], dim=1).numpy().astype('<f4')
    audio.astype('<f4').tofile(out_dir / f'{stem}_audio.f32')
    posteriors.tofile(out_dir / f'{stem}_posteriors.f32')
    best = posteriors.argmax(axis=1).tolist()
    events = [LABELS[c] for i, c in enumerate(best) if c < len(LABELS) and (i == 0 or best[i - 1] != c)]
    print(f'{stem}: {audio.size} samples, {posteriors.shape[0]} frames of {posteriors.shape[1]} classes, '
          f'probability range {posteriors.min():.3g} to {posteriors.max():.6f}')
    print(f'{stem}: phonemes {" ".join(events) if events else "none"}')


def main(model_dir, out_dir, speech=None):
    from transformers import HubertForCTC

    model_dir = Path(model_dir).resolve()
    out_dir = Path(out_dir)
    with tempfile.TemporaryDirectory() as staged:
        (Path(staged) / 'model.safetensors').symlink_to(model_dir / WEIGHTS)
        (Path(staged) / 'config.json').symlink_to(model_dir / 'config.json')
        model = HubertForCTC.from_pretrained(staged, local_files_only=True).eval()
    write_fixture(model, synthetic_signal(), out_dir, 'hubert_reference')
    if speech:
        clip = np.fromfile(speech, dtype='<f4')[:RATE * SPEECH_SECONDS]
        write_fixture(model, clip, out_dir, 'hubert_speech')


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
