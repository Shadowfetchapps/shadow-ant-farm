#pragma once
// The colony simulation. A decentralised display model: every worker senses only its surroundings, the
// trails other workers laid and space the colony has already opened, then picks an action by scoring the
// feasible ones and sampling with a bounded softmax. Tunnels are the accumulated result of individual
// excavation decisions; nothing about their shape is scripted.

#include "antfarm/config.hpp"
#include "antfarm/fields.hpp"
#include "antfarm/rng.hpp"
#include "antfarm/world.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace antfarm {

enum class AntState : uint8_t {
	Explore,
	FollowTrail,
	SelectDigFace,
	Excavate,
	PickUpSoil,
	CarrySoilOut,
	DepositSoil,
	SearchForFood,
	CarryFood,
	Groom,
	Rest,
	Count
};
const char *stateName(AntState s);

enum class Task : uint8_t { Dig, Forage, Rest, Patrol };
enum class Load : uint8_t { None, Soil, Food };

/// What the renderer should animate (derived from the state; carries no decision weight).
enum class Anim : uint8_t { Walk, Dig, Groom, Rest, PickUp, Deposit, Probe };

struct Traits {
	float size = 1;         ///< body scale (0.9–1.1)
	float speed = 1;        ///< walking speed multiplier
	float persistence = 1;  ///< how long a heading or task is kept
	float digPreference = 1;
	float forageBias = 1;
	float restScale = 1;
	float strength = 1;     ///< excavation rate multiplier
	float curiosity = 1;    ///< exploration drift
};

struct Ant {
	uint32_t id = 0;
	float x = 0, y = 0, heading = 0;
	float px = 0, py = 0, pheading = 0; ///< previous tick, for render interpolation
	float speed = 0;
	float gait = 0;                     ///< stride phase, advances with distance walked
	AntState state = AntState::Explore;
	Task task = Task::Patrol;
	Load load = Load::None;
	int loadVolume = 0;
	Anim anim = Anim::Walk;
	float stateTime = 0;
	float actionTime = 0;               ///< duration of the current timed action
	float holdTime = 0;
	float desiredHeading = 0;
	float energy = 1;
	float activeTimer = 600;
	float pauseTime = 0;
	float yieldTime = 0;
	int targetCell = -1;                ///< reserved face (Excavate) or -1
	float digWork = 0;
	float strokeTimer = 0;
	// Memory (local and self-acquired only)
	float memX = 0, memY = 0, memDirX = 0, memDirY = 1;
	float memAge = 1e9f;
	float foodX = 0, foodY = 0;
	uint8_t knowsFood = 0;
	float dumpX = -1;                    ///< remembered dumping column
	float goalX = -1;                    ///< surface target column (deposit / food)
	// Stuck detection
	float progX = 0, progY = 0, progTimer = 0;
	int stuckStrikes = 0;
	float digCooldown = 0;
	int32_t backfillCell = -1;           ///< dead-end cell chosen for packing the load (DepositSoil underground)
	uint8_t aheadCount = 0, aheadHeadOn = 0; ///< cached traffic check (refreshed every few ticks)
	uint32_t aheadBlocker = 0;               ///< after failing to find a free face, digging is less attractive
	Traits t;
	Pcg32 rng;
	uint32_t noiseSeed = 0;
};

enum class EventType : uint8_t { DigStroke, PelletFree, Deposit, PickUpFood, DropFood, Step, Groom };

struct Event {
	EventType type;
	float x, y;
	float strength;
	uint32_t ant;
	uint64_t tick;
};

/// Bounded ring buffer of simulation events (for audio and loose-grain visuals). Never grows.
class EventQueue {
public:
	void init(int capacity);
	void push(const Event &e);
	/// Copies events with sequence number >= `from` into `out`; returns the next sequence number.
	uint64_t read(uint64_t from, std::vector<Event> &out, size_t maxCount) const;
	uint64_t written() const { return m_written; }
	uint64_t dropped() const { return m_dropped; }
	int capacity() const { return int(m_buf.size()); }

private:
	std::vector<Event> m_buf;
	uint64_t m_written = 0;
	mutable uint64_t m_dropped = 0;
};

struct FoodCache {
	float x = 0, y = 0;
	int amount = 0;
};

struct Stats {
	uint64_t tick = 0;
	double seconds = 0;
	int ants = 0;
	std::array<int, size_t(AntState::Count)> byState{};
	int carryingSoil = 0, carryingFood = 0;
	double excavatedFraction = 0;     ///< of all excavatable substrate
	int64_t excavatedVolume = 0, depositedVolume = 0, carriedVolume = 0;
	int64_t openUndergroundCells = 0;
	int entrances = 0;
	int activeTips = 0;
	float spoilPressure = 0;
	std::array<int, 8> diag{};
	std::array<int, 6> stuckKinds{}; ///< diagnostics: {carrier, other} x {below free, below traffic, above}
	int maxDepth = 0;                 ///< deepest open cell below the original surface
	int stationFood = 0;
	int storedFood = 0;
	double digDemand = 0, foodNeed = 0;
	int stuckRecoveries = 0, taskResets = 0;
	uint64_t eventsWritten = 0, eventsDropped = 0;
	int deposits = 0, pellets = 0, backfills = 0;
	std::array<int, 6> digReasons{}; ///< face selections by rule: 0 other,1 lump,2 surface,3 own face,4 active site,5 branch
};

class Simulation {
public:
	bool init(const SimConfig &cfg, uint64_t seed);
	void step();
	void stepN(int n);

