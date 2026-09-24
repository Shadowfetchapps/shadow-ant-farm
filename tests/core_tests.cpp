// Native tests of the simulation core (the same code the visible application runs through the GDExtension).
// Accelerated runs process ordinary fixed ticks faster than real time; the timestep never changes.
//
//   antfarm_tests                 run everything (pacing uses 3 seeds × 24 simulated hours)
//   antfarm_tests <name> ...      run selected tests
//   ANTFARM_PACING_SEEDS=6        more seeds for the pacing test

#include "antfarm/sim.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace antfarm;

namespace {

int g_failures = 0;
std::string g_current;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                  \
			++g_failures;                                                                                  \
			return;                                                                                        \
		}                                                                                                      \
	} while (0)

#define NOTE(...)                                                                                                      \
	do {                                                                                                           \
		std::printf("  ▸ ");                                                                                   \
		std::printf(__VA_ARGS__);                                                                              \
		std::printf("\n");                                                                                     \
	} while (0)

constexpr int kTps = 30;
int ticksFor(double seconds) { return int(seconds * kTps); }

SimConfig defaultConfig()
{
	SimConfig c;
	c.ticksPerSecond = kTps;
	return c;
}

// ---- tests -------------------------------------------------------------------------------------

void freshSeeds()
{
	std::set<uint64_t> worlds;
	std::set<int> counts;
	for (uint64_t seed : {1ULL, 2ULL, 0xDEADBEEFULL, 123456789ULL, 987654321987ULL}) {
		Simulation s;
		s.init(defaultConfig(), seed);
		worlds.insert(s.world().checksum());
		counts.insert(int(s.ants().size()));
		CHECK(s.ants().size() >= 350 && s.ants().size() <= 650);
	}
	CHECK(worlds.size() == 5);
	CHECK(counts.size() >= 3);
	NOTE("5 seeds -> 5 distinct worlds, %zu distinct colony sizes", counts.size());
}

void determinism()
{
	Simulation a, b;
	a.init(defaultConfig(), 42);
	b.init(defaultConfig(), 42);
	a.stepN(ticksFor(600));
	b.stepN(ticksFor(600));
	CHECK(a.stateHash() == b.stateHash());
	Simulation c;
	c.init(defaultConfig(), 43);
	c.stepN(ticksFor(600));
	CHECK(c.stateHash() != a.stateHash());
	NOTE("same seed, 10 simulated minutes: identical state hash %016llx; different seed differs", (unsigned long long)a.stateHash());
}

void frameIndependence()
{
	// The visible app steps a variable number of fixed ticks per rendered frame (e.g. 1 or 2 at 60 Hz,
	// several after a hitch). The outcome must not depend on how ticks are batched.
	Simulation a, b, c;
	a.init(defaultConfig(), 7);
	b.init(defaultConfig(), 7);
	c.init(defaultConfig(), 7);
	const int total = ticksFor(300);
	for (int i = 0; i < total; ++i)
		a.step();
	for (int done = 0; done < total;) {
		const int n = std::min(total - done, 1 + (done / 7) % 5);
		b.stepN(n);
		done += n;
	}
	for (int done = 0; done < total;) {
		const int n = std::min(total - done, 60);
		c.stepN(n);
		done += n;
	}
	CHECK(a.stateHash() == b.stateHash());
	CHECK(a.stateHash() == c.stateHash());
	NOTE("1-tick, varied and 60-tick batches give the same state after 5 simulated minutes");
}

