#include "receiver-clock-diagnostics.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace distroav::clocklab {

int64_t Diagnostics::signed_delta(uint64_t current, uint64_t previous) noexcept
{
	if (current >= previous) {
		const uint64_t delta = current - previous;
		return delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
			       ? std::numeric_limits<int64_t>::max()
			       : static_cast<int64_t>(delta);
	}
	const uint64_t delta = previous - current;
	return delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? std::numeric_limits<int64_t>::min()
										  : -static_cast<int64_t>(delta);
}

const char *Diagnostics::event_name(Event event) noexcept
{
	switch (event) {
	case Event::None:
		return "";
	case Event::LoggingEnabled:
		return "logging_enabled";
	case Event::ReceiverReset:
		return "receiver_reset";
	case Event::ReceiverReady:
		return "receiver_ready";
	case Event::ModeChanged:
		return "mode_changed";
	case Event::DiagnosticsChanged:
		return "diagnostics_changed";
	}
	return "unknown";
}

void Diagnostics::clear_stage(StageAtomics &stage) noexcept
{
	stage.sequence.store(0, std::memory_order_relaxed);
	stage.ndi_timestamp_100ns.store(0, std::memory_order_relaxed);
	stage.ndi_timecode_100ns.store(0, std::memory_order_relaxed);
	stage.timestamp_ns.store(0, std::memory_order_relaxed);
	stage.wall_ns.store(0, std::memory_order_relaxed);
	stage.timestamp_delta_ns.store(0, std::memory_order_relaxed);
	stage.wall_delta_ns.store(0, std::memory_order_relaxed);
	stage.unit_a.store(0, std::memory_order_relaxed);
	stage.unit_b.store(0, std::memory_order_relaxed);
	stage.unit_c.store(0, std::memory_order_relaxed);
	stage.observations.store(0, std::memory_order_relaxed);
	stage.pacing_anomalies.store(0, std::memory_order_relaxed);
}

void Diagnostics::publish(StageAtomics &stage, int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns,
			  uint64_t timestamp_ns, uint64_t wall_ns, uint32_t unit_a, uint32_t unit_b,
			  uint32_t unit_c) noexcept
{
	const uint64_t previous_timestamp = stage.timestamp_ns.load(std::memory_order_relaxed);
	const uint64_t previous_wall = stage.wall_ns.load(std::memory_order_relaxed);
	const int64_t timestamp_delta = previous_timestamp ? signed_delta(timestamp_ns, previous_timestamp) : 0;
	const int64_t wall_delta = previous_wall ? signed_delta(wall_ns, previous_wall) : 0;

	stage.sequence.fetch_add(1, std::memory_order_acq_rel);
	stage.ndi_timestamp_100ns.store(ndi_timestamp_100ns, std::memory_order_relaxed);
	stage.ndi_timecode_100ns.store(ndi_timecode_100ns, std::memory_order_relaxed);
	stage.timestamp_ns.store(timestamp_ns, std::memory_order_relaxed);
	stage.wall_ns.store(wall_ns, std::memory_order_relaxed);
	stage.timestamp_delta_ns.store(timestamp_delta, std::memory_order_relaxed);
	stage.wall_delta_ns.store(wall_delta, std::memory_order_relaxed);
	stage.unit_a.store(unit_a, std::memory_order_relaxed);
	stage.unit_b.store(unit_b, std::memory_order_relaxed);
	stage.unit_c.store(unit_c, std::memory_order_relaxed);
	stage.observations.fetch_add(1, std::memory_order_relaxed);
	if (previous_timestamp && previous_wall &&
	    (std::llabs(timestamp_delta - wall_delta) > 2000000LL || timestamp_delta <= 0))
		stage.pacing_anomalies.fetch_add(1, std::memory_order_relaxed);
	stage.sequence.fetch_add(1, std::memory_order_release);
}

