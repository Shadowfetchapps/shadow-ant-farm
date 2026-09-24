#include "ant_audio.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {
constexpr int kMaxVoices = 28;
constexpr float kPi = 3.14159265358979f;
} // namespace

void AntAudioSynth::configure(double mix_rate, int64_t seed, int grid_width)
{
	m_rate = float(std::max(8000.0, mix_rate));
	m_gridW = std::max(1, grid_width);
	m_rng.reseed(antfarm::deriveSeed(uint64_t(seed), antfarm::Stream::Audio), 7u);
	m_voices.clear();
	m_voices.reserve(kMaxVoices);
	for (int c = 0; c < CategoryCount; ++c)
		m_tokens[size_t(c)] = m_tokenRate[size_t(c)];
}

void AntAudioSynth::set_category_volume(int category, double linear)
{
	if (category >= 0 && category < CategoryCount)
		m_volume[size_t(category)] = float(std::clamp(linear, 0.0, 1.5));
}

double AntAudioSynth::get_category_volume(int category) const
{
	return (category >= 0 && category < CategoryCount) ? double(m_volume[size_t(category)]) : 0.0;
}

void AntAudioSynth::startVoice(int kind, float xNorm, float strength, int category)
{
	++m_eventsIn;
	if (m_tokens[size_t(category)] < 1.0f || int(m_voices.size()) >= kMaxVoices || m_volume[size_t(category)] <= 0.0f) {
		++m_eventsLimited;
		return;
	}
	m_tokens[size_t(category)] -= 1.0f;
	++m_eventsPlayed;
	Voice v;
	v.kind = kind;
	v.pan = std::clamp(0.12f + 0.76f * xNorm + m_rng.range(-0.03f, 0.03f), 0.0f, 1.0f);
	const float vol = m_volume[size_t(category)];
	switch (kind) {
	case 0: // a mandible scrape against the soil face
		v.length = int(m_rate * m_rng.range(0.035f, 0.09f));
		v.fc = m_rng.range(2200.0f, 3600.0f) + 2400.0f * std::clamp(strength, 0.0f, 1.0f);
		v.q = m_rng.range(0.9f, 1.6f);
		v.density = m_rng.range(40.0f, 90.0f);
		v.gain = 0.16f * (0.5f + 0.5f * strength) * vol;
		break;
	case 1: // a pellet coming free: a small crumble of grains
		v.length = int(m_rate * m_rng.range(0.12f, 0.22f));
		v.fc = m_rng.range(1600.0f, 2600.0f);
		v.q = 1.4f;
		v.density = m_rng.range(70.0f, 130.0f);
		v.gain = 0.18f * std::clamp(0.5f + strength, 0.5f, 1.4f) * vol;
		break;
	case 2: // a load set down on the pile: a short trickle
		v.length = int(m_rate * m_rng.range(0.25f, 0.45f));
		v.fc = m_rng.range(1200.0f, 2000.0f);
		v.q = 1.8f;
		v.density = m_rng.range(110.0f, 180.0f);
		v.gain = 0.15f * vol;
		break;
	default: // a seed picked up or set down
		v.length = int(m_rate * 0.03f);
		v.fc = m_rng.range(1800.0f, 2600.0f);
		v.q = 0.25f;
		v.density = 0.0f;
		v.nextClick = 0.0f;
		v.gain = 0.1f * vol;
		break;
	}
	v.nextClick = kind == 3 ? 0.0f : m_rng.range(0.0f, m_rate / std::max(1.0f, v.density));
	m_voices.push_back(v);
}

void AntAudioSynth::push_events(const PackedFloat32Array &events)
{
	const int64_t n = events.size() / 4;
	const float *e = events.ptr();
	for (int64_t k = 0; k < n; ++k, e += 4) {
		const int type = int(e[0]);
		const float xn = e[1] / float(m_gridW);
		const float s = e[3];
		switch (type) {
		case 0: startVoice(0, xn, s, Digging); break;  // DigStroke
		case 1: startVoice(1, xn, s, Soil); break;     // PelletFree
		case 2: startVoice(2, xn, s, Soil); break;     // Deposit
		case 3:                                         // PickUpFood
		case 4: startVoice(3, xn, s, Food); break;     // DropFood
		default: break;                                 // steps and grooming are part of the texture
		}
	}
}

