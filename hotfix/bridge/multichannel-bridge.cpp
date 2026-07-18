#include "multichannel-bridge.h"
#include "av-governor.h"
#include "sender-sync-core.h"

#include "main-output.h"
#include "plugin-main.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr const char *kSection = "NDIMultichannelBridge";
constexpr const char *kDockId = "distroav_multichannel_bridge_v030";
constexpr const char *kProgramSourceId = "ndi_multichannel_bridge_program_audio";
constexpr const char *kMicSourceId = "ndi_multichannel_bridge_mic_audio";
constexpr const char *kVersion = "0.5.0-alpha1-buildfix2";
constexpr const char *kGovernorVersion = "1.2";
constexpr const char *kSenderCoreVersion = "2.0";
constexpr const char *kDefaultProgramName = "MCB Desktop / Game";
constexpr const char *kDefaultMicName = "MCB Microphone";

const char *governor_phase_name(mcb::AVGovernorPhase phase)
{
	switch (phase) {
	case mcb::AVGovernorPhase::Bypassed: return "BYPASSED";
	case mcb::AVGovernorPhase::WarmingUp: return "WARMING UP";
	case mcb::AVGovernorPhase::Locked: return "LOCKED";
	case mcb::AVGovernorPhase::Holding: return "HOLDING";
	case mcb::AVGovernorPhase::Relocking: return "RELOCKING";
	}
	return "UNKNOWN";
}

const char *governor_reason_name(mcb::AVGovernorReason reason)
{
	switch (reason) {
	case mcb::AVGovernorReason::None: return "none";
	case mcb::AVGovernorReason::Startup: return "startup";
	case mcb::AVGovernorReason::VideoStall: return "video stall";
	case mcb::AVGovernorReason::AudioDiscontinuity: return "audio timestamp jump";
	case mcb::AVGovernorReason::VideoDiscontinuity: return "video timestamp jump";
	case mcb::AVGovernorReason::AudioNonMonotonic: return "audio timestamp repeated/backward";
	case mcb::AVGovernorReason::VideoNonMonotonic: return "video timestamp repeated/backward";
	case mcb::AVGovernorReason::SkewExceeded: return "A/V deviation exceeded";
	case mcb::AVGovernorReason::PlayoutDepthExceeded: return "shared playout depth left safe range";
	case mcb::AVGovernorReason::SourceReconfigured: return "source timing changed";
	case mcb::AVGovernorReason::ManualReset: return "manual reset";
	}
	return "unknown";
}

std::atomic_int g_role_cache{-1};
std::atomic_bool g_sender_enabled{false};
std::atomic_bool g_sender_active{false};
std::atomic_uint32_t g_sender_track_a{5};
std::atomic_uint32_t g_sender_track_b{6};
std::atomic_uint64_t g_sender_paired{0};
std::atomic_uint64_t g_sender_discarded{0};
std::atomic_uint64_t g_sender_fallback{0};
std::atomic_uint64_t g_sender_discontinuities{0};
std::atomic_uint64_t g_sender_video_discontinuities{0};
std::atomic_uint64_t g_sender_reanchors{0};
std::atomic_uint64_t g_sender_oversized{0};
std::atomic_uint64_t g_sender_contention{0};
std::atomic_uint64_t g_sender_epoch{1};
std::atomic_int64_t g_sender_last_delta_ns{0};
std::atomic_uint32_t g_sender_queue_a{0};
std::atomic_uint32_t g_sender_queue_b{0};
std::atomic<float> g_sender_peak_a{0.0f};
std::atomic<float> g_sender_peak_b{0.0f};
std::atomic_uint64_t g_sender_last_audio_ns{0};
std::atomic_uint64_t g_sender_reanchor_generation{1};
std::atomic_uint64_t g_sender_counter_reset_generation{1};
std::atomic_uint64_t g_sender_last_video_timestamp_ns{0};
std::atomic_bool g_ui_monitoring{false};

config_t *bridge_config()
{
	return obs_frontend_get_user_config();
}

void ensure_defaults()
{
	auto *config = bridge_config();
	if (!config)
		return;
	config_set_default_string(config, kSection, "Role", "unconfigured");
	config_set_default_int(config, kSection, "TrackA", 5);
	config_set_default_int(config, kSection, "TrackB", 6);
	config_set_default_string(config, kSection, "ReceiverSource", "");
	config_set_default_string(config, kSection, "ProgramProxyName", kDefaultProgramName);
	config_set_default_string(config, kSection, "MicProxyName", kDefaultMicName);
	config_set_default_bool(config, kSection, "SuppressOriginal", true);
	config_set_default_bool(config, kSection, "GovernorEnabled", true);
	config_set_default_bool(config, kSection, "GovernorAutoConfigure", true);
	config_set_default_int(config, kSection, "GovernorMaxSkewMs", 120);
	config_set_default_int(config, kSection, "GovernorVideoStallMs", 120);
	config_set_default_int(config, kSection, "GovernorPlayoutDelayMs", 120);
	config_set_default_bool(config, kSection, "GovernorDriftCorrection", true);
	config_set_default_int(config, kSection, "GovernorMaxVideoCorrectionMs", 40);
	config_set_default_int(config, kSection, "GovernorCorrectionSlewPpm", 1000);
	config_set_default_int(config, kSection, "GovernorRelockPairs", 12);
	config_set_default_int(config, kSection, "GovernorBaselineWindowMs", 1000);
	config_set_default_int(config, kSection, "GovernorDriftWindowMs", 30000);
	config_set_default_int(config, kSection, "GovernorDriftMinimumMs", 10000);
	config_set_default_int(config, kSection, "GovernorDriftDeadbandPpm", 8);
}

void save_config()
{
	auto *config = bridge_config();
	if (config)
		config_save_safe(config, "tmp", nullptr);
}

const char *config_string(const char *name, const char *fallback)
{
	auto *config = bridge_config();
	if (!config)
		return fallback;
	const char *value = config_get_string(config, kSection, name);
	return value && *value ? value : fallback;
}

int config_track(const char *name, int fallback)
{
	auto *config = bridge_config();
	if (!config)
		return fallback;
	const int value = static_cast<int>(config_get_int(config, kSection, name));
	return std::clamp(value, 1, static_cast<int>(MAX_AUDIO_MIXES));
}

MCBRole read_role_from_config()
{
	ensure_defaults();
	const char *role = config_string("Role", "unconfigured");
	if (std::strcmp(role, "sender") == 0)
		return MCBRole::Sender;
	if (std::strcmp(role, "receiver") == 0)
		return MCBRole::Receiver;
	return MCBRole::Unconfigured;
}

void set_role_cache(MCBRole role)
{
	g_role_cache.store(static_cast<int>(role), std::memory_order_release);
	g_sender_enabled.store(role == MCBRole::Sender, std::memory_order_release);
	if (role != MCBRole::Sender)
		g_sender_active.store(false, std::memory_order_release);
}

struct ProxyContext {
	obs_source_t *source = nullptr;
	int pair = 0;
};

class ReceiverRouter {
public:
	static ReceiverRouter &instance()
	{
		static ReceiverRouter router;
		return router;
	}

	~ReceiverRouter() { detach(); }

	void register_proxy(int pair, obs_source_t *source)
	{
		if (pair < 0 || pair > 1 || !source)
			return;
		std::lock_guard<std::mutex> lock(mutex_);
		proxies_[static_cast<size_t>(pair)] = source;
	}

	void unregister_proxy(int pair, obs_source_t *source)
	{
		if (pair < 0 || pair > 1)
			return;
		std::lock_guard<std::mutex> lock(mutex_);
		auto &slot = proxies_[static_cast<size_t>(pair)];
		if (slot == source)
			slot = nullptr;
	}

