"""Regenerates the IntervalExtractor reference fixture.

Runs the original get_speech_timestamps from Silero VAD's utils_vad.py on
probability sequences, through a stub model that replays them, and writes each
sequence with its parameters and the timestamps the original returns.

Usage, from the project root:
    python3 tests/yantra/gpu/interval_reference/generate_reference.py \
        <path to utils_vad.py> tests/models/silero/silero_reference_probabilities.f32 \
        tests/yantra/gpu/interval_reference/interval_reference.txt

utils_vad.py is src/silero_vad/utils_vad.py at commit
fb7d7c7f5df63235f0afd037d8397593829aeb41 of github.com/snakers4/silero-vad.
Needs numpy and torch. torchaudio, which utils_vad.py imports and this function
does not use, is replaced by an empty module. Sequences are synthetic except the first, which is the
Silero reference fixture's probabilities.

Fixture format, one case after another:
    case <name>
    params enter min_run min_gap padding gap_at_limit max_run longest length
    scores <float32 values>
    expect <start end pairs>
Lengths are in samples at 16 kHz, max_run 0 means no cap.
"""

import importlib.util
import sys
import types
import warnings
from pathlib import Path

import numpy as np
import torch

warnings.filterwarnings('ignore')

RATE = 16000
CHUNK = 512


class ReplayModel:
    def __init__(self, probabilities):
        self.probabilities = list(probabilities)
        self.position = 0

    def reset_states(self):
        self.position = 0

    def __call__(self, chunk, sampling_rate):
        value = self.probabilities[self.position]
        self.position += 1
        return torch.tensor(value)


def load_original(path):
    sys.modules.setdefault('torchaudio', types.ModuleType('torchaudio'))
    spec = importlib.util.spec_from_file_location('utils_vad', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.get_speech_timestamps


def synthetic(rng, chunks):
    values = []
    speaking = bool(rng.integers(0, 2))
    while len(values) < chunks:
        length = int(rng.integers(1, 40))
        for _ in range(length):
            if speaking:
                value = rng.uniform(0.55, 0.99)
                if rng.random() < 0.08:
                    value = rng.uniform(0.2, 0.5)
            else:
                value = rng.uniform(0.0, 0.3)
                if rng.random() < 0.05:
                    value = rng.uniform(0.4, 0.7)
            values.append(value)
        speaking = not speaking
    return np.array(values[:chunks], dtype=np.float32)


def case(name, scores, length, threshold=0.5, min_speech_ms=250, max_speech_s=float('inf'),
         min_silence_ms=100, pad_ms=30, gap_ms=98, longest=True, original=None):
    scores = np.asarray(scores, dtype=np.float32)
    audio = torch.zeros(length)
    stamps = original(audio, ReplayModel(float(v) for v in scores),
                      threshold=threshold, sampling_rate=RATE,
                      min_speech_duration_ms=min_speech_ms, max_speech_duration_s=max_speech_s,
                      min_silence_duration_ms=min_silence_ms, speech_pad_ms=pad_ms,
                      min_silence_at_max_speech=gap_ms, use_max_poss_sil_at_max_speech=longest)
    max_run = 0 if max_speech_s == float('inf') else int(RATE * max_speech_s)
    lines = [f'case {name}']
    lines.append('params {} {} {} {} {} {} {} {}'.format(
        repr(float(threshold)), RATE * min_speech_ms // 1000, RATE * min_silence_ms // 1000,
        RATE * pad_ms // 1000, RATE * gap_ms // 1000, max_run, int(longest), length))
    lines.append('scores ' + ' '.join('%.9g' % float(v) for v in scores))
    lines.append('expect ' + ' '.join(f"{s['start']} {s['end']}" for s in stamps))
    return '\n'.join(lines) + '\n'


def main():
    original = load_original(sys.argv[1])
    stored = np.fromfile(sys.argv[2], dtype='<f4')
    rng = np.random.default_rng(7)
    out = []

    out.append(case('stored_default', stored, len(stored) * CHUNK - 211, original=original))
    out.append(case('silence', np.zeros(60, dtype=np.float32), 60 * CHUNK, original=original))
    out.append(case('all_speech', np.full(80, 0.9, dtype=np.float32), 80 * CHUNK - 40, original=original))
    out.append(case('single_chunk', [0.9], 400, min_speech_ms=10, original=original))
    out.append(case('single_quiet_chunk', [0.2], 500, original=original))

    for chunks, tail in ((300, 137), (900, 3), (2000, 511)):
        scores = synthetic(rng, chunks)
        out.append(case(f'random_{chunks}', scores, chunks * CHUNK - tail, original=original))

    scores = synthetic(rng, 1200)
    length = 1200 * CHUNK - 77
    out.append(case('low_threshold', scores, length, threshold=0.35, original=original))
    out.append(case('wide_gap_and_padding', scores, length, min_silence_ms=300, pad_ms=150,
                    min_speech_ms=120, original=original))
    out.append(case('no_padding', scores, length, pad_ms=0, original=original))
    out.append(case('capped_longest', scores, length, max_speech_s=2.0, original=original))
    out.append(case('capped_last_gap', scores, length, max_speech_s=2.0, longest=False,
                    original=original))
    out.append(case('capped_tight', scores, length, max_speech_s=0.8, min_silence_ms=200,
                    original=original))
    out.append(case('capped_tight_last_gap', scores, length, max_speech_s=0.8, min_silence_ms=200,
                    longest=False, original=original))

    scores = np.full(400, 0.9, dtype=np.float32)
    out.append(case('capped_unbroken', scores, 400 * CHUNK, max_speech_s=1.5, original=original))
    out.append(case('capped_unbroken_last_gap', scores, 400 * CHUNK, max_speech_s=1.5, longest=False,
                    original=original))

    Path(sys.argv[3]).write_text('\n'.join(out))


main()
