#include "antfarm/sim.hpp"

#include "antfarm/bytes.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace antfarm {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

uint64_t fnv64(const uint8_t *p, size_t n)
{
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; ++i) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

float wrapAngle(float a)
{
	while (a > kPi)
		a -= kTwoPi;
	while (a < -kPi)
		a += kTwoPi;
	return a;
}

float clamp01(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

const char *stateName(AntState s)
{
	switch (s) {
	case AntState::Explore: return "EXPLORE";
	case AntState::FollowTrail: return "FOLLOW_TRAIL";
	case AntState::SelectDigFace: return "SELECT_DIG_FACE";
	case AntState::Excavate: return "EXCAVATE";
	case AntState::PickUpSoil: return "PICK_UP_SOIL";
	case AntState::CarrySoilOut: return "CARRY_SOIL_OUT";
	case AntState::DepositSoil: return "DEPOSIT_SOIL";
	case AntState::SearchForFood: return "SEARCH_FOR_FOOD";
	case AntState::CarryFood: return "CARRY_FOOD";
	case AntState::Groom: return "GROOM";
	case AntState::Rest: return "REST";
	default: return "?";
	}
}

// ---- events ------------------------------------------------------------------------------------

void EventQueue::init(int capacity)
{
	m_buf.assign(size_t(std::max(16, capacity)), Event{});
	m_written = 0;
	m_dropped = 0;
}

void EventQueue::push(const Event &e)
{
	m_buf[size_t(m_written % m_buf.size())] = e;
	++m_written;
}

uint64_t EventQueue::read(uint64_t from, std::vector<Event> &out, size_t maxCount) const
{
	const uint64_t cap = m_buf.size();
	if (m_written > cap && from < m_written - cap) {
		m_dropped += (m_written - cap) - from; // consumer fell behind; oldest events were overwritten
		from = m_written - cap;
	}
	while (from < m_written && out.size() < maxCount) {
		out.push_back(m_buf[size_t(from % cap)]);
		++from;
	}
	return from;
}

// ---- setup -------------------------------------------------------------------------------------

bool Simulation::init(const SimConfig &cfg, uint64_t seed)
{
	m_cfg = cfg;
	m_seed = seed;
	m_tick = 0;
	m_world.generate(cfg, deriveSeed(seed, Stream::Terrain));
	m_reservedBy.assign(size_t(cfg.width * cfg.height), -1);
	m_digTrail.init(cfg.width, cfg.height, cfg.trailCell, cfg.digTrailHalfLife, cfg.trailDiffusion, cfg.trailMax);
	m_foodTrail.init(cfg.width, cfg.height, cfg.trailCell, cfg.foodTrailHalfLife, cfg.trailDiffusion, cfg.trailMax);
	m_restTrail.init(cfg.width, cfg.height, cfg.trailCell, 240.0f, cfg.trailDiffusion * 0.5f, cfg.trailMax);
	m_siteTrail.init(cfg.width, cfg.height, cfg.trailCell, cfg.siteHalfLife, 0.0f, cfg.trailMax);
	m_events.init(cfg.eventCapacity);
	m_colonyRng.reseed(deriveSeed(seed, Stream::Behavior, 0xC0105), 11);
	m_bw = (cfg.width + kBucket - 1) / kBucket;
	m_bh = (cfg.height + kBucket - 1) / kBucket;
	m_bucketStart.assign(size_t(m_bw * m_bh + 1), 0);

	// Feeding station: a spot on the surface near one end of the farm, topped up by the keeper.
	const bool left = m_colonyRng.chance(0.5f);
	m_stationX = float(cfg.width) * (left ? m_colonyRng.range(0.07f, 0.16f) : m_colonyRng.range(0.84f, 0.93f));
	m_stationY = float(m_world.groundTop(int(m_stationX)) - 1);
	m_stationFood = cfg.initialFood;
	m_storedFood = 180; // the colony arrives fed
	m_nextFeeding = cfg.feedingIntervalHours * 3600.0;
	m_caches.clear();
	m_entrances.clear();
	m_anchors.clear();
	m_digDemand = 1;
	m_foodNeed = 0;
	m_exitDirty = true;
	m_lastExit = -10;
	m_lastTrail = 0;
	m_lastColony = -10;
	m_lastRelax = 0;
	m_lastStore = -100;
	m_storeX = m_storeY = -1;
	m_stuckRecoveries = m_taskResets = m_deposits = m_pellets = m_backfills = 0;

	rebuildTrailOpen();
	spawnAnts();
	m_exit.compute(m_world);
	updateColony(0);
	// Each newly introduced worker takes up a first task of its own.
	for (Ant &a : m_ants)
		chooseTask(a);
	return true;
}

void Simulation::rebuildTrailOpen()
{
	const int c = m_digTrail.cellSize();
	m_trailOpen.assign(size_t(m_digTrail.w() * m_digTrail.h()), 0);
	m_trailOpenList.clear();
	for (int y = 0; y < m_world.height(); ++y)
		for (int x = 0; x < m_world.width(); ++x)
			if (m_world.isOpen(x, y)) {
				const size_t i = size_t((y / c) * m_digTrail.w() + x / c);
				if (!m_trailOpen[i])
					m_trailOpenList.push_back(int(i));
				m_trailOpen[i] = 1;
			}
	std::sort(m_trailOpenList.begin(), m_trailOpenList.end());
}

void Simulation::markTrailOpen(int x, int y)
{
	const int c = m_digTrail.cellSize();
	const size_t i = size_t((y / c) * m_digTrail.w() + x / c);
	if (!m_trailOpen[i]) {
		m_trailOpen[i] = 1;
		m_trailOpenList.push_back(int(i));
	}
}

void Simulation::spawnAnts()
{
	Pcg32 rng(deriveSeed(m_seed, Stream::Behavior), 3);
	const int span = std::max(0, m_cfg.maxAnts - m_cfg.minAnts);
	const int count = m_cfg.minAnts + int(rng.below(uint32_t(span + 1)));
	m_ants.assign(size_t(count), Ant{});
	// Workers start on the surface around a release point (as if tipped in from a vial).
	const float cx = float(m_cfg.width) * rng.range(0.3f, 0.7f);
	const float v = m_cfg.traitVariation;
	for (int i = 0; i < count; ++i) {
		Ant &a = m_ants[size_t(i)];
		a.id = uint32_t(i);
		a.rng.reseed(deriveSeed(m_seed, Stream::Behavior, uint64_t(i) + 1), uint64_t(i) * 2 + 1);
		a.noiseSeed = a.rng.next();
		auto trait = [&](float spread) { return std::clamp(1.0f + a.rng.normal() * spread, 1.0f - 2.5f * spread, 1.0f + 2.5f * spread); };
		a.t.size = trait(v * 0.35f);
		a.t.speed = trait(v * 0.7f);
		a.t.persistence = trait(v * 1.2f);
		a.t.digPreference = trait(v * 1.8f);
		a.t.forageBias = trait(v * 1.8f);
		a.t.restScale = trait(v * 1.2f);
		a.t.strength = trait(v) * a.t.size;
		a.t.curiosity = trait(v * 1.5f);
		for (int attempt = 0; attempt < 200; ++attempt) {
			const float x = std::clamp(cx + a.rng.normal() * 70.0f, 8.0f, float(m_cfg.width - 9));
			const int gt = m_world.groundTop(int(x));
			const float y = float(gt) - 1.0f - a.rng.range(0.0f, 2.5f);
			if (m_world.isOpen(int(x), int(y))) {
				a.x = x;
				a.y = y;
				break;
			}
		}
		a.heading = a.rng.range(-kPi, kPi);
		a.desiredHeading = a.heading;
		a.px = a.x;
		a.py = a.y;
		a.pheading = a.heading;
		a.progX = a.x;
		a.progY = a.y;
		a.energy = a.rng.range(0.5f, 1.0f);
		// Newly introduced workers are active for a good while before the first rests (staggered).
		a.activeTimer = a.rng.range(m_cfg.activeMinutesMin, m_cfg.activeMinutesMax) * 60.0f * a.t.restScale * a.rng.range(0.7f, 1.3f);
		a.gait = a.rng.range(0.0f, kTwoPi);
		a.task = Task::Patrol;
		a.state = AntState::Explore;
		a.holdTime = a.rng.range(0.0f, 1.0f);
	}
}

// ---- main loop ---------------------------------------------------------------------------------

void Simulation::stepN(int n)
{
	for (int i = 0; i < n; ++i)
		step();
}

void Simulation::step()
{
	const float dt = 1.0f / float(m_cfg.ticksPerSecond);
	const double now = seconds();
	for (Ant &a : m_ants) {
		a.px = a.x;
		a.py = a.y;
		a.pheading = a.heading;
	}
	if (m_exitDirty && now - m_lastExit >= 1.0) {
		m_exit.compute(m_world);
		m_exitDirty = false;
		m_lastExit = now;
	}
	rebuildSpatialHash();
	{
		// How full the nest is: workers underground per the space they need. Derived every tick.
		int below = 0;
		for (const Ant &a : m_ants)
			below += aboveGround(a.x, a.y) ? 0 : 1;
		m_occupancy = float(below) / std::max(4.0f, float(m_world.openUndergroundCells()) / 7.0f);
	}
	for (Ant &a : m_ants)
		updateAnt(a, dt);

	if (now - m_lastTrail >= 1.0 / m_cfg.trailUpdateHz) {
		const float tdt = float(now - m_lastTrail);
		m_digTrail.update(m_trailOpen, m_trailOpenList, tdt);
		m_foodTrail.update(m_trailOpen, m_trailOpenList, tdt);
		m_restTrail.update(m_trailOpen, m_trailOpenList, tdt);
		m_siteTrail.update(m_trailOpen, m_trailOpenList, tdt);
		m_lastTrail = now;
	}
	if (now - m_lastColony >= 2.0) {
		updateColony(float(now - m_lastColony));
		m_lastColony = now;
	}
	if (now - m_lastRelax >= 1.5) {
		m_world.relaxSpoil(1);
		m_lastRelax = now;
	}
	// Soil poured or slid onto a worker during this tick: it climbs out on top of it at once.
	for (Ant &a : m_ants)
		resolveEmbedded(a);
	++m_tick;
}

void Simulation::rebuildSpatialHash()
{
	std::fill(m_bucketStart.begin(), m_bucketStart.end(), 0);
	auto bucketOf = [&](const Ant &a) {
		const int bx = std::clamp(int(a.x) / kBucket, 0, m_bw - 1);
		const int by = std::clamp(int(a.y) / kBucket, 0, m_bh - 1);
		return by * m_bw + bx;
	};
	for (const Ant &a : m_ants)
		++m_bucketStart[size_t(bucketOf(a) + 1)];
	for (size_t i = 1; i < m_bucketStart.size(); ++i)
		m_bucketStart[i] += m_bucketStart[i - 1];
	m_bucketItems.resize(m_ants.size());
	m_bucketFill.assign(m_bucketStart.begin(), m_bucketStart.end() - 1);
	for (const Ant &a : m_ants)
		m_bucketItems[size_t(m_bucketFill[size_t(bucketOf(a))]++)] = int(a.id);
}

void Simulation::emit(EventType t, float x, float y, float s, uint32_t ant)
{
	m_events.push(Event{t, x, y, s, ant, m_tick});
}

float Simulation::headX(const Ant &a) const
{
	return a.x + std::cos(a.heading) * m_cfg.antLength * 0.45f * a.t.size;
}

float Simulation::headY(const Ant &a) const
{
	return a.y + std::sin(a.heading) * m_cfg.antLength * 0.45f * a.t.size;
}

bool Simulation::bodyFits(float x, float y, float heading, const Ant &a) const
{
	// The worker's centre and the front of its head must be in open space (it pushes into the terrain with
	// its head; the abdomen trails through space it has already occupied, so it is not re-checked here).
	const float half = m_cfg.antLength * 0.42f * a.t.size * 0.7f; // antennae and mandibles may touch the wall
	const float c = std::cos(heading), s = std::sin(heading);
	return m_world.isOpen(int(x), int(y)) && m_world.isOpen(int(x + c * half), int(y + s * half)) &&
	       m_world.isOpen(int(x + c * half * 0.5f), int(y + s * half * 0.5f));
}

float Simulation::clearance(float x, float y, float dx, float dy, float dist) const
{
	int open = 0;
	for (int k = 1; k <= 3; ++k) {
		const float f = dist * float(k) / 3.0f;
		open += m_world.isOpen(int(x + dx * f), int(y + dy * f)) ? 1 : 0;
	}
	return float(open) / 3.0f;
}

void Simulation::exitFlow(float x, float y, float *fx, float *fy) const
{
	// Follow the shortest path to the exit a few cells ahead (greedy descent of the exit distance through
	// open cells, never cutting a solid corner) and point at that spot: this follows bends and staircases.
	{
		int cx = int(x), cy = int(y);
		uint16_t d = m_exit.at(cx, cy);
		if (d != ExitDistance::kUnreachable && d > 0) {
			static const int ox[8] = {1, 0, -1, 0, 1, -1, -1, 1}, oy[8] = {0, 1, 0, -1, 1, 1, -1, -1};
			for (int step = 0; step < 4 && d > 0; ++step) {
				int bk = -1;
				uint16_t bd = d;
				for (int k = 0; k < 8; ++k) {
					const int nx = cx + ox[k], ny = cy + oy[k];
					if (!m_world.isOpen(nx, ny))
						continue;
					if (k >= 4 && (!m_world.isOpen(cx + ox[k], cy) || !m_world.isOpen(cx, cy + oy[k])))
						continue;
					const uint16_t nd = m_exit.at(nx, ny);
					if (nd < bd) {
						bd = nd;
						bk = k;
					}
				}
				if (bk < 0)
					break;
				cx += ox[bk];
				cy += oy[bk];
				d = bd;
			}
			const float tx = float(cx) + 0.5f - x, ty = float(cy) + 0.5f - y;
			const float l = std::hypot(tx, ty);
			if (l > 0.3f) {
				*fx = tx / l;
				*fy = ty / l;
				return;
			}
		}
	}
	// Direction of steepest descent of the exit distance through open cells near the worker: follows the
	// passage it is in rather than pointing straight through soil.
	const int cx = int(x), cy = int(y);
	const uint16_t d0 = m_exit.at(cx, cy);
	float gx = 0, gy = 0;
	if (d0 != ExitDistance::kUnreachable)
		for (int k = 0; k < 8; ++k) {
			static const int ox[8] = {1, 1, 0, -1, -1, -1, 0, 1}, oy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
			// Only samples connected to the worker's cell count: a cell behind a thin wall belongs to another
			// passage and says nothing about this one.
			if (ox[k] && oy[k] && !m_world.isOpen(cx + ox[k], cy) && !m_world.isOpen(cx, cy + oy[k]))
				continue;
			const float inv = (ox[k] && oy[k]) ? 0.7071f : 1.0f;
			for (int r = 1; r <= 2; ++r) {
				const uint16_t d = m_exit.at(cx + ox[k] * r, cy + oy[k] * r);
				if (d == ExitDistance::kUnreachable)
					break;
				const float w = std::clamp(float(int(d0) - int(d)), -2.0f * float(r), 2.0f * float(r)) / float(r);
				gx += w * float(ox[k]) * inv;
				gy += w * float(oy[k]) * inv;
			}
		}
	const float l = std::hypot(gx, gy);
	*fx = l > 1e-4f ? gx / l : 0.0f;
	*fy = l > 1e-4f ? gy / l : 0.0f;
}

float Simulation::localWidth(float x, float y, float heading) const
{
	// Open cells across the direction of travel (up to 5 each side).
	const float nx = -std::sin(heading), ny = std::cos(heading);
	int w = 1;
	for (int side : {-1, 1})
		for (int k = 1; k <= 5; ++k) {
			if (!m_world.isOpen(int(x + nx * float(k * side)), int(y + ny * float(k * side))))
				break;
			++w;
		}
	return float(w);
}

int Simulation::neighboursAhead(const Ant &a, float range, float cosCone, uint32_t *blockerId, bool *headOn) const
{
	int count = 0;
	float best = 1e9f;
	const float hx = std::cos(a.heading), hy = std::sin(a.heading);
	const int bx0 = std::max(0, int(a.x - range) / kBucket), bx1 = std::min(m_bw - 1, int(a.x + range) / kBucket);
	const int by0 = std::max(0, int(a.y - range) / kBucket), by1 = std::min(m_bh - 1, int(a.y + range) / kBucket);
	for (int by = by0; by <= by1; ++by)
		for (int bx = bx0; bx <= bx1; ++bx) {
			const int b = by * m_bw + bx;
			for (int k = m_bucketStart[size_t(b)]; k < m_bucketStart[size_t(b + 1)]; ++k) {
				const Ant &o = m_ants[size_t(m_bucketItems[size_t(k)])];
				if (o.id == a.id)
					continue;
				const float dx = o.x - a.x, dy = o.y - a.y;
				const float d2 = dx * dx + dy * dy;
				if (d2 > range * range || d2 < 1e-6f)
					continue;
				const float d = std::sqrt(d2);
				if ((dx * hx + dy * hy) / d < cosCone)
					continue;
				++count;
				if (d < best) {
					best = d;
					if (blockerId)
						*blockerId = o.id;
					if (headOn)
						*headOn = std::cos(o.heading - a.heading) < -0.3f;
				}
			}
		}
	return count;
}

void Simulation::setState(Ant &a, AntState s)
{
	a.state = s;
	a.stateTime = 0;
	a.holdTime = 0;
	switch (s) {
	case AntState::Excavate: a.anim = Anim::Dig; break;
	case AntState::Groom: a.anim = Anim::Groom; break;
	case AntState::Rest: a.anim = Anim::Rest; break;
	case AntState::PickUpSoil: a.anim = Anim::PickUp; break;
	case AntState::DepositSoil: a.anim = Anim::Deposit; break;
	default: a.anim = Anim::Walk; break;
	}
}

void Simulation::release(Ant &a)
{
	if (a.targetCell >= 0 && m_reservedBy[size_t(a.targetCell)] == int32_t(a.id))
		m_reservedBy[size_t(a.targetCell)] = -1;
	a.targetCell = -1;
}

// ---- colony-level drives -----------------------------------------------------------------------

void Simulation::updateColony(float dt)
{
	const double hours = seconds() / 3600.0;
	const double cellVol = m_cfg.cellVolume;
	const double maxVol = m_cfg.maxExcavationFraction * double(m_world.diggableTotal());
	const double desired = std::min(maxVol, (m_cfg.spacePerAnt * double(m_ants.size()) + m_cfg.expansionPerHour * hours) * cellVol);
	const double have = double(m_world.excavatedVolume());
	m_digDemand = float(std::clamp((desired - have) / (desired * 0.12 + 250.0 * cellVol), 0.0, 1.0));
	m_foodNeed = clamp01((m_cfg.foodTarget - float(m_storedFood)) / m_cfg.foodTarget);

	// Keeper feeding: every few hours the station is topped up if it is running low.
	if (seconds() >= m_nextFeeding) {
		if (m_stationFood < m_cfg.feedingAmount)
			m_stationFood = std::min(m_stationFood + m_cfg.feedingAmount, m_cfg.feedingAmount * 2);
		m_nextFeeding += m_cfg.feedingIntervalHours * 3600.0;
	}
	m_stationY = float(m_world.groundTop(int(m_stationX)) - 1);
	updateEntrances();
	countActiveTips();
	if (seconds() - m_lastStore > 300.0 || m_storeX < 0) {
		pickStorePoint();
		m_lastStore = seconds();
	}
	(void)dt;
}

void Simulation::countActiveTips()
{
	// Live excavation fronts, counted on a coarse 16-cell grid. The colony keeps only a handful of them at
	// once, which is what makes a nest of a few long galleries rather than a dense thicket.
	const std::vector<float> &v = m_siteTrail.values();
	const int tw = m_siteTrail.w(), bin = std::max(1, 16 / m_siteTrail.cellSize());
	const int bw = (tw + bin - 1) / bin, bh = (m_siteTrail.h() + bin - 1) / bin;
	std::vector<uint8_t> hit(size_t(bw * bh), 0);
	int n = 0;
	for (int i : m_trailOpenList) {
		if (v[size_t(i)] <= m_cfg.siteThreshold)
			continue;
		const size_t b = size_t((i / tw) / bin * bw + (i % tw) / bin);
		if (!hit[b]) {
			hit[b] = 1;
			++n;
		}
	}
	m_activeTips = n;
}

int Simulation::openRun(float x, float y, float dx, float dy, int maxSteps) const
{
	int n = 0;
	for (int k = 1; k <= maxSteps; ++k) {
		if (!m_world.isOpen(int(std::floor(x + dx * float(k))), int(std::floor(y + dy * float(k)))))
			break;
		++n;
	}
	return n;
}

bool Simulation::tunnelShapeAllows(Ant &a, int cx, int cy, float ux, float uy, bool chamberMode)
{
	const float fx = float(cx) + 0.5f, fy = float(cy) + 0.5f;
	const float px = -uy, py = ux;
	// Breaking through into other open space beyond the face.
	for (int k = 2; k <= 3; ++k)
		for (int w = -1; w <= 1; ++w)
			if (m_world.isOpen(int(std::floor(fx + ux * float(k) + px * float(w))), int(std::floor(fy + uy * float(k) + py * float(w))))) {
				if (!a.rng.chance(0.02f))
					return false;
				k = 4;
				break;
			}
	if (chamberMode) {
		// Enlarging a chamber: widen existing open space (walls with open space beside them), up to a
		// bounded, rounded room; never start new passages from here.
		int around = 0, room = 0;
		for (int qy = -2; qy <= 2; ++qy)
			for (int qx = -2; qx <= 2; ++qx)
				around += m_world.isOpen(cx + qx, cy + qy) ? 1 : 0;
		for (int qy = -7; qy <= 7; ++qy)
			for (int qx = -9; qx <= 9; ++qx)
				room += m_world.isOpen(cx + qx, cy + qy) ? 1 : 0;
		return around >= 9 && room < 110;
	}
	// Only the dead end of a passage is worked: the open cells beside the face must be (nearly) the farthest
	// from the exit in their neighbourhood. Side walls further back are left alone.
	int adjD = -1, localD = -1;
	for (int k = 0; k < 4; ++k) {
		static const int ox[4] = {1, -1, 0, 0}, oy[4] = {0, 0, 1, -1};
		const uint16_t d = m_exit.at(cx + ox[k], cy + oy[k]);
		if (d != ExitDistance::kUnreachable && m_world.isOpen(cx + ox[k], cy + oy[k]))
			adjD = std::max(adjD, int(d));
	}
	if (adjD < 0)
		return false; // opened since the navigation cache was built: wait for it
	for (int qy = -3; qy <= 3; ++qy)
		for (int qx = -3; qx <= 3; ++qx) {
			const uint16_t d = m_exit.at(cx + qx, cy + qy);
			if (d != ExitDistance::kUnreachable)
				localD = std::max(localD, int(d));
		}
	const int behindTip = localD - adjD;
	if (behindTip > 3)
		return false;
	// The passage axis: away from the exit along the open space the worker stands in.
	float fx0 = 0, fy0 = 0;
	exitFlow(a.x, a.y, &fx0, &fy0);
	const float deeper = -(ux * fx0 + uy * fy0);
	// Open run through the worker along the face direction, and across it.
	const int along = 1 + openRun(a.x, a.y, ux, uy, 6) + openRun(a.x, a.y, -ux, -uy, 6);
	const int across = 1 + openRun(a.x, a.y, px, py, 4) + openRun(a.x, a.y, -px, -py, 4);
	const int target = 2 + int((a.id * 2246822519u) >> 31); // 2 or 3 cells, per worker
	if (along < target && std::fabs(deeper) < 0.6f)
		return true; // widening the end of a passage that is still narrow across its axis
	// Advancing the dead end of a passage of working width, away from the exit.
	if (behindTip > 1 || deeper < 0.45f || across < 2 || across > target + 1)
		return false;
	// Keep a thick wall to other passages: nothing open in a widening cone ahead of the face. This is what
	// stops a tip from splitting into close forks and passages from running side by side.
	for (int k = 1; k <= 6; ++k) {
		const int span = std::min(4, k + 1);
		for (int w = -span; w <= span; ++w)
			if (m_world.isOpen(int(std::floor(fx + ux * float(k) + px * float(w))), int(std::floor(fy + uy * float(k) + py * float(w)))))
				return a.rng.chance(0.01f); // an occasional junction
	}
	return true;
}

int Simulation::findBackfillCell(const Ant &a) const
{
	// An open underground cell next to the worker's head that is a dead-end pocket (soil on three sides),
	// away from the entrances, and not occupied: filling it cannot cut a passage.
	const int hx = int(headX(a)), hy = int(headY(a));
	for (int r = 1; r <= 2; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != r)
					continue;
				const int x = hx + dx, y = hy + dy;
				if (!m_world.isOpen(x, y) || m_world.isAboveGround(x, y) || (x == int(a.x) && y == int(a.y)))
					continue;
				const uint16_t d = m_exit.at(x, y);
				if (d == ExitDistance::kUnreachable || d < 12)
					continue;
				int solid4 = 0, open8 = 0;
				for (int qy = -1; qy <= 1; ++qy)
					for (int qx = -1; qx <= 1; ++qx) {
						if (!qx && !qy)
							continue;
						const bool o = m_world.isOpen(x + qx, y + qy);
						open8 += o ? 1 : 0;
						if (!o && (qx == 0 || qy == 0))
							++solid4;
					}
				if (solid4 < 3 || open8 > 3)
					continue;
				bool occupied = false;
				for (const Ant &o : m_ants)
					if (o.id != a.id && int(o.x) == x && int(o.y) == y)
						occupied = true;
				if (!occupied)
					return m_world.index(x, y);
			}
	return -1;
}

