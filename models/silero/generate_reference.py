"""Regenerates the Silero VAD reference fixture.

Runs Silero's own TorchScript network (silero_vad.jit) with the weights of
silero_vad_16k.safetensors loaded into it, on a fixed synthetic signal, and
writes the signal and the probability the network gives each 512 sample chunk
as raw little endian float32.

The jit file and the safetensors file hold different weights of the same
architecture, so the safetensors weights are copied into the jit module before
it runs. The jit file itself is only the network code.

Usage, from the project root:
    python3 tests/models/silero/generate_reference.py data/tensors/silero/silero_vad.jit \
        data/tensors/silero/silero_vad_16k.safetensors tests/models/silero

Needs numpy and torch. The signal is synthetic: pulse trains through resonators
with a 4 Hz amplitude swell, noise and silence. It contains no recorded speech.
"""

import json
import struct
import sys
import warnings
from pathlib import Path

import numpy as np
import torch

warnings.filterwarnings('ignore')

CHUNK = 512

PARAMETER_NAMES = {
    'conv1.weight': 'encoder.0.reparam_conv.weight',
    'conv1.bias': 'encoder.0.reparam_conv.bias',
    'conv2.weight': 'encoder.1.reparam_conv.weight',
    'conv2.bias': 'encoder.1.reparam_conv.bias',
    'conv3.weight': 'encoder.2.reparam_conv.weight',
    'conv3.bias': 'encoder.2.reparam_conv.bias',
    'conv4.weight': 'encoder.3.reparam_conv.weight',
    'conv4.bias': 'encoder.3.reparam_conv.bias',
    'lstm_cell.weight_ih': 'decoder.rnn.weight_ih',
    'lstm_cell.weight_hh': 'decoder.rnn.weight_hh',
    'lstm_cell.bias_ih': 'decoder.rnn.bias_ih',
    'lstm_cell.bias_hh': 'decoder.rnn.bias_hh',
    'final_conv.weight': 'decoder.decoder.2.weight',
    'final_conv.bias': 'decoder.decoder.2.bias',
}


def load_safetensors(path):
    with open(path, 'rb') as handle:
        length = struct.unpack('<Q', handle.read(8))[0]
        header = json.loads(handle.read(length))
        base = 8 + length
        tensors = {}
        for name, info in header.items():
            if name == '__metadata__':
                continue
            start, end = info['data_offsets']
            handle.seek(base + start)
            tensors[name] = np.frombuffer(handle.read(end - start), dtype=np.float32).reshape(info['shape'])
        return tensors


def voiced(samples, f0, amplitude, formants, seed):
    rng = np.random.default_rng(seed)
    pulses = np.zeros(samples)
    period = 16000.0 / f0
    position = 0.0
    while position < samples:
        pulses[int(position)] = 1.0
        position += period * (1.0 + 0.01 * rng.standard_normal())
    out = np.zeros(samples)
    for frequency, bandwidth in formants:
        radius = np.exp(-np.pi * bandwidth / 16000.0)
        a1 = -2.0 * radius * np.cos(2.0 * np.pi * frequency / 16000.0)
        a2 = radius * radius
        y1 = y2 = 0.0
        for i in range(samples):
            y = pulses[i] - a1 * y1 - a2 * y2
            out[i] += y
            y2, y1 = y1, y
    seconds = np.arange(samples) / 16000.0
    out *= 0.6 + 0.4 * np.sin(2.0 * np.pi * 4.0 * seconds)
    out /= np.max(np.abs(out)) + 1e-9
    return (amplitude * out).astype(np.float32)


def signal():
    rng = np.random.default_rng(3)
    return np.concatenate([
        np.zeros(8 * CHUNK, np.float32),
        voiced(24 * CHUNK, 120.0, 0.6, ((700, 80), (1200, 90), (2500, 120)), 1),
        (0.2 * rng.uniform(-1, 1, 10 * CHUNK)).astype(np.float32),
        voiced(24 * CHUNK, 210.0, 0.4, ((500, 70), (1700, 100), (2600, 130)), 2),
        np.zeros(8 * CHUNK, np.float32),
        (0.02 * rng.uniform(-1, 1, 4 * CHUNK)).astype(np.float32),
    ])


def main(jit_path, weights_path, output):
    weights = load_safetensors(weights_path)
    model = torch.jit.load(jit_path, map_location='cpu')
    model.eval()
    with torch.no_grad():
        for name, parameter in model._model.named_parameters():
            source = next(key for key, value in PARAMETER_NAMES.items() if value == name)
            parameter.copy_(torch.from_numpy(weights[source]))
        model._model.stft.forward_basis_buffer.copy_(torch.from_numpy(weights['stft_conv.weight']))

    audio = signal()
    model.reset_states()
    probabilities = []
    for chunk in range(len(audio) // CHUNK):
        samples = torch.from_numpy(audio[chunk * CHUNK:(chunk + 1) * CHUNK])[None, :]
        with torch.no_grad():
            probabilities.append(float(model(samples, 16000)))

    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    audio.astype('<f4').tofile(output / 'silero_reference_audio.f32')
    np.array(probabilities, dtype='<f4').tofile(output / 'silero_reference_probabilities.f32')
    print(f'{len(probabilities)} chunks, probability range {min(probabilities):.4f} to {max(probabilities):.4f}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
