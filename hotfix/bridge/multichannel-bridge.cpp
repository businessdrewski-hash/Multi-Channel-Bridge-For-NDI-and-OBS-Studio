#include "multichannel-bridge.h"
#include "downstream-sync-core.h"
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
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr const char *kSection = "NDIMultichannelBridge";
constexpr const char *kDockId = "distroav_multichannel_bridge_v030";
constexpr const char *kProgramSourceId = "ndi_multichannel_bridge_program_audio";
constexpr const char *kMicSourceId = "ndi_multichannel_bridge_mic_audio";
constexpr const char *kVideoProbeFilterId = "ndi_multichannel_bridge_downstream_video_probe";
constexpr const char *kAudioClockFilterId = "ndi_multichannel_bridge_linked_audio_clock";
constexpr const char *kVideoProbeFilterName = "[MCB] Downstream Video Clock";
constexpr const char *kAudioClockFilterName = "[MCB] Linked Audio Clock";
constexpr const char *kVersion = "0.6.0-alpha5";
constexpr const char *kGovernorVersion = "2.0";
constexpr const char *kSenderCoreVersion = "2.0";
constexpr const char *kDefaultProgramName = "MCB Desktop / Game";
constexpr const char *kDefaultMicName = "MCB Microphone";

const char *sync_phase_name(mcb::DownstreamSyncPhase phase)
{
	switch (phase) {
	case mcb::DownstreamSyncPhase::Bypassed: return "BYPASSED";
	case mcb::DownstreamSyncPhase::Learning: return "LEARNING";
	case mcb::DownstreamSyncPhase::Locked: return "LOCKED";
	case mcb::DownstreamSyncPhase::Verifying: return "VERIFYING";
	case mcb::DownstreamSyncPhase::Failed: return "NEEDS ATTENTION";
	}
	return "UNKNOWN";
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
	config_set_default_bool(config, kSection, "GovernorDriftCorrection", true);
	config_set_default_int(config, kSection, "GovernorCorrectionSlewPpm", 100);
	config_set_default_int(config, kSection, "GovernorMaxAudioCorrectionPpm", 1000);
	config_set_default_int(config, kSection, "GovernorBaselineWindowMs", 5000);
	config_set_default_int(config, kSection, "GovernorDriftWindowMs", 120000);
	config_set_default_int(config, kSection, "GovernorDriftMinimumMs", 30000);
	config_set_default_int(config, kSection, "GovernorCorrectionDeadZoneMs", 4);
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

obs_source_t *install_private_filter(obs_source_t *parent, const char *id,
	const char *name, obs_data_t *settings = nullptr);
void remove_private_filter(obs_source_t *parent, obs_source_t *&filter);
size_t remove_private_filters_by_id(obs_source_t *parent, const char *id);

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
		const size_t index = static_cast<size_t>(pair);
		// Alpha 5 applies one correction to the four-channel packet before either
		// proxy enters OBS. Remove any serialized alpha 1-4 clock filters; they
		// must never mutate blocks in the OBS mixer path again.
		remove_private_filters_by_id(source, kAudioClockFilterId);
		std::lock_guard<std::mutex> lock(mutex_);
		proxies_[index] = source;
	}

	void unregister_proxy(int pair, obs_source_t *source)
	{
		if (pair < 0 || pair > 1)
			return;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const size_t index = static_cast<size_t>(pair);
			auto &slot = proxies_[index];
			if (slot == source)
				slot = nullptr;
		}
		remove_private_filters_by_id(source, kAudioClockFilterId);
	}

	void reconcile_audio_clock_filters()
	{
		std::array<obs_source_t *, 2> sources{};
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (size_t pair = 0; pair < sources.size(); ++pair) {
				if (proxies_[pair])
					sources[pair] = obs_source_get_ref(proxies_[pair]);
			}
		}

		for (size_t pair = 0; pair < sources.size(); ++pair) {
			if (!sources[pair])
				continue;
			remove_private_filters_by_id(sources[pair], kAudioClockFilterId);
			obs_source_release(sources[pair]);
		}
		obs_log(LOG_INFO,
			"[multichannel-bridge] Removed legacy linked audio clock filters after OBS source loading");
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
		video_probe_filter_ = install_private_filter(candidate, kVideoProbeFilterId,
			kVideoProbeFilterName);
		reset_stats();
		refresh_source_configuration();
		attached_.store(true, std::memory_order_release);
		obs_log(LOG_INFO, "[multichannel-bridge] Receiver attached to raw DistroAV source '%s'", name.c_str());
		return true;
	}

	void detach()
	{
		obs_source_t *old = nullptr;
		obs_source_t *probe = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			old = input_;
			probe = video_probe_filter_;
			video_probe_filter_ = nullptr;
			input_ = nullptr;
			input_name_.clear();
		}
		remove_private_filter(old, probe);
		if (old)
			obs_source_release(old);
		attached_.store(false, std::memory_order_release);
		hard_reset_sync(false);
	}

	void set_suppress_original(bool suppress)
	{
		suppress_original_.store(suppress, std::memory_order_release);
	}

	void configure_governor(bool enabled, bool drift_correction, int max_audio_correction_ppm,
		int correction_slew_ppm, int baseline_window_ms, int drift_window_ms,
		int drift_minimum_ms, int correction_dead_zone_ms)
	{
		sync_core_.configure(enabled && drift_correction, max_audio_correction_ppm,
			correction_slew_ppm, correction_dead_zone_ms, baseline_window_ms,
			drift_window_ms, drift_minimum_ms);
		request_audio_epoch_reset();
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
		obs_log(LOG_INFO, "[multichannel-bridge] Applied recommended source timing settings");
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
			// Personal-build contract: every complete NDI receiver restart is a
			// known-good sync point. Wipe baseline, drift history, PPM command,
			// accumulated frames, and corrected packet timestamps together.
			hard_reset_sync(false);
			obs_log(LOG_INFO,
				"[multichannel-bridge] Requested an in-place DistroAV receiver reconnect and wiped all learned drift state");
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
			hard_reset_sync(false);
			return false;
		}
		obs_data_t *settings = obs_source_get_settings(source);
		const bool configured = settings && !obs_data_get_bool(settings, "ndi_framesync") &&
					obs_data_get_int(settings, "ndi_sync") == 2 && obs_data_get_bool(settings, "ndi_audio");
		if (settings)
			obs_data_release(settings);
		obs_source_release(source);
		if (!configured)
			hard_reset_sync(false);
		return configured;
	}

	bool route(obs_source_t *origin, const obs_source_audio *audio, int channel_count,
		int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns)
	{
		if (!origin || !audio || audio->frames == 0)
			return false;
		if (audio_route_busy_.test_and_set(std::memory_order_acquire))
			return false;
		struct BusyGuard {
			std::atomic_flag &flag;
			~BusyGuard() { flag.clear(std::memory_order_release); }
		} busy_guard{audio_route_busy_};

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

		(void)ndi_timestamp_100ns;
		(void)ndi_timecode_100ns;
		auto &core = sync_core_;
		core.observe_audio_input(audio->timestamp, now_ns);
		const uint64_t requested_epoch = audio_epoch_generation_.load(std::memory_order_acquire);
		if (observed_audio_epoch_ != requested_epoch || correction_sample_rate_ != audio->samples_per_sec) {
			observed_audio_epoch_ = requested_epoch;
			reset_audio_correction_timeline(audio);
		}

		const bool correction_supported = core.enabled() &&
			audio->format == AUDIO_FORMAT_FLOAT_PLANAR && audio->samples_per_sec != 0 &&
			audio->frames <= mcb::LinkedAudioPacketClock::kMaxInputFrames;
		const auto packet_plan = audio_packet_clock_.plan(audio->frames, audio->timestamp,
			audio->samples_per_sec, core.correction_ppm(), correction_supported);
		const uint32_t output_frames = packet_plan.output_frames;
		const uint64_t output_timestamp = packet_plan.output_timestamp_ns;
		const bool resampled = correction_supported && output_frames != audio->frames;
		if (resampled) {
			for (size_t channel = 0; channel < kCorrectionChannels; ++channel) {
				if (channel < static_cast<size_t>(std::max(channel_count, 0)) && audio->data[channel])
					resample_plane(reinterpret_cast<const float *>(audio->data[channel]),
						audio->frames, correction_planes_[channel].data(), output_frames);
			}
		}

		// Both stereo proxies receive one corrected packet duration and timestamp.
		// OBS sees valid source packets at ingestion; no filter changes a block
		// after the standard mixer and its meters have begun processing it.
		for (int pair = 0; pair < 2; ++pair) {
			const int first = pair * 2;
			const uint8_t *left = first < channel_count && audio->data[first]
				? resampled
					? reinterpret_cast<const uint8_t *>(correction_planes_[static_cast<size_t>(first)].data())
					: audio->data[first]
				: nullptr;
			const uint8_t *right = (first + 1) < channel_count && audio->data[first + 1]
				? resampled
					? reinterpret_cast<const uint8_t *>(correction_planes_[static_cast<size_t>(first + 1)].data())
					: audio->data[first + 1]
				: nullptr;
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
			const float peak = mcb_ui_monitoring_enabled()
				? calculate_peak(left, right, output_frames)
				: 0.0f;
			peaks_[static_cast<size_t>(pair)].store(peak, std::memory_order_relaxed);
			if (outputs[static_cast<size_t>(pair)]) {
				obs_source_audio output{};
				output.data[0] = const_cast<uint8_t *>(left);
				output.data[1] = const_cast<uint8_t *>(right);
				output.frames = output_frames;
				output.samples_per_sec = audio->samples_per_sec;
				output.speakers = SPEAKERS_STEREO;
				output.format = audio->format;
				output.timestamp = output_timestamp;
				obs_source_output_audio(outputs[static_cast<size_t>(pair)], &output);
				obs_source_release(outputs[static_cast<size_t>(pair)]);
			}
		}
		core.report_audio_output(output_timestamp, now_ns,
			packet_plan.net_frame_adjustment, correction_sample_rate_);

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
		(void)ndi_timestamp_100ns;
		(void)ndi_timecode_100ns;
		// Video is the master clock. It passes through unchanged; an OBS async
		// video filter observes its downstream timestamp for the audio controller.
		return true;
	}

	mcb::DownstreamSyncSnapshot sync_snapshot() const { return sync_core_.snapshot(); }
	std::string governor_flight_recorder_csv() const { return sync_core_.diagnostics_csv(); }
	mcb::DownstreamSyncCore &sync_core() { return sync_core_; }
	void controller_tick(uint64_t now_ns) { sync_core_.tick(now_ns); }

	void reset_governor(bool reset_counters) { hard_reset_sync(reset_counters); }

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
	static constexpr size_t kCorrectionChannels = 4;

	void request_audio_epoch_reset()
	{
		audio_epoch_generation_.fetch_add(1, std::memory_order_acq_rel);
	}

	void hard_reset_sync(bool reset_counters)
	{
		sync_core_.reset(reset_counters, false);
		request_audio_epoch_reset();
	}

	void reset_audio_correction_timeline(const obs_source_audio *audio)
	{
		correction_sample_rate_ = audio && audio->samples_per_sec ? audio->samples_per_sec : 48000;
		audio_packet_clock_.reset(audio ? audio->timestamp : 0, correction_sample_rate_);
	}

	static void resample_plane(const float *input, uint32_t input_frames,
		float *output, uint32_t output_frames)
	{
		if (!input || !output || !input_frames || !output_frames)
			return;
		if (input_frames == 1 || output_frames == 1) {
			output[0] = input[0];
			return;
		}
		const double scale = static_cast<double>(input_frames - 1) /
			static_cast<double>(output_frames - 1);
		for (uint32_t frame = 0; frame < output_frames; ++frame) {
			const double position = static_cast<double>(frame) * scale;
			const uint32_t left = static_cast<uint32_t>(position);
			const uint32_t right = std::min(left + 1, input_frames - 1);
			const float fraction = static_cast<float>(position - static_cast<double>(left));
			output[frame] = input[left] + (input[right] - input[left]) * fraction;
		}
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
	obs_source_t *video_probe_filter_ = nullptr;
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
	std::atomic_flag audio_route_busy_ = ATOMIC_FLAG_INIT;
	std::atomic_uint64_t audio_epoch_generation_{1};
	uint64_t observed_audio_epoch_ = 0;
	uint32_t correction_sample_rate_ = 48000;
	mcb::LinkedAudioPacketClock audio_packet_clock_;
	std::array<std::array<float, mcb::LinkedAudioPacketClock::kMaxOutputFrames>,
		kCorrectionChannels> correction_planes_{};
	mcb::DownstreamSyncCore sync_core_;
};