bool Simulation::branchHasRoom(int cx, int cy, float ux, float uy) const
{
	// A new branch heads into untouched soil: nothing open ahead of it or just beside its path.
	const float px = -uy, py = ux;
	for (int d = 2; d <= 9; ++d)
		for (int w = -2; w <= 2; ++w) {
			const int x = int(std::floor(float(cx) + 0.5f + ux * float(d) + px * float(w) * 1.5f));
			const int y = int(std::floor(float(cy) + 0.5f + uy * float(d) + py * float(w) * 1.5f));
			if (!m_world.inBounds(x, y) || m_world.isOpen(x, y) || m_world.isAboveGround(x, y))
				return false;
		}
	return true;
}

void Simulation::updateEntrances()
{
	m_entrances.clear();
	int runStart = -1;
	for (int x = 1; x < m_world.width() - 1; ++x) {
		const int s = m_world.originalSurface(x);
		const bool hole = m_world.isOpen(x, s) && m_world.isOpen(x, s + 1);
		if (hole && runStart < 0)
			runStart = x;
		if (!hole && runStart >= 0) {
			m_entrances.push_back((runStart + x - 1) / 2);
			runStart = -1;
		}
	}
	if (runStart >= 0)
		m_entrances.push_back((runStart + m_world.width() - 2) / 2);
}