void excavationRulesAndNavigation()
{
	// Every tick: removed material must come from a face (exposed before the tick) within reach of a
	// worker, stone/root/spoil are never removed, and no worker stands in solid material.
	Simulation s;
	s.init(defaultConfig(), 99);
	const World &w = s.world();
	const int W = w.width(), H = w.height();
	std::vector<uint16_t> prev = w.remainingVolumes();
	std::vector<uint8_t> matPrev = w.materials();
	int removals = 0;
	const float reach = s.config().digReach + s.config().antLength * 0.6f + 1.5f;
	for (int t = 0; t < ticksFor(1800); ++t) {
		s.step();
		const auto &now = w.remainingVolumes();
		for (int i = 0; i < W * H; ++i) {
			if (now[size_t(i)] >= prev[size_t(i)])
				continue;
			if (w.material(i) == Material::Spoil || matPrev[size_t(i)] == uint8_t(Material::Spoil))
				continue; // spoil slumping on the surface (volume-preserving move)
			++removals;
			const int x = i % W, y = i / W;
			CHECK(isDiggable(w.material(i)) || w.material(i) == Material::Spoil); // soil, or packed backfill
			CHECK(!w.wasOriginallyAir(i));
			bool exposed = false;
			for (auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
				if (x + dx >= 0 && y + dy >= 0 && x + dx < W && y + dy < H && prev[size_t((y + dy) * W + x + dx)] == 0)
					exposed = true;
			CHECK(exposed);
			bool near = false;
			for (const Ant &a : s.ants())
				if (std::hypot(a.x - (float(x) + 0.5f), a.y - (float(y) + 0.5f)) <= reach)
					near = true;
			CHECK(near);
		}
		for (const Ant &a : s.ants()) {
			CHECK(a.x >= 0 && a.y >= 0 && a.x < float(W) && a.y < float(H));
			if (!w.isOpen(int(a.x), int(a.y))) {
				const int ci = w.index(int(a.x), int(a.y));
				std::printf("  ant %u (%s) at %.2f,%.2f in material %d remaining %d at t=%.1f s\n", a.id, stateName(a.state), a.x, a.y,
				            int(w.material(ci)), int(w.remaining(ci)), s.seconds());
				CHECK(w.isOpen(int(a.x), int(a.y)));
				return;
			}
		}
		prev = now;
		matPrev = w.materials();
	}
	CHECK(removals > 0);
	NOTE("30 simulated minutes: %d removals, all from exposed faces within reach; no worker inside solid soil", removals);
}

void conservation()
{
	Simulation s;
	s.init(defaultConfig(), 1234);
	for (int k = 0; k < 120; ++k) {
		s.stepN(ticksFor(60));
		const Stats st = s.stats();
		CHECK(st.excavatedVolume == st.depositedVolume + st.carriedVolume);
	}
	const Stats st = s.stats();
	NOTE("2 simulated hours: excavated %lld units = deposited %lld + carried %lld (exact)", (long long)st.excavatedVolume,
	     (long long)st.depositedVolume, (long long)st.carriedVolume);
	CHECK(st.excavatedVolume > 0);
}

void congestion()
{
	// Measure how long any worker that is trying to move goes without meaningful progress.
	Simulation s;
	s.init(defaultConfig(), 555);
	std::vector<float> lastX, lastY, stillFor;
	for (const Ant &a : s.ants()) {
		lastX.push_back(a.x);
		lastY.push_back(a.y);
		stillFor.push_back(0);
	}
	float worst = 0;
	int worstAnt = -1;
	float wx = 0, wy = 0;
	AntState ws = AntState::Explore;
	bool wAbove = false;
	float wTime = 0;
	const float dt = 1.0f / kTps;
	for (int t = 0; t < ticksFor(3 * 3600); ++t) {
		s.step();
		for (const Ant &a : s.ants()) {
			const bool moving = a.state == AntState::Explore || a.state == AntState::FollowTrail || a.state == AntState::CarrySoilOut ||
			                    a.state == AntState::SearchForFood || a.state == AntState::CarryFood;
			const size_t i = a.id;
			if (!moving || std::hypot(a.x - lastX[i], a.y - lastY[i]) > 2.0f) {
				lastX[i] = a.x;
				lastY[i] = a.y;
				stillFor[i] = 0;
				continue;
			}
			stillFor[i] += dt;
			if (stillFor[i] > worst) {
				worst = stillFor[i];
				worstAnt = int(i);
				wx = a.x;
				wy = a.y;
				ws = a.state;
				wAbove = s.world().isAboveGround(int(a.x), int(a.y));
				wTime = float(s.seconds());
			}
		}
	}
	const Stats st = s.stats();
	NOTE("3 simulated hours: longest time a moving worker stayed within 2 cells = %.1f s (ant %d, %s at %.1f,%.1f, %s, until t=%.0f s); %d stuck recoveries, %d task resets",
	     worst, worstAnt, stateName(ws), wx, wy, wAbove ? "above ground" : "underground", wTime, st.stuckRecoveries, st.taskResets);
	CHECK(worst < 120.0f); // no permanent collective deadlock
}

void persistence()
{
	Simulation a;
	a.init(defaultConfig(), 2024);
	a.stepN(ticksFor(3600));
	const std::vector<uint8_t> ck = a.saveCheckpoint();
	a.stepN(ticksFor(1800));
	Simulation b;
	b.init(defaultConfig(), 1); // a different colony, replaced by the checkpoint
	std::string err;
	CHECK(b.loadCheckpoint(ck, &err));
	CHECK(b.tick() == uint64_t(ticksFor(3600)));
	b.stepN(ticksFor(1800));
	CHECK(a.stateHash() == b.stateHash());
	// Corruption is detected.
	std::vector<uint8_t> bad = ck;
	bad[bad.size() / 2] ^= 0x5A;
	Simulation c;
	c.init(defaultConfig(), 3);
	CHECK(!c.loadCheckpoint(bad, &err));
	NOTE("checkpoint %.1f MB; restored run matches the original after 30 more minutes; corrupted file rejected (%s)",
	     double(ck.size()) / 1e6, err.c_str());
}

void pacing()
{
	int seeds = 3;
	if (const char *e = std::getenv("ANTFARM_PACING_SEEDS"))
		seeds = std::max(1, std::atoi(e));
	std::vector<std::vector<double>> curves;
	for (int k = 0; k < seeds; ++k) {
		const uint64_t seed = 0x5EED0000ULL + uint64_t(k) * 7919;
		Simulation s;
		s.init(defaultConfig(), seed);
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<double> frac;
		const size_t mem0 = s.memoryFootprint();
		int pelletsPrev = 0;
		std::vector<int> pelletsPerHour;
		s.stepN(ticksFor(600));
		const double early = s.stats().excavatedFraction;
		const int earlyPellets = s.stats().pellets;
		for (int h = 1; h <= 24; ++h) {
			s.stepN(ticksFor(3600) - (h == 1 ? ticksFor(600) : 0));
			const Stats st = s.stats();
			frac.push_back(st.excavatedFraction);
			pelletsPerHour.push_back(st.pellets - pelletsPrev);
			pelletsPrev = st.pellets;
			CHECK(st.excavatedVolume == st.depositedVolume + st.carriedVolume);
			CHECK(s.foodCaches().size() <= 24);
			CHECK(s.memoryFootprint() <= mem0 + 4096);
		}
		const Stats st = s.stats();
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		NOTE("seed %llx: %d ants; 10 min %.2f%% (%d pellets); 1h %.1f%% 4h %.1f%% 8h %.1f%% 12h %.1f%% 18h %.1f%% 24h %.1f%%; depth %d, "
		     "entrances %d, pellets in hour 24: %d, stored food %d, station %d; %.0f s wall",
		     (unsigned long long)seed, st.ants, early * 100, earlyPellets, frac[0] * 100, frac[3] * 100, frac[7] * 100, frac[11] * 100,
		     frac[17] * 100, frac[23] * 100, st.maxDepth, st.entrances, pelletsPerHour[23], st.storedFood, st.stationFood, secs);
		CHECK(earlyPellets >= 20);                   // excavation is noticeable within the first minutes
		CHECK(frac[23] < 0.5);                       // most substrate remains intact after a day
		CHECK(frac[23] > 0.025);                     // but the colony has built a real network (single-entrance colonies dig ~3%)
		CHECK(frac[3] > frac[0] && frac[11] > frac[3] && frac[23] > frac[11]); // keeps developing
		CHECK(pelletsPerHour[23] > 30);              // still working late in the session
		CHECK(st.maxDepth > 60);
		curves.push_back(frac);
	}
	// Different seeds develop differently.
	if (curves.size() >= 2)
		CHECK(std::fabs(curves[0][23] - curves[1][23]) > 1e-4);
}

struct TestCase {
	const char *name;
	std::function<void()> fn;
};

} // namespace

int main(int argc, char **argv)
{
	const std::vector<TestCase> tests = {
		{"fresh_seeds", freshSeeds},
		{"determinism", determinism},
		{"frame_independence", frameIndependence},
		{"excavation_and_navigation", excavationRulesAndNavigation},
		{"conservation", conservation},
		{"congestion", congestion},
		{"persistence", persistence},
		{"pacing_24h", pacing},
	};
	std::set<std::string> want(argv + 1, argv + argc);
	int ran = 0;
	for (const TestCase &t : tests) {
		if (!want.empty() && !want.count(t.name))
			continue;
		g_current = t.name;
		const int before = g_failures;
		const auto t0 = std::chrono::steady_clock::now();
		std::printf("[ RUN  ] %s\n", t.name);
		std::fflush(stdout);
		t.fn();
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("[ %s ] %s (%.1f s)\n", g_failures == before ? " OK " : "FAIL", t.name, s);
		std::fflush(stdout);
		++ran;
	}
	std::printf("\n%d test(s), %d failure(s)\n", ran, g_failures);
	return g_failures ? 1 : 0;
}