	bool attach(const std::string &name)
	{
		detach();
		if (name.empty()) {
			set_error("Select a DistroAV NDI Source first.");
			return false;
		}

		obs_source_t *candidate = obs_get_source_by_name(name.c_str());
		if (!candidate) {
			set_error("Selected OBS source was not found.");
			return false;
		}
		const char *id = obs_source_get_id(candidate);
		if (!id || std::strcmp(id, "ndi_source") != 0) {
			set_error("Selected source is not a DistroAV NDI Source.");
			obs_source_release(candidate);
			return false;
		}
		if ((obs_source_get_output_flags(candidate) & OBS_SOURCE_AUDIO) == 0) {
			set_error("Selected DistroAV source is not audio-capable.");
			obs_source_release(candidate);
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);
			input_ = candidate; // obs_get_source_by_name returns a retained reference.
			input_name_ = name;
			last_error_.clear();
		}
		reset_stats();
		refresh_source_configuration();
		attached_.store(true, std::memory_order_release);
		obs_log(LOG_INFO, "[multichannel-bridge] Receiver attached to raw DistroAV source '%s'", name.c_str());
		return true;
	}

	void detach()
	{
		obs_source_t *old = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			old = input_;
			input_ = nullptr;
			input_name_.clear();
		}
		if (old)
			obs_source_release(old);
		attached_.store(false, std::memory_order_release);
		reset_governor(false);
	}

	void set_suppress_original(bool suppress)
	{
		suppress_original_.store(suppress, std::memory_order_release);
	}

	void configure_governor(bool enabled, int max_deviation_ms, int video_stall_ms, int playout_delay_ms,
		bool drift_correction, int max_video_correction_ms, int correction_slew_ppm, int relock_pairs,
		int baseline_window_ms, int drift_window_ms, int drift_minimum_ms, int drift_deadband_ppm)
	{
		governor_.configure(enabled, max_deviation_ms, video_stall_ms, playout_delay_ms, drift_correction,
			max_video_correction_ms, correction_slew_ppm, relock_pairs, baseline_window_ms,
			drift_window_ms, drift_minimum_ms, drift_deadband_ppm);
	}

	bool apply_recommended_source_settings()
	{
		obs_source_t *source = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (input_)
				source = obs_source_get_ref(input_);
		}
		if (!source)
			return false;

		obs_data_t *settings = obs_source_get_settings(source);
		if (!settings) {
			obs_source_release(source);
			return false;
		}
		obs_data_set_bool(settings, "ndi_framesync", false);
		obs_data_set_int(settings, "ndi_sync", 2);
		obs_data_set_bool(settings, "ndi_audio", true);
		obs_source_update(source, settings);
		obs_data_release(settings);
		obs_source_release(source);
		const bool reconnect_requested = force_reconnect();
		governor_.set_source_configured(true);
		governor_.reset(false);
		obs_log(LOG_INFO,
			"[multichannel-bridge] Applied recommended source timing and requested receiver reconnect: %s",
			reconnect_requested ? "yes" : "procedure unavailable");
		return true;
	}

	bool force_reconnect()
	{
		obs_source_t *source = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (input_)
				source = obs_source_get_ref(input_);
		}
		if (!source)
			return false;

		calldata_t params{};
		calldata_init(&params);
		const bool procedure_found = proc_handler_call(
			obs_source_get_proc_handler(source), "mcb_force_reconnect", &params);
		const bool accepted = procedure_found && calldata_bool(&params, "accepted");
		calldata_free(&params);
		obs_source_release(source);
		if (accepted) {
			reset_governor(false);
			obs_log(LOG_INFO, "[multichannel-bridge] Requested an in-place DistroAV receiver reconnect");
		}
		return accepted;
	}

	bool refresh_source_configuration()
	{
		obs_source_t *source = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (input_)
				source = obs_source_get_ref(input_);
		}
		if (!source) {
			governor_.set_source_configured(false);
			return false;
		}
		obs_data_t *settings = obs_source_get_settings(source);
		const bool configured = settings && !obs_data_get_bool(settings, "ndi_framesync") &&
					obs_data_get_int(settings, "ndi_sync") == 2 && obs_data_get_bool(settings, "ndi_audio");
		if (settings)
			obs_data_release(settings);
		obs_source_release(source);
		governor_.set_source_configured(configured);
		return configured;
	}

	bool route(obs_source_t *origin, const obs_source_audio *audio, int channel_count,
		int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
	{
		if (!origin || !audio || audio->frames == 0)
			return false;

		std::array<obs_source_t *, 2> outputs{};
		{
			std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
			if (!lock.owns_lock())
				return false;
			if (!input_ || input_ != origin)
				return false;
			for (size_t pair = 0; pair < outputs.size(); ++pair) {
				if (proxies_[pair])
					outputs[pair] = obs_source_get_ref(proxies_[pair]);
			}
		}

		const bool split_active = outputs[0] && outputs[1] && obs_source_active(outputs[0]) && obs_source_active(outputs[1]);
		const uint64_t now_ns = os_gettime_ns();
		channels_.store(std::max(channel_count, 0), std::memory_order_relaxed);
		packets_.fetch_add(1, std::memory_order_relaxed);
		last_packet_ns_.store(now_ns, std::memory_order_relaxed);

		mcb::AVGovernorDecision audio_decision{true, audio->timestamp};
		if (split_active)
			audio_decision = governor_.process_audio(audio->timestamp, now_ns, ndi_timestamp_100ns, ndi_timecode_100ns);
		if (!audio_decision.accept) {
			// Fail open. Timing protection must never make a live NDI source
			// disappear while the governor is learning or re-locking.
			audio_decision.accept = true;
			audio_decision.output_timestamp_ns = audio->timestamp;
			audio_decision.audio_gain_start = 1.0f;
			audio_decision.audio_gain_end = 1.0f;
		}

		const bool apply_gain = audio->format == AUDIO_FORMAT_FLOAT_PLANAR &&
			(audio_decision.audio_gain_start != 1.0f || audio_decision.audio_gain_end != 1.0f);
		for (int pair = 0; pair < 2; ++pair) {
			const int first = pair * 2;
			const uint8_t *left = first < channel_count ? audio->data[first] : nullptr;
			const uint8_t *right = (first + 1) < channel_count ? audio->data[first + 1] : nullptr;
			if (!left && !right) {
				missing_[static_cast<size_t>(pair)].fetch_add(1, std::memory_order_relaxed);
				peaks_[static_cast<size_t>(pair)].store(0.0f, std::memory_order_relaxed);
				if (outputs[static_cast<size_t>(pair)])
					obs_source_release(outputs[static_cast<size_t>(pair)]);
				continue;
			}
			if (!left)
				left = right;
			if (!right)
				right = left;
			if (apply_gain && audio->frames <= mcb::SenderSyncCore::kMaxFrames) {
				auto &left_scratch = fade_scratch_[static_cast<size_t>(first)];
				auto &right_scratch = fade_scratch_[static_cast<size_t>(first + 1)];
				apply_gain_ramp(reinterpret_cast<const float *>(left), left_scratch, audio->frames,
					audio_decision.audio_gain_start, audio_decision.audio_gain_end);
				apply_gain_ramp(reinterpret_cast<const float *>(right), right_scratch, audio->frames,
					audio_decision.audio_gain_start, audio_decision.audio_gain_end);
				left = reinterpret_cast<const uint8_t *>(left_scratch.data());
				right = reinterpret_cast<const uint8_t *>(right_scratch.data());
			}

			const float peak = mcb_ui_monitoring_enabled()
				? calculate_peak(left, right, audio->frames)
				: 0.0f;
			peaks_[static_cast<size_t>(pair)].store(peak, std::memory_order_relaxed);
			if (outputs[static_cast<size_t>(pair)]) {
				obs_source_audio output{};
				output.data[0] = const_cast<uint8_t *>(left);
				output.data[1] = const_cast<uint8_t *>(right);
				output.frames = audio->frames;
				output.samples_per_sec = audio->samples_per_sec;
				output.speakers = SPEAKERS_STEREO;
				output.format = audio->format;
				output.timestamp = audio_decision.output_timestamp_ns;
				obs_source_output_audio(outputs[static_cast<size_t>(pair)], &output);
				obs_source_release(outputs[static_cast<size_t>(pair)]);
			}
		}

		const bool suppress = suppress_original_.load(std::memory_order_acquire) && split_active && channel_count >= 4;
		if (suppress)
			suppressed_.fetch_add(1, std::memory_order_relaxed);
		return suppress;
	}

	bool route_video(obs_source_t *origin, obs_source_frame *video,
		int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
	{
		if (!origin || !video)
			return true;
		bool selected = false;
		bool split_active = false;
		{
			std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
			if (!lock.owns_lock())
				return true;
			selected = input_ && input_ == origin;
			split_active = proxies_[0] && proxies_[1] && obs_source_active(proxies_[0]) &&
				       obs_source_active(proxies_[1]);
		}
		if (!selected || !split_active)
			return true;
		const auto decision = governor_.process_video(video->timestamp, os_gettime_ns(), ndi_timestamp_100ns, ndi_timecode_100ns);
		if (decision.accept)
			video->timestamp = decision.output_timestamp_ns;
		// Fail open with the original DistroAV timestamp during acquisition or
		// recovery. A timing controller may degrade to monitoring, never black video.
		return true;
	}

	mcb::AVGovernorSnapshot governor_snapshot() const { return governor_.snapshot(); }
	std::string governor_flight_recorder_csv() const { return governor_.flight_recorder_csv(); }

	void reset_governor(bool reset_counters) { governor_.reset(reset_counters); }

	bool attached() const
	{
		return attached_.load(std::memory_order_acquire);
	}

	bool outputs_ready() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return proxies_[0] != nullptr && proxies_[1] != nullptr;
	}

	bool outputs_active() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return proxies_[0] && proxies_[1] && obs_source_active(proxies_[0]) && obs_source_active(proxies_[1]);
	}

	uint64_t packets() const { return packets_.load(std::memory_order_relaxed); }
	uint64_t suppressed() const { return suppressed_.load(std::memory_order_relaxed); }
	uint64_t missing_program() const { return missing_[0].load(std::memory_order_relaxed); }
	uint64_t missing_mic() const { return missing_[1].load(std::memory_order_relaxed); }
	int channels() const { return channels_.load(std::memory_order_relaxed); }
	float peak(int pair) const
	{
		return peaks_[static_cast<size_t>(std::clamp(pair, 0, 1))].load(std::memory_order_relaxed);
	}
	uint64_t last_packet_ns() const { return last_packet_ns_.load(std::memory_order_relaxed); }

	std::string input_name() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return input_name_;
	}

	std::string last_error() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return last_error_;
	}

	void reset_stats()
	{
		packets_.store(0, std::memory_order_relaxed);
		suppressed_.store(0, std::memory_order_relaxed);
		missing_[0].store(0, std::memory_order_relaxed);
		missing_[1].store(0, std::memory_order_relaxed);
		channels_.store(0, std::memory_order_relaxed);
		peaks_[0].store(0.0f, std::memory_order_relaxed);
		peaks_[1].store(0.0f, std::memory_order_relaxed);
		last_packet_ns_.store(0, std::memory_order_relaxed);
		reset_governor(true);
	}

