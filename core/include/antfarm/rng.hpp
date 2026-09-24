#pragma once
// Deterministic random number generation. Every consumer gets its own stream derived from the session
// seed, so changing graphics or audio settings can never change what the colony does.

#include <cmath>
#include <cstdint>

namespace antfarm {

/// SplitMix64: used to derive independent seeds from one session seed.
inline uint64_t splitmix64(uint64_t &state)
{
	uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/// Stream identifiers. Terrain, behaviour, decoration (render-only) and audio never share state.
enum class Stream : uint64_t { Terrain = 1, Behavior = 2, Decoration = 3, Audio = 4 };

inline uint64_t deriveSeed(uint64_t sessionSeed, Stream s, uint64_t sub = 0)
{
	uint64_t st = sessionSeed ^ (uint64_t(s) * 0xD1B54A32D192ED03ULL) ^ (sub * 0x8CB92BA72F3D8DD7ULL);
	splitmix64(st);
	return splitmix64(st);
}

/// PCG32 (O'Neill), small and fast with a serializable state.
struct Pcg32 {
	uint64_t state = 0x853C49E6748FEA9BULL;
	uint64_t inc = 0xDA3E39CB94B95BDBULL;

	Pcg32() = default;
	explicit Pcg32(uint64_t seed, uint64_t seq = 54u) { reseed(seed, seq); }

	void reseed(uint64_t seed, uint64_t seq = 54u)
	{
		state = 0u;
		inc = (seq << 1u) | 1u;
		next();
		state += seed;
		next();
	}

	uint32_t next()
	{
		uint64_t old = state;
		state = old * 6364136223846793005ULL + inc;
		uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
		uint32_t rot = uint32_t(old >> 59u);
		return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
	}

	/// Uniform in [0, 1).
	float uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); }
	/// Uniform in [a, b).
	float range(float a, float b) { return a + (b - a) * uniform(); }
	/// Integer in [0, n).
	uint32_t below(uint32_t n)
	{
		if (n == 0)
			return 0;
		return uint32_t((uint64_t(next()) * n) >> 32);
	}
	/// Approximately normal (sum of uniforms), mean 0, sd 1.
	float normal()
	{
		float s = 0;
		for (int i = 0; i < 4; ++i)
			s += uniform();
		return (s - 2.0f) * 1.7320508f;
	}
	bool chance(float p) { return uniform() < p; }
};

/// Deterministic value noise (lattice hash) used for terrain and smooth per-ant variation.
inline uint32_t hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7FEB352DU;
	x ^= x >> 15;
	x *= 0x846CA68BU;
	x ^= x >> 16;
	return x;
}

inline float hashUnit(int32_t x, int32_t y, uint32_t seed)
{
	uint32_t h = hash32(uint32_t(x) * 0x9E3779B1U ^ hash32(uint32_t(y) * 0x85EBCA77U ^ seed));
	return float(h >> 8) * (1.0f / 16777216.0f);
}

inline float smoothstep01(float t) { return t * t * (3.0f - 2.0f * t); }

inline float valueNoise(float x, float y, uint32_t seed)
{
	const float fx = std::floor(x), fy = std::floor(y);
	const int32_t ix = int32_t(fx), iy = int32_t(fy);
	const float tx = smoothstep01(x - fx), ty = smoothstep01(y - fy);
	const float a = hashUnit(ix, iy, seed), b = hashUnit(ix + 1, iy, seed);
	const float c = hashUnit(ix, iy + 1, seed), d = hashUnit(ix + 1, iy + 1, seed);
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

/// Fractal value noise in [0, 1].
inline float fbm(float x, float y, uint32_t seed, int octaves = 4, float lacunarity = 2.0f, float gain = 0.5f)
{
	float sum = 0, amp = 0.5f, norm = 0;
	for (int i = 0; i < octaves; ++i) {
		sum += amp * valueNoise(x, y, seed + uint32_t(i) * 131u);
		norm += amp;
		x *= lacunarity;
		y *= lacunarity;
		amp *= gain;
	}
	return sum / norm;
}

/// 1-D smooth noise (for per-ant behavioural drift over time).
inline float noise1(float t, uint32_t seed)
{
	const float f = std::floor(t);
	const int32_t i = int32_t(f);
	const float a = hashUnit(i, 0, seed), b = hashUnit(i + 1, 0, seed);
	return a + (b - a) * smoothstep01(t - f);
}

} // namespace antfarm
