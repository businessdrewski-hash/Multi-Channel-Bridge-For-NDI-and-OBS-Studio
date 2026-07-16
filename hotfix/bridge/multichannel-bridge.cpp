#include "multichannel-bridge.h"

#include "main-output.h"
#include "plugin-main.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
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
constexpr const char *kVersion = "0.3.1-alpha";
constexpr const char *kDefaultProgramName = "MCB Desktop / Game";
constexpr const char *kDefaultMicName = "MCB Microphone";

std::atomic_int g_role_cache{-1};
std::atomic_bool g_sender_enabled{false};
std::atomic_bool g_sender_active{false};
std::atomic_uint32_t g_sender_track_a{5};
std::atomic_uint32_t g_sender_track_b{6};
std::atomic_uint64_t g_sender_paired{0};
std::atomic_uint64_t g_sender_discarded{0};
std::atomic_uint64_t g_sender_fallback{0};
std::atomic_int64_t g_sender_last_delta_ns{0};
std::atomic_uint32_t g_sender_queue_a{0};
std::atomic_uint32_t g_sender_queue_b{0};
std::atomic<float> g_sender_peak_a{0.0f};
std::atomic<float> g_sender_peak_b{0.0f};
std::atomic_uint64_t g_sender_last_audio_ns{0};

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
	}

	void set_suppress_original(bool suppress)
	{
		suppress_original_.store(suppress, std::memory_order_release);
	}

	bool route(obs_source_t *origin, const obs_source_audio *audio, int channel_count)
	{
		if (!origin || !audio || audio->frames == 0)
			return false;

		std::array<obs_source_t *, 2> outputs{};
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!input_ || input_ != origin)
				return false;
			for (size_t pair = 0; pair < outputs.size(); ++pair) {
				if (proxies_[pair])
					outputs[pair] = obs_source_get_ref(proxies_[pair]);
			}
		}

		channels_.store(std::max(channel_count, 0), std::memory_order_relaxed);
		packets_.fetch_add(1, std::memory_order_relaxed);
		last_packet_ns_.store(os_gettime_ns(), std::memory_order_relaxed);

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

			peaks_[static_cast<size_t>(pair)].store(calculate_peak(left, right, audio->frames),
								  std::memory_order_relaxed);
			if (outputs[static_cast<size_t>(pair)]) {
				obs_source_audio output{};
				output.data[0] = const_cast<uint8_t *>(left);
				output.data[1] = const_cast<uint8_t *>(right);
				output.frames = audio->frames;
				output.samples_per_sec = audio->samples_per_sec;
				output.speakers = SPEAKERS_STEREO;
				output.format = audio->format;
				output.timestamp = audio->timestamp;
				obs_source_output_audio(outputs[static_cast<size_t>(pair)], &output);
				obs_source_release(outputs[static_cast<size_t>(pair)]);
			}
		}

		const bool active = outputs_active();
		const bool suppress = suppress_original_.load(std::memory_order_acquire) && active && channel_count >= 4;
		if (suppress)
			suppressed_.fetch_add(1, std::memory_order_relaxed);
		return suppress;
	}

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
	}

private:
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
};

const char *program_source_name(void *) { return "Multichannel Bridge - Desktop / Game Audio"; }
const char *mic_source_name(void *) { return "Multichannel Bridge - Microphone Audio"; }

