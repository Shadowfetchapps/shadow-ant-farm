#include "ant_farm_sim.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

using namespace godot;
using namespace antfarm;

namespace {

constexpr float kPi = 3.14159265358979f;

float wrap(float a)
{
	while (a > kPi)
		a -= 2.0f * kPi;
	while (a < -kPi)
		a += 2.0f * kPi;
	return a;
}

uint64_t osRandomSeed()
{
	uint64_t v = 0;
	if (getrandom(&v, sizeof v, 0) != sizeof v) {
		if (std::FILE *f = std::fopen("/dev/urandom", "rb")) {
			if (std::fread(&v, sizeof v, 1, f) != 1)
				v = 0;
			std::fclose(f);
		}
	}
	return v ? v : 0x9E3779B97F4A7C15ull;
}

std::string toStd(const String &s)
{
	const CharString c = s.utf8();
	return std::string(c.get_data(), size_t(c.length()));
}

// Felzenszwalb & Huttenlocher 1-D squared distance transform (f = 0 at sources, kInf elsewhere).
constexpr double kInf = 1e20;
void edt1d(const double *f, int n, double *d, int *v, double *z)
{
	int k = 0;
	v[0] = 0;
	z[0] = -1e30;
	z[1] = 1e30;
	for (int q = 1; q < n; ++q) {
		double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
		while (s <= z[k]) {
			--k;
			s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
		}
		++k;
		v[k] = q;
		z[k] = s;
		z[k + 1] = 1e30;
	}
	k = 0;
	for (int q = 0; q < n; ++q) {
		while (z[k + 1] < double(q))
			++k;
		const double dq = double(q - v[k]);
		d[q] = dq * dq + f[v[k]];
	}
}

} // namespace

AntFarmSim::AntFarmSim() = default;
AntFarmSim::~AntFarmSim()
{
	if (m_terrainJob.valid())
		m_terrainJob.wait();
}

bool AntFarmSim::start_new(int64_t seed, const String &config_path)
{
	SimConfig cfg;
	if (!config_path.is_empty()) {
		std::string err;
		if (!cfg.loadFile(toStd(config_path), &err)) {
			m_error = String::utf8(err.c_str());
			return false;
		}
	}
	auto sim = std::make_unique<Simulation>();
	const uint64_t s = seed == 0 ? osRandomSeed() : uint64_t(seed);
	if (!sim->init(cfg, s)) {
		m_error = "could not create the colony";
		return false;
	}
	if (m_terrainJob.valid())
		m_terrainJob.wait();
	m_sim = std::move(sim);
	m_accum = 0;
	m_droppedTicks = 0;
	m_eventCursor = m_sim->events().written();
	m_terrainDirty = true;
	return true;
}

bool AntFarmSim::load_checkpoint(const String &path)
{
	std::FILE *f = std::fopen(toStd(path).c_str(), "rb");
	if (!f) {
		m_error = "checkpoint not found";
		return false;
	}
	std::vector<uint8_t> data;
	uint8_t buf[1 << 16];
	for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;)
		data.insert(data.end(), buf, buf + got);
	std::fclose(f);
	auto sim = std::make_unique<Simulation>();
	std::string err;
	if (!sim->loadCheckpoint(data, &err)) {
		m_error = String::utf8(err.c_str());
		return false;
	}
	if (m_terrainJob.valid())
		m_terrainJob.wait();
	m_sim = std::move(sim);
	m_accum = 0;
	m_eventCursor = m_sim->events().written();
	m_terrainDirty = true;
	return true;
}

bool AntFarmSim::save_checkpoint(const String &path) const
{
	if (!m_sim)
		return false;
	const std::vector<uint8_t> data = m_sim->saveCheckpoint();
	const std::string final = toStd(path);
	const std::string tmp = final + ".tmp";
	const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	size_t off = 0;
	while (off < data.size()) {
		const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
		if (w <= 0) {
			::close(fd);
			::unlink(tmp.c_str());
			return false;
		}
		off += size_t(w);
	}
	const bool synced = ::fsync(fd) == 0;
	::close(fd);
	if (!synced || std::rename(tmp.c_str(), final.c_str()) != 0) {
		::unlink(tmp.c_str());
		return false;
	}
	const size_t slash = final.find_last_of('/');
	if (slash != std::string::npos) {
		const int dfd = ::open(final.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dfd >= 0) {
			::fsync(dfd);
			::close(dfd);
		}
	}
	return true;
}