const char *video_probe_display_name(void *) { return "Multichannel Bridge Downstream Video Probe"; }
const char *audio_clock_display_name(void *) { return "Multichannel Bridge Legacy Audio Clock (inactive)"; }

void *video_probe_create(obs_data_t *, obs_source_t *) { return reinterpret_cast<void *>(1); }
void video_probe_destroy(void *) {}

obs_source_frame *video_probe_filter(void *, obs_source_frame *frame)
{
	if (frame && frame->timestamp)
		ReceiverRouter::instance().sync_core().observe_video(frame->timestamp, os_gettime_ns());
	return frame;
}

// Keep the old source type registered so OBS can deserialize alpha 1-4 scene
// collections safely. It is a strict pass-through and is removed from both MCB
// proxies after source loading; all alpha 5 correction happens before source
// ingestion in ReceiverRouter::route().
void *linked_audio_clock_create(obs_data_t *, obs_source_t *) { return reinterpret_cast<void *>(1); }
void linked_audio_clock_destroy(void *) {}
obs_audio_data *linked_audio_clock_filter(void *, obs_audio_data *audio)
{
	return audio;
}

obs_source_t *install_private_filter(obs_source_t *parent, const char *id,
	const char *name, obs_data_t *settings)
{
	if (!parent || !id || !name)
		return nullptr;
	remove_private_filters_by_id(parent, id);
	obs_source_t *filter = obs_source_create_private(id, name, settings);
	if (filter)
		obs_source_filter_add(parent, filter);
	return filter;
}

