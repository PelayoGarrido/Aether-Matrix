"""Bounded score/performance bridge; no ML imports or random humanisation."""
def integer(obj, name, low, high, default=None):
    value = obj.get(name, default)
    if type(value) is not int or not low <= value <= high:
        raise ValueError("Invalid performance field: " + name)
    return value


def timing_range(notes, index, source, elapsed_ms):
    """Half-open timing-token range; constrain cumulative onset, not each gap."""
    expected = round(notes[index]["tick"] * 60000 / (source["bpm"] * source.get("ticks_per_beat", 480)))
    chord = (index > 0 and notes[index-1]["tick"] == notes[index]["tick"]) or (index+1 < len(notes) and notes[index+1]["tick"] == notes[index]["tick"])
    tolerance = 0 if index == 0 else 45 if chord else 90
    low = max(261, 261 + expected - tolerance - elapsed_ms)
    high = min(5252, 261 + expected + tolerance - elapsed_ms + 1)
    if low >= high:
        raise ValueError("Score cannot fit the model's timing vocabulary within the tempo tolerance")
    return low, high


def score(value):
    if not isinstance(value, dict):
        raise ValueError("Expected score object")
    integer(value, "schema", 1, 2, 1)
    bpm, bars = integer(value, "bpm", 40, 300), integer(value, "bars", 1, 32)
    ppq = integer(value, "ticks_per_beat", 96, 15360, 480)
    integer(value, "gm_program", 0, 0, 0)
    integer(value, "sharps", -7, 7, 0); integer(value, "minor", 0, 1, 0)
    notes = value.get("notes")
    if not isinstance(notes, list) or not 2 <= len(notes) <= 512:
        raise ValueError("Expected 2..512 score notes")
    end = bars * 4 * ppq
    result = []
    for n in notes:
        if not isinstance(n, dict):
            raise ValueError("Expected note object")
        tick = integer(n, "tick", 0, end - 1)
        duration = integer(n, "duration", 1, end - tick)
        if integer(n, "onset_offset_us", -500000, 500000, 0) or integer(n, "release_offset_us", -500000, 500000, 0):
            raise ValueError("Expression input must be an unperformed score; do not double-perform it")
        result.append(dict(tick=tick, duration=duration, pitch=integer(n, "pitch", 21, 108), velocity=integer(n, "velocity", 1, 127)))
    result.sort(key=lambda n: (n["tick"], n["pitch"]))
    active = {}
    for n in result:
        if active.get(n["pitch"], -1) > n["tick"]:
            raise ValueError("Resolve ambiguous same-pitch score overlap first")
        active[n["pitch"]] = n["tick"] + n["duration"]
    if result[-1]["tick"] == result[0]["tick"]:
        raise ValueError("Score needs more than one onset for tempo-preserving alignment")
    tokens, previous = [], 0
    for n in result:
        ms = round(n["tick"] * 60000 / (bpm * ppq))
        duration = round(n["duration"] * 60000 / (bpm * ppq))
        if ms - previous > 4990 or duration > 4999:
            raise ValueError("Score exceeds model's interval/duration vocabulary; split the phrase explicitly")
        tokens.extend((5 + n["pitch"], 261 + ms - previous, 133 + n["velocity"], 261 + max(1, duration), 5261, 5261, 5261, 5261))
        previous = ms
    return result, tokens


def perform(source, notes, tokens, timing="global"):
    if len(tokens) != len(notes) * 8 or any(type(t) is not int for t in tokens):
        raise ValueError("Incomplete generated performance")
    ranges = ((5, 133), (261, 5252), (134, 261), (262, 5261)) + ((5261, 5389),) * 4
    for i, token in enumerate(tokens):
        if not ranges[i % 8][0] <= token < ranges[i % 8][1]:
            raise ValueError("Generated token outside constrained vocabulary")
        if i % 8 == 0 and token - 5 != notes[i // 8]["pitch"]:
            raise ValueError("Model changed a score pitch")
    bpm, ppq, bars = source["bpm"], source.get("ticks_per_beat", 480), source["bars"]
    us_tick = 60000000 / (bpm * ppq)
    end_tick, end_us = bars * 4 * ppq, round(bars * 4 * 60000000 / bpm)
    raw, time_ms = [], 0
    for i in range(0, len(tokens), 8):
        time_ms += tokens[i + 1] - 261
        raw.append(time_ms * 1000)
    span = raw[-1] - raw[0]
    if span <= 0:
        raise ValueError("Model produced no advancing performance timeline")
    score_start, score_last = round(notes[0]["tick"] * us_tick), round(notes[-1]["tick"] * us_tick)
    if timing not in ("global", "tempo"):
        raise ValueError("Unknown timing policy")
    scale = (score_last - score_start) / span if timing == "global" else 1.0
    if not 0.5 <= scale <= 2:
        raise ValueError("Model's tempo drift exceeds the accepted alignment range")
    starts = [round(score_start + (v - raw[0]) * scale) for v in raw] if timing == "global" else raw
    output, finishes = [], []
    clipped, resolved = 0, 0

    def release(note, finish):
        last_tick = max(note["tick"] + 1, min(end_tick, round(finish / us_tick)))
        note["duration"] = last_tick - note["tick"]
        note["release_offset_us"] = finish - round(last_tick * us_tick)

    previous_pitch = {}
    for i, n in enumerate(notes):
        onset = starts[i] - round(n["tick"] * us_tick)
        if abs(onset) > 500000:
            raise ValueError("Learned rubato exceeds 500 ms; result rejected rather than silently flattened")
        requested = starts[i] + round((tokens[8 * i + 3] - 261) * 1000 * scale)
        finish = min(end_us, requested)
        clipped += int(finish != requested)
        if finish <= starts[i]:
            raise ValueError("Invalid generated note end")
        note = dict(n, velocity=tokens[8 * i + 2] - 133, onset_offset_us=onset)
        release(note, finish)
        if n["pitch"] in previous_pitch:
            previous = previous_pitch[n["pitch"]]
            if finishes[previous] > starts[i]:
                if starts[i] <= starts[previous]:
                    raise ValueError("Unresolvable simultaneous same-pitch performance")
                finishes[previous] = starts[i]; release(output[previous], starts[i]); resolved += 1
        previous_pitch[n["pitch"]] = i
        finishes.append(finish); output.append(note)
    points = {}
    for i, start in enumerate(starts):
        following = starts[i + 1] if i + 1 < len(starts) else end_us
        for j in range(4):
            points[round(start + (following - start) * j / 4)] = tokens[8 * i + 4 + j] - 5261
    pedals, previous = [], 0
    for micros, value in sorted(points.items()):
        if value == previous:
            continue
        tick = max(0, min(end_tick, round(micros / us_tick)))
        pedals.append(dict(tick=tick, offset_us=micros - round(tick * us_tick), value=value))
        previous = value
    result = {k: source.get(k, d) for k, d in (("bpm", 120), ("bars", 8), ("ticks_per_beat", 480), ("sharps", 0), ("minor", 0))}
    result.update(schema=2, gm_program=0, notes=output, pedals=pedals)
    diagnostics = dict(global_time_scale=scale, alignment="first and last score onsets anchored; fixed DAW tempo" if timing == "global" else "tempo constrained during learned decoding; no post hoc time stretch", clipped_note_tails=clipped,
                       resolved_same_pitch_releases=resolved, velocity_values=sorted({n["velocity"] for n in output}),
                       maximum_onset_offset_us=max(abs(n["onset_offset_us"]) for n in output), pedal_events=len(pedals), pitches_preserved=True)
    return result, diagnostics