void AntAudioSynth::renderInto(float *out, int frames)
{
	const float dt = 1.0f / m_rate;
	for (int c = 0; c < CategoryCount; ++c)
		m_tokens[size_t(c)] = std::min(m_tokenRate[size_t(c)], m_tokens[size_t(c)] + m_tokenRate[size_t(c)] * dt * float(frames));
	const float clickDecay = std::exp(-1.0f / (0.00045f * m_rate));
	const float stepDecay = std::exp(-1.0f / (0.00025f * m_rate));
	const float amb = m_volume[Ambience] * m_master;
	const float walk = m_volume[Walking] * m_master;
	const float lpA = 1.0f - std::exp(-2.0f * kPi * 140.0f / m_rate);
	const float stepRate = std::min(260.0f, float(m_walkers) * 0.9f); // aggregate footfalls per second

	for (int i = 0; i < frames; ++i) {
		float l = 0, r = 0;
		// Room tone: very quiet low rumble with a slow drift (the farm sits in a quiet room).
		// independent left/right beds so the room is wide rather than a mono hum
		m_brown = 0.998f * m_brown + 0.02f * noise();
		m_brownR = 0.998f * m_brownR + 0.02f * noise();
		m_lp1 += lpA * (m_brown - m_lp1);
		m_lp2 += lpA * (m_lp1 - m_lp2);
		m_lp1R += lpA * (m_brownR - m_lp1R);
		m_lp2R += lpA * (m_lp1R - m_lp2R);
		m_ambPhase += dt * 0.021f;
		if (m_ambPhase > 1.0f)
			m_ambPhase -= 1.0f;
		const float drift = (0.85f + 0.15f * std::sin(m_ambPhase * 2.0f * kPi)) * amb * 0.12f;
		// a whisper of air (very soft broadband hiss) above the rumble
		m_hiss += 0.35f * (noise() - m_hiss);
		m_hissR += 0.35f * (noise() - m_hissR);
		l += m_lp2 * drift + m_hiss * 0.0016f * amb;
		r += m_lp2R * drift + m_hissR * 0.0016f * amb;
		// Footfall texture: a sparse, very soft crackle spread across the glass.
		if (walk > 0.0f && stepRate > 0.0f) {
			m_stepClock -= 1.0f;
			if (m_stepClock <= 0.0f) {
				const float lvl = m_rng.range(0.2f, 1.0f) * 0.02f * walk;
				const float p = m_rng.uniform();
				m_stepEnvL += lvl * std::cos(p * 0.5f * kPi);
				m_stepEnvR += lvl * std::sin(p * 0.5f * kPi);
				m_stepClock = -std::log(std::max(1e-6f, m_rng.uniform())) * m_rate / stepRate;
			}
			const float nL = noise() * m_stepEnvL, nR = noise() * m_stepEnvR;
			l += nL - m_stepHpL;
			r += nR - m_stepHpR;
			m_stepHpL = nL * 0.6f;
			m_stepHpR = nR * 0.6f;
			m_stepEnvL *= stepDecay;
			m_stepEnvR *= stepDecay;
		}
		out[i * 2] = l;
		out[i * 2 + 1] = r;
	}

	for (Voice &v : m_voices) {
		const float f = 2.0f * std::sin(kPi * std::min(v.fc, m_rate * 0.2f) / m_rate);
		const float pl = std::cos(v.pan * 0.5f * kPi), pr = std::sin(v.pan * 0.5f * kPi);
		for (int i = 0; i < frames && v.age < v.length; ++i, ++v.age) {
			const float t = float(v.age) / float(std::max(1, v.length));
			float env;
			switch (v.kind) {
			case 0: env = std::min(1.0f, float(v.age) / (0.004f * m_rate)) * std::exp(-3.2f * t); break;
			case 3: env = std::exp(-9.0f * t); break;
			default: env = 1.0f - t; break;
			}
			// Grain clicks (a Poisson process whose rate falls off over the voice).
			float click = 0;
			if (v.kind == 3) {
				if (v.age == 0)
					v.clickEnv = 1.0f, v.clickLevel = 1.0f;
			} else {
				v.nextClick -= 1.0f;
				if (v.nextClick <= 0.0f) {
					v.clickEnv = 1.0f;
					v.clickLevel = m_rng.range(0.25f, 1.0f);
					const float rate = std::max(1.0f, v.density * (v.kind == 0 ? 1.0f : (1.0f - 0.8f * t)));
					v.nextClick = -std::log(std::max(1e-6f, m_rng.uniform())) * m_rate / rate;
				}
			}
			const float cn = noise() * v.clickEnv * v.clickLevel;
			click = cn - v.hp; // first-order high-pass: crisp grain ticks
			v.hp = cn * 0.7f;
			v.clickEnv *= clickDecay;
			// Band-limited noise body (state-variable band-pass).
			const float in = v.kind == 3 ? click : noise();
			v.bpState[0] += f * v.bpState[1];                                   // low
			const float high = in - v.bpState[0] - v.q * v.bpState[1];
			v.bpState[1] += f * high;                                            // band
			float body;
			switch (v.kind) {
			case 0: body = v.bpState[1] * 0.9f + click * 0.55f; break;
			case 1: body = v.bpState[1] * 0.25f + click * 1.0f; break;
			case 2: body = v.bpState[1] * 0.18f + click * 0.9f; break;
			default: body = v.bpState[1] * 2.2f + click * 0.4f; break;
			}
			const float s = body * env * v.gain * m_master;
			out[i * 2] += s * pl;
			out[i * 2 + 1] += s * pr;
		}
	}
	m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [](const Voice &v) { return v.age >= v.length; }), m_voices.end());

	for (int i = 0; i < frames * 2; ++i) {
		const float x = out[i];
		const float y = std::tanh(x * 1.4f) / 1.4f; // gentle soft limit, never clips
		m_peak = std::max(m_peak, std::fabs(y));
		out[i] = y;
	}
	m_framesOut += frames;
}

