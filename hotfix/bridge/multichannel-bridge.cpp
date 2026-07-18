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
#include <QIcon>
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
#include <QStringList>
#include <QTimer>
#include <QToolButton>
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
constexpr const char *kVersion = "0.5.1-alpha1";
constexpr const char *kGovernorVersion = "1.3";
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
	case mcb::AVGovernorPhase::Verifying: return "VERIFYING";
	case mcb::AVGovernorPhase::Failed: return "NEEDS ATTENTION";
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
	case mcb::AVGovernorReason::BaselineMismatch: return "recovered timing did not match the trusted reference";
	case mcb::AVGovernorReason::RecoveryLimit: return "too many recoveries in a short period";
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
	config_set_default_int(config, kSection, "GovernorBaselineWindowMs", 5000);
	config_set_default_int(config, kSection, "GovernorDriftWindowMs", 120000);
	config_set_default_int(config, kSection, "GovernorDriftMinimumMs", 30000);
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
		obs_data_set_int(settings, "ndi_behavior", 0); // Keep the canonical receiver active across scenes.
		obs_source_update(source, settings);
		obs_data_release(settings);
		obs_source_release(source);
		const bool reconnect_requested = force_reconnect();
		governor_.set_source_configured(true);
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
			governor_.set_source_configured(false);
			governor_.set_source_configured(true);
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

std::string ndi_sender_for_source(obs_source_t *source)
{
	if (!source)
		return {};
	obs_data_t *settings = obs_source_get_settings(source);
	const char *sender = settings ? obs_data_get_string(settings, "ndi_source_name") : nullptr;
	std::string result = sender ? sender : "";
	if (settings)
		obs_data_release(settings);
	return result;
}

size_t matching_receiver_count(const std::string &source_name)
{
	obs_source_t *selected = obs_get_source_by_name(source_name.c_str());
	if (!selected)
		return 0;
	const std::string sender_name = ndi_sender_for_source(selected);
	obs_source_release(selected);
	if (sender_name.empty())
		return 0;

	struct MatchContext {
		const std::string *sender = nullptr;
		size_t count = 0;
	} context{&sender_name, 0};
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *match = static_cast<MatchContext *>(param);
			const char *id = obs_source_get_id(source);
			if (id && std::strcmp(id, "ndi_source") == 0 &&
				ndi_sender_for_source(source) == *match->sender)
				++match->count;
			return true;
		},
		&context);
	return context.count;
}

QStringList scenes_containing_source(const std::string &source_name)
{
	QStringList names;
	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *scene_source = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		if (scene && obs_scene_find_source(scene, source_name.c_str())) {
			const char *name = obs_source_get_name(scene_source);
			if (name && *name)
				names.append(QString::fromUtf8(name));
		}
	}
	obs_frontend_source_list_free(&scenes);
	return names;
}