void Simulation::pickStorePoint()
{
	// The food store is the most spacious well-connected spot at moderate depth that the colony has dug.
	float bestScore = -1;
	int bx = -1, by = -1;
	const int step = 3;
	for (int y = 0; y < m_world.height(); y += step)
		for (int x = 2; x < m_world.width() - 2; x += step) {
			const uint16_t d = m_exit.at(x, y);
			if (d == ExitDistance::kUnreachable || d < 14)
				continue;
			int open = 0;
			for (int dy = -3; dy <= 3; ++dy)
				for (int dx = -3; dx <= 3; ++dx)
					open += m_world.isOpen(x + dx, y + dy) ? 1 : 0;
			const float score = float(open) + std::min(float(d), 60.0f) * 0.15f;
			if (score > bestScore) {
				bestScore = score;
				bx = x;
				by = y;
			}
		}
	if (bx >= 0 && bestScore > 20) {
		m_storeX = float(bx);
		m_storeY = float(by);
	} else {
		m_storeX = -1;
		m_storeY = -1;
	}
}

float Simulation::chooseDepositColumn(Ant &a)
{
	// Workers dump away from entrances, often on existing piles, and each keeps a preferred side.
	const int w = m_world.width();
	float bestScore = -1e9f, best = a.x;
	for (int k = 0; k < 14; ++k) {
		float cand;
		if (k == 0 && a.dumpX >= 0)
			cand = a.dumpX + a.rng.range(-4.0f, 4.0f);
		else
			cand = a.x + a.rng.range(-150.0f, 150.0f);
		cand = std::clamp(cand, 6.0f, float(w - 7));
		const int cx = int(cand);
		float nearest = 1e9f;
		for (int e : m_entrances)
			nearest = std::min(nearest, std::fabs(float(e) - cand));
		const float entranceTerm = nearest < 8 ? -3.0f : (nearest < 60 ? 1.0f - std::fabs(nearest - 28.0f) / 40.0f : 0.1f);
		const float pile = float(m_world.originalSurface(cx) - m_world.groundTop(cx));
		const float pileTerm = pile < 6 ? pile / 6.0f * 0.5f : -((pile - 6.0f) / 5.0f);
		const float stationTerm = std::fabs(cand - m_stationX) < 12 ? -2.0f : 0.0f;
		const float protectTerm = m_world.isProtected(cx) ? -4.0f : 0.0f;
		const float travel = std::fabs(cand - a.x) / 150.0f;
		const float stick = (a.dumpX >= 0 && std::fabs(cand - a.dumpX) < 10) ? 0.6f : 0.0f;
		const float s = entranceTerm + pileTerm + stationTerm + protectTerm - travel + stick + a.rng.range(0.0f, 0.3f);
		if (s > bestScore) {
			bestScore = s;
			best = cand;
		}
	}
	a.dumpX = best;
	return best;
}

// ---- individual behaviour ----------------------------------------------------------------------

void Simulation::chooseTask(Ant &a)
{
	const float digU = a.t.digPreference * (0.15f + 0.85f * m_digDemand) * (m_digDemand > 0.01f ? 1.0f : 0.05f) *
	                   (a.digCooldown > 0 ? 0.15f : 1.0f);
	const float forageU = a.t.forageBias * (0.12f + 0.9f * m_foodNeed) * (m_stationFood > 0 ? 1.0f : 0.1f);
	const float restU = (a.activeTimer <= 0 ? 2.5f : 0.05f) + (1.0f - a.energy) * 0.8f;
	const float patrolU = 0.25f * a.t.curiosity;
	const float u[4] = {digU, forageU, restU, patrolU};
	const float temp = 0.35f;
	float mx = std::max({u[0], u[1], u[2], u[3]});
	float w[4], sum = 0;
	for (int i = 0; i < 4; ++i) {
		w[i] = std::exp((u[i] - mx) / temp);
		sum += w[i];
	}
	float r = a.rng.uniform() * sum;
	int pick = 3;
	for (int i = 0; i < 4; ++i) {
		if (r < w[i]) {
			pick = i;
			break;
		}
		r -= w[i];
	}
	a.task = Task(pick == 0 ? Task::Dig : pick == 1 ? Task::Forage : pick == 2 ? Task::Rest : Task::Patrol);
	switch (a.task) {
	case Task::Dig: setState(a, a.memAge < 900 ? AntState::FollowTrail : AntState::Explore); break;
	case Task::Forage: setState(a, AntState::SearchForFood); break;
	case Task::Rest: setState(a, AntState::FollowTrail); break;
	case Task::Patrol: setState(a, AntState::Explore); break;
	}
	a.goalX = -1;
}

