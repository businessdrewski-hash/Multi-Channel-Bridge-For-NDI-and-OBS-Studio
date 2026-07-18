#pragma once

#include <cstddef>
#include <cstdint>

class QWidget;
struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_source_audio;
struct obs_source_frame;

enum class MCBRole {
	Unconfigured = 0,
	Sender = 1,
	Receiver = 2,
};

struct MCBSenderStatus {
	bool enabled = false;
	bool active = false;
	uint32_t track_a = 5;
	uint32_t track_b = 6;
	uint64_t paired_blocks = 0;
	uint64_t discarded_blocks = 0;
	uint64_t silence_fallback_blocks = 0;
	uint64_t discontinuities = 0;
	uint64_t reanchors = 0;
	uint64_t oversized_blocks = 0;
	uint64_t contention_drops = 0;
	uint64_t epoch = 1;
	int64_t last_timestamp_delta_ns = 0;
	uint32_t queue_depth_a = 0;
	uint32_t queue_depth_b = 0;
	float peak_a = 0.0f;
	float peak_b = 0.0f;
	uint64_t last_audio_monotonic_ns = 0;
};

MCBRole mcb_role();
bool mcb_is_sender();
bool mcb_is_receiver();
size_t mcb_sender_track_a_zero_based();
size_t mcb_sender_track_b_zero_based();

void mcb_register_sources();
void mcb_init(QWidget *main_window);
void mcb_shutdown();

MCBSenderStatus mcb_sender_status_snapshot();
void mcb_sender_status_started(size_t track_a_zero_based, size_t track_b_zero_based);
void mcb_sender_status_stopped();
void mcb_sender_status_sync(uint64_t paired, uint64_t discarded, uint64_t fallback,
	uint64_t discontinuities, uint64_t reanchors, uint64_t oversized,
	uint64_t contention_drops, uint64_t epoch, int64_t timestamp_delta_ns,
	uint32_t queue_a, uint32_t queue_b, float peak_a, float peak_b);
void mcb_sender_status_contention_drop();
void mcb_sender_status_reset_counters();
uint64_t mcb_sender_counter_reset_generation();

// Sender-side controls are generation based so the UI/control thread never
// mutates synchronizer storage while the real-time audio callback is active.
void mcb_request_sender_reanchor();
uint64_t mcb_sender_reanchor_generation();
void mcb_sender_observe_video(uint64_t timestamp_ns);
bool mcb_ui_monitoring_enabled();

// Called from DistroAV's raw NDI receive path before OBS remixes the source to
// the profile speaker layout. Returns true when the original audio should be
// suppressed because split proxy output or the A/V Governor consumed it.
bool mcb_receiver_route_audio(obs_source_t *origin, const obs_source_audio *audio, int channel_count,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns);

// Called before a raw NDI video frame is submitted to OBS. The governor may
// adjust the frame timestamp to apply bounded playout delay and video pacing.
// Returns false while an atomic re-lock is in progress.
bool mcb_receiver_route_video(obs_source_t *origin, obs_source_frame *video,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns);