bool add_existing_source_to_current_scene(const std::string &source_name)
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	obs_source_t *source = obs_get_source_by_name(source_name.c_str());
	if (!scene_source || !source) {
		if (source)
			obs_source_release(source);
		if (scene_source)
			obs_source_release(scene_source);
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	const bool success = scene &&
		(source_in_current_scene(scene, source_name.c_str()) || obs_scene_add(scene, source));
	obs_source_release(source);
	obs_source_release(scene_source);
	return success;
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
		outer->setContentsMargins(0, 0, 0, 0);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		auto *body = new QWidget(scroll);
		auto *root_layout = new QVBoxLayout(body);
		root_layout->setContentsMargins(6, 6, 6, 6);
		root_layout->setSpacing(5);

		auto *title_row = new QHBoxLayout;
		setup_toggle_ = new QToolButton(body);
		setup_toggle_->setCheckable(true);
		setup_toggle_->setText("▸ Setup");
		setup_toggle_->setToolTip("Show role, source, recovery, and advanced settings.");
		numbers_toggle_ = new QToolButton(body);
		numbers_toggle_->setCheckable(true);
		numbers_toggle_->setText("▸ Numbers");
		numbers_toggle_->setToolTip("Show the useful timing numbers without opening all setup controls.");
		title_row->addWidget(setup_toggle_);
		title_row->addWidget(numbers_toggle_);
		auto *title = new QLabel(QString("<b>Multichannel Bridge %1</b>").arg(kVersion), body);
		title_row->addWidget(title, 1);
		root_layout->addLayout(title_row);

		monitor_panel_ = new QWidget(body);
		auto *monitor_layout = new QVBoxLayout(monitor_panel_);
		monitor_layout->setContentsMargins(0, 0, 0, 0);
		monitor_layout->setSpacing(4);
		health_banner_ = new QLabel(monitor_panel_);
		health_banner_->setWordWrap(true);
		health_banner_->setTextFormat(Qt::RichText);
		health_banner_->setMinimumHeight(34);
		timing_summary_ = new QLabel(monitor_panel_);
		timing_summary_->setWordWrap(true);
		timing_summary_->setTextFormat(Qt::RichText);
		suggestion_ = new QLabel(monitor_panel_);
		suggestion_->setWordWrap(true);
		suggestion_->setTextFormat(Qt::RichText);
		monitor_layout->addWidget(health_banner_);
		monitor_layout->addWidget(timing_summary_);

		auto *monitor_meter_row = new QHBoxLayout;
		monitor_program_label_ = new QLabel("Desktop + game", monitor_panel_);
		monitor_mic_label_ = new QLabel("Microphone", monitor_panel_);
		monitor_program_meter_ = new QProgressBar(monitor_panel_);
		monitor_mic_meter_ = new QProgressBar(monitor_panel_);
		for (auto *meter : {monitor_program_meter_, monitor_mic_meter_}) {
			meter->setRange(0, 100);
			meter->setTextVisible(false);
			meter->setMaximumHeight(9);
		}
		monitor_meter_row->addWidget(monitor_program_label_);
		monitor_meter_row->addWidget(monitor_program_meter_, 1);
		monitor_meter_row->addWidget(monitor_mic_label_);
		monitor_meter_row->addWidget(monitor_mic_meter_, 1);
		monitor_layout->addLayout(monitor_meter_row);
		monitor_layout->addWidget(suggestion_);

		monitor_numbers_panel_ = new QWidget(monitor_panel_);
		auto *numbers_layout = new QVBoxLayout(monitor_numbers_panel_);
		numbers_layout->setContentsMargins(6, 2, 6, 2);
		monitor_numbers_ = new QLabel(monitor_numbers_panel_);
		monitor_numbers_->setWordWrap(true);
		monitor_numbers_->setTextFormat(Qt::RichText);
		numbers_layout->addWidget(monitor_numbers_);
		monitor_numbers_panel_->setVisible(false);
		monitor_layout->addWidget(monitor_numbers_panel_);
		root_layout->addWidget(monitor_panel_);

		configuration_panel_ = new QWidget(body);
		auto *layout = new QVBoxLayout(configuration_panel_);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(5);
		auto *intro = new QLabel(
			"Install the same package on both PCs, select one role, then apply.", configuration_panel_);
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
		source_context_ = new QLabel(receiver_box_);
		source_context_->setWordWrap(true);
		add_receiver_here_ = new QPushButton("Add this existing receiver to current scene", receiver_box_);
		add_receiver_here_->setToolTip(
			"Adds a reference to the attached receiver. It does not create another NDI connection.");
		receiver_form->addRow("Shared source:", source_context_);
		receiver_form->addRow(add_receiver_here_);

		governor_box_ = new QGroupBox(
			QString("Automatic A/V protection (Governor %1)").arg(kGovernorVersion), receiver_box_);
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
		baseline_window_ms_->setRange(5000, 30000);
		baseline_window_ms_->setSuffix(" ms");
		baseline_window_ms_->setToolTip(
			"Required stable time before the first trusted reference or a recovery verification is accepted.");
		drift_window_ms_ = new QSpinBox(governor_box_);
		drift_window_ms_->setRange(30000, 300000);
		drift_window_ms_->setSuffix(" ms");
		drift_window_ms_->setToolTip(
			"History used to distinguish real clock drift from short-term network jitter.");
		drift_minimum_ms_ = new QSpinBox(governor_box_);
		drift_minimum_ms_->setRange(30000, 300000);
		drift_minimum_ms_->setSuffix(" ms");
		drift_minimum_ms_->setToolTip(
			"A drift direction must persist for at least this long before video timing is adjusted.");
		drift_deadband_ppm_ = new QSpinBox(governor_box_);
		drift_deadband_ppm_->setRange(1, 250);
		drift_deadband_ppm_->setSuffix(" ppm");
		drift_deadband_ppm_->setToolTip(
			"Ignore smaller estimated clock differences so normal jitter cannot trigger correction.");
		governor_help_ = new QLabel(
			"Keeps the last trusted sync through jumps, quarantines recovery samples, waits at least 30 seconds before drift correction, "
			"and bypasses correction if safe recovery cannot be verified.", governor_box_);
		governor_help_->setWordWrap(true);
		governor_status_ = new QLabel(governor_box_);
		governor_status_->setWordWrap(true);
		recommended_governor_ = new QPushButton("Restore recommended settings", governor_box_);
		recommended_governor_->setToolTip("Restore conservative settings suitable for a single two-PC NDI source.");
		advanced_governor_ = new QCheckBox("Show expert timing controls", governor_box_);
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
		governor_form->addRow("Protection:", governor_status_);
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
		root_layout->addWidget(configuration_panel_);
		root_layout->addStretch(1);
		scroll->setWidget(body);
		outer->addWidget(scroll);

		load_ui();
		refresh_sources();
		update_role_visibility();
		setup_toggle_->setChecked(mcb_role() == MCBRole::Unconfigured);
		configuration_panel_->setVisible(setup_toggle_->isChecked());
		setup_toggle_->setText(setup_toggle_->isChecked() ? "▾ Setup" : "▸ Setup");
		update_status();

		connect(setup_toggle_, &QToolButton::toggled, this, [this](bool expanded) {
			configuration_panel_->setVisible(expanded);
			setup_toggle_->setText(expanded ? "▾ Setup" : "▸ Setup");
		});
		connect(numbers_toggle_, &QToolButton::toggled, this, [this](bool expanded) {
			monitor_numbers_panel_->setVisible(expanded);
			numbers_toggle_->setText(expanded ? "▾ Numbers" : "▸ Numbers");
		});
		connect(sender_radio_, &QRadioButton::toggled, this, [this] { role_changed(); });
		connect(receiver_radio_, &QRadioButton::toggled, this, [this] { role_changed(); });
		connect(confirm_, &QCheckBox::toggled, this, [this] { apply_->setEnabled(confirm_->isChecked()); });
		connect(refresh_, &QPushButton::clicked, this, [this] { refresh_sources(); });
		connect(receiver_source_, &QComboBox::currentTextChanged, this, [this] { refresh_source_context(); });
		connect(add_receiver_here_, &QPushButton::clicked, this, [this] {
			const std::string name = receiver_source_->currentText().toUtf8().constData();
			const bool ok = add_existing_source_to_current_scene(name);
			checklist_->setText(ok
				? "Added a reference to the existing NDI receiver in this scene. No duplicate connection was created."
				: "Could not add the existing receiver. Select a valid DistroAV source first.");
			refresh_source_context();
		});
		connect(reconnect_receiver_, &QPushButton::clicked, this, [this] {
			const bool ok = ReceiverRouter::instance().force_reconnect();
			if (ok)
				auto_reconnect_attempts_ = 0;
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
			baseline_window_ms_->setValue(5000);
			drift_window_ms_->setValue(120000);
			drift_minimum_ms_->setValue(30000);
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
		safety_timer_ = new QTimer(this);
		safety_timer_->setInterval(2000);
		connect(safety_timer_, &QTimer::timeout, this, [this] { automatic_recovery_tick(); });
		safety_timer_->start();
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
		refresh_source_context();
	}

	void refresh_source_context()
	{
		const std::string source_name = receiver_source_->currentText().toUtf8().constData();
		if (source_name.empty()) {
			source_context_->setText("No receiver selected.");
			return;
		}
		const size_t matches = matching_receiver_count(source_name);
		const QStringList scenes = scenes_containing_source(source_name);
		QString text = scenes.isEmpty()
			? "Not present in a scene. Add this existing receiver instead of creating a duplicate."
			: QString("Used by: %1.").arg(scenes.join(", "));
		if (matches > 1)
			text += QString(" <b><span style='color:#e6b450'>%1 separate receivers use the same NDI sender.</span></b>")
				.arg(matches);
		else
			text += " One canonical receiver detected.";
		source_context_->setText(text);
	}

	void quick_restart_sender() { restart_sender(); }

private:
	void set_health_banner(const QString &text, const char *foreground, const char *background)
	{
		health_banner_->setText(text);
		health_banner_->setStyleSheet(QString(
			"QLabel { color: %1; background: %2; border: 1px solid %1; border-radius: 4px; padding: 6px; }")
			.arg(QString::fromUtf8(foreground), QString::fromUtf8(background)));
	}

	void update_sender_monitor(const MCBSenderStatus &status, double age_ms, uint32_t rate)
	{
		monitor_program_label_->setText(QString("Track %1").arg(status.track_a));
		monitor_mic_label_->setText(QString("Track %1").arg(status.track_b));
		monitor_program_meter_->setValue(meter_value(status.peak_a));
		monitor_mic_meter_->setValue(meter_value(status.peak_b));
		const bool healthy = status.active && rate == 48000 && status.discarded_blocks == 0 &&
			status.silence_fallback_blocks == 0 && status.oversized_blocks == 0 &&
			status.contention_drops == 0;
		if (healthy) {
			set_health_banner("<b>Sender protected</b> — both audio tracks are paired and moving normally.",
				"#57c785", "rgba(40,95,68,90)");
			timing_summary_->setText(QString(
				"<b>Track %1</b> → NDI 1–2 &nbsp;·&nbsp; <b>Track %2</b> → NDI 3–4")
				.arg(status.track_a).arg(status.track_b));
			suggestion_->setText("<b>Suggested:</b> No action needed.");
		} else if (!status.active) {
			set_health_banner("<b>Sender offline</b> — DistroAV Main Output is not running.",
				"#e05d5d", "rgba(110,45,45,90)");
			timing_summary_->setText("Audio is not currently being packed into the NDI sender.");
			suggestion_->setText("<b>Suggested:</b> Start Main Output or use <b>Restart Bridge</b> in Setup.");
		} else {
			set_health_banner("<b>Sender needs attention</b> — one or more safety counters increased.",
				"#e6b450", "rgba(105,76,30,90)");
			timing_summary_->setText(QString(
				"Discards <b>%1</b> · fallback <b>%2</b> · contention <b>%3</b>")
				.arg(static_cast<qulonglong>(status.discarded_blocks))
				.arg(static_cast<qulonglong>(status.silence_fallback_blocks))
				.arg(static_cast<qulonglong>(status.contention_drops)));
			suggestion_->setText("<b>Suggested:</b> Open Setup and use <b>Re-anchor sync</b>; restart only if counters keep rising.");
		}
		monitor_numbers_->setText(QString(
			"<b>Paired:</b> %1 &nbsp;·&nbsp; <b>Audio age:</b> %2 ms &nbsp;·&nbsp; "
			"<b>Callback spacing:</b> %3 ms<br>Discards %4 · fallback %5 · re-anchors %6 · epoch %7")
			.arg(static_cast<qulonglong>(status.paired_blocks)).arg(age_ms, 0, 'f', 1)
			.arg(static_cast<double>(status.last_timestamp_delta_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<qulonglong>(status.discarded_blocks))
			.arg(static_cast<qulonglong>(status.silence_fallback_blocks))
			.arg(static_cast<qulonglong>(status.reanchors)).arg(static_cast<qulonglong>(status.epoch)));
	}

	void update_receiver_monitor(const ReceiverRouter &router, const mcb::AVGovernorSnapshot &governor,
		double receiver_age_ms)
	{
		monitor_program_label_->setText("Desktop + game");
		monitor_mic_label_->setText("Microphone");
		monitor_program_meter_->setValue(meter_value(router.peak(0)));
		monitor_mic_meter_->setValue(meter_value(router.peak(1)));
		const std::string source_name = router.input_name();
		const size_t duplicate_count = matching_receiver_count(source_name);
		const bool connection_ready = router.attached() && router.outputs_active() && router.channels() >= 4 &&
			receiver_age_ms >= 0.0 && receiver_age_ms < 500.0;
		const double skew_ms = static_cast<double>(governor.av_skew_ns) / 1e6;
		const double deviation_ms = static_cast<double>(governor.baseline_deviation_ns) / 1e6;
		const double correction_ms = static_cast<double>(governor.video_correction_ns) / 1e6;
		const double absolute_deviation = std::fabs(deviation_ms);
		QString direction;
		if (!governor.baseline_valid) {
			direction = "Learning the normal relationship between the shared audio timeline and video.";
		} else if (absolute_deviation < 2.0) {
			direction = "<span style='color:#57c785'><b>Timelines aligned</b></span> — no track is meaningfully rushing or dragging.";
		} else if (deviation_ms > 0.0) {
			direction = QString(
				"<span style='color:#e6b450'><b>Desktop + mic rushing</b></span> by %1 ms · video timeline dragging.")
				.arg(absolute_deviation, 0, 'f', 1);
		} else {
			direction = QString(
				"<span style='color:#e6b450'><b>Video rushing</b></span> by %1 ms · desktop + mic timeline dragging.")
				.arg(absolute_deviation, 0, 'f', 1);
		}
		timing_summary_->setText(direction);

		if (duplicate_count > 1) {
			set_health_banner(QString("<b>Setup conflict</b> — %1 separate NDI receivers use the same sender.").arg(duplicate_count),
				"#e6b450", "rgba(105,76,30,90)");
			suggestion_->setText("<b>Suggested:</b> Keep one receiver and add it to other scenes as an existing source.");
		} else if (!connection_ready) {
			set_health_banner("<b>Receiver waiting</b> — the complete four-channel feed is not active.",
				"#e05d5d", "rgba(110,45,45,90)");
			suggestion_->setText("<b>Suggested:</b> Open Setup, select the canonical receiver, then reconnect or repair outputs.");
		} else if (!governor.enabled) {
			set_health_banner("<b>Monitoring only</b> — split audio is healthy; automatic timing protection is off.",
				"#b8bec9", "rgba(70,74,82,90)");
			suggestion_->setText("<b>Suggested:</b> Leave off for raw DistroAV timing, or enable protection in Setup.");
		} else if (governor.phase == mcb::AVGovernorPhase::Failed || governor.fail_safe_bypassed) {
			set_health_banner("<b>Needs attention</b> — trusted sync could not be restored; correction is bypassed.",
				"#e05d5d", "rgba(110,45,45,90)");
			suggestion_->setText("<b>Suggested:</b> Reconnect the existing receiver. Output is being kept live unchanged.");
		} else if (governor.phase == mcb::AVGovernorPhase::Holding ||
			governor.phase == mcb::AVGovernorPhase::Verifying ||
			governor.phase == mcb::AVGovernorPhase::WarmingUp) {
			set_health_banner(QString("<b>Recovering safely</b> — %1.").arg(governor_reason_name(governor.reason)),
				"#5aa9e6", "rgba(42,76,110,90)");
			suggestion_->setText("<b>Suggested:</b> Leave it running. Normal output stays live while the trusted sync is verified.");
		} else if (governor.correction_limited) {
			set_health_banner("<b>Drift exceeds the safe correction range</b> — protection will not chase it further.",
				"#e6b450", "rgba(105,76,30,90)");
			suggestion_->setText("<b>Suggested:</b> Check the audio device clock or reconnect the canonical receiver.");
		} else if (std::fabs(correction_ms) >= 0.1) {
			set_health_banner("<b>Correcting slow drift</b> — video timing is moving gently toward the trusted sync.",
				"#5aa9e6", "rgba(42,76,110,90)");
			suggestion_->setText("<b>Suggested:</b> No action needed.");
		} else {
			set_health_banner("<b>Protected</b> — four channels are healthy and trusted sync is locked.",
				"#57c785", "rgba(40,95,68,90)");
			suggestion_->setText("<b>Suggested:</b> No action needed.");
		}

		const double drift_seconds = static_cast<double>(governor.drift_samples) * 0.25;
		const QString drift_text = governor.drift_confidence >= 75
			? QString("%1 ppm · verified").arg(static_cast<qlonglong>(governor.drift_ppm))
			: QString("measuring · about %1 s collected").arg(drift_seconds, 0, 'f', 0);
		monitor_numbers_->setText(QString(
			"<b>Current A/V relation:</b> %1 ms &nbsp;·&nbsp; <b>Change from trusted sync:</b> %2 ms<br>"
			"<b>Trusted reference:</b> %3 ms &nbsp;·&nbsp; <b>Drift:</b> %4<br>"
			"<b>Video correction:</b> %5 ms &nbsp;·&nbsp; <b>Packet age:</b> %6 ms<br>"
			"Recoveries %7 · failed recoveries %8 · missing desktop/mic %9/%10")
			.arg(skew_ms, 0, 'f', 2).arg(deviation_ms, 0, 'f', 2)
			.arg(static_cast<double>(governor.baseline_skew_ns) / 1e6, 0, 'f', 2)
			.arg(drift_text).arg(correction_ms, 0, 'f', 2).arg(receiver_age_ms, 0, 'f', 1)
			.arg(static_cast<qulonglong>(governor.atomic_recoveries))
			.arg(static_cast<qulonglong>(governor.failed_recoveries))
			.arg(static_cast<qulonglong>(router.missing_program()))
			.arg(static_cast<qulonglong>(router.missing_mic())));
	}

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
		// A floating OBS dock is a top-level Qt window. Explicitly inherit OBS's
		// application icon so Windows does not show a generic white-page taskbar icon.
		QIcon obs_icon = QApplication::windowIcon();
		for (QWidget *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
			if (!ancestor->windowIcon().isNull())
				obs_icon = ancestor->windowIcon();
		}
		if (!obs_icon.isNull()) {
			setWindowIcon(obs_icon);
			if (QWidget *top = window())
				top->setWindowIcon(obs_icon);
		}
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
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorBaselineWindowMs")) : 5000);
		drift_window_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftWindowMs")) : 120000);
		drift_minimum_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftMinimumMs")) : 30000);
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
		auto_reconnect_attempts_ = 0;
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
		QString text = QString("Multichannel Bridge for DistroAV %1\nRole: %2\nOBS audio rate: %3 Hz\n")
			.arg(kVersion)
			.arg(mcb_is_sender() ? "Gaming PC / Sender" : mcb_is_receiver() ? "Stream PC / Receiver" : "Unconfigured")
			.arg(obs_get_audio() ? audio_output_get_sample_rate(obs_get_audio()) : 0);
		if (mcb_is_sender()) {
			text += QString(
				"Sender Sync Core %1\nActive: %2\nTracks: %3 / %4\nPaired blocks: %5\n"
				"Discards / silence fallback: %6 / %7\nDiscontinuities / re-anchors: %8 / %9\n"
				"Oversized / callback contention: %10 / %11\nCallback spacing: %12 ms\n"
				"Queues: %13 / %14\nAudio age: %15 ms\nEpoch: %16")
				.arg(kSenderCoreVersion).arg(sender.active ? "yes" : "no")
				.arg(sender.track_a).arg(sender.track_b)
				.arg(static_cast<qulonglong>(sender.paired_blocks))
				.arg(static_cast<qulonglong>(sender.discarded_blocks))
				.arg(static_cast<qulonglong>(sender.silence_fallback_blocks))
				.arg(static_cast<qulonglong>(sender.discontinuities))
				.arg(static_cast<qulonglong>(sender.reanchors))
				.arg(static_cast<qulonglong>(sender.oversized_blocks))
				.arg(static_cast<qulonglong>(sender.contention_drops))
				.arg(static_cast<double>(sender.last_timestamp_delta_ns) / 1e6, 0, 'f', 3)
				.arg(sender.queue_depth_a).arg(sender.queue_depth_b).arg(sender_age, 0, 'f', 1)
				.arg(static_cast<qulonglong>(sender.epoch));
			return text;
		}
		if (!mcb_is_receiver())
			return text + "Status: setup required";

		const double deviation = static_cast<double>(governor.baseline_deviation_ns) / 1e6;
		const QString direction = std::fabs(deviation) < 2.0
			? "aligned"
			: deviation > 0.0 ? "desktop + mic ahead; video dragging" : "video ahead; desktop + mic dragging";
		text += QString(
			"Receiver: %1\nCanonical source: %2\nReceivers using same NDI sender: %3\n"
			"Split outputs: %4\nChannels: %5\nPackets / suppressed original: %6 / %7\n"
			"Packet age: %8 ms\nMissing desktop / mic: %9 / %10\n"
			"A/V Governor %11: %12\nState: %13\nReason: %14\nFail-safe bypass: %15\n"
			"Timing direction: %16 by %17 ms\nCurrent raw / filtered relation: %18 / %19 ms\n"
			"Trusted reference / recovery candidate: %20 / %21 ms\n"
			"Drift: %22 ppm (%23 percent maturity, %24 samples)\nVideo correction / target: %25 / %26 ms\n"
			"Correction limited: %27\nPlayout target / audio / video depth: %28 / %29 / %30 ms\n"
			"Clock-domain offset: %31 ms\nDiscontinuities / recoveries / failed: %32 / %33 / %34\n"
			"Quarantined samples: %35\nProtected recorder events: %36\nEpoch: %37")
			.arg(router.attached() ? "attached" : "not attached")
			.arg(QString::fromUtf8(router.input_name().c_str()))
			.arg(static_cast<qulonglong>(matching_receiver_count(router.input_name())))
			.arg(router.outputs_active() ? "active" : "inactive").arg(router.channels())
			.arg(static_cast<qulonglong>(router.packets())).arg(static_cast<qulonglong>(router.suppressed()))
			.arg(receiver_age, 0, 'f', 1).arg(static_cast<qulonglong>(router.missing_program()))
			.arg(static_cast<qulonglong>(router.missing_mic())).arg(kGovernorVersion)
			.arg(governor.enabled ? "enabled" : "disabled").arg(governor_phase_name(governor.phase))
			.arg(governor_reason_name(governor.reason)).arg(governor.fail_safe_bypassed ? "yes" : "no")
			.arg(direction).arg(std::fabs(deviation), 0, 'f', 2)
			.arg(static_cast<double>(governor.raw_av_skew_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.av_skew_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.baseline_skew_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.candidate_baseline_skew_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<qlonglong>(governor.drift_ppm)).arg(governor.drift_confidence)
			.arg(governor.drift_samples)
			.arg(static_cast<double>(governor.video_correction_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(governor.target_video_correction_ns) / 1e6, 0, 'f', 3)
			.arg(governor.correction_limited ? "yes" : "no")
			.arg(static_cast<double>(governor.playout_delay_ns) / 1e6, 0, 'f', 1)
			.arg(static_cast<double>(governor.audio_playout_depth_ns) / 1e6, 0, 'f', 1)
			.arg(static_cast<double>(governor.video_playout_depth_ns) / 1e6, 0, 'f', 1)
			.arg(static_cast<double>(governor.epoch_rebase_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<qulonglong>(governor.discontinuities))
			.arg(static_cast<qulonglong>(governor.atomic_recoveries))
			.arg(static_cast<qulonglong>(governor.failed_recoveries))
			.arg(static_cast<qulonglong>(governor.quarantined_samples))
			.arg(governor.recorder_events).arg(static_cast<qulonglong>(governor.epoch));
		return text;
	}

	void automatic_recovery_tick()
	{
		if (!mcb_is_receiver() || auto_reconnect_attempts_ != 0)
			return;
		auto &router = ReceiverRouter::instance();
		const auto governor = router.governor_snapshot();
		if (!governor.fail_safe_bypassed)
			return;
		++auto_reconnect_attempts_;
		const bool reconnected = router.force_reconnect();
		checklist_->setText(reconnected
			? "Automatic recovery reconnected the existing receiver once. The trusted sync is being verified."
			: "Automatic recovery could not reconnect this source. Output remains live unchanged; reconnect manually in Setup.");
		if (isVisible())
			update_status();
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
		if (!mcb_is_receiver()) {
			if (mcb_is_sender())
				update_sender_monitor(status, age_ms, rate);
			else {
				set_health_banner("<b>Setup required</b> — choose whether this is the gaming PC or stream PC.",
					"#e6b450", "rgba(105,76,30,90)");
				timing_summary_->setText("Open <b>Setup</b>, choose this PC's role, and apply once.");
				suggestion_->setText("<b>Suggested:</b> Configure this PC before starting NDI output.");
				monitor_numbers_->setText("No active bridge role.");
			}
			return;
		}

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
			QString("<b>%1</b> · split audio %2 · packet age %3 ms · missing desktop/mic %4/%5")
				.arg(channel_state)
				.arg(router.outputs_active() ? "active" : "not active")
				.arg(receiver_age, 0, 'f', 1)
				.arg(static_cast<qulonglong>(router.missing_program()))
				.arg(static_cast<qulonglong>(router.missing_mic())));
		program_meter_->setValue(meter_value(router.peak(0)));
		mic_meter_->setValue(meter_value(router.peak(1)));

		const auto governor = router.governor_snapshot();
		const double deviation = static_cast<double>(governor.baseline_deviation_ns) / 1e6;
		const QString direction = std::fabs(deviation) < 2.0
			? "aligned"
			: deviation > 0.0 ? "audio ahead / video dragging" : "video ahead / audio dragging";
		const QString drift = governor.drift_confidence >= 75
			? QString("%1 ppm verified").arg(static_cast<qlonglong>(governor.drift_ppm))
			: "still measuring";
		governor_status_->setText(QString(
			"<b>%1</b> · %2 by %3 ms<br>Trusted reference %4 ms · drift %5 · video correction %6 ms<br>"
			"Recoveries %7 · failed %8 · quarantined samples %9")
			.arg(QString::fromUtf8(governor_phase_name(governor.phase))).arg(direction)
			.arg(std::fabs(deviation), 0, 'f', 1)
			.arg(static_cast<double>(governor.baseline_skew_ns) / 1e6, 0, 'f', 1)
			.arg(drift).arg(static_cast<double>(governor.video_correction_ns) / 1e6, 0, 'f', 2)
			.arg(static_cast<qulonglong>(governor.atomic_recoveries))
			.arg(static_cast<qulonglong>(governor.failed_recoveries))
			.arg(static_cast<qulonglong>(governor.quarantined_samples)));
		governor_status_->setStyleSheet(governor.fail_safe_bypassed
			? "QLabel { color: #e05d5d; }"
			: governor.phase == mcb::AVGovernorPhase::Locked
				? "QLabel { color: #57c785; }"
				: "QLabel { color: #5aa9e6; }");
		update_receiver_monitor(router, governor, receiver_age);
		if (configuration_panel_->isVisible())
			refresh_source_context();

	}

	QRadioButton *sender_radio_ = nullptr;
	QRadioButton *receiver_radio_ = nullptr;
	QToolButton *setup_toggle_ = nullptr;
	QToolButton *numbers_toggle_ = nullptr;
	QWidget *configuration_panel_ = nullptr;
	QWidget *monitor_panel_ = nullptr;
	QWidget *monitor_numbers_panel_ = nullptr;
	QLabel *health_banner_ = nullptr;
	QLabel *timing_summary_ = nullptr;
	QLabel *suggestion_ = nullptr;
	QLabel *monitor_numbers_ = nullptr;
	QLabel *monitor_program_label_ = nullptr;
	QLabel *monitor_mic_label_ = nullptr;
	QProgressBar *monitor_program_meter_ = nullptr;
	QProgressBar *monitor_mic_meter_ = nullptr;
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
	QPushButton *add_receiver_here_ = nullptr;
	QLabel *source_context_ = nullptr;
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
	QTimer *safety_timer_ = nullptr;
	uint64_t last_restart_ns_ = 0;
	uint32_t auto_reconnect_attempts_ = 0;
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