bool Simulation::trySelectFace(Ant &a)
{
	const float hx = headX(a), hy = headY(a);
	const float reach = m_cfg.digReach;
	const int r = int(std::ceil(reach)) + 1;
	const bool above = aboveGround(a.x, a.y);
	const float depthNorm = clamp01((a.y - float(m_world.originalSurface(int(a.x)))) / float(m_world.height()));
	const float width = localWidth(a.x, a.y, a.heading);
	const float spaceDemand = m_restTrail.sample(a.x, a.y) / m_cfg.trailMax;
	// A minority of diggers in a well-used resting area enlarge it into a chamber.
	const bool chamberMode = !above && spaceDemand > 0.25f && ((a.id * 2654435761u) >> 28) < 4 && seconds() > 1800 &&
	                         a.y > float(m_world.originalSurface(int(a.x))) + 18.0f;
	const float trailHere = (m_digTrail.sample(a.x, a.y) + m_siteTrail.sample(a.x, a.y)) / m_cfg.trailMax;
	const float bias = (noise1(float(seconds()) * 0.02f + float(a.id), a.noiseSeed) - 0.5f) * 2.0f;
	const uint16_t dHere = m_exit.at(int(a.x), int(a.y));

	float bestScore = -1e9f;
	int bestCell = -1;
	float bdx = 0, bdy = 0;
	int considered = 0;
	bool crowded = false;
	int bestReason = 0;
	// The strongest dig-site mark around the worker: only faces near that peak are the active tip.
	float siteMax = 0.0f;
	for (int qy = -8; qy <= 8; qy += 2)
		for (int qx = -8; qx <= 8; qx += 2)
			siteMax = std::max(siteMax, m_siteTrail.sample(a.x + float(qx), a.y + float(qy)));
	const bool hasMemory = a.memAge < 900;
	// Only a worker with no face of its own and no active site nearby may start a branch.
	const bool frustrated = a.stateTime > 25.0f && a.load == Load::None && siteMax < m_cfg.siteThreshold;
	const int maxTips = 2 + int(std::min(8.0, seconds() / 5400.0));
	(void)maxTips;
	const bool branchAllowed = ((!hasMemory && siteMax < m_cfg.siteThreshold) || frustrated) &&
	                           a.rng.chance(m_cfg.branchChance * (0.5f + 2.0f * m_digDemand));
	for (int dy = -r; dy <= r && considered < 40; ++dy)
		for (int dx = -r; dx <= r; ++dx) {
			const int cx = int(hx) + dx, cy = int(hy) + dy;
			const float ddx = float(cx) + 0.5f - hx, ddy = float(cy) + 0.5f - hy;
			if (ddx * ddx + ddy * ddy > reach * reach)
				continue;
			if (!m_world.isFace(cx, cy))
				continue;
			const int ci = m_world.index(cx, cy);
			if (m_reservedBy[size_t(ci)] >= 0)
				continue;
			++considered;
			const float ox = float(cx) + 0.5f - a.x, oy = float(cy) + 0.5f - a.y;
			const float ol = std::max(0.001f, std::sqrt(ox * ox + oy * oy));
			const float ux = ox / ol, uy = oy / ol;
			const float forward = ux * std::cos(a.heading) + uy * std::sin(a.heading);
			if (forward < -0.2f)
				continue; // behind the worker: not reachable with the mandibles
			if (a.memAge < 900 && std::hypot(float(cx) - a.memX, float(cy) - a.memY) > 5.0f) {
				int o8 = 0;
				for (int qy = -1; qy <= 1; ++qy)
					for (int qx = -1; qx <= 1; ++qx)
						if ((qx || qy) && m_world.isOpen(cx + qx, cy + qy))
							++o8;
				if (o8 < 6)
					continue; // a returning worker continues its own face (but clears lumps in its way)
			}
			// Never undercut loose spoil: the pile would collapse into the passage.
			if (m_world.groundTop(cx) < m_world.originalSurface(cx) && cy <= m_world.originalSurface(cx) + 3)
				continue;
			// Keep the side walls and the feeding spot intact; do not undercut the lid region.
			if (cx < 4 || cx > m_world.width() - 5 || (std::fabs(float(cx) - m_stationX) < 6 && cy <= m_world.originalSurface(cx) + 8))
				continue;
			// Faces on the original ground surface (open only to the surface air) are either the rim of an
			// existing entrance, which is widened only a little, or the start of a brand-new entrance.
			const bool surfaceFace = cy <= m_world.originalSurface(cx);
			if (surfaceFace) {
				// The top soil row is only cut at entrance sites (within 2 cells of an anchor). A new site is a
				// rare individual decision, strongly inhibited near existing entrances.
				bool atEntrance = false;
				for (int e : m_anchors)
					if (std::abs(e - cx) <= 2)
						atEntrance = true;
				if (!atEntrance) {
					// The first entrance is started soon after the colony is introduced; later ones are rare.
					float chance = m_cfg.foundingChance * (m_anchors.empty() ? 5.0f : 0.025f);
					for (int e : m_anchors)
						if (std::fabs(float(e) - float(cx)) < m_cfg.entranceSpacing)
							chance *= 0.02f;
					if (m_anchors.size() >= 12 || !a.rng.chance(chance))
						continue;
					m_anchors.push_back(cx);
					m_world.setProtectedColumns(m_anchors, 4);
				}
			}
			float s = 0;
			// Tunnel tips (soil on three sides) are preferred over the walls of an open area.
			int openAround = 0;
			for (int qy = -2; qy <= 2; ++qy)
				for (int qx = -2; qx <= 2; ++qx)
					openAround += m_world.isOpen(cx + qx, cy + qy) ? 1 : 0;
			const float tip = clamp01(1.0f - float(openAround - 3) / 9.0f);
			// Protruding lumps and isolated specks (open on most sides) are cleared first: walls stay smooth
			// and passages do not fill with pillars.
			int open8 = 0;
			for (int qy = -1; qy <= 1; ++qy)
				for (int qx = -1; qx <= 1; ++qx)
					if ((qx || qy) && m_world.isOpen(cx + qx, cy + qy))
						++open8;
			const bool protrusion = open8 >= 7; // a loose speck, open nearly all round
			// The flat wall of an already open area is not dug unless the passage is too narrow for two workers
			// or the worker is enlarging a chamber: only tips and concave corners advance.
			// Excavation is directional: the worker works the soil in front of its head.
			const bool narrow = width < m_cfg.antWidth * 2.0f;
			if (forward < (protrusion || narrow ? -0.15f : 0.55f))
				continue;
			// Where to dig: an active site (soil removed there recently) or the worker's own face, and there
			// only the tip of a passage; flat walls of open space are left alone except for a rare branch.
			int reason = protrusion ? 1 : surfaceFace ? 2 : 0;
			if (!protrusion && !surfaceFace) {
				const float site = m_siteTrail.sample(float(cx), float(cy));
				const bool inSite = site > m_cfg.siteThreshold && site >= 0.6f * siteMax;
				const bool own = a.memAge < 900 && std::hypot(float(cx) - a.memX, float(cy) - a.memY) <= 3.0f;
				const bool tipGeom = openAround <= m_cfg.maxFaceOpenness || width < m_cfg.antWidth * 2.0f || chamberMode;
				// Gallery shape: a passage is widened until it is two or three cells across, and only then
				// advanced; soil separating it from other open space is not broken through (rare junctions).
				(void)tipGeom;
				if (tunnelShapeAllows(a, cx, cy, ux, uy, chamberMode))
					reason = own ? 3 : inSite ? 4 : 0;
				else if (branchAllowed && branchHasRoom(cx, cy, ux, uy))
					reason = 5;
				else
					continue;
			}
			s += 1.1f * tip + (protrusion ? 1.6f + 0.25f * float(open8 - 5) : 0.0f);
			// Finish cells already started (by this worker or another) before opening new ones.
			if (m_world.remaining(ci) < uint16_t(m_cfg.cellVolume))
				s += 2.0f;
			// Continue an existing digging direction (own memory), else the direction faced.
			const float dirX = a.memAge < 400 ? a.memDirX : std::cos(a.heading);
			const float dirY = a.memAge < 400 ? a.memDirY : std::sin(a.heading);
			// Slow per-worker drift of the preferred direction gives gentle curves instead of zig-zags.
			const float drift = (noise1(float(seconds()) / 240.0f + float(a.id) * 1.7f, a.noiseSeed ^ 0xD1Fu) - 0.5f) * 0.9f;
			const float pdx = dirX * std::cos(drift) - dirY * std::sin(drift), pdy = dirX * std::sin(drift) + dirY * std::cos(drift);
			s += m_cfg.wDirection * (ux * pdx + uy * pdy) * a.t.persistence;
			// Early shallow excavation trends downward; deeper work spreads more horizontally.
			s += uy * (0.45f * (1.0f - depthNorm) + 0.05f);
			// Only a passage too narrow for two workers to pass is widened at the side.
			const float side = std::fabs(ux * -std::sin(a.heading) + uy * std::cos(a.heading));
			if (narrow)
				s += 1.2f * side;
			// Recruitment: faces where soil was recently removed attract workers.
			s += 0.8f * clamp01(m_siteTrail.sample(float(cx), float(cy)) / 3.0f);
			// Soil suitability: softer and damper soil is preferred.
			const float hard = float(m_world.hardness(ci)) / 255.0f;
			const float moist = float(m_world.moisture(ci)) / 255.0f;
			s += m_cfg.wSoil * ((1.0f - hard) * 0.7f + moist * 0.3f);
			// Chambers: where many workers rest, a few diggers enlarge the space sideways.
			if (chamberMode)
				s += m_cfg.wSpaceDemand * side * 1.5f + (1.0f - tip) * 0.6f;
			// Obstacles (stones, roots) near the face push digging around them.
			int blocked = 0;
			for (int ny = -1; ny <= 1; ++ny)
				for (int nx = -1; nx <= 1; ++nx) {
					const int qx = cx + nx, qy = cy + ny;
					if (m_world.inBounds(qx, qy) && !m_world.isOpen(qx, qy) && !m_world.isExcavatable(qx, qy))
						++blocked;
				}
			s -= m_cfg.wObstacle * float(blocked) * 0.12f;
			// Connecting to an already opened passage nearby.
			const int fx = cx + int(std::round(ux * 2.0f)), fy = cy + int(std::round(uy * 2.0f));
			const uint16_t dThere = m_exit.at(fx, fy);
			if (dThere != ExitDistance::kUnreachable && dHere != ExitDistance::kUnreachable && std::abs(int(dThere) - int(dHere)) > 18)
				s += 0.8f;
			// Several workers at the same face: others prefer a neighbouring face (branching).
			int diggersNear = 0;
			for (int qy = -4; qy <= 4; ++qy)
				for (int qx = -4; qx <= 4; ++qx)
					if (m_world.inBounds(cx + qx, cy + qy) && m_reservedBy[size_t(m_world.index(cx + qx, cy + qy))] >= 0)
						++diggersNear;
			if (diggersNear >= m_cfg.maxDiggersPerFace) {
				crowded = true;
				continue; // this face is crowded: only a few workers can dig side by side
			}
			s -= 0.35f * float(diggersNear);
			s += m_cfg.wVariation * bias * (ux * 0.5f);
			s += a.rng.range(0.0f, 0.15f);
			if (s > bestScore) {
				bestScore = s;
				bestCell = ci;
				bdx = ux;
				bdy = uy;
				bestReason = reason;
			}
		}
	++m_diag[0];
	if (considered == 0)
		++m_diag[1];
	if (bestCell < 0) {
		++m_diag[crowded ? 2 : 3];
		if (crowded && a.pauseTime <= 0)
			a.pauseTime = a.rng.range(1.0f, 3.0f); // queue behind the workers at the face
		return false;
	}

	// Whether to start here at all: recruitment (trail), memory and colony demand make it likely; a lone
	// surface worker only occasionally founds a new entrance.
	float p;
	if (above)
		p = 0.55f + (a.memAge < 300 ? 0.3f : 0.0f); // surface faces were already filtered to rims / rare foundings
	else
		p = 0.25f + 0.45f * m_digDemand + 0.3f * trailHere + (a.memAge < 300 ? 0.25f : 0.0f);
	p *= a.t.digPreference;
	if (bestScore < -0.5f || !a.rng.chance(std::min(0.95f, p))) {
		++m_diag[4];
		return false;
	}
	++m_diag[5];

	a.targetCell = bestCell;
	++m_digReasons[size_t(bestReason)];
	m_reservedBy[size_t(bestCell)] = int32_t(a.id);
	a.desiredHeading = std::atan2(bdy, bdx);
	a.digWork = 0;
	a.strokeTimer = a.rng.range(0.1f, 0.4f);
	setState(a, AntState::SelectDigFace);
	a.actionTime = a.rng.range(0.4f, 1.1f); // antenna probing of the face before biting
	// The remembered digging direction turns only gradually while a worker keeps its own gallery going.
	const float keepDir = (bestReason == 3 || bestReason == 1) ? 0.85f : 0.0f;
	const float mdx = keepDir * a.memDirX + (1.0f - keepDir) * bdx, mdy = keepDir * a.memDirY + (1.0f - keepDir) * bdy;
	const float ml = std::max(1e-4f, std::hypot(mdx, mdy));
	a.memDirX = mdx / ml;
	a.memDirY = mdy / ml;
	return true;
}