StageSnapshot Diagnostics::read_stage(const StageAtomics &stage) noexcept
{
	StageSnapshot result;
	for (int attempt = 0; attempt < 4; ++attempt) {
		const uint64_t before = stage.sequence.load(std::memory_order_acquire);
		if (before & 1ULL)
			continue;
		result.ndi_timestamp_100ns = stage.ndi_timestamp_100ns.load(std::memory_order_relaxed);
		result.ndi_timecode_100ns = stage.ndi_timecode_100ns.load(std::memory_order_relaxed);
		result.timestamp_ns = stage.timestamp_ns.load(std::memory_order_relaxed);
		result.wall_ns = stage.wall_ns.load(std::memory_order_relaxed);
		result.timestamp_delta_ns = stage.timestamp_delta_ns.load(std::memory_order_relaxed);
		result.wall_delta_ns = stage.wall_delta_ns.load(std::memory_order_relaxed);
		result.unit_a = stage.unit_a.load(std::memory_order_relaxed);
		result.unit_b = stage.unit_b.load(std::memory_order_relaxed);
		result.unit_c = stage.unit_c.load(std::memory_order_relaxed);
		result.observations = stage.observations.load(std::memory_order_relaxed);
		result.pacing_anomalies = stage.pacing_anomalies.load(std::memory_order_relaxed);
		const uint64_t after = stage.sequence.load(std::memory_order_acquire);
		if (before == after && !(after & 1ULL))
			break;
	}
	return result;
}

void Diagnostics::set_enabled(bool enabled, uint64_t wall_ns)
{
	if (enabled == enabled_.load(std::memory_order_acquire))
		return;
	enabled_.store(false, std::memory_order_release);
	if (!enabled) {
		std::lock_guard<std::mutex> lock(ring_mutex_);
		if (live_output_) {
			live_output_.flush();
			live_output_.close();
		}
		return;
	}

	clear_stage(capture_audio_);
	clear_stage(capture_video_);
	clear_stage(output_audio_);
	clear_stage(output_video_);
	clear_stage(filtered_audio_);
	clear_stage(selected_video_);
	{
		std::lock_guard<std::mutex> lock(ring_mutex_);
		ring_.assign(kCapacity, Sample{});
		write_index_ = 0;
		count_ = 0;
		overwritten_ = 0;
		sampled_event_generation_ = 0;
		live_rows_since_flush_ = 0;
		if (live_output_.is_open())
			live_output_.close();
		if (!live_output_path_.empty()) {
			live_output_.open(live_output_path_, std::ios::binary | std::ios::trunc);
			if (live_output_)
				write_csv_header(live_output_);
		}
	}
	session_.fetch_add(1, std::memory_order_acq_rel);
	last_event_.store(static_cast<uint8_t>(Event::LoggingEnabled), std::memory_order_relaxed);
	last_event_wall_ns_.store(wall_ns, std::memory_order_relaxed);
	event_generation_.fetch_add(1, std::memory_order_acq_rel);
	enabled_.store(true, std::memory_order_release);
}

void Diagnostics::set_live_output_path(std::string path)
{
	std::lock_guard<std::mutex> lock(ring_mutex_);
	live_output_path_ = std::move(path);
}

void Diagnostics::mark_event(Event event, uint64_t wall_ns) noexcept
{
	if (!enabled() || event == Event::None)
		return;
	last_event_.store(static_cast<uint8_t>(event), std::memory_order_relaxed);
	last_event_wall_ns_.store(wall_ns, std::memory_order_relaxed);
	event_generation_.fetch_add(1, std::memory_order_acq_rel);
}

void Diagnostics::observe_capture_audio(int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns, uint64_t timestamp_ns,
					uint64_t wall_ns, uint32_t frames, uint32_t sample_rate,
					uint32_t channels) noexcept
{
	if (enabled())
		publish(capture_audio_, ndi_timestamp_100ns, ndi_timecode_100ns, timestamp_ns, wall_ns, frames,
			sample_rate, channels);
}

void Diagnostics::observe_capture_video(int64_t ndi_timestamp_100ns, int64_t ndi_timecode_100ns, uint64_t timestamp_ns,
					uint64_t wall_ns, uint32_t width, uint32_t height) noexcept
{
	if (enabled())
		publish(capture_video_, ndi_timestamp_100ns, ndi_timecode_100ns, timestamp_ns, wall_ns, width, height,
			0);
}

void Diagnostics::observe_output_audio(uint64_t timestamp_ns, uint64_t wall_ns, uint32_t frames, uint32_t sample_rate,
				       uint32_t channels) noexcept
{
	if (enabled())
		publish(output_audio_, 0, 0, timestamp_ns, wall_ns, frames, sample_rate, channels);
}

