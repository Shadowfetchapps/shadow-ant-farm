#include <iterator>
#include <algorithm>
// Headless simulation tool: runs the same core as the application at accelerated speed (more fixed ticks
// per wall-clock second, same timestep) and writes diagnostic maps of the excavation.
//
//   antfarm_sim --seed 0x1234 --hours 24 --maps out/ --map-hours 0,0.1,1,4,8,12,18,24
//   antfarm_sim --seed 7 --hours 2 --checkpoint out/ck.bin

#include "antfarm/sim.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace antfarm;

namespace {

void writeMap(const Simulation &s, const std::string &path)
{
	const World &w = s.world();
	const int W = w.width(), H = w.height();
	std::vector<uint8_t> px(size_t(W * H * 3));
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const int i = w.index(x, y);
			uint8_t r, g, b;
			const float solid = float(w.remaining(i)) / float(w.cellVolume());
			switch (w.material(i)) {
			case Material::Air: r = 220, g = 226, b = 232; break;
			case Material::Sand: r = 201, g = 170, b = 122; break;
			case Material::Loam: r = 128, g = 92, b = 60; break;
			case Material::Clay: r = 156, g = 110, b = 80; break;
			case Material::Gravel: r = 150, g = 140, b = 125; break;
			case Material::Stone: r = 110, g = 110, b = 105; break;
			case Material::Root: r = 90, g = 60, b = 35; break;
			case Material::Spoil: r = 176, g = 140, b = 96; break;
			default: r = g = b = 255;
			}
			if (w.material(i) != Material::Air && solid < 1.0f && !w.wasOriginallyAir(i)) {
				const float f = 0.25f + 0.75f * solid;
				r = uint8_t(float(r) * f * 0.45f), g = uint8_t(float(g) * f * 0.42f), b = uint8_t(float(b) * f * 0.4f);
			} else if (w.material(i) == Material::Air || (w.wasOriginallyAir(i) && solid == 0)) {
				r = 220, g = 226, b = 232;
			}
			px[size_t(i * 3)] = r;
			px[size_t(i * 3 + 1)] = g;
			px[size_t(i * 3 + 2)] = b;
		}
	for (const Ant &a : s.ants()) {
		const int x = int(a.x), y = int(a.y);
		for (int k = -1; k <= 1; ++k) {
			const int ax = x + int(std::round(std::cos(a.heading) * float(k))), ay = y + int(std::round(std::sin(a.heading) * float(k)));
			if (ax >= 0 && ay >= 0 && ax < W && ay < H) {
				const size_t i = size_t((ay * W + ax) * 3);
				px[i] = a.load == Load::Soil ? 200 : a.load == Load::Food ? 30 : 15;
				px[i + 1] = a.load == Load::Food ? 160 : 12;
				px[i + 2] = 10;
			}
		}
	}
	std::ofstream f(path, std::ios::binary);
	f << "P6\n" << W << " " << H << "\n255\n";
	f.write(reinterpret_cast<const char *>(px.data()), std::streamsize(px.size()));
}

} // namespace