void Simulation::resolveEmbedded(Ant &a)
{
	// Spoil poured or slumping onto a worker: it climbs onto the loose material (a short, local move).
	if (m_world.isOpen(int(a.x), int(a.y)))
		return;
	for (int dy = 1; dy <= 3; ++dy)
		if (m_world.isOpen(int(a.x), int(a.y) - dy)) {
			a.y = float(int(a.y) - dy) + 0.5f;
			return;
		}
	for (int r = 1; r <= 3; ++r)
		for (int dx : {-r, r})
			if (m_world.isOpen(int(a.x) + dx, int(a.y))) {
				a.x = float(int(a.x) + dx) + 0.5f;
				return;
			}
}

void Simulation::updateAnt(Ant &a, float dt)
{
	resolveEmbedded(a);
	a.stateTime += dt;
	a.memAge += dt;
	a.digCooldown = std::max(0.0f, a.digCooldown - dt);
	if (a.state != AntState::Rest) {
		a.activeTimer -= dt;
		a.energy = std::max(0.0f, a.energy - dt / (60.0f * 40.0f));
	}

	switch (a.state) {
	case AntState::SelectDigFace:
	case AntState::Excavate: {
		a.speed = 0;
		const int ci = a.targetCell;
		const int cx = ci >= 0 ? ci % m_world.width() : 0, cy = ci >= 0 ? ci / m_world.width() : 0;
		if (ci < 0 || !m_world.isExcavatable(cx, cy)) {
			release(a);
			setState(a, AntState::FollowTrail);
			return;
		}
		a.heading = a.heading + wrapAngle(a.desiredHeading - a.heading) * std::min(1.0f, dt * 6.0f);
		if (a.state == AntState::SelectDigFace) {
			a.anim = Anim::Probe;
			if (a.stateTime >= a.actionTime)
				setState(a, AntState::Excavate);
			return;
		}
		const float hard = std::max(0.15f, float(m_world.hardness(ci)) / 100.0f);
		const float moist = float(m_world.moisture(ci)) / 255.0f;
		const float rate = a.t.strength * (0.75f + 0.5f * moist) / hard;
		a.digWork += dt * rate / m_cfg.digSecondsPerPellet;
		a.strokeTimer -= dt;
		if (a.strokeTimer <= 0) {
			emit(EventType::DigStroke, float(cx) + 0.5f, float(cy) + 0.5f, std::min(1.0f, hard * 0.6f), a.id);
			a.strokeTimer = a.rng.range(0.22f, 0.55f);
		}
		if (a.digWork >= 1.0f) {
			const int got = m_world.excavate(cx, cy, m_cfg.pelletVolume);
			release(a);
			if (got > 0) {
				a.load = Load::Soil;
				a.loadVolume = got;
				a.memX = float(cx) + 0.5f;
				a.memY = float(cy) + 0.5f;
				a.memAge = 0;
				++m_pellets;
				m_siteTrail.deposit(float(cx) + 0.5f, float(cy) + 0.5f, m_cfg.siteDeposit);
				emit(EventType::PelletFree, float(cx) + 0.5f, float(cy) + 0.5f, float(got) / float(m_cfg.cellVolume), a.id);
				if (m_world.isOpen(cx, cy)) {
					m_exitDirty = true;
					markTrailOpen(cx, cy);
				}
				setState(a, AntState::PickUpSoil);
				a.actionTime = a.rng.range(0.5f, 1.0f);
			} else {
				setState(a, AntState::FollowTrail);
			}
		}
		return;
	}
	case AntState::PickUpSoil:
		a.speed = 0;
		if (a.stateTime >= a.actionTime) {
			setState(a, AntState::CarrySoilOut);
			a.goalX = -1;
			a.desiredHeading = a.heading + kPi; // turn around with the load
		}
		return;
	case AntState::DepositSoil:
		a.speed = 0;
		if (a.stateTime >= a.actionTime) {
			if (a.load == Load::Soil && a.loadVolume > 0) {
				const int bf = a.backfillCell;
				const int bx = bf >= 0 ? bf % m_world.width() : 0, by = bf >= 0 ? bf / m_world.width() : 0;
				if (bf >= 0 && m_world.backfill(bx, by, a.loadVolume)) {
					emit(EventType::Deposit, float(bx) + 0.5f, float(by) + 0.5f, float(a.loadVolume) / float(m_cfg.cellVolume), a.id);
					m_exitDirty = true;
					++m_backfills;
				} else if (bf >= 0) {
					// The pocket was taken meanwhile: keep carrying.
					a.backfillCell = -1;
					setState(a, AntState::CarrySoilOut);
					return;
				} else {
					const float dx = headX(a);
					m_world.depositSpoil(int(dx), a.loadVolume);
					emit(EventType::Deposit, dx, headY(a), float(a.loadVolume) / float(m_cfg.cellVolume), a.id);
					++m_deposits;
				}
			}
			a.backfillCell = -1;
			a.load = Load::None;
			a.loadVolume = 0;
			a.goalX = -1;
			// Keep working this face while the colony still wants space; persistent workers stay longer.
			const float keep = std::clamp(0.35f + 0.55f * m_digDemand * a.t.persistence * a.t.digPreference, 0.0f, 0.95f);
			if (a.activeTimer > 0 && a.rng.chance(keep))
				setState(a, AntState::FollowTrail);
			else
				chooseTask(a);
		}
		return;
	case AntState::Groom:
		a.speed = 0;
		if (a.stateTime >= a.actionTime) {
			emit(EventType::Groom, a.x, a.y, 0.2f, a.id);
			setState(a, a.load == Load::Soil ? AntState::CarrySoilOut : a.load == Load::Food ? AntState::CarryFood
			                                  : a.task == Task::Rest || a.task == Task::Dig ? AntState::FollowTrail : AntState::Explore);
		}
		return;
	case AntState::Rest:
		a.speed = 0;
		a.energy = std::min(1.0f, a.energy + dt / (60.0f * 4.0f));
		if (a.stateTime >= a.actionTime) {
			a.activeTimer = a.rng.range(m_cfg.activeMinutesMin, m_cfg.activeMinutesMax) * 60.0f / a.t.restScale;
			chooseTask(a);
		}
		return;
	default: break;
	}

	// Moving states: decide on a staggered cadence (or immediately when blocked / the hold expires).
	a.holdTime -= dt;
	const bool cadence = (m_tick + a.id) % uint64_t(std::max(1, m_cfg.decisionIntervalTicks)) == 0;
	if (a.holdTime <= 0 || cadence)
		decideMovement(a);
	if (a.state == AntState::Excavate || a.state == AntState::SelectDigFace || a.state == AntState::Rest ||
	    a.state == AntState::DepositSoil || a.state == AntState::Groom)
		return;
	moveAnt(a, dt);

	// Stuck detection: little progress for several seconds while trying to move.
	a.progTimer += dt;
	if (a.progTimer >= 4.0f) {
		const float d = std::hypot(a.x - a.progX, a.y - a.progY);
		if (d < 0.7f && a.pauseTime <= 0 && a.yieldTime <= 0) {
			++a.stuckStrikes;
			++m_stuckRecoveries;
			++m_stuckKinds[size_t((a.load == Load::Soil ? 0 : 3) + (aboveGround(a.x, a.y) ? 2 : a.aheadCount > 0 ? 1 : 0))];
			// Turn somewhere else and re-plan from there.
			a.desiredHeading = a.heading + a.rng.range(1.0f, 2.5f) * (a.rng.chance(0.5f) ? 1.0f : -1.0f);
			a.holdTime = 0.6f;
			if (a.stuckStrikes >= 5) {
				// Give up the current errand (no teleporting): drop the task and pick a new one here.
				++m_taskResets;
				a.stuckStrikes = 0;
				a.memAge = 1e9f;
				if (a.load == Load::None)
					chooseTask(a);
				else
					a.goalX = -1;
			}
		} else {
			a.stuckStrikes = std::max(0, a.stuckStrikes - 1);
		}
		a.progX = a.x;
		a.progY = a.y;
		a.progTimer = 0;
	}
}

