# HuBERT phoneme fixtures

Reference data for `hubert_phoneme_test.cpp`, written by `generate_reference.py` from the original network
(transformers `HubertForCTC` with the weights of `data/tensors/hubert_phoneme/hubert_base_phoneme_en.safetensors`).
Every file is raw little endian float32. Posteriors are frame after frame, 43 classes each.

| File | Content |
|---|---|
| `hubert_reference_audio.f32`, `hubert_reference_posteriors.f32` | 2 s of synthetic signal, 99 frames. No recorded speech; every frame is the blank class. |
| `hubert_speech_audio.f32`, `hubert_speech_posteriors.f32` | The first 3 s of a recorded sentence, 149 frames. |

The speech fixture is cut from `https://cdn-media.huggingface.co/speech_samples/sample1.flac` (SHA-256
`cb5c48a2d1d6f7dedd0330f088a4cbe76de1a86e6a6109c06d255bb1ca2f7542`), which the model cards of
`facebook/wav2vec2-base-960h` and others present as "Librispeech sample 1". LibriSpeech (Panayotov, Chen, Povey
and Khudanpur, 2015, https://www.openslr.org/12) is released under CC BY 4.0. The excerpt was converted to mono
16 kHz float32 with ffmpeg and is otherwise unchanged. The network hears its opening as
`G OW IH NG AH L AO NG S L AH SH IY K AH N T R IY R OW D Z` ("going along slushy country roads").