int AntAudioSynth::fill(const Ref<AudioStreamGeneratorPlayback> &playback)
{
	if (playback.is_null())
		return 0;
	const int frames = std::min(int(playback->get_frames_available()), int(m_rate * 0.25f));
	if (frames <= 0)
		return 0;
	m_scratch.resize(size_t(frames) * 2);
	renderInto(m_scratch.data(), frames);
	PackedVector2Array buf;
	buf.resize(frames);
	Vector2 *b = buf.ptrw();
	for (int i = 0; i < frames; ++i)
		b[i] = Vector2(m_scratch[size_t(i) * 2], m_scratch[size_t(i) * 2 + 1]);
	playback->push_buffer(buf);
	return frames;
}

PackedVector2Array AntAudioSynth::render(int frames)
{
	PackedVector2Array buf;
	if (frames <= 0)
		return buf;
	m_scratch.resize(size_t(frames) * 2);
	renderInto(m_scratch.data(), frames);
	buf.resize(frames);
	Vector2 *b = buf.ptrw();
	for (int i = 0; i < frames; ++i)
		b[i] = Vector2(m_scratch[size_t(i) * 2], m_scratch[size_t(i) * 2 + 1]);
	return buf;
}

Dictionary AntAudioSynth::get_stats() const
{
	Dictionary d;
	d["events_in"] = m_eventsIn;
	d["events_played"] = m_eventsPlayed;
	d["events_limited"] = m_eventsLimited;
	d["voices"] = int64_t(m_voices.size());
	d["frames_out"] = m_framesOut;
	d["peak"] = m_peak;
	d["mix_rate"] = m_rate;
	return d;
}

void AntAudioSynth::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("configure", "mix_rate", "seed", "grid_width"), &AntAudioSynth::configure);
	ClassDB::bind_method(D_METHOD("set_category_volume", "category", "linear"), &AntAudioSynth::set_category_volume);
	ClassDB::bind_method(D_METHOD("get_category_volume", "category"), &AntAudioSynth::get_category_volume);
	ClassDB::bind_method(D_METHOD("set_master_volume", "linear"), &AntAudioSynth::set_master_volume);
	ClassDB::bind_method(D_METHOD("push_events", "events"), &AntAudioSynth::push_events);
	ClassDB::bind_method(D_METHOD("set_walkers", "count"), &AntAudioSynth::set_walkers);
	ClassDB::bind_method(D_METHOD("fill", "playback"), &AntAudioSynth::fill);
	ClassDB::bind_method(D_METHOD("render", "frames"), &AntAudioSynth::render);
	ClassDB::bind_method(D_METHOD("get_stats"), &AntAudioSynth::get_stats);
	BIND_CONSTANT(Ambience);
	BIND_CONSTANT(Digging);
	BIND_CONSTANT(Soil);
	BIND_CONSTANT(Walking);
	BIND_CONSTANT(Food);
}