private:
	static void apply_gain_ramp(const float *input,
		std::array<float, mcb::SenderSyncCore::kMaxFrames> &scratch, uint32_t frames,
		float start_gain, float end_gain)
	{
		if (!input || frames == 0)
			return;
		const float denominator = frames > 1 ? static_cast<float>(frames - 1) : 1.0f;
		const float step = (end_gain - start_gain) / denominator;
		for (uint32_t i = 0; i < frames; ++i)
			scratch[i] = input[i] * (start_gain + step * static_cast<float>(i));
	}

	static float calculate_peak(const uint8_t *left, const uint8_t *right, uint32_t frames)
	{
		if ((!left && !right) || frames == 0)
			return 0.0f;
		const auto *l = reinterpret_cast<const float *>(left ? left : right);
		const auto *r = reinterpret_cast<const float *>(right ? right : left);
		float peak = 0.0f;
		for (uint32_t i = 0; i < frames; ++i)
			peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
		return std::min(peak, 1.0f);
	}

	void set_error(const char *message)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		last_error_ = message ? message : "Unknown receiver error.";
	}

	mutable std::mutex mutex_;
	obs_source_t *input_ = nullptr;
	std::array<obs_source_t *, 2> proxies_{};
	std::string input_name_;
	std::string last_error_;
	std::atomic_bool attached_{false};
	std::atomic_bool suppress_original_{true};
	std::atomic_uint64_t packets_{0};
	std::atomic_uint64_t suppressed_{0};
	std::array<std::atomic_uint64_t, 2> missing_{};
	std::atomic_int channels_{0};
	std::array<std::atomic<float>, 2> peaks_{};
	std::atomic_uint64_t last_packet_ns_{0};
	std::array<std::array<float, mcb::SenderSyncCore::kMaxFrames>, 4> fade_scratch_{};
	mcb::AVGovernor governor_;
};

const char *program_source_name(void *) { return "Multichannel Bridge - Desktop / Game Audio"; }
const char *mic_source_name(void *) { return "Multichannel Bridge - Microphone Audio"; }

void *create_proxy_common(obs_source_t *source, int pair)
{
	auto *context = new ProxyContext;
	context->source = source;
	context->pair = pair;
	// Audio-only proxy sources must explicitly participate in OBS's mixer.
	// OBS documents this flag as controlling whether a source is shown and active
	// in the audio mixer; outputting packets alone is not sufficient.
	obs_source_set_audio_active(source, true);
	ReceiverRouter::instance().register_proxy(pair, source);
	return context;
}

void *create_program_proxy(obs_data_t *, obs_source_t *source) { return create_proxy_common(source, 0); }
void *create_mic_proxy(obs_data_t *, obs_source_t *source) { return create_proxy_common(source, 1); }

void destroy_proxy(void *data)
{
	auto *context = static_cast<ProxyContext *>(data);
	if (!context)
		return;
	ReceiverRouter::instance().unregister_proxy(context->pair, context->source);
	obs_source_set_audio_active(context->source, false);
	delete context;
}

obs_source_info create_proxy_info(const char *id, const char *(*name_cb)(void *),
				  void *(*create_cb)(obs_data_t *, obs_source_t *))
{
	obs_source_info info{};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = name_cb;
	info.create = create_cb;
	info.destroy = destroy_proxy;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	return info;
}

obs_source_info g_program_info{};
obs_source_info g_mic_info{};

bool source_in_current_scene(obs_scene_t *scene, const char *name)
{
	return scene && name && obs_scene_find_source(scene, name) != nullptr;
}

obs_source_t *get_or_create_proxy(const char *id, const char *name)
{
	if (!name || !*name)
		return nullptr;
	obs_source_t *source = obs_get_source_by_name(name);
	if (source) {
		const char *existing_id = obs_source_get_id(source);
		if (!existing_id || std::strcmp(existing_id, id) != 0) {
			obs_log(LOG_WARNING,
				"[multichannel-bridge] Cannot create '%s': another source with that name already exists", name);
			obs_source_release(source);
			return nullptr;
		}
		return source;
	}
	return obs_source_create(id, name, nullptr, nullptr);
}

bool add_proxy_to_current_scene(const char *id, const char *name, bool repair_audio_state)
{
	obs_source_t *current_scene_source = obs_frontend_get_current_scene();
	if (!current_scene_source)
		return false;
	obs_scene_t *scene = obs_scene_from_source(current_scene_source);
	obs_source_t *proxy = get_or_create_proxy(id, name);
	bool success = false;
	if (scene && proxy) {
		if (!source_in_current_scene(scene, name))
			obs_scene_add(scene, proxy);
		obs_source_set_audio_active(proxy, true);
		if (repair_audio_state) {
			obs_source_set_muted(proxy, false);
			if (obs_source_get_volume(proxy) <= 0.0001f)
				obs_source_set_volume(proxy, 1.0f);
			if (obs_source_get_audio_mixers(proxy) == 0)
				obs_source_set_audio_mixers(proxy, 0x3fU);
		}
		success = true;
	}
	if (proxy)
		obs_source_release(proxy);
	obs_source_release(current_scene_source);
	return success;
}

std::vector<std::string> list_ndi_audio_sources()
{
	std::vector<std::string> names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *out = static_cast<std::vector<std::string> *>(param);
			const char *id = obs_source_get_id(source);
			if (id && std::strcmp(id, "ndi_source") == 0 &&
			    (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
				obs_data_t *settings = obs_source_get_settings(source);
				const bool audio_only = settings && obs_data_get_int(settings, "ndi_bw_mode") == 2;
				if (settings)
					obs_data_release(settings);
				if (!audio_only) {
					const char *name = obs_source_get_name(source);
					if (name && *name)
						out->emplace_back(name);
				}
			}
			return true;
		},
		&names);
	std::sort(names.begin(), names.end());
	return names;
}

int meter_value(float peak)
{
	if (peak <= 0.00001f)
		return 0;
	const double db = 20.0 * std::log10(static_cast<double>(peak));
	return std::clamp(static_cast<int>(std::lround((db + 60.0) * (100.0 / 60.0))), 0, 100);
}

