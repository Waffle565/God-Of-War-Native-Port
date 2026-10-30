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
    blocks = min(size, len(track) - start) // (interleave * channels)  # some tracks end before their SSbd size
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
    # the clip also holds the text card: its pictures replace the English card (intro_card)
    return intro_card(with_samples(usa_movie, pcm), eu_clips[track], clip_at / 48000)


# ---- dub files: a European movie track played over the USA movie at run time (ps2xIOP disc_source.cpp) ----------
# A USA movie's audio is 16-bit PCM; the European disc has the speaking movies with five PS2 ADPCM tracks (0 en,
# 1 fr, 2 de, 3 it, 4 es; same length and timing as the USA audio within 3 ms, measured 2026-10-30). Whole dubbed
# copies would not fit on the Switch's SD card (about 3.4 GB per language), so the overlay keeps the USA movie plus a
# small dub file per movie (the language's ADPCM, ~55 KB per second), and disc_source decodes it into the PCM bytes
# of the movie's audio packets as their sectors are read. Format (little endian):
#   0  'GOWDUB1\0'
#   8  u32 movie size           12 u32 ranges (audio packets)
#   16 u32 PCM start in the track (after the SShd/SSbd headers, which stay the USA ones)
#   20 u32 PCM bytes (SSbd size) 24 u32 PCM interleave (bytes per channel block)
#   28 u32 channels              32 s32 lag: USA sample n plays ADPCM sample n - lag
#   36 u32 ADPCM interleave (bytes per channel chunk)   40 u32 ADPCM bytes   44..63 zero
#   64 ranges x {u32 offset in the movie file, u32 length}: the track bytes of each audio packet (after its 4-byte
#      ff a0 00 00 sub-header), in file order
#   then the ADPCM bytes (the European track's samples, channels interleaved in <ADPCM interleave> chunks)
DUB_MAGIC = b'GOWDUB1\0'


