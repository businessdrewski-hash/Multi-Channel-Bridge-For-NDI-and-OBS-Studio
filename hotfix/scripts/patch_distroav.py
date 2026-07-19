#!/usr/bin/env python3
"""Apply Multichannel Bridge for DistroAV v0.6.0-alpha5 to DistroAV 6.2.1.

The resulting custom DistroAV package is installed on BOTH computers. The OBS
Dock selects Gaming PC / Sender or Stream PC / Receiver.

Sender mode captures two OBS stereo mixes, aligns them with a preallocated
fixed-capacity synchronizer while tolerating OBS's normal one-block timestamp
phase between raw multitrack callbacks, and transmits one four-channel NDI
audio frame beside DistroAV's existing video frame. The bridge packing path
performs no allocation and adds no callback mutex wait.

Receiver mode intercepts DistroAV's raw planar NDI audio before OBS remixes it,
then exposes channels 1-2 and 3-4 as two independent stereo OBS mixer sources.

Downstream Sync Core 2.0 observes the video and split-audio timelines after they
enter OBS. Video remains the master and passes through untouched. One shared,
slew-limited audio-rate command corrects both stereo outputs together after
stable long-window evidence, with trusted-reference recovery after discontinuities.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path

PATCH_MARKER = "Multichannel Bridge for DistroAV v0.6.0-alpha5"


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
        r"\g<0>\n\g<indent>src/sender-sync-core.cpp\n\g<indent>src/sender-sync-core.h"
        r"\n\g<indent>src/downstream-sync-core.cpp\n\g<indent>src/downstream-sync-core.h"
        r"\n\g<indent>src/multichannel-bridge.cpp\n\g<indent>src/multichannel-bridge.h",
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
    audio_patched = "mcb_receiver_route_audio" in text
    video_patched = "mcb_receiver_route_video" in text
    if audio_patched and video_patched:
        print(f"Already patched: {path}")
        return
    if audio_patched or video_patched:
        raise RuntimeError(f"Partially patched NDI source detected: {path}")
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
        "\t\tmcb_receiver_route_audio(obs_source, obs_audio_frame, channelCount,\n"
        "\t\t\tndi_audio_frame->timestamp, ndi_audio_frame->timecode);\n"
        "\tif (!suppress_original_audio)\n"
        "\t\tobs_source_output_audio(obs_source, obs_audio_frame);\n"
    )
    text = replace_once(text, old, new, "raw receiver audio hook")
    old_video = "\tobs_source_output_video(obs_source, obs_video_frame);\n"
    new_video = (
        "\t// Downstream Sync Core observes video later in OBS; keep this handoff untouched.\n"
        "\tif (mcb_receiver_route_video(obs_source, obs_video_frame,\n"
        "\t\t\tndi_video_frame->timestamp, ndi_video_frame->timecode))\n"
        "\t\tobs_source_output_video(obs_source, obs_video_frame);\n"
    )
    text = replace_once(text, old_video, new_video, "raw receiver video governor hook")
    reconnect_proc = r'''
static void mcb_force_reconnect_proc(void *data, calldata_t *params)
{
	auto *source = static_cast<ndi_source_t *>(data);
	const bool accepted = source != nullptr;
	if (source)
		source->config.reset_ndi_receiver = true;
	if (params)
		calldata_set_bool(params, "accepted", accepted);
}

'''
    text = replace_once(
        text,
        "void *ndi_source_create(obs_data_t *settings, obs_source_t *obs_source)\n",
        reconnect_proc + "void *ndi_source_create(obs_data_t *settings, obs_source_t *obs_source)\n",
        "receiver reconnect procedure",
    )
    text = replace_once(
        text,
        "\tndi_source_update(s, settings);\n",
        "\tndi_source_update(s, settings);\n"
        "\tproc_handler_add(obs_source_get_proc_handler(obs_source),\n"
        "\t\t\"void mcb_force_reconnect(out bool accepted)\", mcb_force_reconnect_proc, s);\n",
        "receiver reconnect registration",
    )
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
        '#include "sender-sync-core.h"\n'
        '#include <algorithm>\n#include <atomic>\n#include <cstdint>\n',
        "NDI output includes",
    )

    state_types = r'''
// Multichannel Bridge for DistroAV v0.6.0-alpha5. SenderSyncCore owns all
// sample storage up front. The atomic flag is a non-blocking safety guard: OBS
// normally serializes selected-mixer callbacks, but an unexpected concurrent
// callback is dropped instead of waiting on the real-time audio thread.
struct multichannel_audio_state {
	mcb::SenderSyncCore core;
	std::atomic_flag callback_busy = ATOMIC_FLAG_INIT;
	uint64_t applied_reanchor_generation = 0;
	uint64_t applied_counter_reset_generation = 0;
	std::atomic_uint64_t contention_drops{0};
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
\t\t\to->multichannel_state->core.configure(o->audio_samplerate);
\t\t\to->multichannel_state->applied_reanchor_generation = mcb_sender_reanchor_generation();
\t\t\to->multichannel_state->applied_counter_reset_generation = mcb_sender_counter_reset_generation();
\t\t\taudio_convert_info conversion{};
\t\t\tconversion.samples_per_sec = o->audio_samplerate;
\t\t\tconversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
\t\t\tconversion.speakers = SPEAKERS_STEREO;
\t\t\tobs_output_set_audio_conversion(o->output, &conversion);
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
    text = replace_once(
        text,
        "		o->started = obs_output_begin_data_capture(o->output, flags);\n		if (o->started) {\n",
        "		o->started = obs_output_begin_data_capture(o->output, flags);\n		if (o->started) {\n"
        "			if (o->multichannel_audio)\n"
        "				mcb_sender_status_started(o->multichannel_track_a, o->multichannel_track_b);\n",
        "sender status activation",
    )

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
        "\t\tif (o->multichannel_state)\n"
        "\t\t\to->multichannel_state->core.reset(false);\n",
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
class multichannel_callback_guard {
public:
	explicit multichannel_callback_guard(multichannel_audio_state *state) : state_(state)
	{
		acquired_ = state_ && !state_->callback_busy.test_and_set(std::memory_order_acquire);
		if (state_ && !acquired_)
			++state_->contention_drops;
	}
	~multichannel_callback_guard()
	{
		if (acquired_)
			state_->callback_busy.clear(std::memory_order_release);
	}
	explicit operator bool() const { return acquired_; }
private:
	multichannel_audio_state *state_ = nullptr;
	bool acquired_ = false;
};

static void multichannel_publish_status(multichannel_audio_state *state)
{
	const auto snapshot = state->core.snapshot();
	mcb_sender_status_sync(snapshot.paired_blocks, snapshot.discarded_blocks,
		snapshot.silence_fallback_blocks, snapshot.discontinuities,
		snapshot.reanchors, snapshot.oversized_blocks,
		state->contention_drops.load(std::memory_order_relaxed),
		snapshot.epoch, snapshot.last_timestamp_delta_ns,
		snapshot.queue_depth_a, snapshot.queue_depth_b,
		snapshot.peak_a, snapshot.peak_b);
}

static void multichannel_send_output(ndi_output_t *o, const mcb::SenderSyncCore::Output &output)
{
	audio_data combined{};
	combined.frames = output.frames;
	combined.timestamp = output.timestamp_ns;
	for (size_t channel = 0; channel < mcb::SenderSyncCore::kOutputChannels; ++channel)
		combined.data[channel] = reinterpret_cast<uint8_t *>(const_cast<float *>(output.data[channel]));
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

	auto *state = o->multichannel_state;
	multichannel_callback_guard guard(state);
	if (!guard) {
		mcb_sender_status_contention_drop();
		return;
	}

	const uint64_t requested_reanchor = mcb_sender_reanchor_generation();
	if (requested_reanchor != state->applied_reanchor_generation) {
		state->core.reanchor();
		state->applied_reanchor_generation = requested_reanchor;
	}
	const uint64_t requested_counter_reset = mcb_sender_counter_reset_generation();
	if (requested_counter_reset != state->applied_counter_reset_generation) {
		state->core.reset(true);
		state->contention_drops.store(0, std::memory_order_relaxed);
		state->applied_counter_reset_generation = requested_counter_reset;
	}

	const float *left = reinterpret_cast<const float *>(frame->data[0] ? frame->data[0] : frame->data[1]);
	const float *right = reinterpret_cast<const float *>(frame->data[1] ? frame->data[1] : frame->data[0]);
	if (!state->core.push(slot, frame->timestamp, frame->frames, left, right,
		mcb_ui_monitoring_enabled())) {
		multichannel_publish_status(state);
		return;
	}

	mcb::SenderSyncCore::Output output;
	bool sent = false;
	while (state->core.pop_output(output)) {
		multichannel_send_output(o, output);
		sent = true;
	}
	if (sent || mcb_ui_monitoring_enabled())
		multichannel_publish_status(state);
}

'''
    text = replace_once(
        text,
        "\tvideo_frame.timecode = NDIlib_send_timecode_synthesize;\n",
        "\tif (o->multichannel_audio)\n"
        "\t\tmcb_sender_observe_video(frame->timestamp);\n"
        "\t// Preserve DistroAV's known-good NDI transport clock. Raw OBS\n"
        "\t// monotonic timestamps are local to the gaming PC and must not be\n"
        "\t// exported as receiver playout timecodes.\n"
        "\tvideo_frame.timecode = NDIlib_send_timecode_synthesize;\n",
        "sender video observation hook",
    )

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
    for name in (
        "sender-sync-core.cpp",
        "sender-sync-core.h",
        "downstream-sync-core.cpp",
        "downstream-sync-core.h",
        "multichannel-bridge.cpp",
        "multichannel-bridge.h",
    ):
        source = bridge_dir / name
        if not source.exists():
            raise FileNotFoundError(source)
        shutil.copy2(source, root / "src" / name)
        print(f"Copied: {root / 'src' / name}")


def write_notice(root: Path) -> None:
    (root / "MULTICHANNEL-BRIDGE.md").write_text(
        "# Multichannel Bridge for DistroAV v0.6.0-alpha5\n\n"
        "Custom DistroAV 6.2.1 build. Install the same package on both PCs, then use "
        "Docks > Multichannel Bridge for DistroAV to select Gaming PC / Sender or Stream PC / Receiver.\n\n"
        "Sender defaults: OBS Track 5 -> NDI channels 1-2; OBS Track 6 -> channels 3-4.\n"
        "Receiver: one normal DistroAV NDI Source provides video while the bridge exposes the two stereo "
        "pairs as independent OBS audio-only sources.\n\n"
        "Sender Sync Core 2.0 uses fixed preallocated audio storage, canonical mix-interval timestamps, "
        "automatic discontinuity re-anchoring, and no blocking callback lock. Downstream Sync Core 2.0 "
        "measures downstream video against raw receiver audio, leaves video untouched, and commands one "
        "source-level four-channel audio-rate correction before the split outputs enter OBS. Every complete "
        "NDI receiver restart wipes learned drift and begins a fresh expected-sync baseline. It is optional "
        "and enabled by default.\n\n"
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
            "src/sender-sync-core.cpp",
            "src/sender-sync-core.h",
            "src/downstream-sync-core.cpp",
            "src/downstream-sync-core.h",
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
        "mcb_receiver_route_audio(obs_source, obs_audio_frame, channelCount,",
        "mcb_receiver_route_video(obs_source, obs_video_frame,",
        "NDIlib_send_timecode_synthesize",
        'kGovernorVersion = "2.0"',
        "GovernorMaxAudioCorrectionPpm",
        "governor_flight_recorder_csv",
        "mcb_receiver_route_video",
        "mcb_force_reconnect_proc",
        "void mcb_force_reconnect(out bool accepted)",
        "mcb_sender_status_sync",
        "class SenderSyncCore",
        "class DownstreamSyncCore",
        "mcb_sender_reanchor_generation",
        "mcb_sender_observe_video",
        "mcb_sender_status_sync",
        "src/sender-sync-core.cpp",
        "src/downstream-sync-core.cpp",
        "src/multichannel-bridge.cpp",
        "Track A and Track B must be different",
    ]
    missing = [item for item in required if item not in joined]
    if missing:
        raise RuntimeError("Patch verification failed; missing: " + ", ".join(missing))
    if "frame->timestamp / 100ULL" in joined:
        raise RuntimeError("Patch verification failed; raw sender OBS timestamps leaked into NDI timecodes")
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