void Simulation::decideMovement(Ant &a)
{
	const bool above = aboveGround(a.x, a.y);
	const uint16_t d0raw = m_exit.at(int(a.x), int(a.y));
	const float d0 = d0raw == ExitDistance::kUnreachable ? 0.0f : float(d0raw);

	// Task-level decisions that happen where the worker is.
	if (a.activeTimer <= 0 && a.load == Load::None && a.task != Task::Rest)
		chooseTask(a);
	if (a.state == AntState::CarrySoilOut && above) {
		if (a.goalX < 0)
			a.goalX = chooseDepositColumn(a);
		const int gt = m_world.groundTop(int(a.x));
		if (std::fabs(a.x - a.goalX) < 1.5f && a.y >= float(gt) - 3.0f) {
			setState(a, AntState::DepositSoil);
			a.actionTime = a.rng.range(0.6f, 1.2f);
			return;
		}
	}
	if (a.state == AntState::CarrySoilOut && !above && a.stateTime > 240.0f && a.rng.chance(0.2f)) {
		// A long way out with no progress: pack the load into a nearby dead end instead (backfilling).
		const int cell = findBackfillCell(a);
		if (cell >= 0) {
			a.backfillCell = cell;
			a.desiredHeading = std::atan2(float(cell / m_world.width()) + 0.5f - a.y, float(cell % m_world.width()) + 0.5f - a.x);
			setState(a, AntState::DepositSoil);
			a.actionTime = a.rng.range(1.0f, 2.0f);
			return;
		}
	}
	if (a.state == AntState::SearchForFood && above && m_stationFood > 0 && std::hypot(a.x - m_stationX, a.y - m_stationY) < 3.0f) {
		--m_stationFood;
		a.load = Load::Food;
		a.loadVolume = 1;
		a.knowsFood = 1;
		a.foodX = m_stationX;
		a.foodY = m_stationY;
		emit(EventType::PickUpFood, a.x, a.y, 0.3f, a.id);
		setState(a, AntState::CarryFood);
		a.desiredHeading = a.heading + kPi;
		return;
	}
	if (a.state == AntState::SearchForFood && m_stationFood <= 0 && a.stateTime > 120)
		chooseTask(a);
	if (a.state == AntState::CarryFood) {
		// Food goes to the store chamber when this nest has one; otherwise to the first roomy spot well inside.
		bool drop = false;
		if (m_storeX >= 0 && std::hypot(a.x - m_storeX, a.y - m_storeY) < 5.0f)
			drop = true;
		else if (!above && ((d0 >= 12 && localWidth(a.x, a.y, a.heading) >= 2.5f && a.stateTime > 20) || a.stateTime > 75))
			drop = true;
		else if (above && a.stateTime > 150)
			drop = true; // no way in: leave it by the entrance
		if (drop) {
			++m_storedFood;
			emit(EventType::DropFood, a.x, a.y, 0.3f, a.id);
			FoodCache *best = nullptr;
			float bestD = 7.0f;
			for (FoodCache &c : m_caches) {
				const float dd = std::hypot(c.x - a.x, c.y - a.y);
				if (dd < bestD) {
					bestD = dd;
					best = &c;
				}
			}
			if (best)
				++best->amount;
			else if (m_caches.size() < 24)
				m_caches.push_back({a.x, a.y, 1});
			else
				++m_caches.front().amount;
			a.load = Load::None;
			a.loadVolume = 0;
			if (m_foodNeed > 0.25f && a.rng.chance(0.6f))
				setState(a, AntState::SearchForFood);
			else
				chooseTask(a);
			return;
		}
	}
	if (a.task == Task::Rest && (a.state == AntState::FollowTrail || a.state == AntState::Explore)) {
		const bool noNest = m_exit.maxDistance() < 12;
		const float width = localWidth(a.x, a.y, a.heading);
		const bool settle = a.stateTime > 90.0f; // could not find a good spot: settle down here
		// A full nest: rest outside on the soil instead of squeezing in.
		const bool outside = above && m_occupancy > 0.6f && a.stateTime > 6 && a.y >= float(m_world.groundTop(int(a.x))) - 3.0f;
		if ((noNest && a.stateTime > 20) || outside || (!above && d0 >= 10 && width >= 2.5f && a.stateTime > 4) || settle) {
			setState(a, AntState::Rest);
			a.actionTime = a.rng.range(m_cfg.restMinutesMin, m_cfg.restMinutesMax) * 60.0f * a.t.restScale;
			if (m_storedFood > 0 && a.rng.chance(m_cfg.eatChance)) {
				--m_storedFood; // resting workers eat from the colony store now and then
				FoodCache *c = nullptr;
				for (FoodCache &fc : m_caches)
					if (fc.amount > 0 && (!c || fc.amount > c->amount))
						c = &fc;
				if (c)
					--c->amount;
				m_caches.erase(std::remove_if(m_caches.begin(), m_caches.end(), [](const FoodCache &fc) { return fc.amount <= 0; }),
					       m_caches.end());
			}
			m_restTrail.deposit(a.x, a.y, 2.0f);
			return;
		}
	}
	if (a.task == Task::Dig && a.load == Load::None && (a.state == AntState::FollowTrail || a.state == AntState::Explore)) {
		const bool hasMemory = a.memAge < 900;
		const bool nearMemory = hasMemory && std::hypot(a.x - a.memX, a.y - a.memY) < 6.0f;
		const bool recruited = m_siteTrail.sample(a.x, a.y) > 0.8f;
		if ((nearMemory || (!hasMemory && (recruited || a.stateTime > 30.0f))) && trySelectFace(a))
			return;
		if (a.stateTime > 75.0f) {
			// No free face found: the tips are busy. Leave digging to others for a while.
			a.digCooldown = a.rng.range(180.0f, 600.0f);
			a.memAge = 1e9f;
			chooseTask(a);
			return;
		}
	}
	// Occasional grooming and antenna checks (not synchronised: each worker's own random stream).
	if (a.load != Load::Soil && a.stateTime > 8 && a.rng.chance(0.004f / a.t.persistence)) {
		setState(a, AntState::Groom);
		a.actionTime = a.rng.range(1.5f, 5.0f);
		return;
	}
	if (a.pauseTime <= 0 && a.rng.chance(0.03f * (1.4f - 0.4f * a.t.persistence)))
		a.pauseTime = a.rng.range(0.25f, 1.1f);

	if (a.holdTime > 0 && a.speed > 0.1f)
		return; // keep the current choice long enough to look purposeful

	// ---- A. sense, B. generate feasible headings, C. score, D. sample, E. hold ----
	constexpr int kCandidates = 13;
	float scores[kCandidates];
	float headings[kCandidates];
	bool feasible[kCandidates];
	const float probe = m_cfg.antLength * 0.9f;
	const TrailField *trail = nullptr;
	if (a.task == Task::Dig && a.load == Load::None)
		trail = &m_digTrail;
	else if (a.state == AntState::SearchForFood)
		trail = &m_foodTrail;
	else if (a.task == Task::Rest)
		trail = &m_restTrail;
	const TrailField *trail2 = (a.task == Task::Dig && a.load == Load::None) ? &m_siteTrail : nullptr;
	const float trailHere = (trail ? trail->sample(a.x, a.y) : 0.0f) + (trail2 ? 2.0f * trail2->sample(a.x, a.y) : 0.0f);
	const float variation = (noise1(float(seconds()) * 0.15f * a.t.curiosity + float(a.id) * 7.1f, a.noiseSeed) - 0.5f) * 1.6f;
	float flowX = 0, flowY = 0; // towards the exit along the passage
	if (!above)
		exitFlow(a.x, a.y, &flowX, &flowY);
	int nFeasible = 0;
	for (int k = 0; k < kCandidates; ++k) {
		const float h = (k == 0) ? a.heading : a.heading + (float(k - 1) - 5.5f) / 6.0f * kPi;
		headings[k] = h;
		const float ux = std::cos(h), uy = std::sin(h);
		const float px = a.x + ux * probe, py = a.y + uy * probe;
		const float clear = clearance(a.x, a.y, ux, uy, probe);
		// The path must be open from the first step (a heading that clips a corner right away is not an option
		// even if the passage continues beyond it).
		feasible[k] = m_world.isOpen(int(a.x + ux * 0.45f), int(a.y + uy * 0.45f)) && m_world.isOpen(int(a.x + ux * 1.2f), int(a.y + uy * 1.2f)) &&
		              m_world.isOpen(int(a.x + ux * 2.2f), int(a.y + uy * 2.2f)) && clear > 0.3f;
		if (!feasible[k]) {
			scores[k] = -1e9f;
			continue;
		}
		++nFeasible;
		const uint16_t dpRaw = m_exit.at(int(px), int(py));
		const float dp = dpRaw == ExitDistance::kUnreachable ? d0 : float(dpRaw);
		// >0 = towards the surface: the local passage flow, plus the distance change at the probe.
		const float dGrad = above ? std::clamp((d0 - dp) / probe, -1.0f, 1.0f)
		                          : std::clamp(0.7f * (ux * flowX + uy * flowY) + 0.3f * (d0 - dp) / probe, -1.0f, 1.0f);
		float task = 0;
		const bool pAbove = aboveGround(px, py);
		switch (a.state) {
		case AntState::CarrySoilOut:
			if (above) {
				const float gx = a.goalX >= 0 ? a.goalX : a.x;
				task = std::clamp((gx - a.x) / 6.0f, -1.0f, 1.0f) * ux;
			} else {
				task = dGrad;
			}
			break;
		case AntState::SearchForFood:
			if (above)
				task = std::clamp((m_stationX - a.x) / 8.0f, -1.0f, 1.0f) * ux + std::clamp((m_stationY - a.y) / 8.0f, -1.0f, 1.0f) * uy * 0.5f;
			else
				task = dGrad;
			break;
		case AntState::CarryFood:
			if (!above) {
				// Inside: deeper along the passage, drawn towards the store when it is close by.
				task = -dGrad;
				if (m_storeX >= 0 && std::hypot(m_storeX - a.x, m_storeY - a.y) < 25.0f) {
					const float tx = m_storeX - a.x, ty = m_storeY - a.y;
					const float tl = std::max(0.01f, std::hypot(tx, ty));
					task = 0.5f * (ux * tx + uy * ty) / tl + 0.5f * -dGrad;
				}
			} else if (!m_entrances.empty()) {
				float ne = float(m_entrances.front());
				for (int e : m_entrances)
					if (std::fabs(float(e) - a.x) < std::fabs(ne - a.x))
						ne = float(e);
				task = std::clamp((ne - a.x) / 6.0f, -1.0f, 1.0f) * ux + 0.3f * uy;
			}
			break;
		case AntState::FollowTrail:
		case AntState::Explore:
			if (a.task == Task::Dig && a.memAge < 900) {
				const float tx = a.memX - a.x, ty = a.memY - a.y;
				const float tl = std::max(0.01f, std::hypot(tx, ty));
				const uint16_t dmRaw = m_exit.at(int(a.memX), int(a.memY));
				const float deeper = dmRaw != ExitDistance::kUnreachable && float(dmRaw) > d0 ? -dGrad : dGrad;
				task = 0.55f * (ux * tx + uy * ty) / tl + 0.45f * deeper;
			} else if (a.task == Task::Dig) {
				task = above ? 0.1f * uy : -0.35f * dGrad; // no memory: drift deeper along open space
			} else if (a.task == Task::Patrol && !above && m_occupancy > 0.8f) {
				task = 0.4f * dGrad; // make room: patrol outside
			} else if (a.task == Task::Rest) {
				task = above ? 0.0f : (m_occupancy > 0.9f ? 0.5f : -0.6f) * dGrad; // head into the nest unless it is full
				if (above && !m_entrances.empty() && m_occupancy < 0.6f) {
					float ne = float(m_entrances.front());
					for (int e : m_entrances)
						if (std::fabs(float(e) - a.x) < std::fabs(ne - a.x))
							ne = float(e);
					task = std::clamp((ne - a.x) / 6.0f, -1.0f, 1.0f) * ux + 0.4f * uy;
				}
			}
			break;
		default: break;
		}
		const float trailThere = (trail ? trail->sample(px, py) : 0.0f) + (trail2 ? 2.0f * trail2->sample(px, py) : 0.0f);
		const float trailTerm = trail ? std::clamp((trailThere - trailHere) / m_cfg.trailMax * 4.0f, -1.0f, 1.0f) : 0.0f;
		const float persistence = std::cos(h - a.heading) * a.t.persistence;
		const float varTerm = std::cos(h - (a.heading + variation));
		int ahead = 0;
		{
			const float r = 2.5f;
			const int bx0 = std::max(0, int(px - r) / kBucket), bx1 = std::min(m_bw - 1, int(px + r) / kBucket);
			const int by0 = std::max(0, int(py - r) / kBucket), by1 = std::min(m_bh - 1, int(py + r) / kBucket);
			for (int by = by0; by <= by1; ++by)
				for (int bx = bx0; bx <= bx1; ++bx) {
					const int b = by * m_bw + bx;
					for (int q = m_bucketStart[size_t(b)]; q < m_bucketStart[size_t(b + 1)]; ++q) {
						const Ant &o = m_ants[size_t(m_bucketItems[size_t(q)])];
						const float ex = o.x - px, ey = o.y - py;
						if (o.id != a.id && ex * ex + ey * ey < r * r)
							++ahead;
					}
				}
		}
		const float congestion = std::min(1.0f, float(ahead) / 3.0f);
		float obstacle = 1.0f - clear;
		// Above ground, workers keep close to the soil or the lower glass instead of wandering up to the lid.
		if (pAbove) {
			// Height above the highest ground nearby (so climbing out of an entrance between piles is fine).
			int top = m_world.height();
			for (int qx = int(px) - 6; qx <= int(px) + 6; qx += 2)
				top = std::min(top, m_world.groundTop(std::clamp(qx, 0, m_world.width() - 1)));
			const float height = float(top) - py;
			if (height > 4.0f)
				obstacle += std::min(1.5f, (height - 4.0f) / 6.0f);
		}
		const float travel = std::max(0.0f, -std::cos(h - a.heading));
		scores[k] = m_cfg.wTask * task + m_cfg.wTrail * trailTerm + m_cfg.wPersistence * persistence * 0.6f + m_cfg.wVariation * varTerm -
		            m_cfg.wCongestion * congestion - m_cfg.wObstacle * obstacle - m_cfg.wTravel * travel;
	}
	if (nFeasible == 0) {
		// Boxed in (a tight passage): follow the open cells themselves. Workers heading out take the neighbour
		// nearest the exit; the others the one that continues their direction.
		const bool outward = a.state == AntState::CarrySoilOut || (a.state == AntState::SearchForFood && !above) ||
		                     (a.task == Task::Patrol && m_occupancy > 0.8f);
		int bestK = -1;
		float bestS = -1e9f;
		static const int ox[8] = {1, 1, 0, -1, -1, -1, 0, 1}, oy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
		const int cx = int(a.x), cy = int(a.y);
		for (int k = 0; k < 8; ++k) {
			const int nx = cx + ox[k], ny = cy + oy[k];
			if (!m_world.isOpen(nx, ny))
				continue;
			if (ox[k] && oy[k] && !m_world.isOpen(cx + ox[k], cy) && !m_world.isOpen(cx, cy + oy[k]))
				continue; // no squeezing through a solid corner
			const float h = std::atan2(float(oy[k]), float(ox[k]));
			const uint16_t d = m_exit.at(nx, ny);
			float sc = std::cos(h - a.heading) * 0.5f + a.rng.range(0.0f, 0.3f);
			if (outward && d != ExitDistance::kUnreachable)
				sc += (d0 - float(d)) * 2.0f;
			if (sc > bestS) {
				bestS = sc;
				bestK = k;
			}
		}
		a.desiredHeading = bestK >= 0 ? std::atan2(float(oy[bestK]), float(ox[bestK]))
		                              : a.heading + a.rng.range(1.2f, 2.4f) * (a.rng.chance(0.5f) ? 1.0f : -1.0f);
		a.holdTime = 0.3f;
		return;
	}
	float mx = -1e9f;
	for (int k = 0; k < kCandidates; ++k)
		mx = std::max(mx, scores[k]);
	float weights[kCandidates], sum = 0;
	const float temp = std::max(0.05f, m_cfg.softmaxTemperature);
	for (int k = 0; k < kCandidates; ++k) {
		weights[k] = feasible[k] ? std::exp(std::max(-30.0f, (scores[k] - mx) / temp)) : 0.0f;
		sum += weights[k];
	}
	float r = a.rng.uniform() * sum;
	int pick = 0;
	for (int k = 0; k < kCandidates; ++k) {
		if (!feasible[k])
			continue;
		pick = k;
		if (r < weights[k])
			break;
		r -= weights[k];
	}
	a.desiredHeading = headings[pick];
	a.holdTime = a.rng.range(0.35f, 1.3f) * a.t.persistence;

	// Trails are laid by the actions they describe.
	if (a.state == AntState::CarrySoilOut)
		m_digTrail.deposit(a.x, a.y, m_cfg.trailDeposit);
	else if (a.state == AntState::CarryFood)
		m_foodTrail.deposit(a.x, a.y, m_cfg.trailDeposit);
}