def dub_file(usa_movie, eu_movie, track, lag=0):
    """the dub file playing European track <track> of eu_movie over usa_movie (bytes)"""
    packets = audio_packets(usa_movie)
    ranges = [(start + 4, end - start - 4) for start, end in packets]
    usa_track = bytearray()
    for start, end in packets:
        usa_track += usa_movie[start + 4:end]
    kind, rate, channels, interleave, body, size = _header(usa_track)
    if kind != 1:
        raise ValueError('the USA movies have PCM audio')
    eu = tracks(eu_movie)[track]
    ekind, erate, echannels, einterleave, ebody, esize = _header(eu)
    if ekind != 16 or erate != rate or echannels != channels:
        raise ValueError(f'European track {track}: type {ekind}, {erate} Hz, {echannels} channels')
    adpcm = bytes(eu[ebody:ebody + esize])
    adpcm = adpcm[:len(adpcm) // (einterleave * channels) * (einterleave * channels)]
    head = DUB_MAGIC + struct.pack('<IIIIIIiII', len(usa_movie), len(ranges), body, size, interleave, channels, lag,
                                   einterleave, len(adpcm))
    head += b'\0' * (64 - len(head))
    return head + b''.join(struct.pack('<II', o, n) for o, n in ranges) + adpcm


# ---- the intro's text card ("Three Weeks Earlier / The Aegean Sea") ----------------------------------------------
# The USA intro (SEMO01.PSW, 640x480 at 29.97, anamorphic 16:9) ends with that card burned into the picture. The
# European intro has it in a separate clip per language (SEMO01A-E.PSS: 640x512 at 25, 4:3; D = "Tre settimane
# prima / Mar Egeo"), which also holds the last spoken sentence. intro_card() re-encodes the USA movie's last
# pictures from the clip and writes them into the movie's own video packets: packets, sizes, audio and the pictures
# before stay as they are.

def _video_packets(movie):
    """(packet start, payload start, payload end) of every video (0xE0) packet, in file order"""
    i, out = 0, []
    while i + 6 <= len(movie):
        if movie[i:i + 3] != b'\x00\x00\x01':
            j = movie.find(b'\x00\x00\x01', i + 1)
            if j < 0:
                break
            i = j
            continue
        code = movie[i + 3]
        if code == 0xBA:
            i += 14 + (movie[i + 13] & 7)
            continue
        if code == 0xB9:
            i += 4
            continue
        length = struct.unpack_from('>H', movie, i + 4)[0]
        if code == 0xE0:
            out.append((i, i + 9 + movie[i + 8], i + 6 + length))
        i += 6 + length
    return out


def _start_codes(es, code):
    marker = b'\x00\x00\x01' + bytes([code])
    out, j = [], es.find(marker)
    while j >= 0:
        out.append(j)
        j = es.find(marker, j + 4)
    return out


def _read_pts(movie, at):
    b = movie[at:at + 5]
    return ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1


def _write_pts(out, at, prefix, pts):
    out[at:at + 5] = bytes([prefix << 4 | ((pts >> 30) & 7) << 1 | 1, (pts >> 22) & 0xFF,
                            ((pts >> 15) & 0x7F) << 1 | 1, (pts >> 7) & 0xFF, (pts & 0x7F) << 1 | 1])


def _card_pictures(clip, count, first_time, scale=1.25):
    """count pictures (640x480 rgb, anamorphic 16:9) at 29.97 per second from first_time (seconds into the clip):
    the clip's 4:3 picture in the middle, scaled so its text is about as wide as the USA card's; the sides repeat
    its edge columns (black, white during the closing flash)"""
    import io
    import av
    import numpy as np
    container = av.open(io.BytesIO(clip), format='mpeg')
    size = int(480 * scale) // 2 * 2
    frames = [f.reformat(width=size, height=size, format='rgb24').to_ndarray() for f in container.decode(video=0)]
    rate = float(container.streams.video[0].average_rate or 25)
    top, side = (size - 480) // 2, (640 - size) // 2
    out = []
    for k in range(count):
        index = int(np.floor((first_time + k * 1001 / 30000) * rate + 1e-6))
        if index < 0:
            out.append(np.zeros((480, 640, 3), np.uint8))
            continue
        picture = frames[min(index, len(frames) - 1)][top:top + 480]
        out.append(np.pad(picture, ((0, 0), (side, 640 - size - side), (0, 0)), mode='edge'))
    return out


def _encode(pictures):
    """MPEG-2 video: 640x480 at 30000/1001, closed GOPs of 16 I/P pictures (decode order = display order, so each
    picture's DTS is its PTS), a sequence header before each GOP"""
    from fractions import Fraction
    import av
    codec = av.CodecContext.create('mpeg2video', 'w')
    codec.width, codec.height, codec.pix_fmt = 640, 480, 'yuv420p'
    codec.time_base = Fraction(1001, 30000)
    codec.framerate = Fraction(30000, 1001)
    codec.gop_size = 16
    codec.max_b_frames = 0
    codec.bit_rate = 6000000
    codec.options = {'flags': '+cgop', 'sc_threshold': '1000000000', 'qmin': '2', 'qmax': '8'}
    es = bytearray()
    for k, picture in enumerate(pictures):
        frame = av.VideoFrame.from_ndarray(picture, format='rgb24').reformat(format='yuv420p')
        frame.pts = k
        for packet in codec.encode(frame):
            es += bytes(packet)
    for packet in codec.encode(None):
        es += bytes(packet)
    return es


def intro_card(usa_movie, clip, clip_at):
    """usa_movie (SEMO01.PSW) with its pictures from the last closed GOP at or before clip_at (seconds: where the
    English clip starts in the USA movie) to the end re-encoded from the European clip (SEMO01B-E.PSS)"""
    out = bytearray(usa_movie)
    packets = _video_packets(usa_movie)
    es = bytearray()
    payload_at = []  # es offset of each packet's payload
    for _, begin, end in packets:
        payload_at.append(len(es))
        es += usa_movie[begin:end]
    pictures = _start_codes(es, 0x00)
    gops = _start_codes(es, 0xB8)
    sequences = _start_codes(es, 0xB3)

    def shown_before(at):  # closed GOPs: a GOP's first displayed picture = the pictures before it
        return sum(1 for p in pictures if p < at)

    gop = max(g for g in gops if shown_before(g) <= int(clip_at * 30000 / 1001))
    shown = shown_before(gop)
    start = max([s for s in sequences if s < gop and not any(s < p < gop for p in pictures)], default=gop)
    count = len(pictures) - shown
    new = _encode(_card_pictures(clip, count, shown * 1001 / 30000 - clip_at))
    if new.endswith(b'\x00\x00\x01\xb7'):
        new = new[:-4]
    for s in _start_codes(new, 0xB3):  # the USA sequence's aspect code
        new[s + 7] = (es[sequences[0] + 7] & 0xF0) | (new[s + 7] & 0x0F)
    room = len(es) - start - 4
    if len(new) > room or len(_start_codes(new, 0x00)) != count:
        raise ValueError(f'card: {len(new)} bytes for {room}, {len(_start_codes(new, 0x00))} pictures for {count}')
    # the new pictures and a sequence end fill the packets from the start; the video packets after that become
    # padding packets (a long run of zero stuffing would overflow libmpeg2's chunk buffer)
    end_at = start + len(new) + 4
    es[start:] = new + b'\x00\x00\x01\xb7' + b'\0' * (room - len(new))
    new_pictures = [p for p in _start_codes(es, 0x00) if p >= start]
    # the PTS of picture 0: a packet's PTS belongs to the first picture starting in it (display index = pictures
    # before its GOP + its temporal reference)
    bases = []
    for k, (head, begin, end) in enumerate(packets):
        if usa_movie[head + 7] & 0x80:
            first = next((p for p in pictures if payload_at[k] <= p < payload_at[k] + end - begin), None)
            if first is not None and first < start:
                temporal = es[first + 4] << 2 | es[first + 5] >> 6
                display = shown_before(max(g for g in gops if g < first)) + temporal
                bases.append(_read_pts(usa_movie, head + 9) - display * 3003)
    base = max(set(bases), key=bases.count)
    for k, (head, begin, end) in enumerate(packets):
        lo, hi = payload_at[k], payload_at[k] + end - begin
        if hi <= start:
            continue
        if lo >= end_at:
            out[head + 3] = 0xBE
            out[head + 6:end] = b'\xff' * (end - head - 6)
            continue
        out[begin:end] = es[lo:hi]
        flags = usa_movie[head + 7] >> 6
        if not flags or any(lo <= p < start for p in pictures):  # no PTS, or it belongs to an old picture
            continue
        first = next((n for n, p in enumerate(new_pictures) if lo <= p < hi), None)
        if first is None:  # no picture starts here any more: no optional fields, the header bytes become stuffing
            out[head + 7] = 0
            out[head + 9:begin] = b'\xff' * (begin - head - 9)
            continue
        pts = (base + (shown + first) * 3003) & ((1 << 33) - 1)
        _write_pts(out, head + 9, 3 if flags == 3 else 2, pts)
        if flags == 3:
            _write_pts(out, head + 14, 1, pts)
    return bytes(out)
