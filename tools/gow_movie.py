"""Movie audio of God of War (PSS/PSW, MPEG-2 program streams), for dubbing the USA movies (docs/settings.md).

The audio is private stream 0xBD: every packet's payload starts with 4 bytes ff a0|a1 00 <track>, then the track's
bytes, which begin with an "SShd" header {u32 size 24, u32 type, u32 rate, u32 channels, u32 interleave, 8 x ff}
and an "SSbd" header {u32 size} before the samples. Type 1 is 16-bit PCM, type 16 PS2 ADPCM; the channels take
turns in blocks of <interleave> bytes. The USA movies have one PCM track. The European intro (SEMO01.PSS) has five
ADPCM tracks, one per language (0 en, 1 fr, 2 de, 3 it, 4 es), whose packets come in any order; its last sentence
is a separate PCM clip per language (SEMO01A-E.PSS, A = en). dub_intro() puts a language into the USA movie's own
packets: same picture, sizes and timestamps, only the samples change.

Needs numpy.
"""
import struct

ADPCM_COEF = [(0.0, 0.0), (60 / 64, 0.0), (115 / 64, -52 / 64), (98 / 64, -55 / 64), (122 / 64, -60 / 64)]


def audio_packets(movie):
    """(payload start, payload end) of every audio (0xBD) packet, in file order"""
    i, out = 0, []
    while i + 6 <= len(movie):
        if movie[i:i + 3] != b'\x00\x00\x01':
            j = movie.find(b'\x00\x00\x01', i + 1)
            if j < 0:
                break
            i = j
            continue
        code = movie[i + 3]
        if code == 0xBA:  # pack header
            i += 14 + (movie[i + 13] & 7)
            continue
        if code == 0xB9:  # end code
            i += 4
            continue
        length = struct.unpack_from('>H', movie, i + 4)[0]
        if code == 0xBD:
            out.append((i + 9 + movie[i + 8], i + 6 + length))
        i += 6 + length
    return out


def tracks(movie):
    """{track number: the track's bytes (SShd ...)}"""
    out = {}
    for start, end in audio_packets(movie):
        out.setdefault(movie[start + 3], bytearray()).extend(movie[start + 4:end])
    return out


def _header(track):
    kind, rate, channels, interleave = struct.unpack_from('<IIII', track, 8)
    body = track.find(b'SSbd')
    size = struct.unpack_from('<I', track, body + 4)[0]
    return kind, rate, channels, interleave, body + 8, size


def _adpcm(data):
    import numpy as np
    out = np.empty(len(data) // 16 * 28)
    h1 = h2 = 0.0
    k = 0
    for f in range(0, len(data) - 15, 16):
        shift, predictor = data[f] & 0xF, min((data[f] >> 4) & 7, 4)
        c1, c2 = ADPCM_COEF[predictor]
        for byte in data[f + 2:f + 16]:
            for nibble in (byte & 0xF, byte >> 4):
                v = (((nibble - 16 if nibble >= 8 else nibble) << 12) >> shift) + h1 * c1 + h2 * c2
                h2, h1 = h1, v
                out[k] = v
                k += 1
    return np.clip(out[:k], -32768, 32767).astype(np.int16)


def samples(track):
    """a track's samples as an (n, channels) int16 array (PCM or ADPCM)"""
    import numpy as np
    kind, rate, channels, interleave, start, size = _header(track)
    blocks = size // (interleave * channels)
    data = np.frombuffer(bytes(track[start:start + blocks * interleave * channels]), dtype=np.uint8)
    data = data.reshape(blocks, channels, interleave)
    if kind == 1:
        return np.stack([data[:, c, :].copy().view('<i2').reshape(-1) for c in range(channels)], axis=1)
    if kind == 16:
        return np.stack([_adpcm(data[:, c, :].tobytes()) for c in range(channels)], axis=1)
    raise ValueError(f'audio type {kind}')


def with_samples(movie, pcm):
    """the movie (one PCM track) with its samples replaced by pcm, cut or padded with silence to the same length"""
    import numpy as np
    out = bytearray(movie)
    packets = audio_packets(movie)
    track = bytearray()
    for start, end in packets:
        track += movie[start + 4:end]
    kind, rate, channels, interleave, body, size = _header(track)
    if kind != 1:
        raise ValueError('the USA movies have PCM audio')
    per_block = interleave // 2
    frames = size // (interleave * channels) * per_block
    new = np.zeros((frames, channels), dtype=np.int16)
    n = min(frames, len(pcm))
    new[:n] = pcm[:n]
    blocks = np.stack([new[:, c].reshape(-1, per_block) for c in range(channels)], axis=1)
    data = blocks.astype('<i2').tobytes()
    track[body:body + len(data)] = data
    pos = 0
    for start, end in packets:
        out[start + 4:end] = track[pos:pos + end - start - 4]
        pos += end - start - 4
    return bytes(out)


def lag(big, small):
    """where small starts inside big (mono cross-correlation by FFT), in samples"""
    import numpy as np
    a, b = big.mean(axis=1), small.mean(axis=1)
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    corr = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    return int(np.argmax(corr[:len(a)]))


def dub_intro(usa_movie, eu_movie, eu_clips, track):
    """the USA intro (SEMO01.PSW/PSS) speaking European track <track> (1 fr .. 4 es): the track from the start, then
    its clip where the English clip (eu_clips[0]) lies in the USA audio. eu_clips: the five clips' movies, A..E."""
    import numpy as np
    usa = samples(next(iter(tracks(usa_movie).values())))
    eu = tracks(eu_movie)
    english_clip = samples(next(iter(tracks(eu_clips[0]).values())))
    clip_at = lag(usa.astype(np.float64), english_clip.astype(np.float64))
    voice = samples(eu[track])
    clip = samples(next(iter(tracks(eu_clips[track]).values())))
    pcm = np.zeros((max(clip_at + len(clip), len(voice)), 2), dtype=np.int16)
    pcm[:len(voice)] = voice
    pcm[clip_at:clip_at + len(clip)] = clip
    return with_samples(usa_movie, pcm)