void *create_proxy_common(obs_source_t *source, int pair)
{
	auto *context = new ProxyContext;
	context->source = source;
	context->pair = pair;
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

bool add_proxy_to_current_scene(const char *id, const char *name)
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
		layout->addWidget(sender_box_);

		receiver_box_ = new QGroupBox("2. Stream PC receiver", body);
		auto *receiver_form = new QFormLayout(receiver_box_);
		receiver_source_ = new QComboBox(receiver_box_);
		refresh_ = new QPushButton("Refresh NDI sources", receiver_box_);
		open_source_ = new QPushButton("Open properties", receiver_box_);
		auto *source_row = new QWidget(receiver_box_);
		auto *source_row_layout = new QHBoxLayout(source_row);
		source_row_layout->setContentsMargins(0, 0, 0, 0);
		source_row_layout->addWidget(receiver_source_, 1);
		source_row_layout->addWidget(refresh_);
		source_row_layout->addWidget(open_source_);
		receiver_form->addRow("DistroAV NDI video source:", source_row);
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
		actions->addWidget(apply_, 1);
		actions->addWidget(reset_stats_);
		actions->addWidget(copy_diagnostics_);
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
		connect(open_source_, &QPushButton::clicked, this, [this] {
			obs_source_t *source = obs_get_source_by_name(receiver_source_->currentText().toUtf8().constData());
			if (source) {
				obs_frontend_open_source_properties(source);
				obs_source_release(source);
			}
		});
		connect(apply_, &QPushButton::clicked, this, [this] { apply_settings(false); });
		connect(create_receiver_, &QPushButton::clicked, this, [this] { apply_settings(true); });
		connect(reset_stats_, &QPushButton::clicked, this, [this] {
			mcb_sender_status_reset_counters();
			ReceiverRouter::instance().reset_stats();
			update_status();
		});
		connect(copy_diagnostics_, &QPushButton::clicked, this, [this] {
			QApplication::clipboard()->setText(diagnostics());
		});

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, [this] { update_status(); });
		timer_->start(500);
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

private:
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
				"<b>Receiver checklist:</b> Add one normal DistroAV NDI Source for the gaming-PC feed, "
				"select it above, then create the two split mixer sources.");
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
		save_config();
		set_role_cache(new_role);

		if (new_role == MCBRole::Sender) {
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
		if (create_sources) {
			const bool program_ok = add_proxy_to_current_scene(
				kProgramSourceId, program_name_->text().toUtf8().constData());
			const bool mic_ok = add_proxy_to_current_scene(kMicSourceId, mic_name_->text().toUtf8().constData());
			checklist_->setText(program_ok && mic_ok
						   ? "Receiver attached and both split audio sources are present in the current scene."
						   : "Receiver attached, but one or both split sources could not be added. Check for duplicate names.");
		} else {
			checklist_->setText("Receiver attached. Use Create / repair if the two mixer sources are missing.");
		}
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
		return QString(
			       "Multichannel Bridge for DistroAV %1\nRole: %2\nOBS audio rate: %3 Hz\n"
			       "Sender active: %4\nTracks: %5 / %6\nPaired: %7\nDiscarded: %8\nSilence fallback: %9\n"
			       "Last timestamp delta: %10 ms\nQueues: %11 / %12\nSender audio age: %13 ms\n"
			       "Receiver attached: %14\nSplit outputs ready: %15\nSplit outputs active: %16\nDetected channels: %17\nPackets: %18\n"
			       "Suppressed original packets: %19\nReceiver packet age: %20 ms\nMissing program: %21\nMissing mic: %22")
			.arg(kVersion)
			.arg(mcb_is_sender() ? "Gaming PC / Sender" : mcb_is_receiver() ? "Stream PC / Receiver" : "Unconfigured")
			.arg(obs_get_audio() ? audio_output_get_sample_rate(obs_get_audio()) : 0)
			.arg(sender.active ? "yes" : "no")
			.arg(sender.track_a)
			.arg(sender.track_b)
			.arg(static_cast<qulonglong>(sender.paired_blocks))
			.arg(static_cast<qulonglong>(sender.discarded_blocks))
			.arg(static_cast<qulonglong>(sender.silence_fallback_blocks))
			.arg(static_cast<double>(sender.last_timestamp_delta_ns) / 1e6, 0, 'f', 3)
			.arg(sender.queue_depth_a)
			.arg(sender.queue_depth_b)
			.arg(sender_age, 0, 'f', 1)
			.arg(router.attached() ? "yes" : "no")
			.arg(router.outputs_ready() ? "yes" : "no")
			.arg(router.outputs_active() ? "yes" : "no")
			.arg(router.channels())
			.arg(static_cast<qulonglong>(router.packets()))
			.arg(static_cast<qulonglong>(router.suppressed()))
			.arg(receiver_age, 0, 'f', 1)
			.arg(static_cast<qulonglong>(router.missing_program()))
			.arg(static_cast<qulonglong>(router.missing_mic()));
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
		sender_status_->setText(
			QString("%1 · Track %2 → NDI 1-2 · Track %3 → NDI 3-4 · paired %4 · discarded %5 · "
				"fallback %6 · delta %7 ms%8 · queues %9/%10 · audio age %11 ms%12")
				.arg(status.active ? "ACTIVE" : "not active")
				.arg(status.track_a)
				.arg(status.track_b)
				.arg(static_cast<qulonglong>(status.paired_blocks))
				.arg(static_cast<qulonglong>(status.discarded_blocks))
				.arg(static_cast<qulonglong>(status.silence_fallback_blocks))
				.arg(delta_ms, 0, 'f', 3)
				.arg(phase_note)
				.arg(status.queue_depth_a)
				.arg(status.queue_depth_b)
				.arg(age_ms, 0, 'f', 1)
				.arg(rate_warning));
		sender_program_meter_->setValue(meter_value(status.peak_a));
		sender_mic_meter_->setValue(meter_value(status.peak_b));

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
	QComboBox *receiver_source_ = nullptr;
	QPushButton *refresh_ = nullptr;
	QPushButton *open_source_ = nullptr;
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
	QLabel *checklist_ = nullptr;
	QTimer *timer_ = nullptr;
};