	uint64_t seed() const { return m_seed; }
	uint64_t tick() const { return m_tick; }
	double seconds() const { return double(m_tick) / double(m_cfg.ticksPerSecond); }
	const SimConfig &config() const { return m_cfg; }
	const World &world() const { return m_world; }
	World &worldMutable() { return m_world; }
	const std::vector<Ant> &ants() const { return m_ants; }
	const EventQueue &events() const { return m_events; }
	const ExitDistance &exitDistance() const { return m_exit; }
	const TrailField &digTrail() const { return m_digTrail; }
	const TrailField &foodTrail() const { return m_foodTrail; }
	const TrailField &siteTrail() const { return m_siteTrail; }
	float stationX() const { return m_stationX; }
	float stationY() const { return m_stationY; }
	int stationFood() const { return m_stationFood; }
	const std::vector<FoodCache> &foodCaches() const { return m_caches; }
	const std::vector<int> &entrances() const { return m_entrances; }
	float storeX() const { return m_storeX; }
	float storeY() const { return m_storeY; }
	Stats stats() const;

	/// Hash of the complete logical state (world, ants, colony, fields): equal hashes mean equal runs.
	uint64_t stateHash() const;

	std::vector<uint8_t> saveCheckpoint() const;
	bool loadCheckpoint(const std::vector<uint8_t> &data, std::string *error);
	static constexpr uint32_t kCheckpointVersion = 2;

	/// Bytes of memory held by the simulation's containers (for resource-bound checks).
	size_t memoryFootprint() const;

private:
	void spawnAnts();
	void rebuildSpatialHash();
	void updateAnt(Ant &a, float dt);
	void chooseTask(Ant &a);
	void decideMovement(Ant &a);
	void moveAnt(Ant &a, float dt);
	bool trySelectFace(Ant &a);
	bool bodyFits(float x, float y, float heading, const Ant &a) const;
	float clearance(float x, float y, float dirX, float dirY, float dist) const;
	int neighboursAhead(const Ant &a, float range, float cosCone, uint32_t *blockerId, bool *headOn) const;
	float localWidth(float x, float y, float heading) const;
	void exitFlow(float x, float y, float *fx, float *fy) const;
	void updateColony(float dt);
	void updateEntrances();
	void resolveEmbedded(Ant &a);
	float chooseDepositColumn(Ant &a);
	void pickStorePoint();
	void countActiveTips();
	bool branchHasRoom(int cx, int cy, float ux, float uy) const;
	int findBackfillCell(const Ant &a) const;
	bool tunnelShapeAllows(Ant &a, int cx, int cy, float ux, float uy, bool chamberMode);
	int openRun(float x, float y, float dx, float dy, int maxSteps) const;
	void setState(Ant &a, AntState s);
	void release(Ant &a);
	void emit(EventType t, float x, float y, float s, uint32_t ant);
	float headX(const Ant &a) const;
	float headY(const Ant &a) const;
	bool aboveGround(float x, float y) const { return m_world.isAboveGround(int(x), int(y)); }
	bool insideGrid(float x, float y) const
	{
		return x >= 0.5f && y >= 0.5f && x <= float(m_world.width()) - 0.5f && y <= float(m_world.height()) - 0.5f;
	}

	SimConfig m_cfg;
	uint64_t m_seed = 0;
	uint64_t m_tick = 0;
	World m_world;
	ExitDistance m_exit;
	TrailField m_digTrail, m_foodTrail, m_restTrail, m_siteTrail; ///< site = recently excavated faces
	std::vector<Ant> m_ants;
	std::vector<int32_t> m_reservedBy;
	std::vector<uint8_t> m_trailOpen; ///< one byte per trail cell: contains open space
	std::vector<int> m_trailOpenList; ///< indices of open trail cells, in ascending order of discovery
	void rebuildTrailOpen();
	void markTrailOpen(int x, int y);
	EventQueue m_events;
	Pcg32 m_colonyRng;

	// Spatial hash (bucket = 4 cells)
	static constexpr int kBucket = 4;
	int m_bw = 0, m_bh = 0;
	std::vector<int> m_bucketStart, m_bucketItems, m_bucketFill;

	// Colony state
	float m_stationX = 0, m_stationY = 0;
	int m_stationFood = 0;
	int m_storedFood = 0;
	double m_nextFeeding = 0;
	float m_storeX = -1, m_storeY = -1;
	std::vector<FoodCache> m_caches;
	std::vector<int> m_entrances;
	std::vector<int> m_anchors; ///< entrance sites where founding started (top-row digging is limited to these)
	float m_digDemand = 1, m_foodNeed = 0;
	float m_occupancy = 0; ///< workers underground per available space (derived each tick)
	int m_activeTips = 0;
	float m_spoilPressure = 0; ///< share of the surface air taken by mounds, above a threshold (drives backfilling)
	std::array<int, 6> m_stuckKinds{};
	std::array<int, 8> m_diag{}; ///< diagnostics only (not part of the logical state) ///< 16-cell regions with a live dig-site mark (derived; recomputed on load)
	bool m_exitDirty = true;
	double m_lastExit = -10, m_lastTrail = 0, m_lastColony = -10, m_lastRelax = 0, m_lastStore = -100;
	std::array<int, 6> m_digReasons{};
	int m_stuckRecoveries = 0, m_taskResets = 0, m_deposits = 0, m_pellets = 0, m_backfills = 0;
};

} // namespace antfarm