void Diagnostics::observe_output_video(uint64_t timestamp_ns, uint64_t wall_ns, uint32_t width,
				       uint32_t height) noexcept
{
	if (enabled())
		publish(output_video_, 0, 0, timestamp_ns, wall_ns, width, height, 0);
}

void Diagnostics::observe_filtered_audio(uint64_t timestamp_ns, uint64_t wall_ns, uint32_t frames) noexcept
{
	if (enabled())
		publish(filtered_audio_, 0, 0, timestamp_ns, wall_ns, frames, 0, 0);
}

void Diagnostics::observe_selected_video(uint64_t timestamp_ns, uint64_t wall_ns) noexcept
{
	if (enabled())
		publish(selected_video_, 0, 0, timestamp_ns, wall_ns, 0, 0, 0);
}

void Diagnostics::update_scheduler(const SchedulerSnapshot &snapshot) noexcept
{
	scheduler_.mode.store(snapshot.mode, std::memory_order_relaxed);
	scheduler_.receiver_epoch_ns.store(snapshot.receiver_epoch_ns, std::memory_order_relaxed);
	scheduler_.next_audio_deadline_ns.store(snapshot.next_audio_deadline_ns, std::memory_order_relaxed);
	scheduler_.next_video_deadline_ns.store(snapshot.next_video_deadline_ns, std::memory_order_relaxed);
	scheduler_.cumulative_audio_frames.store(snapshot.cumulative_audio_frames, std::memory_order_relaxed);
	scheduler_.video_ticks.store(snapshot.video_ticks, std::memory_order_relaxed);
	scheduler_.audio_deadline_error_ns.store(snapshot.audio_deadline_error_ns, std::memory_order_relaxed);
	scheduler_.video_deadline_error_ns.store(snapshot.video_deadline_error_ns, std::memory_order_relaxed);
	scheduler_.audio_catchups.store(snapshot.audio_catchups, std::memory_order_relaxed);
	scheduler_.video_catchups.store(snapshot.video_catchups, std::memory_order_relaxed);
	scheduler_.repeated_video_frames.store(snapshot.repeated_video_frames, std::memory_order_relaxed);
	scheduler_.empty_audio_pulls.store(snapshot.empty_audio_pulls, std::memory_order_relaxed);
	scheduler_.empty_video_pulls.store(snapshot.empty_video_pulls, std::memory_order_relaxed);
	scheduler_.ndi_total_audio_frames.store(snapshot.ndi_total_audio_frames, std::memory_order_relaxed);
	scheduler_.ndi_total_video_frames.store(snapshot.ndi_total_video_frames, std::memory_order_relaxed);
	scheduler_.ndi_dropped_audio_frames.store(snapshot.ndi_dropped_audio_frames, std::memory_order_relaxed);
	scheduler_.ndi_dropped_video_frames.store(snapshot.ndi_dropped_video_frames, std::memory_order_relaxed);
	scheduler_.ndi_queued_audio_frames.store(snapshot.ndi_queued_audio_frames, std::memory_order_relaxed);
	scheduler_.ndi_queued_video_frames.store(snapshot.ndi_queued_video_frames, std::memory_order_relaxed);
}