int AntFarmSim::advance(double real_dt, int max_ticks)
{
	if (!m_sim)
		return 0;
	const double tps = m_sim->config().ticksPerSecond;
	// Gaps longer than a couple of seconds (system suspend, a stalled display) are not replayed at once;
	// the skipped time is counted instead.
	if (real_dt > 2.5)
		m_droppedTicks += int64_t((real_dt - 2.5) * tps);
	m_accum += std::clamp(real_dt, 0.0, 2.5);
	int n = int(std::floor(m_accum * tps));
	if (n > max_ticks) {
		m_droppedTicks += n - max_ticks;
		n = max_ticks;
		m_accum = 0.0;
	} else {
		m_accum -= double(n) / tps;
	}
	for (int i = 0; i < n; ++i)
		m_sim->step();
	return n;
}

int AntFarmSim::step_ticks(int n)
{
	if (!m_sim)
		return 0;
	for (int i = 0; i < n; ++i)
		m_sim->step();
	return n;
}

double AntFarmSim::get_alpha() const
{
	return m_sim ? std::clamp(m_accum * m_sim->config().ticksPerSecond, 0.0, 1.0) : 0.0;
}

double AntFarmSim::get_sim_seconds() const { return m_sim ? m_sim->seconds() : 0.0; }
int64_t AntFarmSim::get_tick() const { return m_sim ? int64_t(m_sim->tick()) : 0; }
int64_t AntFarmSim::get_seed() const { return m_sim ? int64_t(m_sim->seed()) : 0; }
int AntFarmSim::get_ticks_per_second() const { return m_sim ? m_sim->config().ticksPerSecond : 30; }

Vector2i AntFarmSim::get_grid_size() const
{
	return m_sim ? Vector2i(m_sim->world().width(), m_sim->world().height()) : Vector2i();
}

void AntFarmSim::startTerrainJob()
{
	const World &w = m_sim->world();
	TerrainSnapshot t;
	t.w = w.width();
	t.h = w.height();
	t.cellVolume = w.cellVolume();
	t.material = w.materials();
	t.remaining = w.remainingVolumes();
	t.moisture = w.moistures();
	const size_t n = size_t(t.w) * size_t(t.h);
	t.origAir.resize(n);
	for (size_t i = 0; i < n; ++i)
		t.origAir[i] = w.wasOriginallyAir(int(i)) ? 1 : 0;
	m_sim->worldMutable().clearDirty();
	m_terrainDirty = false;
	m_terrainJob = std::async(std::launch::async, [this, t = std::move(t)]() { return buildTerrain(t); });
}

