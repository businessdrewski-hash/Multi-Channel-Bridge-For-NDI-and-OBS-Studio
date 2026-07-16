#!/usr/bin/env python3
"""Apply Multichannel Bridge for DistroAV v0.3.1-alpha to DistroAV 6.2.1.

The resulting custom DistroAV package is installed on BOTH computers. The OBS
Dock selects Gaming PC / Sender or Stream PC / Receiver.

Sender mode captures two OBS stereo mixes, aligns them with bounded FIFO
queues while tolerating OBS's normal one-block timestamp phase between raw
multitrack callbacks, and transmits one four-channel NDI audio frame beside
DistroAV's existing video frame.

Receiver mode intercepts DistroAV's raw planar NDI audio before OBS remixes it,
then exposes channels 1-2 and 3-4 as two independent stereo OBS mixer sources.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path

PATCH_MARKER = "Multichannel Bridge for DistroAV v0.3.1-alpha"


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


def patch_cmake(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "src/multichannel-bridge.cpp" in text:
        print(f"Already patched: {path}")
        return
    text = regex_once(
        text,
        r"^(?P<indent>[ \t]*)src/main-output\.h[ \t]*$",
        r"\g<0>\n\g<indent>src/multichannel-bridge.cpp\n\g<indent>src/multichannel-bridge.h",
        "CMake source list",
    )
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Patched: {path}")


def patch_plugin_main(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if '#include "multichannel-bridge.h"' in text:
        print(f"Already patched: {path}")
        return
    text = regex_once(
        text,
        r'^(?P<indent>[ \t]*)#include[ \t]+"preview-output\.h"[ \t]*$',
        r'\g<0>\n\g<indent>#include "multichannel-bridge.h"',
        "plugin include",
    )
    text = regex_once(
        text,
        r'(?P<indent>^[ \t]*)alpha_filter_info[ \t]*=[ \t]*create_alpha_filter_info\(\);[ \t]*\n'
        r'(?P=indent)obs_register_source\(&alpha_filter_info\);',
        r'\g<0>\n\g<indent>mcb_register_sources();',
        "bridge source registration",
    )
    text = regex_once(
        text,
        r'(?P<indent>^[ \t]*)if[ \t]*\([ \t]*main_window[ \t]*\)[ \t]*\{[ \t]*\n'
        r'(?P<body>[ \t]*)auto[ \t]+menu_action[ \t]*=',
        r'\g<indent>if (main_window) {\n\g<body>mcb_init(main_window);\n\g<body>auto menu_action =',
        "bridge dock initialization",
    )
    text = regex_once(
        text,
        r'(void[ \t]+obs_module_unload\(void\)[ \t]*(?:\n[ \t]*)?\{[ \t]*\n'
        r'(?P<indent>[ \t]*)obs_log\(LOG_DEBUG,[ \t]*"\+obs_module_unload\(\)"\);)',
        r'\g<0>\n\g<indent>mcb_shutdown();',
        "bridge shutdown",
    )
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Patched: {path}")


def patch_main_output(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if 'obs_data_set_bool(output_settings, "multichannel_audio", true);' in text:
        print(f"Already patched: {path}")
        return
    text = replace_once(
        text,
        '#include "plugin-main.h"\n',
        '#include "plugin-main.h"\n#include "multichannel-bridge.h"\n',
        "Main Output include",
    )
    text = regex_once(
        text,
        r'(?P<head>void[ \t]+main_output_init\(\)[ \t]*(?:\n[ \t]*)?\{[ \t]*\n'
        r'(?P<indent>[ \t]*)obs_log\(LOG_DEBUG,[ \t]*"\+main_output_init\(\)"\);)',
        r'\g<head>\n\g<indent>if (mcb_is_receiver()) {\n'
        r'\g<indent>\tmain_output_deinit();\n'
        r'\g<indent>\tobs_log(LOG_INFO, "[multichannel-bridge] Receiver role: DistroAV Main Output is disabled on this PC");\n'
        r'\g<indent>\treturn;\n\g<indent>}',
        "receiver role Main Output guard",
    )

    create_pattern = re.compile(
        r'(?P<indent>^[ \t]*)context\.output\s*=\s*obs_output_create\(\s*'
        r'"ndi_output"\s*,\s*"NDI Main Output"\s*,\s*output_settings\s*,\s*nullptr\s*\)\s*;',
        flags=re.MULTILINE,
    )
    matches = list(create_pattern.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(f"Main Output creation: expected one semantic match, found {len(matches)}")
    match = matches[0]
    indent = match.group("indent")
    before = (
        f'{indent}// {PATCH_MARKER}\n'
        f'{indent}const bool multichannel_bridge_sender = mcb_is_sender();\n'
        f'{indent}const size_t multichannel_track_a = mcb_sender_track_a_zero_based();\n'
        f'{indent}const size_t multichannel_track_b = mcb_sender_track_b_zero_based();\n'
        f'{indent}if (multichannel_bridge_sender) {{\n'
        f'{indent}\tobs_data_set_bool(output_settings, "multichannel_audio", true);\n'
        f'{indent}\tobs_data_set_int(output_settings, "multichannel_track_a", (long long)multichannel_track_a);\n'
        f'{indent}\tobs_data_set_int(output_settings, "multichannel_track_b", (long long)multichannel_track_b);\n'
        f'{indent}}}\n\n'
    )
    text = text[: match.start()] + before + text[match.start() :]

    relocated_create = create_pattern.search(text, match.start() + len(before))
    if not relocated_create:
        raise RuntimeError("Could not relocate Main Output creation after settings insertion")
    release_pattern = re.compile(
        r'(?P<indent>^[ \t]*)obs_data_release\(\s*output_settings\s*\)\s*;', flags=re.MULTILINE
    )
    release = release_pattern.search(text, relocated_create.end())
    if not release:
        raise RuntimeError("Could not find Main Output settings release after output creation")
    indent = release.group("indent")
    after = (
        f'\n{indent}if (context.output && multichannel_bridge_sender) {{\n'
        f'{indent}\tconst size_t mixers = (size_t(1) << multichannel_track_a) | (size_t(1) << multichannel_track_b);\n'
        f'{indent}\tobs_output_set_mixers(context.output, mixers);\n'
        f'{indent}\tobs_log(LOG_INFO, "[multichannel-bridge] Sender enabled: Track %zu -> NDI 1-2, Track %zu -> NDI 3-4",\n'
        f'{indent}\t\tmultichannel_track_a + 1, multichannel_track_b + 1);\n'
        f'{indent}}}\n'
    )
    text = text[: release.end()] + after + text[release.end() :]
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Patched: {path}")


def patch_ndi_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "mcb_receiver_route_audio" in text:
        print(f"Already patched: {path}")
        return
    text = replace_once(
        text,
        '#include "plugin-main.h"\n',
        '#include "plugin-main.h"\n#include "multichannel-bridge.h"\n',
        "NDI source include",
    )
    old = "\tobs_source_output_audio(obs_source, obs_audio_frame);\n"
    new = (
        "\t// Multichannel Bridge for DistroAV receiver hook: split raw planar NDI channels\n"
        "\t// before OBS remixes the source to the profile speaker layout.\n"
        "\tconst bool suppress_original_audio =\n"
        "\t\tmcb_receiver_route_audio(obs_source, obs_audio_frame, channelCount);\n"
        "\tif (!suppress_original_audio)\n"
        "\t\tobs_source_output_audio(obs_source, obs_audio_frame);\n"
    )
    text = replace_once(text, old, new, "raw receiver audio hook")
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Patched: {path}")


def patch_ndi_output(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if PATCH_MARKER in text:
        print(f"Already patched: {path}")
        return
    text = replace_once(
        text,
        '#include <util/threading.h>\n#include <chrono>\n',
        '#include <util/threading.h>\n#include <chrono>\n\n'
        '#include "multichannel-bridge.h"\n'
        '#include <algorithm>\n#include <array>\n#include <cmath>\n#include <cstdlib>\n#include <cstring>\n'
        '#include <deque>\n#include <mutex>\n#include <vector>\n',
        "NDI output includes",
    )

    state_types = r'''
// Multichannel Bridge for DistroAV v0.3.1-alpha
// A bounded FIFO is required because OBS uses one shared raw-output frame
// counter for all selected mixers. Adjacent mixer callbacks therefore normally
// carry timestamps one 1024-frame block apart even when their samples represent
// the same OBS mix interval. Accept one block of phase; discard only when a
// queue falls farther behind than that.
struct multichannel_stereo_block {
	uint64_t timestamp = 0;
	uint32_t frames = 0;
	float peak = 0.0f;
	std::array<std::vector<float>, 2> channels;
};

struct multichannel_audio_state {
	std::mutex mutex;
	std::deque<multichannel_stereo_block> pending[2];
	std::vector<float> packed;
	std::vector<float> silence;
	float latest_peak[2] = {0.0f, 0.0f};
	uint64_t paired_blocks = 0;
	uint64_t discarded_blocks = 0;
	uint64_t fallback_blocks = 0;
};

'''
    text = replace_once(text, "typedef struct {\n", state_types + "typedef struct {\n", "multichannel state types")
    text = replace_once(
        text,
        "\tbool uses_audio;\n",
        "\tbool uses_audio;\n\tbool multichannel_audio;\n\tsize_t multichannel_track_a;\n"
        "\tsize_t multichannel_track_b;\n\tmultichannel_audio_state *multichannel_state;\n",
        "output fields",
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
        "\tpthread_mutex_init(&o->ndi_sender_mutex, NULL);\n\to->multichannel_state = new multichannel_audio_state();\n",
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
\t\t\tmcb_sender_status_started(o->multichannel_track_a, o->multichannel_track_b);
\t\t\tobs_log(LOG_INFO,
\t\t\t\t"[multichannel-bridge] Multichannel Main Output started: OBS Track %zu -> NDI 1-2, OBS Track %zu -> NDI 3-4",
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
\tif (o->multichannel_track_a == o->multichannel_track_b)
\t\to->multichannel_track_b = (o->multichannel_track_a + 1) % MAX_AUDIO_MIXES;
'''
    text = replace_once(text, old_update, new_update, "settings update")

    text = replace_once(
        text,
        "\t\tobs_output_end_data_capture(o->output);\n",
        "\t\tobs_output_end_data_capture(o->output);\n"
        "\t\tif (o->multichannel_audio)\n\t\t\tmcb_sender_status_stopped();\n"
        "\t\tif (o->multichannel_state) {\n"
        "\t\t\tstd::lock_guard<std::mutex> lock(o->multichannel_state->mutex);\n"
        "\t\t\to->multichannel_state->pending[0].clear();\n"
        "\t\t\to->multichannel_state->pending[1].clear();\n\t\t}\n",
        "stop cleanup",
    )
    text = replace_once(
        text,
        "\tpthread_mutex_destroy(&o->ndi_sender_mutex);\n",
        "\tdelete o->multichannel_state;\n\to->multichannel_state = nullptr;\n"
        "\tpthread_mutex_destroy(&o->ndi_sender_mutex);\n",
        "state destruction",
    )

    raw_audio2 = r'''
static float multichannel_peak(const multichannel_stereo_block *block)
{
	return block ? block->peak : 0.0f;
}

static void multichannel_send_block(ndi_output_t *o, const multichannel_stereo_block *track_a,
	const multichannel_stereo_block *track_b)
{
	auto *state = o->multichannel_state;
	const multichannel_stereo_block *reference = track_a ? track_a : track_b;
	if (!state || !reference || reference->frames == 0)
		return;
	const size_t frames = reference->frames;
	state->silence.assign(frames, 0.0f);
	state->packed.resize(frames * 4);
	for (size_t channel = 0; channel < 2; ++channel) {
		const float *a = track_a ? track_a->channels[channel].data() : state->silence.data();
		const float *b = track_b ? track_b->channels[channel].data() : state->silence.data();
		std::memcpy(state->packed.data() + channel * frames, a, frames * sizeof(float));
		std::memcpy(state->packed.data() + (channel + 2) * frames, b, frames * sizeof(float));
	}

	audio_data combined{};
	combined.frames = (uint32_t)frames;
	combined.timestamp = track_a && track_b ? std::max(track_a->timestamp, track_b->timestamp)
						 : track_a ? track_a->timestamp : track_b->timestamp;
	for (size_t channel = 0; channel < 4; ++channel)
		combined.data[channel] = reinterpret_cast<uint8_t *>(state->packed.data() + channel * frames);

	mcb_sender_status_levels(multichannel_peak(track_a), multichannel_peak(track_b));
	// DistroAV's NDI send call is synchronous; packed storage remains stable while
	// the state mutex is held by the caller.
	ndi_output_rawaudio(o, &combined);
}

void ndi_output_rawaudio2(void *data, size_t mix_idx, audio_data *frame)
{
	auto *o = (ndi_output_t *)data;
	if (!frame)
		return;

	if (!o->multichannel_audio) {
		// OBS_OUTPUT_MULTI_TRACK is a type-level capability. Preserve ordinary
		// DistroAV outputs by forwarding only their first selected mixer.
		size_t mask = obs_output_get_mixers(o->output);
		size_t primary = 0;
		if (mask) {
			while (primary < MAX_AUDIO_MIXES && (mask & (size_t(1) << primary)) == 0)
				++primary;
		}
		if (mix_idx == primary)
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

	multichannel_stereo_block incoming;
	incoming.timestamp = frame->timestamp;
	incoming.frames = frame->frames;
	for (size_t channel = 0; channel < 2; ++channel) {
		incoming.channels[channel].resize(frame->frames);
		const uint8_t *source = frame->data[channel] ? frame->data[channel] : frame->data[0];
		if (source) {
			std::memcpy(incoming.channels[channel].data(), source, frame->frames * sizeof(float));
			const float *samples = reinterpret_cast<const float *>(source);
			for (uint32_t i = 0; i < frame->frames; ++i)
				incoming.peak = std::max(incoming.peak, std::fabs(samples[i]));
		} else {
			std::fill(incoming.channels[channel].begin(), incoming.channels[channel].end(), 0.0f);
		}
	}
	incoming.peak = std::min(incoming.peak, 1.0f);

	auto *state = o->multichannel_state;
	std::lock_guard<std::mutex> lock(state->mutex);
	state->latest_peak[slot] = incoming.peak;
	mcb_sender_status_levels(state->latest_peak[0], state->latest_peak[1]);
	state->pending[slot].push_back(std::move(incoming));

	constexpr size_t max_queue_depth = 12;
	if (state->pending[slot].size() > max_queue_depth) {
		state->pending[slot].pop_front();
		++state->discarded_blocks;
		mcb_sender_status_discarded(0, state->pending[0].size(), state->pending[1].size());
	}

	auto &queue_a = state->pending[0];
	auto &queue_b = state->pending[1];
	const uint64_t one_sample_ns = o->audio_samplerate ? 1000000000ULL / o->audio_samplerate : 20834ULL;

	while (!queue_a.empty() && !queue_b.empty()) {
		auto &a = queue_a.front();
		auto &b = queue_b.front();
		const int64_t delta = (int64_t)b.timestamp - (int64_t)a.timestamp;
		const uint64_t absolute_delta = (uint64_t)std::llabs(delta);
		const uint64_t block_duration_ns = o->audio_samplerate
			? ((uint64_t)std::max(a.frames, b.frames) * 1000000000ULL) / o->audio_samplerate
			: 21333333ULL;
		// OBS increments one shared raw-output audio counter once for every selected
		// mixer callback. With two mixers, corresponding blocks normally differ by
		// exactly one block duration (21.333 ms at 48 kHz / 1024 frames).
		const uint64_t timestamp_tolerance_ns =
			block_duration_ns + std::max<uint64_t>(500000ULL, one_sample_ns * 8ULL);

		if (a.frames == b.frames && absolute_delta <= timestamp_tolerance_ns) {
			multichannel_send_block(o, &a, &b);
			queue_a.pop_front();
			queue_b.pop_front();
			++state->paired_blocks;
			mcb_sender_status_paired(delta, queue_a.size(), queue_b.size());
			continue;
		}

		// More than one full block apart means one FIFO is genuinely stale.
		// Discard only the older front and preserve the newer block for recovery.
		if (a.timestamp <= b.timestamp)
			queue_a.pop_front();
		else
			queue_b.pop_front();
		++state->discarded_blocks;
		mcb_sender_status_discarded(delta, queue_a.size(), queue_b.size());
	}

	// Keep NDI audio alive if one selected OBS mix genuinely stops producing
	// callbacks. Four queued blocks gives the other mix time to catch up first.
	constexpr size_t fallback_depth = 4;
	if (queue_a.size() >= fallback_depth && queue_b.empty()) {
		multichannel_send_block(o, &queue_a.front(), nullptr);
		queue_a.pop_front();
		++state->fallback_blocks;
		mcb_sender_status_silence_fallback(queue_a.size(), queue_b.size());
	} else if (queue_b.size() >= fallback_depth && queue_a.empty()) {
		multichannel_send_block(o, nullptr, &queue_b.front());
		queue_b.pop_front();
		++state->fallback_blocks;
		mcb_sender_status_silence_fallback(queue_a.size(), queue_b.size());
	}
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


def copy_bridge_files(root: Path, bridge_dir: Path) -> None:
    for name in ("multichannel-bridge.cpp", "multichannel-bridge.h"):
        source = bridge_dir / name
        if not source.exists():
            raise FileNotFoundError(source)
        shutil.copy2(source, root / "src" / name)
        print(f"Copied: {root / 'src' / name}")


def write_notice(root: Path) -> None:
    (root / "MULTICHANNEL-BRIDGE.md").write_text(
        "# Multichannel Bridge for DistroAV v0.3.1-alpha\n\n"
        "Custom DistroAV 6.2.1 build. Install the same package on both PCs, then use "
        "Docks > Multichannel Bridge for DistroAV to select Gaming PC / Sender or Stream PC / Receiver.\n\n"
        "Sender defaults: OBS Track 5 -> NDI channels 1-2; OBS Track 6 -> channels 3-4.\n"
        "Receiver: one normal DistroAV NDI Source provides video while the bridge exposes the two stereo "
        "pairs as independent OBS audio-only sources.\n\n"
        "Experimental. Not affiliated with or endorsed by DistroAV. DistroAV remains GPL-2.0-or-later.\n",
        encoding="utf-8",
        newline="\n",
    )


def verify(root: Path) -> None:
    joined = "\n".join(
        (root / path).read_text(encoding="utf-8")
        for path in (
            "src/ndi-output.cpp",
            "src/ndi-source.cpp",
            "src/main-output.cpp",
            "src/plugin-main.cpp",
            "src/multichannel-bridge.cpp",
            "src/multichannel-bridge.h",
            "CMakeLists.txt",
        )
    )
    required = [
        PATCH_MARKER,
        "OBS_OUTPUT_AV | OBS_OUTPUT_MULTI_TRACK",
        "raw_audio2 = ndi_output_rawaudio2",
        "mcb_register_sources();",
        "mcb_init(main_window);",
        "obs_frontend_add_dock_by_id",
        "mcb_receiver_route_audio(obs_source, obs_audio_frame, channelCount)",
        "mcb_sender_status_paired",
        "src/multichannel-bridge.cpp",
        "Track A and Track B must be different",
    ]
    missing = [item for item in required if item not in joined]
    if missing:
        raise RuntimeError("Patch verification failed; missing: " + ", ".join(missing))
    print("Patch verification passed.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path, help="Path to a clean DistroAV 6.2.1 checkout")
    parser.add_argument(
        "--bridge-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "bridge",
        help="Directory containing multichannel-bridge.cpp/.h",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    bridge_dir = args.bridge_dir.resolve()

    required = [
        root / "buildspec.json",
        root / "CMakeLists.txt",
        root / "src" / "ndi-output.cpp",
        root / "src" / "ndi-source.cpp",
        root / "src" / "main-output.cpp",
        root / "src" / "plugin-main.cpp",
    ]
    for path in required:
        if not path.exists():
            raise FileNotFoundError(path)
    spec = json.loads((root / "buildspec.json").read_text(encoding="utf-8"))
    if spec.get("name") != "distroav" or spec.get("version") != "6.2.1":
        raise RuntimeError(
            f"This patch targets a clean DistroAV 6.2.1 tree; found "
            f"name={spec.get('name')!r}, version={spec.get('version')!r}"
        )

    copy_bridge_files(root, bridge_dir)
    patch_cmake(root / "CMakeLists.txt")
    patch_plugin_main(root / "src" / "plugin-main.cpp")
    patch_main_output(root / "src" / "main-output.cpp")
    patch_ndi_source(root / "src" / "ndi-source.cpp")
    patch_ndi_output(root / "src" / "ndi-output.cpp")
    write_notice(root)
    verify(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