SchedulerSnapshot Diagnostics::read_scheduler() const noexcept
{
	SchedulerSnapshot result;
	result.mode = scheduler_.mode.load(std::memory_order_relaxed);
	result.receiver_epoch_ns = scheduler_.receiver_epoch_ns.load(std::memory_order_relaxed);
	result.next_audio_deadline_ns = scheduler_.next_audio_deadline_ns.load(std::memory_order_relaxed);
	result.next_video_deadline_ns = scheduler_.next_video_deadline_ns.load(std::memory_order_relaxed);
	result.cumulative_audio_frames = scheduler_.cumulative_audio_frames.load(std::memory_order_relaxed);
	result.video_ticks = scheduler_.video_ticks.load(std::memory_order_relaxed);
	result.audio_deadline_error_ns = scheduler_.audio_deadline_error_ns.load(std::memory_order_relaxed);
	result.video_deadline_error_ns = scheduler_.video_deadline_error_ns.load(std::memory_order_relaxed);
	result.audio_catchups = scheduler_.audio_catchups.load(std::memory_order_relaxed);
	result.video_catchups = scheduler_.video_catchups.load(std::memory_order_relaxed);
	result.repeated_video_frames = scheduler_.repeated_video_frames.load(std::memory_order_relaxed);
	result.empty_audio_pulls = scheduler_.empty_audio_pulls.load(std::memory_order_relaxed);
	result.empty_video_pulls = scheduler_.empty_video_pulls.load(std::memory_order_relaxed);
	result.ndi_total_audio_frames = scheduler_.ndi_total_audio_frames.load(std::memory_order_relaxed);
	result.ndi_total_video_frames = scheduler_.ndi_total_video_frames.load(std::memory_order_relaxed);
	result.ndi_dropped_audio_frames = scheduler_.ndi_dropped_audio_frames.load(std::memory_order_relaxed);
	result.ndi_dropped_video_frames = scheduler_.ndi_dropped_video_frames.load(std::memory_order_relaxed);
	result.ndi_queued_audio_frames = scheduler_.ndi_queued_audio_frames.load(std::memory_order_relaxed);
	result.ndi_queued_video_frames = scheduler_.ndi_queued_video_frames.load(std::memory_order_relaxed);
	return result;
}

void Diagnostics::sample(uint64_t wall_ns)
{
	if (!enabled())
		return;
	Sample row;
	row.wall_ns = wall_ns;
	row.session = session_.load(std::memory_order_relaxed);
	row.event_generation = event_generation_.load(std::memory_order_acquire);
	row.capture_audio = read_stage(capture_audio_);
	row.capture_video = read_stage(capture_video_);
	row.output_audio = read_stage(output_audio_);
	row.output_video = read_stage(output_video_);
	row.filtered_audio = read_stage(filtered_audio_);
	row.selected_video = read_stage(selected_video_);
	row.scheduler = read_scheduler();

	std::lock_guard<std::mutex> lock(ring_mutex_);
	if (ring_.empty())
		return;
	if (row.event_generation != sampled_event_generation_) {
		row.event = static_cast<Event>(last_event_.load(std::memory_order_relaxed));
		row.event_wall_ns = last_event_wall_ns_.load(std::memory_order_relaxed);
		sampled_event_generation_ = row.event_generation;
	}
	row.overwritten_at_capture = overwritten_;
	ring_[write_index_] = row;
	write_index_ = (write_index_ + 1) % ring_.size();
	if (count_ < ring_.size())
		++count_;
	else
		++overwritten_;
	if (live_output_) {
		write_csv_row(live_output_, row);
		if (++live_rows_since_flush_ >= 16) {
			live_output_.flush();
			live_rows_since_flush_ = 0;
		}
	}
}

namespace {
void write_stage_header(std::ostream &out, const char *prefix, const char *unit_a, const char *unit_b,
			const char *unit_c)
{
	out << ',' << prefix << "_timestamp_ns," << prefix << "_wall_ns," << prefix << "_timestamp_delta_ns," << prefix
	    << "_wall_delta_ns," << prefix << "_ndi_timestamp_100ns," << prefix << "_ndi_timecode_100ns," << prefix
	    << '_' << unit_a << ',' << prefix << '_' << unit_b << ',' << prefix << '_' << unit_c << ',' << prefix
	    << "_observations," << prefix << "_pacing_anomalies";
}

void write_stage(std::ostream &out, const StageSnapshot &stage)
{
	out << ',' << stage.timestamp_ns << ',' << stage.wall_ns << ',' << stage.timestamp_delta_ns << ','
	    << stage.wall_delta_ns << ',' << stage.ndi_timestamp_100ns << ',' << stage.ndi_timecode_100ns << ','
	    << stage.unit_a << ',' << stage.unit_b << ',' << stage.unit_c << ',' << stage.observations << ','
	    << stage.pacing_anomalies;
}

int64_t projected_relation(const StageSnapshot &a, const StageSnapshot &b)
{
	if (!a.timestamp_ns || !a.wall_ns || !b.timestamp_ns || !b.wall_ns)
		return 0;
	auto delta = [](uint64_t current, uint64_t previous) {
		if (current >= previous)
			return static_cast<int64_t>(std::min<uint64_t>(current - previous, INT64_MAX));
		return -static_cast<int64_t>(std::min<uint64_t>(previous - current, INT64_MAX));
	};
	return delta(a.timestamp_ns, b.timestamp_ns) + delta(b.wall_ns, a.wall_ns);
}
} // namespace