std::vector<uint8_t> AntFarmSim::buildTerrain(const TerrainSnapshot &t)
{
	const int W = t.w, H = t.h;
	const size_t n = size_t(W) * size_t(H);
	m_distA.resize(n);
	m_distB.resize(n);
	const int m = std::max(W, H);
	m_f.resize(size_t(m));
	m_d.resize(size_t(m));
	m_z.resize(size_t(m) + 1);
	m_v.resize(size_t(m));
	// A: squared distance to the nearest open cell; B: to the nearest solid cell.
	for (size_t i = 0; i < n; ++i) {
		const bool open = t.remaining[i] == 0;
		m_distA[i] = open ? 0.0 : kInf;
		m_distB[i] = open ? kInf : 0.0;
	}
	for (std::vector<double> *grid : {&m_distA, &m_distB}) {
		std::vector<double> &g = *grid;
		for (int x = 0; x < W; ++x) {
			for (int y = 0; y < H; ++y)
				m_f[size_t(y)] = g[size_t(y * W + x)];
			edt1d(m_f.data(), H, m_d.data(), m_v.data(), m_z.data());
			for (int y = 0; y < H; ++y)
				g[size_t(y * W + x)] = m_d[size_t(y)];
		}
		for (int y = 0; y < H; ++y) {
			for (int x = 0; x < W; ++x)
				m_f[size_t(x)] = g[size_t(y * W + x)];
			edt1d(m_f.data(), W, m_d.data(), m_v.data(), m_z.data());
			for (int x = 0; x < W; ++x)
				g[size_t(y * W + x)] = m_d[size_t(x)];
		}
	}
	// Signed distance (cells, solid positive), then a small separable blur so passage walls are smooth
	// curves rather than the outline of grid cells.
	m_sdf.resize(n);
	m_sdfTmp.resize(n);
	for (size_t i = 0; i < n; ++i) {
		const bool open = t.remaining[i] == 0;
		m_sdf[i] = open ? -(float(std::sqrt(std::min(m_distB[i], 400.0))) - 0.5f) : (float(std::sqrt(std::min(m_distA[i], 400.0))) - 0.5f);
	}
	static const float k[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			float acc = 0;
			for (int tt = -2; tt <= 2; ++tt)
				acc += k[tt + 2] * m_sdf[size_t(y * W + std::clamp(x + tt, 0, W - 1))];
			m_sdfTmp[size_t(y * W + x)] = acc;
		}
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			float acc = 0;
			for (int tt = -2; tt <= 2; ++tt)
				acc += k[tt + 2] * m_sdfTmp[size_t(std::clamp(y + tt, 0, H - 1) * W + x)];
			m_sdf[size_t(y * W + x)] = acc;
		}
	std::vector<uint8_t> out(n * 4);
	const float vol = float(t.cellVolume);
	for (size_t i = 0; i < n; ++i) {
		const bool open = t.remaining[i] == 0;
		const int a = std::clamp(int(std::lround(128.0f + m_sdf[i] * 12.0f)), 0, 255);
		out[i * 4 + 0] = uint8_t(int(t.material[i]) * 32 + (t.origAir[i] ? 16 : 0) + (open ? 8 : 0));
		out[i * 4 + 1] = uint8_t(std::clamp(int(float(t.remaining[i]) / vol * 255.0f + 0.5f), 0, 255));
		out[i * 4 + 2] = t.moisture[i];
		out[i * 4 + 3] = uint8_t(a);
	}
	return out;
}