class BridgeDock final : public QWidget {
public:
	explicit BridgeDock(QWidget *parent) : QWidget(parent)
	{
		ensure_defaults();
		auto *outer = new QVBoxLayout(this);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		auto *body = new QWidget(scroll);
		auto *layout = new QVBoxLayout(body);

		auto *title = new QLabel(QString("<b>Multichannel Bridge for DistroAV %1</b>").arg(kVersion), body);
		layout->addWidget(title);
		auto *intro = new QLabel(
			"Install the same custom DistroAV package on both PCs. Select exactly one role on each PC, "
			"confirm it, then apply.", body);
		intro->setWordWrap(true);
		layout->addWidget(intro);

		auto *role_box = new QGroupBox("1. This PC's role", body);
		auto *role_layout = new QVBoxLayout(role_box);
		sender_radio_ = new QRadioButton("Gaming PC — send video + two stereo OBS tracks", role_box);
		receiver_radio_ = new QRadioButton("Stream PC — receive and split the four NDI channels", role_box);
		confirm_ = new QCheckBox("I confirm the selected role is correct for this PC", role_box);
		role_layout->addWidget(sender_radio_);
		role_layout->addWidget(receiver_radio_);
		role_layout->addWidget(confirm_);
		layout->addWidget(role_box);

		sender_box_ = new QGroupBox("2. Gaming PC sender", body);
		auto *sender_form = new QFormLayout(sender_box_);
		track_a_ = new QSpinBox(sender_box_);
		track_b_ = new QSpinBox(sender_box_);
		track_a_->setRange(1, static_cast<int>(MAX_AUDIO_MIXES));
		track_b_->setRange(1, static_cast<int>(MAX_AUDIO_MIXES));
		track_a_->setToolTip("Default: OBS Track 5. Sent as NDI channels 1-2.");
		track_b_->setToolTip("Default: OBS Track 6. Sent as NDI channels 3-4.");
		sender_form->addRow("Desktop / game OBS track:", track_a_);
		sender_form->addRow("Microphone OBS track:", track_b_);
		sender_status_ = new QLabel(sender_box_);
		sender_status_->setWordWrap(true);
		sender_form->addRow("Live status:", sender_status_);
		sender_program_meter_ = new QProgressBar(sender_box_);
		sender_mic_meter_ = new QProgressBar(sender_box_);
		for (auto *meter : {sender_program_meter_, sender_mic_meter_}) {
			meter->setRange(0, 100);
			meter->setTextVisible(false);
		}
		sender_form->addRow("Desktop/game level:", sender_program_meter_);
		sender_form->addRow("Mic level:", sender_mic_meter_);
		auto *sender_actions = new QWidget(sender_box_);
		auto *sender_actions_layout = new QHBoxLayout(sender_actions);
		sender_actions_layout->setContentsMargins(0, 0, 0, 0);
		reanchor_sender_ = new QPushButton("Re-anchor sync", sender_actions);
		restart_sender_ = new QPushButton("Restart Bridge", sender_actions);
		reanchor_sender_->setToolTip(
			"Flush the fixed sender audio queues and begin a fresh timing epoch without recreating NDI.");
		restart_sender_->setToolTip(
			"Stop and recreate only the Multichannel Bridge DistroAV Main Output with the same settings.");
		sender_actions_layout->addWidget(reanchor_sender_);
		sender_actions_layout->addWidget(restart_sender_);
		sender_form->addRow("Quick recovery:", sender_actions);
		layout->addWidget(sender_box_);

		receiver_box_ = new QGroupBox("2. Stream PC receiver", body);
		auto *receiver_form = new QFormLayout(receiver_box_);
		receiver_source_ = new QComboBox(receiver_box_);
		refresh_ = new QPushButton("Refresh NDI sources", receiver_box_);
		reconnect_receiver_ = new QPushButton("Reconnect", receiver_box_);
		open_source_ = new QPushButton("Open properties", receiver_box_);
		auto *source_row = new QWidget(receiver_box_);
		auto *source_row_layout = new QHBoxLayout(source_row);
		source_row_layout->setContentsMargins(0, 0, 0, 0);
		source_row_layout->addWidget(receiver_source_, 1);
		source_row_layout->addWidget(refresh_);
		source_row_layout->addWidget(reconnect_receiver_);
		source_row_layout->addWidget(open_source_);
		receiver_form->addRow("DistroAV NDI video source:", source_row);

		governor_box_ = new QGroupBox(
			QString("A/V Governor %1 - shared timeline protection").arg(kGovernorVersion), receiver_box_);
		governor_box_->setCheckable(true);
		auto *governor_form = new QFormLayout(governor_box_);
		auto_configure_ = new QCheckBox(
			"Automatically disable NDI Frame Sync and use Source Timecode (recommended)", governor_box_);
		playout_delay_ms_ = new QSpinBox(governor_box_);
		playout_delay_ms_->setRange(40, 500);
		playout_delay_ms_->setSuffix(" ms");
		playout_delay_ms_->setToolTip(
			"Adds the same fixed timestamp delay to audio and video so OBS can absorb brief arrival jitter before playout.");
		max_skew_ms_ = new QSpinBox(governor_box_);
		max_skew_ms_->setRange(40, 500);
		max_skew_ms_->setSuffix(" ms");
		max_skew_ms_->setToolTip(
			"Hard safety limit. If A/V movement exceeds this, correction pauses and the model re-locks while normal output continues.");
		video_stall_ms_ = new QSpinBox(governor_box_);
		video_stall_ms_->setRange(60, 1000);
		video_stall_ms_->setSuffix(" ms");
		video_stall_ms_->setToolTip(
			"If video stops arriving for this long, audio is held before it can run ahead.");
		drift_correction_ = new QCheckBox(
			"Gently pace video timestamps to follow verified gradual audio-clock drift", governor_box_);
		max_video_correction_ms_ = new QSpinBox(governor_box_);
		max_video_correction_ms_->setRange(0, 120);
		max_video_correction_ms_->setSuffix(" ms");
		max_video_correction_ms_->setToolTip(
			"Maximum temporary video timestamp correction. Audio samples are never resampled or cut.");
		correction_slew_ppm_ = new QSpinBox(governor_box_);
		correction_slew_ppm_->setRange(50, 10000);
		correction_slew_ppm_->setSuffix(" ppm");
		correction_slew_ppm_->setToolTip(
			"Maximum speed at which video timing may move toward the measured drift. 1000 ppm equals 1 ms per second.");
		relock_pairs_ = new QSpinBox(governor_box_);
		relock_pairs_->setRange(3, 60);
		relock_pairs_->setSuffix(" pairs");
		relock_pairs_->setToolTip(
			"Minimum number of sane audio/video observations required during the baseline-learning window.");
		baseline_window_ms_ = new QSpinBox(governor_box_);
		baseline_window_ms_->setRange(250, 5000);
		baseline_window_ms_->setSuffix(" ms");
		baseline_window_ms_->setToolTip(
			"How long the governor learns a robust median baseline before releasing either path.");
		drift_window_ms_ = new QSpinBox(governor_box_);
		drift_window_ms_->setRange(5000, 120000);
		drift_window_ms_->setSuffix(" ms");
		drift_window_ms_->setToolTip(
			"History used to distinguish real clock drift from short-term network jitter.");
		drift_minimum_ms_ = new QSpinBox(governor_box_);
		drift_minimum_ms_->setRange(2000, 120000);
		drift_minimum_ms_->setSuffix(" ms");
		drift_minimum_ms_->setToolTip(
			"A drift direction must persist for at least this long before video timing is adjusted.");
		drift_deadband_ppm_ = new QSpinBox(governor_box_);
		drift_deadband_ppm_->setRange(1, 250);
		drift_deadband_ppm_->setSuffix(" ppm");
		drift_deadband_ppm_->setToolTip(
			"Ignore smaller estimated clock differences so normal jitter cannot trigger correction.");
		governor_help_ = new QLabel(
			"Recommended mode learns a one-second median baseline, ignores short jitter, adjusts only video after confirmed drift, "
			"and fails open with normal DistroAV output whenever its timing model is not ready.", governor_box_);
		governor_help_->setWordWrap(true);
		governor_status_ = new QLabel(governor_box_);
		governor_status_->setWordWrap(true);
		recommended_governor_ = new QPushButton("Restore recommended settings", governor_box_);
		recommended_governor_->setToolTip("Restore conservative settings suitable for a single two-PC NDI source.");
		advanced_governor_ = new QCheckBox("Show advanced timing controls", governor_box_);
		advanced_governor_panel_ = new QWidget(governor_box_);
		auto *advanced_form = new QFormLayout(advanced_governor_panel_);
		advanced_form->setContentsMargins(0, 0, 0, 0);
		advanced_form->addRow("Hard A/V deviation limit:", max_skew_ms_);
		advanced_form->addRow("Video-stall hold threshold:", video_stall_ms_);
		advanced_form->addRow("Maximum video correction:", max_video_correction_ms_);
		advanced_form->addRow("Video correction slew:", correction_slew_ppm_);
		advanced_form->addRow("Minimum baseline observations:", relock_pairs_);
		advanced_form->addRow("Baseline learning time:", baseline_window_ms_);
		advanced_form->addRow("Drift analysis window:", drift_window_ms_);
		advanced_form->addRow("Minimum confirmed drift time:", drift_minimum_ms_);
		advanced_form->addRow("Drift deadband:", drift_deadband_ppm_);
		advanced_governor_panel_->setVisible(false);
		governor_form->addRow(auto_configure_);
		governor_form->addRow("Shared playout delay:", playout_delay_ms_);
		governor_form->addRow(drift_correction_);
		governor_form->addRow(advanced_governor_);
		governor_form->addRow(advanced_governor_panel_);
		governor_form->addRow(governor_help_);
		governor_form->addRow(recommended_governor_);
		governor_form->addRow("Governor status:", governor_status_);
		receiver_form->addRow(governor_box_);

		program_name_ = new QLineEdit(receiver_box_);
		mic_name_ = new QLineEdit(receiver_box_);
		receiver_form->addRow("Desktop/game mixer source name:", program_name_);
		receiver_form->addRow("Mic mixer source name:", mic_name_);
		suppress_original_ = new QCheckBox(
			"Suppress the original 4-channel audio after both split sources are ready", receiver_box_);
		suppress_original_->setChecked(true);
		receiver_form->addRow(suppress_original_);
		create_receiver_ = new QPushButton("Create / repair split audio sources in current scene", receiver_box_);
		receiver_form->addRow(create_receiver_);
		receiver_status_ = new QLabel(receiver_box_);
		receiver_status_->setWordWrap(true);
		receiver_form->addRow("Live status:", receiver_status_);
		program_meter_ = new QProgressBar(receiver_box_);
		mic_meter_ = new QProgressBar(receiver_box_);
		for (auto *meter : {program_meter_, mic_meter_}) {
			meter->setRange(0, 100);
			meter->setTextVisible(false);
		}
		receiver_form->addRow("Desktop/game level:", program_meter_);
		receiver_form->addRow("Mic level:", mic_meter_);
		layout->addWidget(receiver_box_);

		auto *actions = new QHBoxLayout;
		apply_ = new QPushButton("Apply role and settings", body);
		reset_stats_ = new QPushButton("Reset counters", body);
		copy_diagnostics_ = new QPushButton("Copy diagnostics", body);
		copy_flight_recorder_ = new QPushButton("Copy A/V flight recorder", body);
		copy_flight_recorder_->setToolTip("Copies a bounded CSV timeline including raw NDI timestamp/timecode and OBS output timing.");
		export_diagnostics_ = new QPushButton("Export diagnostics", body);
		export_diagnostics_->setToolTip("Writes a timestamped diagnostics folder containing status and the A/V flight recorder.");
		actions->addWidget(apply_, 1);
		actions->addWidget(reset_stats_);
		actions->addWidget(copy_diagnostics_);
		actions->addWidget(copy_flight_recorder_);
		actions->addWidget(export_diagnostics_);
		layout->addLayout(actions);

		checklist_ = new QLabel(body);
		checklist_->setWordWrap(true);
		layout->addWidget(checklist_);
		layout->addStretch(1);
		scroll->setWidget(body);
		outer->addWidget(scroll);

		load_ui();
		refresh_sources();
		update_role_visibility();
		update_status();

		connect(sender_radio_, &QRadioButton::toggled, this, [this] { role_changed(); });
		connect(receiver_radio_, &QRadioButton::toggled, this, [this] { role_changed(); });
		connect(confirm_, &QCheckBox::toggled, this, [this] { apply_->setEnabled(confirm_->isChecked()); });
		connect(refresh_, &QPushButton::clicked, this, [this] { refresh_sources(); });
		connect(reconnect_receiver_, &QPushButton::clicked, this, [this] {
			const bool ok = ReceiverRouter::instance().force_reconnect();
			checklist_->setText(ok
				? "Existing DistroAV source reconnected in place; scene layout and filters were preserved."
				: "Reconnect was unavailable. Confirm this is a DistroAV NDI Source created by the bridge build.");
		});
		connect(advanced_governor_, &QCheckBox::toggled, this, [this](bool visible) {
			advanced_governor_panel_->setVisible(visible);
		});
		connect(recommended_governor_, &QPushButton::clicked, this, [this] {
			governor_box_->setChecked(true);
			auto_configure_->setChecked(true);
			playout_delay_ms_->setValue(120);
			max_skew_ms_->setValue(120);
			video_stall_ms_->setValue(120);
			drift_correction_->setChecked(true);
			max_video_correction_ms_->setValue(40);
			correction_slew_ppm_->setValue(1000);
			relock_pairs_->setValue(12);
			baseline_window_ms_->setValue(1000);
			drift_window_ms_->setValue(30000);
			drift_minimum_ms_->setValue(10000);
			drift_deadband_ppm_->setValue(8);
			checklist_->setText("Recommended A/V Governor settings restored. Click Apply role and settings.");
		});
		connect(open_source_, &QPushButton::clicked, this, [this] {
			obs_source_t *source = obs_get_source_by_name(receiver_source_->currentText().toUtf8().constData());
			if (source) {
				obs_frontend_open_source_properties(source);
				obs_source_release(source);
			}
		});
		connect(reanchor_sender_, &QPushButton::clicked, this, [this] { reanchor_sender(); });
		connect(restart_sender_, &QPushButton::clicked, this, [this] { restart_sender(); });
		connect(apply_, &QPushButton::clicked, this, [this] { apply_settings(false); });
		connect(create_receiver_, &QPushButton::clicked, this, [this] { apply_settings(true); });
		connect(reset_stats_, &QPushButton::clicked, this, [this] {
			mcb_sender_status_reset_counters();
			if (mcb_is_receiver())
				ReceiverRouter::instance().reset_stats();
			update_status();
		});
		connect(copy_diagnostics_, &QPushButton::clicked, this, [this] {
			QApplication::clipboard()->setText(diagnostics());
		});
		connect(copy_flight_recorder_, &QPushButton::clicked, this, [this] {
			const std::string csv = ReceiverRouter::instance().governor_flight_recorder_csv();
			QApplication::clipboard()->setText(QString::fromStdString(csv));
			checklist_->setText("Recent A/V flight recorder copied as CSV.");
		});
		connect(export_diagnostics_, &QPushButton::clicked, this, [this] { export_diagnostics(); });

		timer_ = new QTimer(this);
		timer_->setInterval(1000);
		connect(timer_, &QTimer::timeout, this, [this] { update_status(); });
	}

