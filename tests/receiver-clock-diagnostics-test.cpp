#include "receiver-clock-diagnostics.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

int main()
{
	using distroav::clocklab::Diagnostics;
	using distroav::clocklab::SchedulerSnapshot;

	const std::string path = "receiver-clock-diagnostics-test.csv";
	Diagnostics diagnostics;
	diagnostics.set_live_output_path(path);
	diagnostics.set_enabled(true, 1000000000ULL);
	diagnostics.observe_capture_audio(100, 200, 1000000000ULL, 1100000000ULL, 1024, 48000, 2);
	diagnostics.observe_capture_video(101, 201, 1000000000ULL, 1100000000ULL, 1920, 1080);
	diagnostics.observe_output_audio(1200000000ULL, 1200000000ULL, 1024, 48000, 2);
	diagnostics.observe_output_video(1200000000ULL, 1200000000ULL, 1920, 1080);
	diagnostics.observe_filtered_audio(1200000000ULL, 1200000000ULL, 1024);
	diagnostics.observe_selected_video(1200000000ULL, 1200000000ULL);
	SchedulerSnapshot scheduler;
	scheduler.mode = 2;
	scheduler.ndi_total_audio_frames = 42;
	scheduler.ndi_total_video_frames = 60;
	diagnostics.update_scheduler(scheduler);
	diagnostics.sample(1250000000ULL);
	diagnostics.set_enabled(false, 1300000000ULL);

	const std::string snapshot = diagnostics.csv();
	assert(snapshot.find("selected_video_minus_filtered_audio_projected_ns") != std::string::npos);
	assert(snapshot.find("1250000000,1,logging_enabled") != std::string::npos);
	assert(diagnostics.sample_count() == 1);

	std::ifstream live(path, std::ios::binary);
	const std::string live_text((std::istreambuf_iterator<char>(live)), std::istreambuf_iterator<char>());
	assert(live_text == snapshot);
	live.close();
	std::remove(path.c_str());
	return 0;
}