struct PrivateFilterCollector {
	const char *id = nullptr;
	std::vector<obs_source_t *> matches;
};

void collect_private_filter(obs_source_t *, obs_source_t *child, void *param)
{
	auto *collector = static_cast<PrivateFilterCollector *>(param);
	if (!collector || !child || !collector->id)
		return;
	const char *child_id = obs_source_get_unversioned_id(child);
	if (!child_id)
		child_id = obs_source_get_id(child);
	if (child_id && std::strcmp(child_id, collector->id) == 0)
		collector->matches.push_back(obs_source_get_ref(child));
}

size_t remove_private_filters_by_id(obs_source_t *parent, const char *id)
{
	if (!parent || !id)
		return 0;
	PrivateFilterCollector collector{id, {}};
	obs_source_enum_filters(parent, collect_private_filter, &collector);
	for (obs_source_t *filter : collector.matches) {
		obs_source_filter_remove(parent, filter);
		obs_source_release(filter);
	}
	return collector.matches.size();
}

void remove_private_filter(obs_source_t *parent, obs_source_t *&filter)
{
	if (!filter)
		return;
	if (parent)
		obs_source_filter_remove(parent, filter);
	obs_source_release(filter);
	filter = nullptr;
}

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
		restart_ndi_ = new QPushButton("RESTART NDI", monitor_panel_);
		restart_ndi_->setToolTip(
			"Reconnect the selected DistroAV receiver, discard the old timing reference, and learn a fresh baseline. Audio/video will cut briefly.");
		restart_ndi_->setVisible(false);
		monitor_layout->addWidget(health_banner_);
		monitor_layout->addWidget(timing_summary_);
		auto *monitor_action_row = new QHBoxLayout;
		monitor_action_row->setContentsMargins(0, 0, 0, 0);
		monitor_action_row->setSpacing(6);
		monitor_action_row->addWidget(suggestion_, 1);
		monitor_action_row->addWidget(restart_ndi_);
		monitor_layout->addLayout(monitor_action_row);

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
			QString("Automatic audio drift correction (Sync Core %1)").arg(kGovernorVersion), receiver_box_);
		governor_box_->setCheckable(true);
		auto *governor_form = new QFormLayout(governor_box_);
		auto_configure_ = new QCheckBox(
			"Automatically disable NDI Frame Sync and use Source Timecode (recommended)", governor_box_);
		drift_correction_ = new QCheckBox(
			"Keep both audio tracks aligned to video automatically (recommended)", governor_box_);
		max_audio_correction_ppm_ = new QSpinBox(governor_box_);
		max_audio_correction_ppm_->setRange(25, 5000);
		max_audio_correction_ppm_->setSuffix(" ppm");
		max_audio_correction_ppm_->setToolTip(
			"Maximum linked audio-rate correction. 1000 ppm is 0.1% and is used only to catch an existing offset; steady correction is normally much smaller.");
		correction_slew_ppm_ = new QSpinBox(governor_box_);
		correction_slew_ppm_->setRange(1, 1000);
		correction_slew_ppm_->setSuffix(" ppm/sec");
		correction_slew_ppm_->setToolTip(
			"How quickly the linked desktop and microphone correction can change. Braking toward neutral is allowed at twice this rate.");
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
			"A downstream drift direction must persist for at least this long before audio is resampled.");
		correction_dead_zone_ms_ = new QSpinBox(governor_box_);
		correction_dead_zone_ms_->setRange(1, 50);
		correction_dead_zone_ms_->setSuffix(" ms");
		correction_dead_zone_ms_->setToolTip(
			"Existing corrected error inside this range is left alone so normal callback jitter cannot cause hunting.");
		governor_help_ = new QLabel(
			"Measures the actual OBS-facing video and audio timelines, keeps video untouched, and applies one linked rate correction to desktop/game and microphone.", governor_box_);
		governor_help_->setWordWrap(true);
		governor_status_ = new QLabel(governor_box_);
		governor_status_->setWordWrap(true);
		recommended_governor_ = new QPushButton("Restore recommended settings", governor_box_);
		recommended_governor_->setToolTip("Restore conservative settings suitable for a single two-PC NDI source.");
		advanced_governor_ = new QCheckBox("Show expert timing controls", governor_box_);
		advanced_governor_panel_ = new QWidget(governor_box_);
		auto *advanced_form = new QFormLayout(advanced_governor_panel_);
		advanced_form->setContentsMargins(0, 0, 0, 0);
		advanced_form->addRow("Maximum audio correction:", max_audio_correction_ppm_);
		advanced_form->addRow("Correction slew:", correction_slew_ppm_);
		advanced_form->addRow("Baseline learning time:", baseline_window_ms_);
		advanced_form->addRow("Drift analysis window:", drift_window_ms_);
		advanced_form->addRow("Minimum confirmed drift time:", drift_minimum_ms_);
		advanced_form->addRow("Correction dead zone:", correction_dead_zone_ms_);
		advanced_governor_panel_->setVisible(false);
		governor_form->addRow(auto_configure_);
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
		copy_flight_recorder_ = new QPushButton("Copy downstream sync CSV", body);
		copy_flight_recorder_->setToolTip("Copies a bounded CSV timeline including raw NDI timestamp/timecode and OBS output timing.");
		export_diagnostics_ = new QPushButton("Export diagnostics", body);
		export_diagnostics_->setToolTip("Writes a timestamped diagnostics folder containing status and downstream sync data.");
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
				? "Existing DistroAV source restarted; the old timing reference was discarded and a fresh baseline is being learned."
				: "Reconnect was unavailable. Confirm this is a DistroAV NDI Source created by the bridge build.");
		});
		connect(restart_ndi_, &QPushButton::clicked, this, [this] { restart_ndi_receiver(); });
		connect(advanced_governor_, &QCheckBox::toggled, this, [this](bool visible) {
			advanced_governor_panel_->setVisible(visible);
		});
		connect(recommended_governor_, &QPushButton::clicked, this, [this] {
			governor_box_->setChecked(true);
			auto_configure_->setChecked(true);
			drift_correction_->setChecked(true);
			max_audio_correction_ppm_->setValue(1000);
			correction_slew_ppm_->setValue(100);
			baseline_window_ms_->setValue(5000);
			drift_window_ms_->setValue(120000);
			drift_minimum_ms_->setValue(30000);
			correction_dead_zone_ms_->setValue(4);
			checklist_->setText("Recommended downstream audio-sync settings restored. Click Apply role and settings.");
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
			checklist_->setText("Downstream sync summary copied as CSV.");
		});
		connect(export_diagnostics_, &QPushButton::clicked, this, [this] { export_diagnostics(); });

		timer_ = new QTimer(this);
		timer_->setInterval(1000);
		connect(timer_, &QTimer::timeout, this, [this] { update_status(); });
		safety_timer_ = new QTimer(this);
		safety_timer_->setInterval(2000);
		connect(safety_timer_, &QTimer::timeout, this, [this] { automatic_recovery_tick(); });
		safety_timer_->start();
		sync_timer_ = new QTimer(this);
		sync_timer_->setInterval(250);
		connect(sync_timer_, &QTimer::timeout, this, [] {
			ReceiverRouter::instance().controller_tick(os_gettime_ns());
		});
		sync_timer_->start();
	}

	void frontend_finished_loading()
	{
		set_role_cache(read_role_from_config());
		refresh_sources();
		if (mcb_is_receiver()) {
			apply_receiver(false);
		}
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

	void update_receiver_monitor(const ReceiverRouter &router, const mcb::DownstreamSyncSnapshot &sync,
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
		restart_ndi_->setEnabled(router.attached());
		const int64_t raw_relation_ns = sync.baseline_valid
			? sync.baseline_ns + sync.raw_deviation_ns : sync.relation_ns;
		const double raw_relation_ms = static_cast<double>(raw_relation_ns) / 1e6;
		const double raw_deviation_ms = static_cast<double>(sync.raw_deviation_ns) / 1e6;
		const double deviation_ms = static_cast<double>(sync.corrected_deviation_ns) / 1e6;
		const double absolute_deviation = std::fabs(deviation_ms);
		QString direction;
		if (!sync.baseline_valid) {
			direction = "Learning the OBS-facing relationship between video and the linked audio tracks.";
		} else if (absolute_deviation < 2.0) {
			direction = "<span style='color:#57c785'><b>Timelines aligned</b></span> — no track is meaningfully rushing or dragging.";
		} else if (deviation_ms > 0.0) {
			direction = QString(
				"<span style='color:#e6b450'><b>Video rushing</b></span> by %1 ms · desktop + mic dragging.")
				.arg(absolute_deviation, 0, 'f', 1);
		} else {
			direction = QString(
				"<span style='color:#e6b450'><b>Desktop + mic rushing</b></span> by %1 ms · video dragging.")
				.arg(absolute_deviation, 0, 'f', 1);
		}
		QString correction_summary;
		if (!sync.baseline_valid) {
			correction_summary = "<b>Audio correction:</b> waiting for a fresh trusted baseline.";
		} else {
			const QString raw_motion = std::fabs(raw_deviation_ms) < 2.0
				? "Audio offset"
				: raw_deviation_ms > 0.0 ? "Audio dragging" : "Audio rushing";
			const QString raw_signed = QString("%1%2")
				.arg(raw_deviation_ms >= 0.0 ? "+" : "")
				.arg(raw_deviation_ms, 0, 'f', 1);
			const QString corrected_signed = QString("%1%2")
				.arg(deviation_ms >= 0.0 ? "+" : "")
				.arg(deviation_ms, 0, 'f', 1);
			correction_summary = QString(
				"<b>%1 by %2 ms</b> · applying %3 ppm · corrected to %4 ms")
				.arg(raw_motion, raw_signed)
				.arg(sync.correction_ppm, 0, 'f', 1)
				.arg(corrected_signed);
		}
		timing_summary_->setText(direction + "<br>" + correction_summary);

		if (duplicate_count > 1) {
			set_health_banner(QString("<b>Setup conflict</b> — %1 separate NDI receivers use the same sender.").arg(duplicate_count),
				"#e6b450", "rgba(105,76,30,90)");
			suggestion_->setText("<b>Suggested:</b> Keep one receiver and add it to other scenes as an existing source.");
		} else if (!connection_ready) {
			set_health_banner("<b>Receiver waiting</b> — the complete four-channel feed is not active.",
				"#e05d5d", "rgba(110,45,45,90)");
			suggestion_->setText("<b>Suggested:</b> Open Setup, select the canonical receiver, then reconnect or repair outputs.");
		} else if (!sync.enabled) {
			set_health_banner("<b>Monitoring only</b> — split audio is healthy; automatic timing protection is off.",
				"#b8bec9", "rgba(70,74,82,90)");
			suggestion_->setText("<b>Suggested:</b> Enable linked audio correction in Setup for long recordings.");
		} else if (sync.phase == mcb::DownstreamSyncPhase::Failed) {
			set_health_banner("<b>Needs attention</b> — trusted sync could not be restored; correction is bypassed.",
				"#e05d5d", "rgba(110,45,45,90)");
			suggestion_->setText("<b>Suggested:</b> Reconnect the existing receiver. Output is being kept live unchanged.");
		} else if (sync.phase == mcb::DownstreamSyncPhase::Verifying ||
			sync.phase == mcb::DownstreamSyncPhase::Learning) {
			set_health_banner("<b>Learning downstream sync</b> — watching the timelines OBS actually receives.",
				"#5aa9e6", "rgba(42,76,110,90)");
			suggestion_->setText("<b>Suggested:</b> Leave it running; correction begins only after stable evidence.");
		} else if (sync.correction_limited) {
			set_health_banner("<b>Drift exceeds the safe correction range</b> — protection will not chase it further.",
				"#e6b450", "rgba(105,76,30,90)");
			suggestion_->setText("<b>Suggested:</b> Check the audio device clock or reconnect the canonical receiver.");
		} else if (sync.correction_active) {
			set_health_banner("<b>Correcting slow drift</b> — both audio tracks are moving gently toward video.",
				"#5aa9e6", "rgba(42,76,110,90)");
			suggestion_->setText("<b>Suggested:</b> No action needed.");
		} else {
			set_health_banner("<b>Protected</b> — four channels are healthy and trusted sync is locked.",
				"#57c785", "rgba(40,95,68,90)");
			suggestion_->setText("<b>Suggested:</b> No action needed.");
		}

		const double drift_seconds = static_cast<double>(sync.drift_samples) * 0.25;
		const QString drift_text = sync.confidence >= 75
			? QString("%1 ppm · verified").arg(static_cast<qlonglong>(sync.drift_ppm))
			: QString("measuring · about %1 s collected").arg(drift_seconds, 0, 'f', 0);
		monitor_numbers_->setText(QString(
			"<b>Downstream raw A/V:</b> %1 ms &nbsp;·&nbsp; <b>Corrected change:</b> %2 ms<br>"
			"<b>Trusted reference:</b> %3 ms &nbsp;·&nbsp; <b>Native drift:</b> %4<br>"
			"<b>Linked audio correction:</b> %5 ppm &nbsp;·&nbsp; <b>Packet age:</b> %6 ms<br>"
			"Adjusted frames %7 · missing desktop/mic %8/%9")
			.arg(raw_relation_ms, 0, 'f', 2).arg(deviation_ms, 0, 'f', 2)
			.arg(static_cast<double>(sync.baseline_ns) / 1e6, 0, 'f', 2)
			.arg(drift_text).arg(sync.correction_ppm, 0, 'f', 1).arg(receiver_age_ms, 0, 'f', 1)
			.arg(static_cast<qlonglong>(sync.net_frame_adjustment))
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

	void restart_ndi_receiver()
	{
		if (!mcb_is_receiver())
			return;
		const uint64_t now_ns = os_gettime_ns();
		if (last_receiver_restart_ns_ && now_ns >= last_receiver_restart_ns_ &&
			now_ns - last_receiver_restart_ns_ < 2000000000ULL) {
			checklist_->setText("RESTART NDI is cooling down. Wait two seconds before trying again.");
			return;
		}
		last_receiver_restart_ns_ = now_ns;
		restart_ndi_->setEnabled(false);
		const bool restarted = ReceiverRouter::instance().force_reconnect();
		if (restarted)
			auto_reconnect_attempts_ = 0;
		restart_ndi_->setEnabled(ReceiverRouter::instance().attached());
		checklist_->setText(restarted
			? "NDI receiver restarted. The old baseline was discarded; audio/video may cut briefly while a fresh baseline is learned."
			: "NDI restart was unavailable. Select a bridge-patched DistroAV receiver in Setup.");
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
		drift_correction_->setChecked(
			config ? config_get_bool(config, kSection, "GovernorDriftCorrection") : true);
		max_audio_correction_ppm_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorMaxAudioCorrectionPpm")) : 1000);
		correction_slew_ppm_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorCorrectionSlewPpm")) : 100);
		baseline_window_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorBaselineWindowMs")) : 5000);
		drift_window_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftWindowMs")) : 120000);
		drift_minimum_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorDriftMinimumMs")) : 30000);
		correction_dead_zone_ms_->setValue(
			config ? static_cast<int>(config_get_int(config, kSection, "GovernorCorrectionDeadZoneMs")) : 4);
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
				"automatic audio drift correction on, then create the two split mixer sources. Recommended timing settings are applied automatically.");
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
		config_set_bool(config, kSection, "GovernorDriftCorrection", drift_correction_->isChecked());
		config_set_int(config, kSection, "GovernorMaxAudioCorrectionPpm", max_audio_correction_ppm_->value());
		config_set_int(config, kSection, "GovernorCorrectionSlewPpm", correction_slew_ppm_->value());
		config_set_int(config, kSection, "GovernorBaselineWindowMs", baseline_window_ms_->value());
		config_set_int(config, kSection, "GovernorDriftWindowMs", drift_window_ms_->value());
		config_set_int(config, kSection, "GovernorDriftMinimumMs", drift_minimum_ms_->value());
		config_set_int(config, kSection, "GovernorCorrectionDeadZoneMs", correction_dead_zone_ms_->value());
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
		// OBS restores serialized filters after each proxy source's create
		// callback. Reconcile by filter type after loading or manual Apply so a
		// saved copy and the callback-created copy cannot stack across restarts.
		ReceiverRouter::instance().reconcile_audio_clock_filters();
		const std::string source_name = receiver_source_->currentText().toUtf8().constData();
		if (!ReceiverRouter::instance().attach(source_name)) {
			checklist_->setText(QString("Receiver could not attach: %1")
						   .arg(QString::fromUtf8(ReceiverRouter::instance().last_error().c_str())));
			return;
		}
		auto_reconnect_attempts_ = 0;
		ReceiverRouter::instance().set_suppress_original(suppress_original_->isChecked());
		ReceiverRouter::instance().configure_governor(
			governor_box_->isChecked(), drift_correction_->isChecked(), max_audio_correction_ppm_->value(),
			correction_slew_ppm_->value(), baseline_window_ms_->value(), drift_window_ms_->value(),
			drift_minimum_ms_->value(), correction_dead_zone_ms_->value());
		if (governor_box_->isChecked() && auto_configure_->isChecked())
			ReceiverRouter::instance().apply_recommended_source_settings();
		else
			ReceiverRouter::instance().refresh_source_configuration();
		// Applying any receiver-side bridge setting is an explicit new timing
		// epoch. Rebuild the NDI receiver once after all settings are committed and
		// discard the previous baseline instead of comparing a reset video clock
		// against an older trusted relationship.
		const bool receiver_restarted = ReceiverRouter::instance().force_reconnect();
		if (create_sources) {
			const bool program_ok = add_proxy_to_current_scene(
				kProgramSourceId, program_name_->text().toUtf8().constData(), true);
			const bool mic_ok = add_proxy_to_current_scene(
				kMicSourceId, mic_name_->text().toUtf8().constData(), true);
			QString result;
			if (!program_ok || !mic_ok)
				result = "Receiver attached, but one or both split sources could not be added. Check for duplicate names.";
			else if (receiver_restarted)
				result = "Receiver settings applied, NDI restarted, and both split audio sources are present. A fresh baseline is being learned.";
			else
				result = "Receiver attached and both split audio sources are present, but the NDI restart procedure was unavailable.";
			checklist_->setText(result);
		} else {
			checklist_->setText(receiver_restarted
				? "Receiver settings applied and NDI restarted. The old baseline was discarded; a fresh baseline is being learned."
				: "Receiver attached. NDI restart was unavailable; use Create / repair if the two mixer sources are missing.");
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
			"This bundle contains OBS-side downstream A/V status.\n"
			"The CSV summarizes the trusted reference, native audio drift, linked audio correction,\n"
			"and observations made after the canonical source entered OBS.\n");
		const bool ok = write_text("bridge-status.txt", status) &&
			write_text("downstream-sync.csv", recorder) &&
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
		const auto sync = router.sync_snapshot();
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

		const double deviation = static_cast<double>(sync.corrected_deviation_ns) / 1e6;
		const QString direction = std::fabs(deviation) < 2.0
			? "aligned"
			: deviation > 0.0 ? "video ahead; linked audio dragging" : "linked audio ahead; video dragging";
		text += QString(
			"Receiver: %1\nCanonical source: %2\nReceivers using same NDI sender: %3\n"
			"Split outputs: %4\nChannels: %5\nPackets / suppressed original: %6 / %7\n"
			"Packet age: %8 ms\nMissing desktop / mic: %9 / %10\n"
			"Downstream Sync Core %11: %12\nState: %13\nMeasurement fresh: %14\n"
			"Timing direction: %15 by %16 ms\nCorrected relation / trusted reference: %17 / %18 ms\n"
			"Raw / corrected change: %19 / %20 ms\nNative audio error: %21 ppm (%22 percent confidence, %23 samples)\n"
			"Linked audio correction / target: %24 / %25 ppm\nCorrection limited: %26\n"
			"Adjusted frames / corrected blocks: %27 / %28\n"
			"Video / audio observations: %29 / %30\nDiscontinuities / quarantined: %31 / %32")
			.arg(router.attached() ? "attached" : "not attached")
			.arg(QString::fromUtf8(router.input_name().c_str()))
			.arg(static_cast<qulonglong>(matching_receiver_count(router.input_name())))
			.arg(router.outputs_active() ? "active" : "inactive").arg(router.channels())
			.arg(static_cast<qulonglong>(router.packets())).arg(static_cast<qulonglong>(router.suppressed()))
			.arg(receiver_age, 0, 'f', 1).arg(static_cast<qulonglong>(router.missing_program()))
			.arg(static_cast<qulonglong>(router.missing_mic())).arg(kGovernorVersion)
			.arg(sync.enabled ? "enabled" : "disabled").arg(sync_phase_name(sync.phase))
			.arg(sync.measurement_fresh ? "yes" : "no")
			.arg(direction).arg(std::fabs(deviation), 0, 'f', 2)
			.arg(static_cast<double>(sync.relation_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(sync.baseline_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(sync.raw_deviation_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<double>(sync.corrected_deviation_ns) / 1e6, 0, 'f', 3)
			.arg(static_cast<qlonglong>(sync.native_audio_error_ppm)).arg(sync.confidence)
			.arg(sync.drift_samples).arg(sync.correction_ppm, 0, 'f', 2)
			.arg(sync.target_ppm, 0, 'f', 2).arg(sync.correction_limited ? "yes" : "no")
			.arg(static_cast<qlonglong>(sync.net_frame_adjustment))
			.arg(static_cast<qulonglong>(sync.corrected_blocks))
			.arg(static_cast<qulonglong>(sync.video_observations))
			.arg(static_cast<qulonglong>(sync.audio_observations))
			.arg(static_cast<qulonglong>(sync.discontinuities))
			.arg(static_cast<qulonglong>(sync.quarantined_samples));
		return text;
	}

	void automatic_recovery_tick()
	{
		if (!mcb_is_receiver() || auto_reconnect_attempts_ != 0)
			return;
		auto &router = ReceiverRouter::instance();
		const auto sync = router.sync_snapshot();
		if (sync.phase != mcb::DownstreamSyncPhase::Failed)
			return;
		++auto_reconnect_attempts_;
		const bool reconnected = router.force_reconnect();
		checklist_->setText(reconnected
			? "Automatic recovery restarted the receiver once and wiped the old timing epoch. A fresh baseline is being learned."
			: "Automatic recovery could not reconnect this source. Output remains live unchanged; reconnect manually in Setup.");
		if (isVisible())
			update_status();
	}

	void update_status()
	{
		restart_ndi_->setVisible(mcb_is_receiver());
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

		const auto sync = router.sync_snapshot();
		const double deviation = static_cast<double>(sync.corrected_deviation_ns) / 1e6;
		const QString direction = std::fabs(deviation) < 2.0
			? "aligned"
			: deviation > 0.0 ? "video ahead / audio dragging" : "audio ahead / video dragging";
		const QString drift = sync.confidence >= 75
			? QString("%1 ppm verified").arg(static_cast<qlonglong>(sync.native_audio_error_ppm))
			: "still measuring";
		governor_status_->setText(QString(
			"<b>%1</b> · %2 by %3 ms<br>Trusted reference %4 ms · native drift %5 · linked audio %6 ppm<br>"
			"Adjusted frames %7 · discontinuities %8 · quarantined %9")
			.arg(QString::fromUtf8(sync_phase_name(sync.phase))).arg(direction)
			.arg(std::fabs(deviation), 0, 'f', 1)
			.arg(static_cast<double>(sync.baseline_ns) / 1e6, 0, 'f', 1)
			.arg(drift).arg(sync.correction_ppm, 0, 'f', 2)
			.arg(static_cast<qlonglong>(sync.net_frame_adjustment))
			.arg(static_cast<qulonglong>(sync.discontinuities))
			.arg(static_cast<qulonglong>(sync.quarantined_samples)));
		governor_status_->setStyleSheet(sync.phase == mcb::DownstreamSyncPhase::Failed
			? "QLabel { color: #e05d5d; }"
			: sync.phase == mcb::DownstreamSyncPhase::Locked
				? "QLabel { color: #57c785; }"
				: "QLabel { color: #5aa9e6; }");
		update_receiver_monitor(router, sync, receiver_age);
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
	QPushButton *restart_ndi_ = nullptr;
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
	QCheckBox *drift_correction_ = nullptr;
	QSpinBox *max_audio_correction_ppm_ = nullptr;
	QSpinBox *correction_slew_ppm_ = nullptr;
	QSpinBox *baseline_window_ms_ = nullptr;
	QSpinBox *drift_window_ms_ = nullptr;
	QSpinBox *drift_minimum_ms_ = nullptr;
	QSpinBox *correction_dead_zone_ms_ = nullptr;
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
	QTimer *sync_timer_ = nullptr;
	uint64_t last_restart_ns_ = 0;
	uint64_t last_receiver_restart_ns_ = 0;
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
	obs_source_info video_probe_info{};
	video_probe_info.id = kVideoProbeFilterId;
	video_probe_info.type = OBS_SOURCE_TYPE_FILTER;
	video_probe_info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	video_probe_info.get_name = video_probe_display_name;
	video_probe_info.create = video_probe_create;
	video_probe_info.destroy = video_probe_destroy;
	video_probe_info.filter_video = video_probe_filter;
	obs_register_source(&video_probe_info);

	obs_source_info audio_clock_info{};
	audio_clock_info.id = kAudioClockFilterId;
	audio_clock_info.type = OBS_SOURCE_TYPE_FILTER;
	audio_clock_info.output_flags = OBS_SOURCE_AUDIO;
	audio_clock_info.get_name = audio_clock_display_name;
	audio_clock_info.create = linked_audio_clock_create;
	audio_clock_info.destroy = linked_audio_clock_destroy;
	audio_clock_info.filter_audio = linked_audio_clock_filter;
	obs_register_source(&audio_clock_info);

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