	void frontend_finished_loading()
	{
		set_role_cache(read_role_from_config());
		refresh_sources();
		if (mcb_is_receiver())
			apply_receiver(false);
	}

	void refresh_sources()
	{
		const QString selected = receiver_source_->currentText();
		const QString saved = QString::fromUtf8(config_string("ReceiverSource", ""));
		receiver_source_->clear();
		for (const auto &name : list_ndi_audio_sources())
			receiver_source_->addItem(QString::fromUtf8(name.c_str()));
		const QString desired = !selected.isEmpty() ? selected : saved;
		const int index = receiver_source_->findText(desired);
		if (index >= 0)
			receiver_source_->setCurrentIndex(index);
	}

	void quick_restart_sender() { restart_sender(); }

private:
	void reanchor_sender()
	{
		if (!mcb_is_sender()) {
			checklist_->setText("Re-anchor is available only on the Gaming PC / Sender role.");
			return;
		}
		mcb_request_sender_reanchor();
		checklist_->setText("Sender timing re-anchor requested. It will apply on the next audio callback.");
	}

	void restart_sender()
	{
		if (!mcb_is_sender()) {
			checklist_->setText("Restart Bridge is available only on the Gaming PC / Sender role.");
			return;
		}
		const uint64_t now_ns = os_gettime_ns();
		if (last_restart_ns_ && now_ns >= last_restart_ns_ &&
			now_ns - last_restart_ns_ < 2000000000ULL) {
			checklist_->setText("Restart Bridge is cooling down. Wait two seconds before trying again.");
			return;
		}
		last_restart_ns_ = now_ns;
		restart_sender_->setEnabled(false);
		reanchor_sender_->setEnabled(false);
		mcb_request_sender_reanchor();
		main_output_deinit();
		main_output_init();
		restart_sender_->setEnabled(true);
		reanchor_sender_->setEnabled(true);
		checklist_->setText("Multichannel NDI sender restarted with the existing track mapping.");
		update_status();
	}

	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		g_ui_monitoring.store(true, std::memory_order_release);
		update_status();
		if (timer_)
			timer_->start();
	}

	void hideEvent(QHideEvent *event) override
	{
		if (timer_)
			timer_->stop();
		g_ui_monitoring.store(false, std::memory_order_release);
		QWidget::hideEvent(event);
	}

	void load_ui()
	{
		const MCBRole role = mcb_role();
		sender_radio_->setChecked(role == MCBRole::Sender);
		receiver_radio_->setChecked(role == MCBRole::Receiver);
		confirm_->setChecked(role != MCBRole::Unconfigured);
		track_a_->setValue(config_track("TrackA", 5));
		track_b_->setValue(config_track("TrackB", 6));
		program_name_->setText(QString::fromUtf8(config_string("ProgramProxyName", kDefaultProgramName)));
		mic_name_->setText(QString::fromUtf8(config_string("MicProxyName", kDefaultMicName)));
		auto *config = bridge_config();
		suppress_original_->setChecked(config ? config_get_bool(config, kSection, "SuppressOriginal") : true);
		governor_box_->setChecked(config ? config_get_bool(config, kSection, "GovernorEnabled") : true);
		auto_configure_->setChecked(config ? config_get_bool(config, kSection, "GovernorAutoConfigure") : true);
		max_skew_ms_->setValue(config ? static_cast<int>(config_get_int(config, kSection, "GovernorMaxSkewMs")) : 120);
		video_stall_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorVideoStallMs")) : 120);
		playout_delay_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorPlayoutDelayMs")) : 120);
		drift_correction_->setChecked(
			config ? config_get_bool(config, kSection, "GovernorDriftCorrection") : true);
		max_video_correction_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorMaxVideoCorrectionMs")) : 40);
		correction_slew_ppm_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorCorrectionSlewPpm")) : 1000);
		relock_pairs_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorRelockPairs")) : 12);
		baseline_window_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorBaselineWindowMs")) : 1000);
		drift_window_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftWindowMs")) : 30000);
		drift_minimum_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftMinimumMs")) : 10000);
		drift_deadband_ppm_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftDeadbandPpm")) : 8);
		apply_->setEnabled(confirm_->isChecked());
	}

	void role_changed()
	{
		confirm_->setChecked(false);
		update_role_visibility();
	}

	void update_role_visibility()
	{
		sender_box_->setVisible(sender_radio_->isChecked());
		receiver_box_->setVisible(receiver_radio_->isChecked());
		if (sender_radio_->isChecked()) {
			checklist_->setText(
				"<b>Sender checklist:</b> In Edit → Advanced Audio Properties, route desktop/game to "
				"Track A and mic to Track B. Enable DistroAV Main Output. Applying here rebuilds Main Output.");
		} else if (receiver_radio_->isChecked()) {
			checklist_->setText(
				"<b>Receiver checklist:</b> Add one normal DistroAV NDI Source, select it above, leave the "
				"A/V Governor on, then create the two split mixer sources. Recommended timing settings are applied automatically.");
		} else {
			checklist_->setText("Select and confirm a role before using the bridge.");
		}
	}

	void apply_settings(bool create_sources)
	{
		if (!confirm_->isChecked() || (!sender_radio_->isChecked() && !receiver_radio_->isChecked())) {
			checklist_->setText("Select a role and tick the confirmation box first.");
			return;
		}
		if (sender_radio_->isChecked() && track_a_->value() == track_b_->value()) {
			checklist_->setText("Track A and Track B must be different OBS tracks.");
			return;
		}

		const MCBRole previous_role = mcb_role();
		auto *config = bridge_config();
		if (!config)
			return;
		const MCBRole new_role = sender_radio_->isChecked() ? MCBRole::Sender : MCBRole::Receiver;
		config_set_string(config, kSection, "Role", new_role == MCBRole::Sender ? "sender" : "receiver");
		config_set_int(config, kSection, "TrackA", track_a_->value());
		config_set_int(config, kSection, "TrackB", track_b_->value());
		config_set_string(config, kSection, "ReceiverSource", receiver_source_->currentText().toUtf8().constData());
		config_set_string(config, kSection, "ProgramProxyName", program_name_->text().toUtf8().constData());
		config_set_string(config, kSection, "MicProxyName", mic_name_->text().toUtf8().constData());
		config_set_bool(config, kSection, "SuppressOriginal", suppress_original_->isChecked());
		config_set_bool(config, kSection, "GovernorEnabled", governor_box_->isChecked());
		config_set_bool(config, kSection, "GovernorAutoConfigure", auto_configure_->isChecked());
		config_set_int(config, kSection, "GovernorMaxSkewMs", max_skew_ms_->value());
		config_set_int(config, kSection, "GovernorVideoStallMs", video_stall_ms_->value());
		config_set_int(config, kSection, "GovernorPlayoutDelayMs", playout_delay_ms_->value());
		config_set_bool(config, kSection, "GovernorDriftCorrection", drift_correction_->isChecked());
		config_set_int(config, kSection, "GovernorMaxVideoCorrectionMs", max_video_correction_ms_->value());
		config_set_int(config, kSection, "GovernorCorrectionSlewPpm", correction_slew_ppm_->value());
		config_set_int(config, kSection, "GovernorRelockPairs", relock_pairs_->value());
		config_set_int(config, kSection, "GovernorBaselineWindowMs", baseline_window_ms_->value());
		config_set_int(config, kSection, "GovernorDriftWindowMs", drift_window_ms_->value());
		config_set_int(config, kSection, "GovernorDriftMinimumMs", drift_minimum_ms_->value());
		config_set_int(config, kSection, "GovernorDriftDeadbandPpm", drift_deadband_ppm_->value());
		save_config();
		set_role_cache(new_role);

		if (new_role == MCBRole::Sender) {
			if (previous_role == MCBRole::Receiver)
				ReceiverRouter::instance().detach();
			main_output_init();
			checklist_->setText("Sender role applied. DistroAV Main Output was rebuilt with the selected tracks.");
		} else {
			main_output_deinit();
			apply_receiver(create_sources);
		}
		update_status();
	}

	void apply_receiver(bool create_sources)
	{
		const std::string source_name = receiver_source_->currentText().toUtf8().constData();
		if (!ReceiverRouter::instance().attach(source_name)) {
			checklist_->setText(QString("Receiver could not attach: %1")
						   .arg(QString::fromUtf8(ReceiverRouter::instance().last_error().c_str())));
			return;
		}
		ReceiverRouter::instance().set_suppress_original(suppress_original_->isChecked());
		ReceiverRouter::instance().configure_governor(
			governor_box_->isChecked(), max_skew_ms_->value(), video_stall_ms_->value(),
			playout_delay_ms_->value(), drift_correction_->isChecked(), max_video_correction_ms_->value(),
			correction_slew_ppm_->value(), relock_pairs_->value(), baseline_window_ms_->value(),
			drift_window_ms_->value(), drift_minimum_ms_->value(), drift_deadband_ppm_->value());
		if (governor_box_->isChecked() && auto_configure_->isChecked())
			ReceiverRouter::instance().apply_recommended_source_settings();
		else
			ReceiverRouter::instance().refresh_source_configuration();
		if (create_sources) {
			const bool program_ok = add_proxy_to_current_scene(
				kProgramSourceId, program_name_->text().toUtf8().constData(), true);
			const bool mic_ok = add_proxy_to_current_scene(
				kMicSourceId, mic_name_->text().toUtf8().constData(), true);
			checklist_->setText(program_ok && mic_ok
						   ? "Receiver attached and both split audio sources are present in the current scene."
						   : "Receiver attached, but one or both split sources could not be added. Check for duplicate names.");
		} else {
			checklist_->setText("Receiver attached. Use Create / repair if the two mixer sources are missing.");
		}
	}

	void export_diagnostics()
	{
		const QString parent = QFileDialog::getExistingDirectory(this, "Choose diagnostics location");
		if (parent.isEmpty())
			return;
		const QString folder_name = QString("Multichannel-Bridge-Diagnostics-%1")
			.arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
		QDir parent_dir(parent);
		if (!parent_dir.mkpath(folder_name)) {
			QMessageBox::warning(this, "Diagnostics export", "Could not create the diagnostics folder.");
			return;
		}
		QDir output_dir(parent_dir.filePath(folder_name));
		auto write_text = [&output_dir](const QString &name, const QByteArray &data) {
			QFile file(output_dir.filePath(name));
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
				return false;
			return file.write(data) == static_cast<qint64>(data.size());
		};
		const QByteArray status = diagnostics().toUtf8();
		const QByteArray recorder = QByteArray::fromStdString(
			ReceiverRouter::instance().governor_flight_recorder_csv());
		const QByteArray notes(
			"This bundle contains OBS-side bridge status and a bounded A/V timing history.\n"
			"The CSV includes raw NDI timestamp/timecode values, converted OBS timestamps,\n"
			"playout depth, drift estimates, and recovery actions.\n");
		const bool ok = write_text("bridge-status.txt", status) &&
			write_text("av-governor-flight-recorder.csv", recorder) &&
			write_text("README.txt", notes);
		if (!ok) {
			QMessageBox::warning(this, "Diagnostics export", "One or more diagnostics files could not be written.");
			return;
		}
		checklist_->setText(QString("Diagnostics exported to %1").arg(output_dir.absolutePath()));
		QMessageBox::information(this, "Diagnostics export", output_dir.absolutePath());
	}

	QString diagnostics() const
	{
		const auto sender = mcb_sender_status_snapshot();
		const auto &router = ReceiverRouter::instance();
		const uint64_t now = os_gettime_ns();
		const double sender_age = sender.last_audio_monotonic_ns && now >= sender.last_audio_monotonic_ns
			? static_cast<double>(now - sender.last_audio_monotonic_ns) / 1e6
			: -1.0;
		const double receiver_age = router.last_packet_ns() && now >= router.last_packet_ns()
			? static_cast<double>(now - router.last_packet_ns()) / 1e6
			: -1.0;
		const auto governor = router.governor_snapshot();
		QString text;
		text += QString("Multichannel Bridge for DistroAV %1\nRole: %2\nOBS audio rate: %3 Hz\n")
			.arg(kVersion)
			.arg(mcb_is_sender() ? "Gaming PC / Sender" : mcb_is_receiver() ? "Stream PC / Receiver" : "Unconfigured")
			.arg(obs_get_audio() ? audio_output_get_sample_rate(obs_get_audio()) : 0);
		text += QString("Sender Sync Core %1\nSender active: %2\nTracks: %3 / %4\nPaired: %5\nDiscarded: %6\nSilence fallback: %7\n")
			.arg(kSenderCoreVersion)
			.arg(sender.active ? "yes" : "no").arg(sender.track_a).arg(sender.track_b)
			.arg(static_cast<qulonglong>(sender.paired_blocks))
			.arg(static_cast<qulonglong>(sender.discarded_blocks))
			.arg(static_cast<qulonglong>(sender.silence_fallback_blocks));
		text += QString("Discontinuities: %1\nRe-anchors: %2\nOversized blocks: %3\nCallback contention drops: %4\nSender epoch: %5\n")
			.arg(static_cast<qulonglong>(sender.discontinuities))
			.arg(static_cast<qulonglong>(sender.reanchors))
			.arg(static_cast<qulonglong>(sender.oversized_blocks))
			.arg(static_cast<qulonglong>(sender.contention_drops))
			.arg(static_cast<qulonglong>(sender.epoch));
		text += QString("Last timestamp delta: %1 ms\nQueues: %2 / %3\nSender audio age: %4 ms\n")
			.arg(static_cast<double>(sender.last_timestamp_delta_ns) / 1e6, 0, 'f', 3)
			.arg(sender.queue_depth_a).arg(sender.queue_depth_b).arg(sender_age, 0, 'f', 1);
		text += QString("Receiver attached: %1\nSplit outputs ready: %2\nSplit outputs active: %3\nDetected channels: %4\n")
			.arg(router.attached() ? "yes" : "no").arg(router.outputs_ready() ? "yes" : "no")
			.arg(router.outputs_active() ? "yes" : "no").arg(router.channels());
		text += QString("Packets: %1\nSuppressed original packets: %2\nReceiver packet age: %3 ms\nMissing program: %4\nMissing mic: %5\n")
			.arg(static_cast<qulonglong>(router.packets())).arg(static_cast<qulonglong>(router.suppressed()))
			.arg(receiver_age, 0, 'f', 1).arg(static_cast<qulonglong>(router.missing_program()))
			.arg(static_cast<qulonglong>(router.missing_mic()));
		text += QString("A/V Governor %1: %2\nPhase: %3\nReason: %4\nSource timing configured: %5\n")
			.arg(kGovernorVersion).arg(governor.enabled ? "enabled" : "disabled")
			.arg(governor_phase_name(governor.phase)).arg(governor_reason_name(governor.reason))
			.arg(governor.source_configured ? "yes" : "no");
		text += QString("Playout delay: %1 ms\nPlayout depth audio/video: %2 / %3 ms\nEstimated video interval: %4 ms\n")
			.arg(static_cast<double>(governor.playout_delay_ns) / 1e6, 0, 'f', 1)
			.arg(static_cast<double>(governor.audio_playout_depth_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.video_playout_depth_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.estimated_video_interval_ns) / 1e6, 0, 'f', 3);
		text += QString("Raw/filtered A/V skew: %1 / %2 ms\nLearned baseline: %3 ms (%4 samples over %5 ms)\nBaseline deviation: %6 ms\n")
			.arg(static_cast<double>(governor.raw_av_skew_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.av_skew_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.baseline_skew_ns) / 1e6, 0, 'f', 3)
			.arg(governor.baseline_samples)
			.arg(static_cast<double>(governor.baseline_window_ns) / 1e6, 0, 'f', 0)
			.arg(static_cast<double>(governor.baseline_deviation_ns) / 1e6, 0, 'f', 3);
		text += QString("Estimated drift: %1 ppm (%2% confidence, %3 samples)\nVideo correction: %4 ms (target %5 ms)\nEpoch rebase: %6 ms\n")
			.arg(static_cast<qlonglong>(governor.drift_ppm)).arg(governor.drift_confidence).arg(governor.drift_samples)
			.arg(static_cast<double>(governor.video_correction_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.target_video_correction_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.epoch_rebase_ns) / 1e6, 0, 'f', 3);
		text += QString("Blocked audio/video: %1 / %2\nFade-out/fade-in packets: %3 / %4\nMonotonic clamps: %5\n")
			.arg(static_cast<qulonglong>(governor.blocked_audio)).arg(static_cast<qulonglong>(governor.blocked_video))
			.arg(static_cast<qulonglong>(governor.fade_out_packets)).arg(static_cast<qulonglong>(governor.fade_in_packets))
			.arg(static_cast<qulonglong>(governor.monotonic_clamps));
		text += QString("Raw NDI audio timestamp/timecode: %1 / %2 (100 ns)\nRaw NDI video timestamp/timecode: %3 / %4 (100 ns)\n")
			.arg(static_cast<qlonglong>(governor.last_audio_ndi_timestamp_100ns))
			.arg(static_cast<qlonglong>(governor.last_audio_ndi_timecode_100ns))
			.arg(static_cast<qlonglong>(governor.last_video_ndi_timestamp_100ns))
			.arg(static_cast<qlonglong>(governor.last_video_ndi_timecode_100ns));
		text += QString("Discontinuities: %1\nLock acquisitions: %2\nAtomic recoveries: %3\nEpoch: %4\n")
			.arg(static_cast<qulonglong>(governor.discontinuities))
			.arg(static_cast<qulonglong>(governor.lock_acquisitions))
			.arg(static_cast<qulonglong>(governor.atomic_recoveries))
			.arg(static_cast<qulonglong>(governor.epoch));
		text += QString("Re-lock progress: %1 / %2\nCorrection updates: %3\nFlight recorder events: %4")
			.arg(governor.relock_progress).arg(governor.relock_required)
			.arg(static_cast<qulonglong>(governor.correction_updates)).arg(governor.recorder_events);
		return text;
	}

	void update_status()
	{
		const auto status = mcb_sender_status_snapshot();
		const uint64_t now = os_gettime_ns();
		const double age_ms = status.last_audio_monotonic_ns && now >= status.last_audio_monotonic_ns
				      ? static_cast<double>(now - status.last_audio_monotonic_ns) / 1e6
				      : -1.0;
		audio_t *obs_audio = obs_get_audio();
		const uint32_t rate = obs_audio ? audio_output_get_sample_rate(obs_audio) : 0;
		const QString rate_warning = rate == 48000
					     ? ""
					     : QString(" <b>Warning: OBS is %1 Hz; 48,000 Hz is recommended.</b>").arg(rate);
		const double delta_ms = static_cast<double>(status.last_timestamp_delta_ns) / 1e6;
		const QString phase_note = std::fabs(delta_ms) >= 15.0 && std::fabs(delta_ms) <= 30.0
					   ? " (normal OBS two-mix phase)"
					   : "";
		const bool sender_healthy = status.active && status.discarded_blocks == 0 &&
			status.silence_fallback_blocks == 0 && status.oversized_blocks == 0 &&
			status.contention_drops == 0;
		sender_status_->setText(
			QString("%1 · Sync Core %2 · Track %3 → NDI 1-2 · Track %4 → NDI 3-4 · "
				"paired %5 · discards/fallback %6/%7 · re-anchors %8 · epoch %9 · "
				"delta %10 ms%11 · queues %12/%13 · audio age %14 ms%15")
				.arg(sender_healthy ? "PROTECTED" : status.active ? "ACTIVE - check counters" : "not active")
				.arg(kSenderCoreVersion)
				.arg(status.track_a)
				.arg(status.track_b)
				.arg(static_cast<qulonglong>(status.paired_blocks))
				.arg(static_cast<qulonglong>(status.discarded_blocks))
				.arg(static_cast<qulonglong>(status.silence_fallback_blocks))
				.arg(static_cast<qulonglong>(status.reanchors))
				.arg(static_cast<qulonglong>(status.epoch))
				.arg(delta_ms, 0, 'f', 3)
				.arg(phase_note)
				.arg(status.queue_depth_a)
				.arg(status.queue_depth_b)
				.arg(age_ms, 0, 'f', 1)
				.arg(rate_warning));
		sender_program_meter_->setValue(meter_value(status.peak_a));
		sender_mic_meter_->setValue(meter_value(status.peak_b));
		if (!mcb_is_receiver())
			return;

		auto &router = ReceiverRouter::instance();
		const double receiver_age = router.last_packet_ns() && now >= router.last_packet_ns()
					    ? static_cast<double>(now - router.last_packet_ns()) / 1e6
					    : -1.0;
		QString channel_state;
		if (router.channels() >= 4)
			channel_state = "4 channels detected";
		else if (router.channels() > 0)
			channel_state = QString("only %1 channel(s) detected — full bridge audio is missing").arg(router.channels());
		else
			channel_state = "waiting for audio";
		receiver_status_->setText(
			QString("%1 · split outputs %2/%3 · %4 · packet age %5 ms · packets %6 · suppressed %7 · "
				"missing program %8 · missing mic %9")
				.arg(router.attached() ? "ATTACHED" : "not attached")
				.arg(router.outputs_ready() ? "created" : "missing")
				.arg(router.outputs_active() ? "active" : "inactive")
				.arg(channel_state)
				.arg(receiver_age, 0, 'f', 1)
				.arg(static_cast<qulonglong>(router.packets()))
				.arg(static_cast<qulonglong>(router.suppressed()))
				.arg(static_cast<qulonglong>(router.missing_program()))
				.arg(static_cast<qulonglong>(router.missing_mic())));
		program_meter_->setValue(meter_value(router.peak(0)));
		mic_meter_->setValue(meter_value(router.peak(1)));

		const auto governor = router.governor_snapshot();
		QString governor_state = QString::fromUtf8(governor_phase_name(governor.phase));
		if (governor.video_stalled)
			governor_state = "VIDEO STALL — correction bypassed while re-locking";
		const QString config_warning = governor.enabled && !governor.source_configured
			? " · source settings not recommended"
			: "";
		const QString recovery_note = governor.phase == mcb::AVGovernorPhase::Holding ||
			governor.phase == mcb::AVGovernorPhase::Relocking
			? QString(" · %1 · re-lock %2/%3")
				.arg(governor_reason_name(governor.reason))
				.arg(governor.relock_progress).arg(governor.relock_required)
			: "";
		governor_status_->setText(
			QString("%1 · delay/depth A/V %2/%3/%4 ms · skew raw/filtered/base %5/%6/%7 ms · "
				"deviation %8 ms · drift %9 ppm (%10%) · video correction %11→%12 ms · "
				"bypassed A/V decisions %13/%14 · recoveries %15 · fades %16/%17%18%19")
				.arg(governor_state)
				.arg(static_cast<double>(governor.playout_delay_ns) / 1e6, 0, 'f', 0)
				.arg(static_cast<double>(governor.audio_playout_depth_ns) / 1e6, 0, 'f', 1)
				.arg(static_cast<double>(governor.video_playout_depth_ns) / 1e6, 0, 'f', 1)
				.arg(static_cast<double>(governor.raw_av_skew_ns) / 1e6, 0, 'f', 2)
				.arg(static_cast<double>(governor.av_skew_ns) / 1e6, 0, 'f', 2)
				.arg(static_cast<double>(governor.baseline_skew_ns) / 1e6, 0, 'f', 2)
				.arg(static_cast<double>(governor.baseline_deviation_ns) / 1e6, 0, 'f', 2)
				.arg(static_cast<qlonglong>(governor.drift_ppm)).arg(governor.drift_confidence)
				.arg(static_cast<double>(governor.video_correction_ns) / 1e6, 0, 'f', 2)
				.arg(static_cast<double>(governor.target_video_correction_ns) / 1e6, 0, 'f', 2)
				.arg(static_cast<qulonglong>(governor.blocked_audio))
				.arg(static_cast<qulonglong>(governor.blocked_video))
				.arg(static_cast<qulonglong>(governor.atomic_recoveries))
				.arg(static_cast<qulonglong>(governor.fade_out_packets))
				.arg(static_cast<qulonglong>(governor.fade_in_packets))
				.arg(recovery_note).arg(config_warning));

	}

	QRadioButton *sender_radio_ = nullptr;
	QRadioButton *receiver_radio_ = nullptr;
	QCheckBox *confirm_ = nullptr;
	QGroupBox *sender_box_ = nullptr;
	QGroupBox *receiver_box_ = nullptr;
	QSpinBox *track_a_ = nullptr;
	QSpinBox *track_b_ = nullptr;
	QLabel *sender_status_ = nullptr;
	QProgressBar *sender_program_meter_ = nullptr;
	QProgressBar *sender_mic_meter_ = nullptr;
	QPushButton *reanchor_sender_ = nullptr;
	QPushButton *restart_sender_ = nullptr;
	QComboBox *receiver_source_ = nullptr;
	QPushButton *refresh_ = nullptr;
	QPushButton *reconnect_receiver_ = nullptr;
	QPushButton *open_source_ = nullptr;
	QGroupBox *governor_box_ = nullptr;
	QCheckBox *auto_configure_ = nullptr;
	QCheckBox *advanced_governor_ = nullptr;
	QWidget *advanced_governor_panel_ = nullptr;
	QSpinBox *playout_delay_ms_ = nullptr;
	QSpinBox *max_skew_ms_ = nullptr;
	QSpinBox *video_stall_ms_ = nullptr;
	QCheckBox *drift_correction_ = nullptr;
	QSpinBox *max_video_correction_ms_ = nullptr;
	QSpinBox *correction_slew_ppm_ = nullptr;
	QSpinBox *relock_pairs_ = nullptr;
	QSpinBox *baseline_window_ms_ = nullptr;
	QSpinBox *drift_window_ms_ = nullptr;
	QSpinBox *drift_minimum_ms_ = nullptr;
	QSpinBox *drift_deadband_ppm_ = nullptr;
	QLabel *governor_help_ = nullptr;
	QLabel *governor_status_ = nullptr;
	QPushButton *recommended_governor_ = nullptr;
	QLineEdit *program_name_ = nullptr;
	QLineEdit *mic_name_ = nullptr;
	QCheckBox *suppress_original_ = nullptr;
	QPushButton *create_receiver_ = nullptr;
	QLabel *receiver_status_ = nullptr;
	QProgressBar *program_meter_ = nullptr;
	QProgressBar *mic_meter_ = nullptr;
	QPushButton *apply_ = nullptr;
	QPushButton *reset_stats_ = nullptr;
	QPushButton *copy_diagnostics_ = nullptr;
	QPushButton *copy_flight_recorder_ = nullptr;
	QPushButton *export_diagnostics_ = nullptr;
	QLabel *checklist_ = nullptr;
	QTimer *timer_ = nullptr;
	uint64_t last_restart_ns_ = 0;
};

