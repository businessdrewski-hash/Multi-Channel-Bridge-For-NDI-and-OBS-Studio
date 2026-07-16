#include "../src/audio-pair-assembler.hpp"
#include "../src/timeline-mapper.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

static StereoAudioBlock make_block(uint64_t timestamp, float left, float right)
{
  StereoAudioBlock block;
  block.timestamp_ns = timestamp;
  block.sample_rate = 48000;
  block.frames = 4;
  block.channels[0] = std::vector<float>(4, left);
  block.channels[1] = std::vector<float>(4, right);
  return block;
}

int main()
{
  AudioPairAssembler assembler;
  assert(!assembler.push(1, make_block(1'000'500, 3.0f, 4.0f)));
  auto merged = assembler.push(0, make_block(1'000'000, 1.0f, 2.0f));
  assert(merged);
  assert(merged->channels[0][0] == 1.0f);
  assert(merged->channels[1][0] == 2.0f);
  assert(merged->channels[2][0] == 3.0f);
  assert(merged->channels[3][0] == 4.0f);

  // OBS can deliver selected mixer callbacks one 1024-sample quantum apart.
  AudioPairAssembler shifted;
  auto a1024 = make_block(10'000'000, 1.0f, 2.0f);
  auto b1024 = make_block(31'333'333, 3.0f, 4.0f);
  a1024.frames = b1024.frames = 1024;
  for (auto &c : a1024.channels) c.resize(1024);
  for (auto &c : b1024.channels) c.resize(1024);
  assert(!shifted.push(0, std::move(a1024)));
  auto shifted_merged = shifted.push(1, std::move(b1024));
  assert(shifted_merged);
  assert(shifted.realigned() == 1);

  TimelineMapper mapper(10'000'000ULL);
  const uint64_t first = mapper.map(100'000, 1'000'000'000ULL);
  const uint64_t second = mapper.map(110'000, 1'001'000'000ULL);
  assert(first == 1'010'000'000ULL);
  assert(second - first == 1'000'000ULL); // 10,000 * 100 ns

  std::cout << "core tests passed\n";
  return 0;
}