PackedByteArray AntFarmSim::take_terrain_update(bool force)
{
	PackedByteArray out;
	if (!m_sim)
		return out;
	auto toPacked = [&out](const std::vector<uint8_t> &bytes) {
		out.resize(int64_t(bytes.size()));
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	};
	if (force) {
		// Synchronous: the caller needs the current terrain now (start-up, captures).
		if (m_terrainJob.valid())
			m_terrainJob.get();
		startTerrainJob();
		toPacked(m_terrainJob.get());
		return out;
	}
	bool dirty = m_terrainDirty;
	for (uint8_t d : m_sim->world().dirtyChunks())
		dirty = dirty || d != 0;
	if (m_terrainJob.valid()) {
		if (m_terrainJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return out;
		toPacked(m_terrainJob.get());
		if (dirty)
			startTerrainJob(); // the world changed while this one was being built
		return out;
	}
	if (dirty)
		startTerrainJob();
	return out;
}

void AntFarmSim::_notification(int what)
{
	if (what == NOTIFICATION_PREDELETE && m_terrainJob.valid())
		m_terrainJob.wait();
}

PackedFloat32Array AntFarmSim::build_ant_buffer(double alpha, double cell_size, double z)
{
	PackedFloat32Array out;
	if (!m_sim)
		return out;
	const auto &ants = m_sim->ants();
	const SimConfig &cfg = m_sim->config();
	const float W = float(m_sim->world().width()), H = float(m_sim->world().height());
	const float cs = float(cell_size), al = float(std::clamp(alpha, 0.0, 1.0));
	out.resize(int64_t(ants.size() * 16));
	float *o = out.ptrw();
	for (const Ant &a : ants) {
		const float x = a.px + (a.x - a.px) * al;
		const float y = a.py + (a.y - a.py) * al;
		const float h = a.pheading + wrap(a.heading - a.pheading) * al;
		const float phi = -h;
		const float s = cfg.antLength * a.t.size * cs;
		const float c = std::cos(phi) * s, sn = std::sin(phi) * s;
		// Basis columns: x (forward) = (c, sn, 0), y (left) = (-sn, c, 0), z (dorsal) = (0, 0, s).
		o[0] = c;
		o[1] = -sn;
		o[2] = 0.0f;
		o[3] = (x - W * 0.5f) * cs;
		o[4] = sn;
		o[5] = c;
		o[6] = 0.0f;
		o[7] = (H * 0.5f - y) * cs;
		o[8] = 0.0f;
		o[9] = 0.0f;
		o[10] = s;
		o[11] = float(z);
		o[12] = a.gait;
		o[13] = float(int(a.anim)) + 8.0f * float(int(a.load));
		o[14] = std::clamp(a.speed / cfg.walkSpeed, 0.0f, 2.0f);
		o[15] = float((a.id * 2654435761u) >> 8) / float(1u << 24);
		o += 16;
	}
	return out;
}

PackedFloat32Array AntFarmSim::build_load_buffer(double alpha, double cell_size, double z)
{
	PackedFloat32Array out;
	m_loadCount = 0;
	if (!m_sim)
		return out;
	const auto &ants = m_sim->ants();
	const SimConfig &cfg = m_sim->config();
	const float W = float(m_sim->world().width()), H = float(m_sim->world().height());
	const float cs = float(cell_size), al = float(std::clamp(alpha, 0.0, 1.0));
	out.resize(int64_t(ants.size() * 16));
	float *o = out.ptrw();
	for (const Ant &a : ants) {
		if (a.load == Load::None)
			continue;
		const float x = a.px + (a.x - a.px) * al;
		const float y = a.py + (a.y - a.py) * al;
		const float h = a.pheading + wrap(a.heading - a.pheading) * al;
		const float reach = cfg.antLength * a.t.size * 0.56f;
		const float lx = x + std::cos(h) * reach, ly = y + std::sin(h) * reach;
		const float s = (a.load == Load::Soil ? 1.05f : 0.8f) * cs * a.t.size;
		const float phi = -h;
		const float c = std::cos(phi) * s, sn = std::sin(phi) * s;
		o[0] = c;
		o[1] = -sn;
		o[2] = 0.0f;
		o[3] = (lx - W * 0.5f) * cs;
		o[4] = sn;
		o[5] = c;
		o[6] = 0.0f;
		o[7] = (H * 0.5f - ly) * cs;
		o[8] = 0.0f;
		o[9] = 0.0f;
		o[10] = s * 0.8f;
		o[11] = float(z) + 0.35f * s;
		o[12] = float(int(a.load));
		o[13] = float((a.id * 2246822519u) >> 8) / float(1u << 24);
		o[14] = 0.0f;
		o[15] = 0.0f;
		o += 16;
		++m_loadCount;
	}
	return out;
}

PackedFloat32Array AntFarmSim::poll_events(int max_events)
{
	PackedFloat32Array out;
	if (!m_sim)
		return out;
	m_eventScratch.clear();
	m_eventCursor = m_sim->events().read(m_eventCursor, m_eventScratch, size_t(std::max(0, max_events)));
	out.resize(int64_t(m_eventScratch.size() * 4));
	float *o = out.ptrw();
	for (const Event &e : m_eventScratch) {
		o[0] = float(int(e.type));
		o[1] = e.x;
		o[2] = e.y;
		o[3] = e.strength;
		o += 4;
	}
	return out;
}

PackedFloat32Array AntFarmSim::get_food() const
{
	PackedFloat32Array out;
	if (!m_sim)
		return out;
	out.push_back(m_sim->stationX());
	out.push_back(m_sim->stationY());
	out.push_back(float(m_sim->stationFood()));
	for (const FoodCache &c : m_sim->foodCaches()) {
		out.push_back(c.x);
		out.push_back(c.y);
		out.push_back(float(c.amount));
	}
	return out;
}

PackedFloat32Array AntFarmSim::get_entrances() const
{
	PackedFloat32Array out;
	if (m_sim)
		for (int e : m_sim->entrances())
			out.push_back(float(e));
	return out;
}

Dictionary AntFarmSim::get_stats() const
{
	Dictionary d;
	if (!m_sim)
		return d;
	const Stats s = m_sim->stats();
	d["tick"] = int64_t(s.tick);
	d["seconds"] = s.seconds;
	d["ants"] = s.ants;
	d["excavated_fraction"] = s.excavatedFraction;
	d["excavated_volume"] = s.excavatedVolume;
	d["deposited_volume"] = s.depositedVolume;
	d["carried_volume"] = s.carriedVolume;
	d["open_cells"] = s.openUndergroundCells;
	d["entrances"] = s.entrances;
	d["max_depth"] = s.maxDepth;
	d["station_food"] = s.stationFood;
	d["stored_food"] = s.storedFood;
	d["carrying_soil"] = s.carryingSoil;
	d["carrying_food"] = s.carryingFood;
	d["pellets"] = s.pellets;
	d["deposits"] = s.deposits;
	d["backfills"] = s.backfills;
	d["stuck_recoveries"] = s.stuckRecoveries;
	d["task_resets"] = s.taskResets;
	d["events_written"] = int64_t(s.eventsWritten);
	d["events_dropped"] = int64_t(s.eventsDropped);
	d["dig_demand"] = s.digDemand;
	d["dropped_ticks"] = m_droppedTicks;
	Dictionary states;
	for (int k = 0; k < int(AntState::Count); ++k)
		states[String(stateName(AntState(k)))] = s.byState[size_t(k)];
	d["states"] = states;
	return d;
}

String AntFarmSim::get_config_dump() const
{
	return m_sim ? String::utf8(m_sim->config().dump().c_str()) : String();
}

String AntFarmSim::get_state_hash() const
{
	if (!m_sim)
		return String();
	char buf[24];
	std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)m_sim->stateHash());
	return String(buf);
}