BridgeDock *g_dock = nullptr;
QAction *g_restart_action = nullptr;

void frontend_event(enum obs_frontend_event event, void *)
{
	if (!g_dock)
		return;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		QMetaObject::invokeMethod(g_dock, [] {
			if (g_dock)
				g_dock->frontend_finished_loading();
		}, Qt::QueuedConnection);
	} else if ((event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_PROFILE_CHANGING) &&
		mcb_is_receiver()) {
		ReceiverRouter::instance().detach();
	}
}
} // namespace

MCBRole mcb_role()
{
	int cached = g_role_cache.load(std::memory_order_acquire);
	if (cached < 0) {
		const MCBRole role = read_role_from_config();
		set_role_cache(role);
		cached = static_cast<int>(role);
	}
	return static_cast<MCBRole>(cached);
}

bool mcb_is_sender() { return mcb_role() == MCBRole::Sender; }
bool mcb_is_receiver() { return mcb_role() == MCBRole::Receiver; }
size_t mcb_sender_track_a_zero_based() { return static_cast<size_t>(config_track("TrackA", 5) - 1); }
size_t mcb_sender_track_b_zero_based() { return static_cast<size_t>(config_track("TrackB", 6) - 1); }

void mcb_register_sources()
{
	g_program_info = create_proxy_info(kProgramSourceId, program_source_name, create_program_proxy);
	g_mic_info = create_proxy_info(kMicSourceId, mic_source_name, create_mic_proxy);
	obs_register_source(&g_program_info);
	obs_register_source(&g_mic_info);
	obs_log(LOG_INFO, "[multichannel-bridge] Registered receiver proxy sources (%s)", kVersion);
}

