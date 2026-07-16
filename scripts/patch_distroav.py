#!/usr/bin/env python3
"""Patch DistroAV 6.2.1 to send OBS tracks 5/6 as four-channel NDI audio.

This script intentionally patches a clean DistroAV 6.2.1 checkout. It keeps
DistroAV's existing video sender and only replaces the audio capture path used
by Main Output with OBS raw multi-track capture.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

PATCH_MARKER = "DistroAV Multichannel Main Output hotfix v0.1.1"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one exact match, found {count}")
    return text.replace(old, new, 1)


def regex_once(text: str, pattern: str, replacement: str, label: str) -> str:
    result, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE | re.DOTALL)
    if count != 1:
        raise RuntimeError(f"{label}: expected one regex match, found {count}")
    return result


def patch_ndi_output(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if PATCH_MARKER in text:
        print(f"Already patched: {path}")
        return

    text = replace_once(
        text,
        '#include <util/threading.h>\n#include <chrono>\n',
        '#include <util/threading.h>\n#include <chrono>\n\n'
        '#include <algorithm>\n'
        '#include <array>\n'
        '#include <cstring>\n'
        '#include <mutex>\n'
        '#include <vector>\n',
        "C++ includes",
    )

    state_types = r'''
// DistroAV Multichannel Main Output hotfix v0.1.1
// OBS Track A is packed into NDI channels 1-2 and Track B into channels 3-4.
struct multichannel_stereo_block {
	bool valid = false;
	uint64_t timestamp = 0;
	uint32_t frames = 0;
	std::array<std::vector<float>, 2> channels;
};

struct multichannel_audio_state {
	std::mutex mutex;
	multichannel_stereo_block pending[2];
	std::vector<float> packed;
	uint64_t paired_blocks = 0;
	uint64_t dropped_blocks = 0;
};

'''
    text = replace_once(text, "typedef struct {\n", state_types + "typedef struct {\n", "state types")

    text = replace_once(
        text,
        "\tbool uses_audio;\n",
        "\tbool uses_audio;\n"
        "\tbool multichannel_audio;\n"
        "\tsize_t multichannel_track_a;\n"
        "\tsize_t multichannel_track_b;\n"
        "\tmultichannel_audio_state *multichannel_state;\n",
        "output state fields",
    )

    text = replace_once(
        text,
        '\tobs_data_set_default_bool(settings, "uses_audio", true);\n',
        '\tobs_data_set_default_bool(settings, "uses_audio", true);\n'
        '\tobs_data_set_default_bool(settings, "multichannel_audio", false);\n'
        '\tobs_data_set_default_int(settings, "multichannel_track_a", 4);\n'
        '\tobs_data_set_default_int(settings, "multichannel_track_b", 5);\n',
        "output defaults",
    )

    text = replace_once(
        text,
        "\tpthread_mutex_init(&o->ndi_sender_mutex, NULL);\n",
        "\tpthread_mutex_init(&o->ndi_sender_mutex, NULL);\n"
        "\to->multichannel_state = new multichannel_audio_state();\n",
        "state allocation",
    )

    old_audio_start = '''\tif (o->uses_audio && audio) {
\t\to->audio_samplerate = audio_output_get_sample_rate(audio);
\t\to->audio_channels = audio_output_get_channels(audio);
\t\tflags |= OBS_OUTPUT_AUDIO;
\t}
'''
    new_audio_start = '''\tif (o->uses_audio && audio) {
\t\to->audio_samplerate = audio_output_get_sample_rate(audio);
\t\tif (o->multichannel_audio) {
\t\t\to->audio_channels = 4;

\t\t\taudio_convert_info conversion{};
\t\t\tconversion.samples_per_sec = o->audio_samplerate;
\t\t\tconversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
\t\t\tconversion.speakers = SPEAKERS_STEREO;
\t\t\tobs_output_set_audio_conversion(o->output, &conversion);

\t\t\tobs_log(LOG_INFO,
\t\t\t\t"Multichannel NDI Main Output: OBS track %zu -> NDI 1-2, OBS track %zu -> NDI 3-4",
\t\t\t\to->multichannel_track_a + 1, o->multichannel_track_b + 1);
\t\t} else {
\t\t\to->audio_channels = audio_output_get_channels(audio);
\t\t}
\t\tflags |= OBS_OUTPUT_AUDIO;
\t}
'''
    text = replace_once(text, old_audio_start, new_audio_start, "audio start block")

    old_update = '''\to->uses_video = obs_data_get_bool(settings, "uses_video");
\to->uses_audio = obs_data_get_bool(settings, "uses_audio");
'''
    new_update = '''\to->uses_video = obs_data_get_bool(settings, "uses_video");
\to->uses_audio = obs_data_get_bool(settings, "uses_audio");
\to->multichannel_audio = obs_data_get_bool(settings, "multichannel_audio");

\tconst int64_t requested_track_a = obs_data_get_int(settings, "multichannel_track_a");
\tconst int64_t requested_track_b = obs_data_get_int(settings, "multichannel_track_b");
\to->multichannel_track_a =
\t\t(requested_track_a >= 0 && requested_track_a < MAX_AUDIO_MIXES) ? (size_t)requested_track_a : 4;
\to->multichannel_track_b =
\t\t(requested_track_b >= 0 && requested_track_b < MAX_AUDIO_MIXES) ? (size_t)requested_track_b : 5;
\tif (o->multichannel_track_a == o->multichannel_track_b) {
\t\to->multichannel_track_b = (o->multichannel_track_a + 1) % MAX_AUDIO_MIXES;
\t\tobs_log(LOG_WARNING,
\t\t\t"Multichannel NDI Main Output was given the same OBS track twice; using tracks %zu and %zu",
\t\t\to->multichannel_track_a + 1, o->multichannel_track_b + 1);
\t}
'''
    text = replace_once(text, old_update, new_update, "settings update")

    text = replace_once(
        text,
        "\t\tobs_output_end_data_capture(o->output);\n",
        "\t\tobs_output_end_data_capture(o->output);\n"
        "\t\tif (o->multichannel_state) {\n"
        "\t\t\tstd::lock_guard<std::mutex> lock(o->multichannel_state->mutex);\n"
        "\t\t\tfor (auto &block : o->multichannel_state->pending) {\n"
        "\t\t\t\tblock.valid = false;\n"
        "\t\t\t\tblock.frames = 0;\n"
        "\t\t\t}\n"
        "\t\t}\n",
        "stop cleanup",
    )

    text = replace_once(
        text,
        "\tpthread_mutex_destroy(&o->ndi_sender_mutex);\n",
        "\tdelete o->multichannel_state;\n"
        "\to->multichannel_state = nullptr;\n"
        "\tpthread_mutex_destroy(&o->ndi_sender_mutex);\n",
        "state destruction",
    )

    raw_audio2 = r'''
void ndi_output_rawaudio2(void *data, size_t mix_idx, audio_data *frame)
{
	auto o = (ndi_output_t *)data;
	if (!frame)
		return;

	// Preserve ordinary DistroAV outputs. Multi-track capture is enabled globally
	// so raw_audio2 is required, but only Main Output enables the four-channel packer.
	if (!o->multichannel_audio) {
		ndi_output_rawaudio(data, frame);
		return;
	}

	if (!o->started || !o->multichannel_state)
		return;

	size_t slot;
	if (mix_idx == o->multichannel_track_a)
		slot = 0;
	else if (mix_idx == o->multichannel_track_b)
		slot = 1;
	else
		return;

	auto *state = o->multichannel_state;
	std::lock_guard<std::mutex> lock(state->mutex);
	auto &incoming = state->pending[slot];
	incoming.valid = true;
	incoming.timestamp = frame->timestamp;
	incoming.frames = frame->frames;

	for (size_t channel = 0; channel < 2; ++channel) {
		auto &destination = incoming.channels[channel];
		destination.resize(frame->frames);
		const uint8_t *source = frame->data[channel] ? frame->data[channel] : frame->data[0];
		if (source) {
			std::memcpy(destination.data(), source, frame->frames * sizeof(float));
		} else {
			std::fill(destination.begin(), destination.end(), 0.0f);
		}
	}

	auto &track_a = state->pending[0];
	auto &track_b = state->pending[1];
	if (!track_a.valid || !track_b.valid)
		return;

	const uint64_t timestamp_delta = track_a.timestamp > track_b.timestamp
					 ? track_a.timestamp - track_b.timestamp
					 : track_b.timestamp - track_a.timestamp;
	const uint64_t one_sample_ns = o->audio_samplerate ? 1000000000ULL / o->audio_samplerate : 20834ULL;

	if (track_a.frames != track_b.frames || timestamp_delta > one_sample_ns) {
		++state->dropped_blocks;
		if (track_a.timestamp == track_b.timestamp || track_a.frames != track_b.frames) {
			track_a.valid = false;
			track_b.valid = false;
		} else if (track_a.timestamp < track_b.timestamp) {
			track_a.valid = false;
		} else {
			track_b.valid = false;
		}

		if ((state->dropped_blocks % 250) == 1) {
			obs_log(LOG_WARNING,
				"Multichannel NDI Main Output discarded an unmatched audio block "
				"(count=%llu, timestamp delta=%llu ns)",
				(unsigned long long)state->dropped_blocks, (unsigned long long)timestamp_delta);
		}
		return;
	}

	const size_t frames = track_a.frames;
	state->packed.resize(frames * 4);
	for (size_t channel = 0; channel < 2; ++channel) {
		std::memcpy(state->packed.data() + channel * frames, track_a.channels[channel].data(),
			frames * sizeof(float));
		std::memcpy(state->packed.data() + (channel + 2) * frames, track_b.channels[channel].data(),
			frames * sizeof(float));
	}

	audio_data combined{};
	combined.frames = (uint32_t)frames;
	combined.timestamp = std::max(track_a.timestamp, track_b.timestamp);
	for (size_t channel = 0; channel < 4; ++channel)
		combined.data[channel] = reinterpret_cast<uint8_t *>(state->packed.data() + channel * frames);

	track_a.valid = false;
	track_b.valid = false;
	++state->paired_blocks;

	// The state mutex intentionally remains held until the synchronous NDI audio
	// call returns, keeping the packed storage stable for the duration of the send.
	ndi_output_rawaudio(data, &combined);
}

'''
    text = replace_once(
        text,
        "obs_output_info create_ndi_output_info()\n",
        raw_audio2 + "obs_output_info create_ndi_output_info()\n",
        "raw_audio2 insertion",
    )

    text = replace_once(
        text,
        "\tndi_output_info.flags = OBS_OUTPUT_AV;\n",
        "\tndi_output_info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_MULTI_TRACK;\n",
        "multi-track capability",
    )
    text = replace_once(
        text,
        "\tndi_output_info.raw_audio = ndi_output_rawaudio;\n",
        "\tndi_output_info.raw_audio2 = ndi_output_rawaudio2;\n",
        "raw audio callback",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Patched: {path}")


def patch_main_output(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = 'obs_data_set_bool(output_settings, "multichannel_audio", true);'
    if marker in text:
        print(f"Already patched: {path}")
        return

    # DistroAV 6.2.1 uses tabs whose exact depth has changed between source
    # archives/checkouts. Anchor on the unique Main Output creation statement
    # rather than matching a whitespace-sensitive multi-line block.
    create_pattern = re.compile(
        r'(?P<indent>^[ \t]*)'
        r'context\.output\s*=\s*obs_output_create\(\s*'
        r'"ndi_output"\s*,\s*"NDI Main Output"\s*,\s*'
        r'output_settings\s*,\s*nullptr\s*\)\s*;',
        flags=re.MULTILINE,
    )
    matches = list(create_pattern.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(
            "Main Output creation: expected one exact semantic match, "
            f"found {len(matches)}"
        )

    match = matches[0]
    indent = match.group("indent")
    injection = (
        f'{indent}// DistroAV Multichannel Main Output hotfix v0.1.1\n'
        f'{indent}// OBS tracks are zero-based internally: 4 = Track 5, 5 = Track 6.\n'
        f'{indent}obs_data_set_bool(output_settings, "multichannel_audio", true);\n'
        f'{indent}obs_data_set_int(output_settings, "multichannel_track_a", 4);\n'
        f'{indent}obs_data_set_int(output_settings, "multichannel_track_b", 5);\n\n'
    )
    text = text[:match.start()] + injection + text[match.start():]

    # Configure both requested OBS mixes immediately after the output is created.
    # Keep this separate from output creation so failure/null handling remains
    # identical to upstream DistroAV.
    release_pattern = re.compile(
        r'(?P<indent>^[ \t]*)obs_data_release\(\s*output_settings\s*\)\s*;',
        flags=re.MULTILINE,
    )

    # main-output.cpp contains one release in the support test and one in
    # main_output_init(). Select the first release occurring after the unique
    # context.output creation we just patched.
    create_pos = text.index(
        'context.output = obs_output_create("ndi_output", "NDI Main Output", output_settings, nullptr);'
    )
    release_match = release_pattern.search(text, create_pos)
    if not release_match:
        raise RuntimeError(
            "Main Output settings release: could not find obs_data_release(output_settings) "
            "after context.output creation"
        )

    indent = release_match.group("indent")
    mixer_setup = (
        f'{indent}if (context.output) {{\n'
        f'{indent}\tconst size_t multichannel_mixers = (size_t(1) << 4) | (size_t(1) << 5);\n'
        f'{indent}\tobs_output_set_mixers(context.output, multichannel_mixers);\n'
        f'{indent}\tobs_log(LOG_INFO,\n'
        f'{indent}\t\t"DistroAV Multichannel Main Output hotfix enabled: "\n'
        f'{indent}\t\t"Track 5 -> NDI 1-2, Track 6 -> NDI 3-4");\n'
        f'{indent}}}\n'
    )
    insert_at = release_match.end()
    text = text[:insert_at] + "\n" + mixer_setup + text[insert_at:]

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Patched: {path}")


def write_notice(root: Path) -> None:
    notice = root / "MULTICHANNEL-HOTFIX.md"
    notice.write_text(
        "# DistroAV Multichannel Main Output hotfix v0.1.1\n\n"
        "This tree was generated from the DistroAV 6.2.1 tag. The hotfix changes "
        "Main Output audio capture to OBS raw multi-track mode and packs:\n\n"
        "- OBS Track 5 left/right into NDI channels 1/2\n"
        "- OBS Track 6 left/right into NDI channels 3/4\n\n"
        "The existing DistroAV video path and its single NDI sender are retained. "
        "Other DistroAV output instances remain in ordinary audio mode.\n\n"
        "This is experimental and is not affiliated with or endorsed by the DistroAV project.\n",
        encoding="utf-8",
        newline="\n",
    )


def verify(root: Path) -> None:
    ndi = (root / "src" / "ndi-output.cpp").read_text(encoding="utf-8")
    main = (root / "src" / "main-output.cpp").read_text(encoding="utf-8")
    required = [
        PATCH_MARKER,
        "OBS_OUTPUT_AV | OBS_OUTPUT_MULTI_TRACK",
        "raw_audio2 = ndi_output_rawaudio2",
        'obs_data_set_bool(output_settings, "multichannel_audio", true)',
        "obs_output_set_mixers(context.output, multichannel_mixers)",
        "o->audio_channels = 4",
    ]
    joined = ndi + "\n" + main
    missing = [marker for marker in required if marker not in joined]
    if missing:
        raise RuntimeError("Patch verification failed; missing: " + ", ".join(missing))
    print("Patch verification passed.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path, help="Path to a clean DistroAV 6.2.1 checkout")
    args = parser.parse_args()
    root = args.root.resolve()

    buildspec_path = root / "buildspec.json"
    ndi_path = root / "src" / "ndi-output.cpp"
    main_path = root / "src" / "main-output.cpp"
    for required in (buildspec_path, ndi_path, main_path):
        if not required.exists():
            raise FileNotFoundError(required)

    spec = json.loads(buildspec_path.read_text(encoding="utf-8"))
    if spec.get("name") != "distroav" or spec.get("version") != "6.2.1":
        raise RuntimeError(
            f"This alpha patch targets a clean DistroAV 6.2.1 tree; found "
            f"name={spec.get('name')!r}, version={spec.get('version')!r}"
        )

    patch_ndi_output(ndi_path)
    patch_main_output(main_path)
    write_notice(root)
    verify(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
