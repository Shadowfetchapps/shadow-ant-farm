#pragma once
// Procedural sound for the farm. Every sound is synthesised here from filtered noise and short clicks
// (no recorded samples), driven by simulation events, voice- and rate-limited, 48 kHz stereo.

#include "antfarm/rng.hpp"

#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <array>
#include <vector>

namespace godot {

class AntAudioSynth : public RefCounted {
	GDCLASS(AntAudioSynth, RefCounted)

public:
	enum Category { Ambience = 0, Digging = 1, Soil = 2, Walking = 3, Food = 4, CategoryCount = 5 };

	void configure(double mix_rate, int64_t seed, int grid_width);
	void set_category_volume(int category, double linear);
	double get_category_volume(int category) const;
	void set_master_volume(double linear) { m_master = float(linear); }
	/// Feeds simulation events ([type, x, y, strength] quadruples, as AntFarmSim::poll_events returns).
	void push_events(const PackedFloat32Array &events);
	/// Workers currently walking (drives the faint footstep texture).
	void set_walkers(int count) { m_walkers = count; }
	/// Fills the generator's free space; returns frames written.
	int fill(const Ref<AudioStreamGeneratorPlayback> &playback);
	/// Renders frames offline (tests and level checks).
	PackedVector2Array render(int frames);
	Dictionary get_stats() const;

protected:
	static void _bind_methods();

private:
	struct Voice {
		int kind = 0;          // 0 scrape, 1 crumble, 2 trickle, 3 tick
		float pan = 0.5f;
		float gain = 0;
		int age = 0, length = 0;
		float bpState[4] = {0, 0, 0, 0};
		float fc = 3000, q = 1.2f;
		float nextClick = 0;   // samples until the next grain click
		float clickEnv = 0;
		float clickLevel = 0;
		float density = 0;     // clicks per second
		float hp = 0, lpState = 0;
	};

	void startVoice(int kind, float xNorm, float strength, int category);
	void renderInto(float *out, int frames);
	float noise() { return m_rng.uniform() * 2.0f - 1.0f; }

	float m_rate = 48000;
	int m_gridW = 960;
	antfarm::Pcg32 m_rng;
	std::vector<Voice> m_voices;
	std::array<float, CategoryCount> m_volume{{0.55f, 0.8f, 0.8f, 0.35f, 0.7f}};
	std::array<float, CategoryCount> m_tokens{};
	std::array<float, CategoryCount> m_tokenRate{{0, 18, 7, 0, 4}};
	float m_master = 0.9f;
	int m_walkers = 0;
	// ambience state
	float m_brown = 0, m_lp1 = 0, m_lp2 = 0, m_ambPhase = 0;
	float m_brownR = 0, m_lp1R = 0, m_lp2R = 0, m_hiss = 0, m_hissR = 0;
	float m_stepClock = 0;
	float m_stepEnvL = 0, m_stepEnvR = 0, m_stepHpL = 0, m_stepHpR = 0;
	// statistics
	int64_t m_eventsIn = 0, m_eventsPlayed = 0, m_eventsLimited = 0, m_framesOut = 0;
	float m_peak = 0;
	std::vector<float> m_scratch;
};

} // namespace godot