void mcb_init(QWidget *main_window)
{
	if (g_dock || !main_window)
		return;
	ensure_defaults();
	set_role_cache(read_role_from_config());
	g_dock = new BridgeDock(main_window);
	if (!obs_frontend_add_dock_by_id(kDockId, "Multichannel Bridge for DistroAV", g_dock)) {
		delete g_dock;
		g_dock = nullptr;
		obs_log(LOG_WARNING, "[multichannel-bridge] Could not add dock; dock ID is already registered");
		return;
	}
	obs_frontend_add_event_callback(frontend_event, nullptr);
	g_restart_action = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction("Restart Multichannel NDI Sender"));
	if (g_restart_action) {
		g_restart_action->setToolTip("Recreate only the Multichannel Bridge DistroAV Main Output.");
		QObject::connect(g_restart_action, &QAction::triggered, g_dock, [] {
			if (g_dock)
				g_dock->quick_restart_sender();
		});
	}
	obs_log(LOG_INFO, "[multichannel-bridge] Dock initialized (%s)", kVersion);
}

void mcb_shutdown()
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	if (mcb_is_receiver())
		ReceiverRouter::instance().detach();
	if (g_dock) {
		obs_frontend_remove_dock(kDockId);
		g_dock = nullptr;
	}
	g_restart_action = nullptr;
	g_ui_monitoring.store(false, std::memory_order_release);
}