BridgeDock *g_dock = nullptr;

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
	} else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_PROFILE_CHANGING) {
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
	obs_log(LOG_INFO, "[multichannel-bridge] Dock initialized (%s)", kVersion);
}

void mcb_shutdown()
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	ReceiverRouter::instance().detach();
	if (g_dock) {
		obs_frontend_remove_dock(kDockId);
		g_dock = nullptr;
	}
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

void mcb_sender_status_paired(int64_t timestamp_delta_ns, size_t queue_a, size_t queue_b)
{
	g_sender_paired.fetch_add(1, std::memory_order_relaxed);
	g_sender_last_delta_ns.store(timestamp_delta_ns, std::memory_order_relaxed);
	g_sender_queue_a.store(static_cast<uint32_t>(queue_a), std::memory_order_relaxed);
	g_sender_queue_b.store(static_cast<uint32_t>(queue_b), std::memory_order_relaxed);
	g_sender_last_audio_ns.store(os_gettime_ns(), std::memory_order_relaxed);
}

void mcb_sender_status_discarded(int64_t timestamp_delta_ns, size_t queue_a, size_t queue_b)
{
	g_sender_discarded.fetch_add(1, std::memory_order_relaxed);
	g_sender_last_delta_ns.store(timestamp_delta_ns, std::memory_order_relaxed);
	g_sender_queue_a.store(static_cast<uint32_t>(queue_a), std::memory_order_relaxed);
	g_sender_queue_b.store(static_cast<uint32_t>(queue_b), std::memory_order_relaxed);
}

void mcb_sender_status_silence_fallback(size_t queue_a, size_t queue_b)
{
	g_sender_fallback.fetch_add(1, std::memory_order_relaxed);
	g_sender_queue_a.store(static_cast<uint32_t>(queue_a), std::memory_order_relaxed);
	g_sender_queue_b.store(static_cast<uint32_t>(queue_b), std::memory_order_relaxed);
	g_sender_last_audio_ns.store(os_gettime_ns(), std::memory_order_relaxed);
}

void mcb_sender_status_levels(float peak_a, float peak_b)
{
	g_sender_peak_a.store(std::clamp(peak_a, 0.0f, 1.0f), std::memory_order_relaxed);
	g_sender_peak_b.store(std::clamp(peak_b, 0.0f, 1.0f), std::memory_order_relaxed);
}

void mcb_sender_status_reset_counters()
{
	g_sender_paired.store(0, std::memory_order_relaxed);
	g_sender_discarded.store(0, std::memory_order_relaxed);
	g_sender_fallback.store(0, std::memory_order_relaxed);
	g_sender_last_delta_ns.store(0, std::memory_order_relaxed);
	g_sender_queue_a.store(0, std::memory_order_relaxed);
	g_sender_queue_b.store(0, std::memory_order_relaxed);
	g_sender_peak_a.store(0.0f, std::memory_order_relaxed);
	g_sender_peak_b.store(0.0f, std::memory_order_relaxed);
	g_sender_last_audio_ns.store(0, std::memory_order_relaxed);
}

bool mcb_receiver_route_audio(obs_source_t *origin, const obs_source_audio *audio, int channel_count)
{
	if (!mcb_is_receiver())
		return false;
	return ReceiverRouter::instance().route(origin, audio, channel_count);
}