int main(int argc, char **argv)
{
	uint64_t seed = 1;
	double hours = 1;
	std::string maps, checkpointOut, configPath, loadPath, checkpointDir;
	std::vector<double> checkpointHours;
	int dumpAnts = 0;
	double reportMinutes = 30;
	std::vector<double> mapHours = {0, 0.1, 1, 4, 8, 12, 18, 24};
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--seed")
			seed = std::stoull(next(), nullptr, 0);
		else if (a == "--hours")
			hours = std::stod(next());
		else if (a == "--maps")
			maps = next();
		else if (a == "--map-hours") {
			mapHours.clear();
			std::stringstream ss(next());
			std::string t;
			while (std::getline(ss, t, ','))
				mapHours.push_back(std::stod(t));
		} else if (a == "--checkpoint")
			checkpointOut = next();
		else if (a == "--dump-ants")
			dumpAnts = std::stoi(next());
		else if (a == "--config")
			configPath = next();
		else if (a == "--report-minutes")
			reportMinutes = std::stod(next());
		else if (a == "--load")
			loadPath = next();
		else if (a == "--checkpoint-dir")
			checkpointDir = next();
		else if (a == "--checkpoint-hours") {
			std::stringstream ss(next());
			std::string t;
			while (std::getline(ss, t, ','))
				checkpointHours.push_back(std::stod(t));
		}
		else {
			std::fprintf(stderr, "usage: antfarm_sim --seed N --hours H [--maps DIR] [--map-hours a,b,c] [--checkpoint FILE] [--config FILE] [--load CHECKPOINT]\n");
			return 2;
		}
	}
	SimConfig cfg;
	if (!configPath.empty()) {
		std::string err;
		if (!cfg.loadFile(configPath, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 2;
		}
	}
	Simulation s;
	s.init(cfg, seed);
	if (!loadPath.empty()) {
		std::ifstream f(loadPath, std::ios::binary);
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::string err;
		if (!s.loadCheckpoint(data, &err)) {
			std::fprintf(stderr, "cannot load %s: %s\n", loadPath.c_str(), err.c_str());
			return 2;
		}
		cfg = s.config();
		seed = s.seed();
		std::printf("loaded %s at %.2f h\n", loadPath.c_str(), s.seconds() / 3600.0);
	}
	std::printf("seed 0x%llx  ants %zu  grid %dx%d  config %016llx\n", (unsigned long long)seed, s.ants().size(), cfg.width, cfg.height,
	            (unsigned long long)cfg.hash());
	std::printf("%7s %7s %6s %6s %5s %4s %5s %5s %5s %5s %6s %5s\n", "hours", "excav%", "depth", "pellet", "entr", "dig", "carry", "rest",
	            "food", "store", "stuck", "wall");
	const auto t0 = std::chrono::steady_clock::now();
	std::set<size_t> mapsDone;
	const uint64_t total = uint64_t(hours * 3600.0 * cfg.ticksPerSecond);
	const uint64_t reportEvery = std::max<uint64_t>(1, uint64_t(double(cfg.ticksPerSecond) * 60.0 * reportMinutes));
	for (uint64_t t = 0; t <= total; ++t) {
		const double h = double(t) / cfg.ticksPerSecond / 3600.0;
		for (size_t k = 0; k < mapHours.size(); ++k)
			if (!maps.empty() && !mapsDone.count(k) && h >= mapHours[k]) {
				char name[64];
				std::snprintf(name, sizeof name, "/map_%05.2fh.ppm", mapHours[k]);
				writeMap(s, maps + name);
				mapsDone.insert(k);
			}
		for (double ch : checkpointHours)
			if (!checkpointDir.empty() && t == uint64_t(ch * 3600.0 * cfg.ticksPerSecond)) {
				char name[96];
				std::snprintf(name, sizeof name, "/colony_%05.2fh.antfarm", ch);
				const auto data = s.saveCheckpoint();
				std::ofstream f(checkpointDir + name, std::ios::binary);
				f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
			}
		if (t % reportEvery == 0) {
			const Stats st = s.stats();
			const int digging = st.byState[size_t(AntState::Excavate)] + st.byState[size_t(AntState::SelectDigFace)];
			std::printf("%7.2f %7.2f %6d %6d %5d %4d %5d %5d %5d %5d %6d %5.0f\n", h, st.excavatedFraction * 100, st.maxDepth, st.pellets,
			            st.entrances, digging, st.carryingSoil, st.byState[size_t(AntState::Rest)], st.stationFood, st.storedFood,
			            st.stuckRecoveries, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
			std::fflush(stdout);
		}
		if (t < total)
			s.step();
	}
	if (const char *tr = std::getenv("ANTFARM_TRACE")) {
		const uint32_t id = uint32_t(std::atoi(tr));
		{
			const Ant &a0 = s.ants()[id];
			for (int y = int(a0.y) - 8; y <= int(a0.y) + 8; ++y) {
				std::printf("   ");
				for (int x = int(a0.x) - 20; x <= int(a0.x) + 20; ++x) {
					char c = '#';
					if (s.world().isOpen(x, y)) {
						const unsigned d = s.exitDistance().at(x, y);
						c = d == 0xFFFF ? '?' : char('0' + d % 10);
					} else if (s.world().inBounds(x, y) && s.world().material(s.world().index(x, y)) == Material::Spoil)
						c = 's';
					for (const Ant &o : s.ants())
						if (int(o.x) == x && int(o.y) == y)
							c = o.id == id ? '@' : (o.load == Load::Soil ? 'c' : 'a');
					std::printf("%c", c);
				}
				std::printf("\n");
			}
		}
		for (int k = 0; k < 90; ++k) {
			const Ant &a = s.ants()[id];
			std::printf("t%d %-14s pos %.3f,%.3f head %.2f want %.2f spd %.2f hold %.2f pause %.2f yield %.2f ahead %d anim %d\n", k, stateName(a.state), a.x, a.y, a.heading, a.desiredHeading, a.speed, a.holdTime, a.pauseTime, a.yieldTime, int(a.aheadCount), int(a.anim));
			s.step();
		}
	}
	if (!checkpointOut.empty()) {
		const auto data = s.saveCheckpoint();
		std::ofstream f(checkpointOut, std::ios::binary);
		f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
	}
	for (int k = 0; k < dumpAnts && k < int(s.ants().size()); ++k) {
		const Ant &a = s.ants()[size_t(k * int(s.ants().size()) / std::max(1, dumpAnts))];
		const int gt = s.world().groundTop(int(a.x));
		std::printf("ant %3u %-15s task %d load %d pos %.1f,%.1f ground %d exit %u spd %.2f head %.2f want %.2f hold %.2f goal %.1f stuck %d active %.0f\n",
		            a.id, stateName(a.state), int(a.task), int(a.load), a.x, a.y, gt, unsigned(s.exitDistance().at(int(a.x), int(a.y))), a.speed,
		            a.heading, a.desiredHeading, a.holdTime, a.goalX, a.stuckStrikes, a.activeTimer);
	}
	int shown = 0;
	for (const Ant &a : s.ants()) {
		if (!dumpAnts || a.load != Load::Soil || a.stuckStrikes < 2 || shown >= 3)
			continue;
		++shown;
		std::printf("stuck carrier %u at %.1f,%.1f heading %.2f exit %u:\n", a.id, a.x, a.y, a.heading, unsigned(s.exitDistance().at(int(a.x), int(a.y))));
		for (int y = int(a.y) - 6; y <= int(a.y) + 6; ++y) {
			std::printf("   ");
			for (int x = int(a.x) - 12; x <= int(a.x) + 12; ++x) {
				char c = '#';
				if (s.world().isOpen(x, y))
					c = s.world().isAboveGround(x, y) ? '.' : ' ';
				else if (s.world().inBounds(x, y) && s.world().material(s.world().index(x, y)) == Material::Spoil)
					c = 's';
				for (const Ant &o : s.ants())
					if (int(o.x) == x && int(o.y) == y)
						c = o.id == a.id ? '@' : (o.load == Load::Soil ? 'c' : 'a');
				std::printf("%c", c);
			}
			std::printf("\n");
		}
	}
	{
		int below[int(AntState::Count)] = {}, above[int(AntState::Count)] = {}, tasks[4] = {};
		for (const Ant &a : s.ants()) {
			(s.world().isAboveGround(int(a.x), int(a.y)) ? above : below)[int(a.state)]++;
			tasks[int(a.task)]++;
		}
		std::printf("tasks: dig %d forage %d rest %d patrol %d\nstate            above  below\n", tasks[0], tasks[1], tasks[2], tasks[3]);
		for (int k = 0; k < int(AntState::Count); ++k)
			std::printf("  %-15s %5d %6d\n", stateName(AntState(k)), above[k], below[k]);
	}
	{
		int hist[8] = {};
		for (const Ant &a : s.ants())
			if (a.state == AntState::CarrySoilOut) {
				const unsigned d = s.exitDistance().at(int(a.x), int(a.y));
				hist[d == 0xFFFF ? 7 : std::min(6u, d / 10)]++;
			}
		std::vector<float> st;
		for (const Ant &a : s.ants())
			if (a.state == AntState::CarrySoilOut)
				st.push_back(a.stateTime);
		std::sort(st.begin(), st.end());
		if (!st.empty())
			std::printf("carrier state time: median %.0f s, p90 %.0f s, max %.0f s\n", st[st.size() / 2], st[st.size() * 9 / 10], st.back());
		const Ant *worst = nullptr;
		for (const Ant &a : s.ants())
			if (a.state == AntState::CarrySoilOut && (!worst || a.stateTime > worst->stateTime))
				worst = &a;
		if (worst && dumpAnts) {
			std::printf("worst carrier %u at %.2f,%.2f heading %.2f want %.2f speed %.2f hold %.2f pause %.2f yield %.2f ahead %d strikes %d\n", worst->id, worst->x, worst->y, worst->heading, worst->desiredHeading, worst->speed, worst->holdTime, worst->pauseTime, worst->yieldTime, int(worst->aheadCount), worst->stuckStrikes);
			for (int y = int(worst->y) - 12; y <= int(worst->y) + 12; ++y) {
				std::printf("   ");
				for (int x = int(worst->x) - 30; x <= int(worst->x) + 30; ++x) {
					char c = '#';
					if (s.world().isOpen(x, y)) {
						const unsigned d = s.exitDistance().at(x, y);
						c = d == 0xFFFF ? '?' : char('0' + d % 10);
					}
					for (const Ant &o : s.ants())
						if (int(o.x) == x && int(o.y) == y)
							c = o.id == worst->id ? '@' : (o.load == Load::Soil ? 'c' : 'a');
					std::printf("%c", c);
				}
				std::printf("\n");
			}
		}
		std::printf("carriers by exit distance (0-9,10-19,..,60+,unreach): %d %d %d %d %d %d %d %d\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
	}
	const Stats fin = s.stats();
	std::printf("face search: calls %d no-face-in-reach %d crowded %d none-acceptable %d declined %d started %d\n", fin.diag[0], fin.diag[1], fin.diag[2], fin.diag[3], fin.diag[4], fin.diag[5]);
	std::printf("deposits %d backfills %d\n", fin.deposits, fin.backfills);
	std::printf("active tips at end: %d\n", fin.activeTips);
	std::printf("stuck: carrier below/free %d below/traffic %d above %d; other below/free %d below/traffic %d above %d\n", fin.stuckKinds[0], fin.stuckKinds[1], fin.stuckKinds[2], fin.stuckKinds[3], fin.stuckKinds[4], fin.stuckKinds[5]);
	std::printf("dig selections: lump %d, surface %d, own face %d, active site %d, branch %d\n", fin.digReasons[1], fin.digReasons[2],
	            fin.digReasons[3], fin.digReasons[4], fin.digReasons[5]);
	std::printf("state hash %016llx\n", (unsigned long long)s.stateHash());
	return 0;
}