void Diagnostics::write_csv_header(std::ostream &out)
{
	out << "sample_wall_ns,session,event,event_generation,event_wall_ns,overwritten_samples";
	write_stage_header(out, "capture_audio", "frames", "sample_rate", "channels");
	write_stage_header(out, "capture_video", "width", "height", "unused");
	write_stage_header(out, "output_audio", "frames", "sample_rate", "channels");
	write_stage_header(out, "output_video", "width", "height", "unused");
	write_stage_header(out, "filtered_audio", "frames", "unused_a", "unused_b");
	write_stage_header(out, "selected_video", "unused_a", "unused_b", "unused_c");
	out << ",mode,receiver_epoch_ns,next_audio_deadline_ns,next_video_deadline_ns,cumulative_audio_frames,video_ticks"
	       ",audio_deadline_error_ns,video_deadline_error_ns,audio_catchups,video_catchups,repeated_video_frames"
	       ",empty_audio_pulls,empty_video_pulls,ndi_total_audio_frames,ndi_total_video_frames"
	       ",ndi_dropped_audio_frames,ndi_dropped_video_frames,ndi_queued_audio_frames,ndi_queued_video_frames"
	       ",capture_video_minus_capture_audio_projected_ns"
	       ",output_video_minus_output_audio_projected_ns,selected_video_minus_output_video_projected_ns"
	       ",filtered_audio_minus_output_audio_projected_ns,selected_video_minus_filtered_audio_projected_ns\n";
}

void Diagnostics::write_csv_row(std::ostream &out, const Sample &row)
{
	out << row.wall_ns << ',' << row.session << ',' << event_name(row.event) << ',' << row.event_generation << ','
	    << row.event_wall_ns << ',' << row.overwritten_at_capture;
	write_stage(out, row.capture_audio);
	write_stage(out, row.capture_video);
	write_stage(out, row.output_audio);
	write_stage(out, row.output_video);
	write_stage(out, row.filtered_audio);
	write_stage(out, row.selected_video);
	const auto &s = row.scheduler;
	out << ',' << s.mode << ',' << s.receiver_epoch_ns << ',' << s.next_audio_deadline_ns << ','
	    << s.next_video_deadline_ns << ',' << s.cumulative_audio_frames << ',' << s.video_ticks << ','
	    << s.audio_deadline_error_ns << ',' << s.video_deadline_error_ns << ',' << s.audio_catchups << ','
	    << s.video_catchups << ',' << s.repeated_video_frames << ',' << s.empty_audio_pulls << ','
	    << s.empty_video_pulls << ',' << s.ndi_total_audio_frames << ',' << s.ndi_total_video_frames << ','
	    << s.ndi_dropped_audio_frames << ',' << s.ndi_dropped_video_frames << ',' << s.ndi_queued_audio_frames
	    << ',' << s.ndi_queued_video_frames << ',' << projected_relation(row.capture_video, row.capture_audio)
	    << ',' << projected_relation(row.output_video, row.output_audio) << ','
	    << projected_relation(row.selected_video, row.output_video) << ','
	    << projected_relation(row.filtered_audio, row.output_audio) << ','
	    << projected_relation(row.selected_video, row.filtered_audio) << '\n';
}

std::string Diagnostics::csv() const
{
	std::lock_guard<std::mutex> lock(ring_mutex_);
	std::ostringstream out;
	write_csv_header(out);
	if (ring_.empty() || !count_)
		return out.str();
	const size_t first = count_ == ring_.size() ? write_index_ : 0;
	for (size_t offset = 0; offset < count_; ++offset)
		write_csv_row(out, ring_[(first + offset) % ring_.size()]);
	return out.str();
}

size_t Diagnostics::sample_count() const
{
	std::lock_guard<std::mutex> lock(ring_mutex_);
	return count_;
}

uint64_t Diagnostics::overwritten_samples() const
{
	std::lock_guard<std::mutex> lock(ring_mutex_);
	return overwritten_;
}

} // namespace distroav::clocklab
