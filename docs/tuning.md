# Tuning

The defaults suit short spoken commands in a quiet room. This page explains what to change for other conditions.

- [Timeouts](#timeouts)
- [VAD threshold](#vad-threshold)
- [Decoder: RNN-T or CTC](#decoder-rnn-t-or-ctc)
- [Quantization](#quantization)
- [Accuracy](#accuracy)

## Timeouts

| Setting | Default | Lower it when… | Raise it when… |
| ------- | ------- | -------------- | -------------- |
| `silence_timeout` | 2.0 s | Commands are short and you want faster replies | Speakers pause mid-sentence and get cut off |
| `no_speech_timeout` | 5.0 s | Recordings should stop quickly when nobody talks | People need time to think before speaking |
| `max_utterance` | 30 s | You only expect short commands | You dictate long passages. The model works best on segments under about 30 s |
| `pad` | 0.3 s | | The first or last syllable gets clipped |

The total delay after you stop talking is about `silence_timeout` plus the model time (tens of milliseconds on a GPU). For snappier interaction, 1.0–1.5 s is a good range.

## VAD threshold

`vad_threshold` (default 0.5) is the Silero probability above which a 32 ms frame counts as speech.

- **Noisy room (fans, motors), with sessions that never end on silence:** raise it to 0.6–0.7. Noise then stops counting as speech and the silence timer can run.
- **Quiet or distant speakers who are not detected:** lower it to 0.3–0.4, or move the microphone closer. A close microphone helps more than any setting.

Robot motors are a common case: the VAD sees motor noise as possible speech and keeps resetting the silence timer. Raise the threshold, mount the microphone away from the motors, or use a directional microphone.

## Decoder: RNN-T or CTC

The model has two decoder heads that share one encoder:

| | RNN-T (default) | CTC |
| - | --------------- | --- |
| Accuracy | Better: 12.0% WER on Common Voice per the model card | 12.8% WER on Common Voice |
| Speed | Slightly slower decoding | Slightly faster |

On short commands the speed difference is a few milliseconds, so keep RNN-T unless you measure a reason not to. To compare on your own audio:

```bash
./build/bin/ptstt-cli --file sample.wav --decoder rnnt
```
```bash
./build/bin/ptstt-cli --file sample.wav --decoder ctc
```

## Quantization

The published model is float32 (452 MB). An 8-bit (`q8_0`) copy is 171 MB, so it loads faster and uses less memory. On CPU-only devices it often runs faster as well; measure it on yours.

```bash
scripts/fetch_models.sh --q8
```
```bash
./build/bin/ptstt-cli --model models/pt-fastconformer-large.q8_0.gguf
```

Check the transcripts on your own recordings before switching: quantization can change a word here and there. On the Orin's GPU the f32 model already fits comfortably in memory, so quantization is optional.

## Accuracy

- **Microphone quality and distance** matter most. A headset or a close directional microphone beats any setting.
- **Speak naturally.** The model was trained on read and conversational Brazilian and European Portuguese (Common Voice, Multilingual LibriSpeech and others).
- **Domain words** such as robot names or technical terms may come out wrong, because the model has no custom vocabulary. Correct the known ones in your own code after transcription.
- **Numbers** usually come out as words ("dois metros"). Convert them in your own code if you need digits.