void Simulation::moveAnt(Ant &a, float dt)
{
	if (a.pauseTime > 0) {
		a.pauseTime -= dt;
		a.speed = std::max(0.0f, a.speed - dt * 20.0f);
		a.anim = Anim::Probe;
		return;
	}
	if (a.anim == Anim::Probe)
		a.anim = Anim::Walk;

	// Traffic: yield head-on in narrow passages (loaded workers have priority), follow otherwise.
	const float len = m_cfg.antLength * a.t.size;
	if ((m_tick + a.id) % 3 == 0) {
		uint32_t b = 0;
		bool ho = false;
		a.aheadCount = uint8_t(std::min(255, neighboursAhead(a, len * 1.05f, 0.6f, &b, &ho)));
		a.aheadBlocker = b;
		a.aheadHeadOn = ho ? 1 : 0;
	}
	const uint32_t blocker = a.aheadBlocker;
	const bool headOn = a.aheadHeadOn != 0;
	const int ahead = a.aheadCount;
	// Workers never block one another outright: in a tight passage they squeeze past or climb over, which
	// slows both. An unloaded worker meeting a loaded one head-on stops for a moment to let it by.
	float speedFactor = 1.0f;
	if (a.yieldTime > 0) {
		a.yieldTime -= dt;
		speedFactor = 0.0f;
	} else if (ahead > 0) {
		const Ant &o = m_ants[size_t(blocker)];
		if (headOn && a.load == Load::None && o.load != Load::None && localWidth(a.x, a.y, a.heading) < 3.2f) {
			a.yieldTime = a.rng.range(0.3f, 0.8f);
			speedFactor = 0.0f;
		} else {
			speedFactor = headOn ? 0.6f : 0.75f;
		}
	}

	float target = m_cfg.walkSpeed * a.t.speed * speedFactor;
	if (a.load != Load::None)
		target *= m_cfg.loadedSpeedFactor;
	target *= 0.85f + 0.3f * noise1(float(seconds()) * 0.4f + float(a.id) * 3.3f, a.noiseSeed ^ 0x51u);
	const float turn = wrapAngle(a.desiredHeading - a.heading);
	// A worker that is not walking pivots on the spot much faster than it can curve while walking.
	const float maxTurn = m_cfg.turnRate * dt * (a.speed < 0.5f ? 3.0f : 1.0f);
	const float applied = std::clamp(turn, -maxTurn, maxTurn);
	a.heading = wrapAngle(a.heading + applied);
	// Slow down for sharp turns (ants pivot rather than skid).
	if (std::fabs(turn) > 0.9f)
		target *= 0.35f;
	a.speed += std::clamp(target - a.speed, -dt * 25.0f, dt * 18.0f);

	const float step = a.speed * dt;
	if (step <= 1e-4f)
		return;
	const float offsets[5] = {0.0f, 0.35f, -0.35f, 0.75f, -0.75f};
	for (float off : offsets) {
		const float h = a.heading + off;
		const float nx = a.x + std::cos(h) * step * (off == 0 ? 1.0f : 0.7f);
		const float ny = a.y + std::sin(h) * step * (off == 0 ? 1.0f : 0.7f);
		if (insideGrid(nx, ny) && bodyFits(nx, ny, a.heading + off * 0.5f, a)) {
			const float moved = std::hypot(nx - a.x, ny - a.y);
			a.x = nx;
			a.y = ny;
			a.heading = wrapAngle(a.heading + off * 0.5f);
			// Legs follow the ground: stride phase advances with distance, not with time.
			a.gait = std::fmod(a.gait + moved / (len * 0.55f) * kTwoPi, kTwoPi * 64.0f);
			return;
		}
	}
	// Squeeze: ants are flexible and pass tight bends with the head brushing the soil. Only the body centre
	// has to stay in open space (it moves at most a fraction of a cell, so it never crosses solid corners).
	{
		const float sq = std::min(step, 0.2f);
		const float nx = a.x + std::cos(a.heading) * sq, ny = a.y + std::sin(a.heading) * sq;
		if (insideGrid(nx, ny) && m_world.isOpen(int(nx), int(ny)) &&
		    m_world.isOpen(int(a.x + std::cos(a.heading) * 0.6f), int(a.y + std::sin(a.heading) * 0.6f))) {
			a.x = nx;
			a.y = ny;
			a.speed = std::min(a.speed, sq / dt);
			a.gait = std::fmod(a.gait + sq / (len * 0.55f) * kTwoPi, kTwoPi * 64.0f);
			return;
		}
		// Slide along the wall: take the part of the step that stays in open space (a worker at a corner
		// edges sideways into the next cell before turning up or down it).
		const float cx = std::cos(a.heading) * sq, cy = std::sin(a.heading) * sq;
		const float slides[2][2] = {{cx, 0.0f}, {0.0f, cy}};
		const int first = std::fabs(cx) >= std::fabs(cy) ? 0 : 1;
		for (int k = 0; k < 2; ++k) {
			const float *d = slides[k == 0 ? first : 1 - first];
			if (std::fabs(d[0]) + std::fabs(d[1]) < 1e-4f)
				continue;
			const float sx = a.x + d[0], sy = a.y + d[1];
			if (insideGrid(sx, sy) && m_world.isOpen(int(sx), int(sy))) {
				a.x = sx;
				a.y = sy;
				a.speed = std::min(a.speed, (std::fabs(d[0]) + std::fabs(d[1])) / dt);
				a.gait = std::fmod(a.gait + (std::fabs(d[0]) + std::fabs(d[1])) / (len * 0.55f) * kTwoPi, kTwoPi * 64.0f);
				return;
			}
		}
	}
	// Blocked by the terrain: stop; unless it is still turning towards its chosen heading, reconsider next tick.
	a.speed = 0;
	if (std::fabs(turn) < 0.2f)
		a.holdTime = 0;
}