MCBSenderStatus mcb_sender_status_snapshot()
{
	MCBSenderStatus status;
	status.enabled = g_sender_enabled.load(std::memory_order_relaxed);
	status.active = g_sender_active.load(std::memory_order_relaxed);
	status.track_a = g_sender_track_a.load(std::memory_order_relaxed);
	status.track_b = g_sender_track_b.load(std::memory_order_relaxed);
	status.paired_blocks = g_sender_paired.load(std::memory_order_relaxed);
	status.discarded_blocks = g_sender_discarded.load(std::memory_order_relaxed);
	status.silence_fallback_blocks = g_sender_fallback.load(std::memory_order_relaxed);
	status.discontinuities = g_sender_discontinuities.load(std::memory_order_relaxed) +
		g_sender_video_discontinuities.load(std::memory_order_relaxed);
	status.reanchors = g_sender_reanchors.load(std::memory_order_relaxed);
	status.oversized_blocks = g_sender_oversized.load(std::memory_order_relaxed);
	status.contention_drops = g_sender_contention.load(std::memory_order_relaxed);
	status.epoch = g_sender_epoch.load(std::memory_order_relaxed);
	status.last_timestamp_delta_ns = g_sender_last_delta_ns.load(std::memory_order_relaxed);
	status.queue_depth_a = g_sender_queue_a.load(std::memory_order_relaxed);
	status.queue_depth_b = g_sender_queue_b.load(std::memory_order_relaxed);
	status.peak_a = g_sender_peak_a.load(std::memory_order_relaxed);
	status.peak_b = g_sender_peak_b.load(std::memory_order_relaxed);
	status.last_audio_monotonic_ns = g_sender_last_audio_ns.load(std::memory_order_relaxed);
	return status;
}

void mcb_sender_status_started(size_t track_a_zero_based, size_t track_b_zero_based)
{
	g_sender_enabled.store(true, std::memory_order_relaxed);
	g_sender_active.store(true, std::memory_order_relaxed);
	g_sender_track_a.store(static_cast<uint32_t>(track_a_zero_based + 1), std::memory_order_relaxed);
	g_sender_track_b.store(static_cast<uint32_t>(track_b_zero_based + 1), std::memory_order_relaxed);
	g_sender_last_video_timestamp_ns.store(0, std::memory_order_relaxed);
	mcb_sender_status_reset_counters();
}

void mcb_sender_status_stopped()
{
	g_sender_active.store(false, std::memory_order_relaxed);
	g_sender_queue_a.store(0, std::memory_order_relaxed);
	g_sender_queue_b.store(0, std::memory_order_relaxed);
	g_sender_peak_a.store(0.0f, std::memory_order_relaxed);
	g_sender_peak_b.store(0.0f, std::memory_order_relaxed);
}

void mcb_sender_status_sync(uint64_t paired, uint64_t discarded, uint64_t fallback,
	uint64_t discontinuities, uint64_t reanchors, uint64_t oversized,
	uint64_t contention_drops, uint64_t epoch, int64_t timestamp_delta_ns,
	uint32_t queue_a, uint32_t queue_b, float peak_a, float peak_b)
{
	g_sender_paired.store(paired, std::memory_order_relaxed);
	g_sender_discarded.store(discarded, std::memory_order_relaxed);
	g_sender_fallback.store(fallback, std::memory_order_relaxed);
	g_sender_discontinuities.store(discontinuities, std::memory_order_relaxed);
	g_sender_reanchors.store(reanchors, std::memory_order_relaxed);
	g_sender_oversized.store(oversized, std::memory_order_relaxed);
	g_sender_contention.store(contention_drops, std::memory_order_relaxed);
	g_sender_epoch.store(epoch, std::memory_order_relaxed);
	g_sender_last_delta_ns.store(timestamp_delta_ns, std::memory_order_relaxed);
	g_sender_queue_a.store(queue_a, std::memory_order_relaxed);
	g_sender_queue_b.store(queue_b, std::memory_order_relaxed);
	g_sender_peak_a.store(std::clamp(peak_a, 0.0f, 1.0f), std::memory_order_relaxed);
	g_sender_peak_b.store(std::clamp(peak_b, 0.0f, 1.0f), std::memory_order_relaxed);
	g_sender_last_audio_ns.store(os_gettime_ns(), std::memory_order_relaxed);
}

void mcb_sender_status_contention_drop()
{
	g_sender_contention.fetch_add(1, std::memory_order_relaxed);
}

void mcb_sender_status_reset_counters()
{
	g_sender_counter_reset_generation.fetch_add(1, std::memory_order_acq_rel);
	g_sender_paired.store(0, std::memory_order_relaxed);
	g_sender_discarded.store(0, std::memory_order_relaxed);
	g_sender_fallback.store(0, std::memory_order_relaxed);
	g_sender_discontinuities.store(0, std::memory_order_relaxed);
	g_sender_video_discontinuities.store(0, std::memory_order_relaxed);
	g_sender_reanchors.store(0, std::memory_order_relaxed);
	g_sender_oversized.store(0, std::memory_order_relaxed);
	g_sender_contention.store(0, std::memory_order_relaxed);
	g_sender_epoch.store(1, std::memory_order_relaxed);
	g_sender_last_delta_ns.store(0, std::memory_order_relaxed);
	g_sender_queue_a.store(0, std::memory_order_relaxed);
	g_sender_queue_b.store(0, std::memory_order_relaxed);
	g_sender_peak_a.store(0.0f, std::memory_order_relaxed);
	g_sender_peak_b.store(0.0f, std::memory_order_relaxed);
	g_sender_last_audio_ns.store(0, std::memory_order_relaxed);
}

uint64_t mcb_sender_counter_reset_generation()
{
	return g_sender_counter_reset_generation.load(std::memory_order_acquire);
}

void mcb_request_sender_reanchor()
{
	g_sender_reanchor_generation.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t mcb_sender_reanchor_generation()
{
	return g_sender_reanchor_generation.load(std::memory_order_acquire);
}

void mcb_sender_observe_video(uint64_t timestamp_ns)
{
	if (!timestamp_ns)
		return;
	const uint64_t previous = g_sender_last_video_timestamp_ns.exchange(
		timestamp_ns, std::memory_order_acq_rel);
	if (!previous)
		return;
	constexpr uint64_t maximum_continuous_gap_ns = 2000000000ULL;
	if (timestamp_ns <= previous || timestamp_ns - previous > maximum_continuous_gap_ns) {
		g_sender_video_discontinuities.fetch_add(1, std::memory_order_relaxed);
		mcb_request_sender_reanchor();
	}
}

bool mcb_ui_monitoring_enabled()
{
	return g_ui_monitoring.load(std::memory_order_acquire);
}

bool mcb_receiver_route_audio(obs_source_t *origin, const obs_source_audio *audio, int channel_count,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
{
	if (!mcb_is_receiver())
		return false;
	return ReceiverRouter::instance().route(origin, audio, channel_count,
		ndi_timestamp_100ns, ndi_timecode_100ns);
}

bool mcb_receiver_route_video(obs_source_t *origin, obs_source_frame *video,
	int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
{
	if (!mcb_is_receiver())
		return true;
	return ReceiverRouter::instance().route_video(origin, video,
		ndi_timestamp_100ns, ndi_timecode_100ns);
}