int64_t AntFarmSim::get_memory_bytes() const
{
	if (!m_sim)
		return 0;
	return int64_t(m_sim->memoryFootprint() + (m_distA.capacity() + m_distB.capacity()) * sizeof(double) + m_rgba.capacity());
}

void AntFarmSim::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("start_new", "seed", "config_path"), &AntFarmSim::start_new);
	ClassDB::bind_method(D_METHOD("load_checkpoint", "path"), &AntFarmSim::load_checkpoint);
	ClassDB::bind_method(D_METHOD("save_checkpoint", "path"), &AntFarmSim::save_checkpoint);
	ClassDB::bind_method(D_METHOD("get_last_error"), &AntFarmSim::get_last_error);
	ClassDB::bind_method(D_METHOD("is_running"), &AntFarmSim::is_running);
	ClassDB::bind_method(D_METHOD("advance", "real_dt", "max_ticks"), &AntFarmSim::advance);
	ClassDB::bind_method(D_METHOD("step_ticks", "n"), &AntFarmSim::step_ticks);
	ClassDB::bind_method(D_METHOD("get_alpha"), &AntFarmSim::get_alpha);
	ClassDB::bind_method(D_METHOD("get_sim_seconds"), &AntFarmSim::get_sim_seconds);
	ClassDB::bind_method(D_METHOD("get_tick"), &AntFarmSim::get_tick);
	ClassDB::bind_method(D_METHOD("get_seed"), &AntFarmSim::get_seed);
	ClassDB::bind_method(D_METHOD("get_dropped_ticks"), &AntFarmSim::get_dropped_ticks);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &AntFarmSim::get_grid_size);
	ClassDB::bind_method(D_METHOD("get_ticks_per_second"), &AntFarmSim::get_ticks_per_second);
	ClassDB::bind_method(D_METHOD("take_terrain_update", "force"), &AntFarmSim::take_terrain_update);
	ClassDB::bind_method(D_METHOD("build_ant_buffer", "alpha", "cell_size", "z"), &AntFarmSim::build_ant_buffer);
	ClassDB::bind_method(D_METHOD("build_load_buffer", "alpha", "cell_size", "z"), &AntFarmSim::build_load_buffer);
	ClassDB::bind_method(D_METHOD("get_load_count"), &AntFarmSim::get_load_count);
	ClassDB::bind_method(D_METHOD("poll_events", "max_events"), &AntFarmSim::poll_events);
	ClassDB::bind_method(D_METHOD("get_food"), &AntFarmSim::get_food);
	ClassDB::bind_method(D_METHOD("get_entrances"), &AntFarmSim::get_entrances);
	ClassDB::bind_method(D_METHOD("get_stats"), &AntFarmSim::get_stats);
	ClassDB::bind_method(D_METHOD("get_config_dump"), &AntFarmSim::get_config_dump);
	ClassDB::bind_method(D_METHOD("get_state_hash"), &AntFarmSim::get_state_hash);
	ClassDB::bind_method(D_METHOD("get_memory_bytes"), &AntFarmSim::get_memory_bytes);
}
