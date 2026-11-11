"""Speak one line with the local Paradee 8M ONNX file.

Zell asks for bm_fable, the British male storyteller. This process does not
download weights and does not open a network connection.
"""
import argparse
import json
import wave
from pathlib import Path

import numpy as np
import onnxruntime as ort
from misaki import en, espeak

SAMPLE_RATE = 24000
MAX_PHONEMES = 510
CHUNK_GAP_SECONDS = 0.15
ESPEAK_PATH_LIMIT = 160


def _data_path():
    import espeakng_loader
    return Path(espeakng_loader.__file__).resolve().parent / "espeak-ng-data"


def _clauses(text):
    parts = []
    start = 0
    for index, character in enumerate(text):
        if character in ".!?\n":
            parts.append(text[start:index + 1])
            start = index + 1
    if start < len(text):
        parts.append(text[start:])
    return [part for part in parts if part.strip()]


def _marked(text, mark):
    parts = []
    start = 0
    for index, character in enumerate(text):
        if character == mark:
            parts.append(text[start:index + 1])
            start = index + 1
    if start < len(text):
        parts.append(text[start:])
    return [part for part in parts if part.strip()]


def _words(text):
    words = text.split(" ")
    parts = []
    for index, word in enumerate(words):
        if word == "":
            continue
        parts.append(word if index == len(words) - 1 else word + " ")
    return parts


def _smaller(text):
    """Break one utterance into strictly smaller pieces."""
    clauses = _clauses(text)
    if len(clauses) > 1:
        return clauses
    for mark in (",", ";", ":"):
        parts = _marked(text, mark)
        if len(parts) > 1:
            return parts
    words = _words(text)
    if len(words) > 1:
        return words
    if len(text) > 1:
        mid = len(text) // 2
        return [text[:mid], text[mid:]]
    return [text]


def pack_chunks(text, count_ids, limit):
    """Group text so each piece phonemizes to at most limit ids.

    Paradee is one utterance per run. A longer reply is several runs
    joined into one recording, instead of stopping at the first run.
    """
    chunks = []
    pending = ""
    pending_count = 0

    def flush():
        nonlocal pending, pending_count
        if pending.strip():
            chunks.append(pending)
        pending = ""
        pending_count = 0

    def absorb(piece, depth):
        nonlocal pending, pending_count
        if piece.strip() == "" or depth > 64:
            return
        count = count_ids(piece)
        if count <= 0:
            return
        if count > limit:
            flush()
            pieces = _smaller(piece)
            if len(piece) <= 1 or pieces == [piece]:
                chunks.append(piece)
                return
            for smaller in pieces:
                absorb(smaller, depth + 1)
            return
        if pending and pending_count + count > limit:
            flush()
        pending += piece
        pending_count += count

    pieces = _clauses(text)
    if not pieces and text.strip():
        pieces = [text]
    for piece in pieces:
        absorb(piece, 0)
    flush()
    return chunks


def _phoneme_ids(grapheme, text, vocab):
    phonemes, _tokens = grapheme(text)
    return [vocab[character] for character in phonemes if character in vocab]


def _fit_chunks(text, count_ids):
    """Re-split a packed chunk when the joined phonemes run past the limit."""
    queue = pack_chunks(text, count_ids, MAX_PHONEMES)
    fitted = []
    guard = 0
    while queue:
        guard += 1
        if guard > 10000:
            break
        chunk = queue.pop(0)
        count = count_ids(chunk)
        if count > MAX_PHONEMES and len(chunk) > 1:
            pieces = _smaller(chunk)
            if pieces == [chunk]:
                mid = len(chunk) // 2
                pieces = [chunk[:mid], chunk[mid:]]
            queue[0:0] = pieces
            continue
        if count > 0:
            fitted.append(chunk)
    return fitted


def synthesize(model, config_path, voice, text, wav_path):
    data_path = _data_path()
    if len(str(data_path).encode()) > ESPEAK_PATH_LIMIT:
        raise SystemExit("espeak-ng data path exceeds 160 bytes: " + str(data_path))
    config = json.loads(Path(config_path).read_text(encoding="utf-8"))
    voices = config["voices"]
    if voice not in voices:
        raise SystemExit("Paradee has no voice " + voice)
    british = voice.startswith("b")
    grapheme = en.G2P(trf=False, british=british,
                      fallback=espeak.EspeakFallback(british=british), unk="")
    vocab = config["vocab"]

    def count_ids(piece):
        return len(_phoneme_ids(grapheme, piece, vocab))

    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(model, options, providers=["CPUExecutionProvider"])
    voice_id = voices.index(voice)
    parts = []
    for chunk in _fit_chunks(text, count_ids):
        identifiers = [0] + _phoneme_ids(grapheme, chunk, vocab)[:MAX_PHONEMES] + [0]
        waveform = session.run(None, {
            "input_ids": np.array([identifiers], np.int64),
            "speed": np.array([1.0], np.float32),
            "voice": np.array([voice_id], np.int64),
        })[0].reshape(-1)
        if waveform.size == 0 or not np.isfinite(waveform).all():
            continue
        parts.append(waveform.astype(np.float32, copy=False))
    if not parts:
        raise SystemExit("Paradee produced no audio")
    if len(parts) == 1:
        waveform = parts[0]
    else:
        gap = np.zeros(int(SAMPLE_RATE * CHUNK_GAP_SECONDS), np.float32)
        waveform = parts[0]
        for part in parts[1:]:
            waveform = np.concatenate((waveform, gap, part))
    pcm = np.clip(waveform, -1.0, 1.0)
    frames = (pcm * 32767.0).astype("<i2").tobytes()
    destination = Path(wav_path)
    with wave.open(str(destination), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(frames)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--voice", required=True)
    parser.add_argument("--text", required=True)
    parser.add_argument("--wav", required=True)
    args = parser.parse_args(argv)
    synthesize(args.model, args.config, args.voice, args.text, args.wav)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))