// ---- inspection --------------------------------------------------------------------------------

Stats Simulation::stats() const
{
	Stats s;
	s.tick = m_tick;
	s.seconds = seconds();
	s.ants = int(m_ants.size());
	for (const Ant &a : m_ants) {
		++s.byState[size_t(a.state)];
		if (a.load == Load::Soil) {
			++s.carryingSoil;
			s.carriedVolume += a.loadVolume;
		} else if (a.load == Load::Food) {
			++s.carryingFood;
		}
	}
	s.excavatedVolume = m_world.excavatedVolume();
	s.depositedVolume = m_world.depositedVolume();
	s.excavatedFraction = m_world.diggableTotal() > 0 ? double(s.excavatedVolume) / double(m_world.diggableTotal()) : 0;
	s.openUndergroundCells = m_world.openUndergroundCells();
	s.entrances = int(m_entrances.size());
	int maxDepth = 0;
	for (int x = 0; x < m_world.width(); x += 2) {
		const int surf = m_world.originalSurface(x);
		for (int y = m_world.height() - 2; y > surf; --y)
			if (m_world.isOpen(x, y)) {
				maxDepth = std::max(maxDepth, y - surf);
				break;
			}
	}
	s.maxDepth = maxDepth;
	s.stationFood = m_stationFood;
	s.storedFood = m_storedFood;
	s.digDemand = m_digDemand;
	s.activeTips = m_activeTips;
	s.stuckKinds = m_stuckKinds;
	s.diag = m_diag;
	s.foodNeed = m_foodNeed;
	s.stuckRecoveries = m_stuckRecoveries;
	s.taskResets = m_taskResets;
	s.eventsWritten = m_events.written();
	s.eventsDropped = m_events.dropped();
	s.deposits = m_deposits;
	s.backfills = m_backfills;
	s.pellets = m_pellets;
	s.digReasons = m_digReasons;
	return s;
}

uint64_t Simulation::stateHash() const
{
	uint64_t h = m_world.checksum();
	auto mix = [&h](const void *p, size_t n) {
		const auto *b = static_cast<const uint8_t *>(p);
		for (size_t i = 0; i < n; ++i) {
			h ^= b[i];
			h *= 1099511628211ULL;
		}
	};
	for (const Ant &a : m_ants) {
		mix(&a.x, sizeof(float));
		mix(&a.y, sizeof(float));
		mix(&a.heading, sizeof(float));
		mix(&a.state, 1);
		mix(&a.load, 1);
		mix(&a.rng.state, sizeof(uint64_t));
	}
	mix(&m_tick, sizeof(m_tick));
	mix(&m_storedFood, sizeof(int));
	mix(&m_stationFood, sizeof(int));
	mix(m_digTrail.values().data(), m_digTrail.values().size() * sizeof(float));
	return h;
}

size_t Simulation::memoryFootprint() const
{
	size_t b = 0;
	b += m_ants.capacity() * sizeof(Ant);
	b += m_reservedBy.capacity() * sizeof(int32_t);
	b += m_bucketStart.capacity() * sizeof(int) + m_bucketItems.capacity() * sizeof(int);
	b += m_digTrail.values().capacity() * sizeof(float) * 2 * 3;
	b += size_t(m_world.width()) * size_t(m_world.height()) * (1 + 2 + 1 + 1 + 1 + 2);
	b += size_t(m_events.capacity()) * sizeof(Event);
	b += m_caches.capacity() * sizeof(FoodCache) + m_entrances.capacity() * sizeof(int);
	return b;
}

// ---- checkpoints -------------------------------------------------------------------------------

std::vector<uint8_t> Simulation::saveCheckpoint() const
{
	ByteWriter w;
	w.data.insert(w.data.end(), {'S', 'A', 'F', 'C', 'K', 'P', 'T', '1'});
	w.pod<uint32_t>(kCheckpointVersion);
	w.pod<uint32_t>(uint32_t(sizeof(Ant)));
	w.str(m_cfg.dump());
	w.pod(m_cfg.hash());
	w.pod(m_seed);
	w.pod(m_tick);
	m_world.save(w);
	w.vec(m_digTrail.values());
	w.vec(m_foodTrail.values());
	w.vec(m_restTrail.values());
	w.vec(m_siteTrail.values());
	w.vec(m_trailOpen); // grows with the nest and is never shrunk: part of the state, not derivable
	w.vec(m_trailOpenList);
	w.vec(m_ants);
	w.vec(m_reservedBy);
	w.vec(m_exit.raw());
	w.pod<int32_t>(m_exit.maxDistance());
	w.pod<uint8_t>(m_exitDirty ? 1 : 0);
	w.vec(m_caches);
	w.vec(m_entrances);
	w.vec(m_anchors);
	w.pod(m_colonyRng);
	w.pod(m_stationX);
	w.pod(m_stationY);
	w.pod(m_stationFood);
	w.pod(m_storedFood);
	w.pod(m_nextFeeding);
	w.pod(m_storeX);
	w.pod(m_storeY);
	w.pod(m_digDemand);
	w.pod(m_activeTips);
	w.pod(m_foodNeed);
	w.pod(m_lastExit);
	w.pod(m_lastTrail);
	w.pod(m_lastColony);
	w.pod(m_lastRelax);
	w.pod(m_lastStore);
	w.pod(m_stuckRecoveries);
	w.pod(m_taskResets);
	w.pod(m_deposits);
	w.pod(m_backfills);
	w.pod(m_pellets);
	w.pod<uint64_t>(stateHash());
	w.pod<uint64_t>(fnv64(w.data.data(), w.data.size())); // whole-file checksum: any flipped byte is caught
	return w.data;
}

bool Simulation::loadCheckpoint(const std::vector<uint8_t> &data, std::string *error)
{
	auto fail = [&](const char *why) {
		if (error)
			*error = why;
		return false;
	};
	if (data.size() < 24 || std::memcmp(data.data(), "SAFCKPT1", 8) != 0)
		return fail("not a Shadow Ant Farm checkpoint");
	{
		uint64_t stored = 0;
		std::memcpy(&stored, data.data() + data.size() - 8, 8);
		if (stored != fnv64(data.data(), data.size() - 8))
			return fail("checkpoint is damaged (checksum mismatch)");
	}
	ByteReader r(data.data() + 8, data.size() - 8);
	const uint32_t version = r.pod<uint32_t>();
	if (version != kCheckpointVersion)
		return fail("checkpoint version is not supported by this build");
	if (r.pod<uint32_t>() != sizeof(Ant))
		return fail("checkpoint was written by an incompatible build");
	const std::string cfgText = r.str();
	const uint64_t cfgHash = r.pod<uint64_t>();
	SimConfig cfg;
	{
		size_t pos = 0;
		while (pos < cfgText.size()) {
			const size_t nl = cfgText.find('\n', pos);
			const std::string line = cfgText.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			pos = nl == std::string::npos ? cfgText.size() : nl + 1;
			const auto eq = line.find(" = ");
			if (eq != std::string::npos)
				cfg.set(line.substr(0, eq), line.substr(eq + 3));
		}
	}
	if (cfg.hash() != cfgHash)
		return fail("checkpoint configuration is corrupt");
	Simulation s;
	s.m_cfg = cfg;
	s.m_seed = r.pod<uint64_t>();
	s.m_tick = r.pod<uint64_t>();
	if (!s.m_world.load(r))
		return fail("checkpoint terrain is corrupt");
	s.m_digTrail.init(cfg.width, cfg.height, cfg.trailCell, cfg.digTrailHalfLife, cfg.trailDiffusion, cfg.trailMax);
	s.m_foodTrail.init(cfg.width, cfg.height, cfg.trailCell, cfg.foodTrailHalfLife, cfg.trailDiffusion, cfg.trailMax);
	s.m_restTrail.init(cfg.width, cfg.height, cfg.trailCell, 240.0f, cfg.trailDiffusion * 0.5f, cfg.trailMax);
	s.m_siteTrail.init(cfg.width, cfg.height, cfg.trailCell, cfg.siteHalfLife, 0.0f, cfg.trailMax);
	r.vec(s.m_digTrail.values());
	r.vec(s.m_foodTrail.values());
	r.vec(s.m_restTrail.values());
	r.vec(s.m_siteTrail.values());
	r.vec(s.m_trailOpen);
	r.vec(s.m_trailOpenList);
	r.vec(s.m_ants, 100000);
	r.vec(s.m_reservedBy);
	r.vec(s.m_exit.raw());
	const int32_t exitMax = r.pod<int32_t>();
	const bool exitDirty = r.pod<uint8_t>() != 0;
	r.vec(s.m_caches, 4096);
	r.vec(s.m_entrances, 100000);
	r.vec(s.m_anchors, 1000);
	s.m_colonyRng = r.pod<Pcg32>();
	s.m_stationX = r.pod<float>();
	s.m_stationY = r.pod<float>();
	s.m_stationFood = r.pod<int>();
	s.m_storedFood = r.pod<int>();
	s.m_nextFeeding = r.pod<double>();
	s.m_storeX = r.pod<float>();
	s.m_storeY = r.pod<float>();
	s.m_digDemand = r.pod<float>();
	s.m_activeTips = r.pod<int>();
	s.m_foodNeed = r.pod<float>();
	s.m_lastExit = r.pod<double>();
	s.m_lastTrail = r.pod<double>();
	s.m_lastColony = r.pod<double>();
	s.m_lastRelax = r.pod<double>();
	s.m_lastStore = r.pod<double>();
	s.m_stuckRecoveries = r.pod<int>();
	s.m_taskResets = r.pod<int>();
	s.m_deposits = r.pod<int>();
	s.m_backfills = r.pod<int>();
	s.m_pellets = r.pod<int>();
	const uint64_t hash = r.pod<uint64_t>();
	if (!r.ok || s.m_ants.empty() || s.m_reservedBy.size() != size_t(cfg.width * cfg.height))
		return fail("checkpoint is truncated or corrupt");
	s.m_events.init(cfg.eventCapacity);
	s.m_bw = (cfg.width + kBucket - 1) / kBucket;
	s.m_bh = (cfg.height + kBucket - 1) / kBucket;
	s.m_bucketStart.assign(size_t(s.m_bw * s.m_bh + 1), 0);
	if (s.m_exit.raw().size() != size_t(cfg.width * cfg.height))
		return fail("checkpoint navigation cache is corrupt");
	s.m_exit.restore(cfg.width, cfg.height, exitMax);
	if (s.m_trailOpen.size() != s.m_siteTrail.values().size())
		return fail("checkpoint trail data is corrupt");
	s.m_world.setProtectedColumns(s.m_anchors, 4);
	s.m_exitDirty = exitDirty;
	if (s.stateHash() != hash)
		return fail("checkpoint failed its integrity check");
	*this = std::move(s);
	return true;
}

} // namespace antfarm
