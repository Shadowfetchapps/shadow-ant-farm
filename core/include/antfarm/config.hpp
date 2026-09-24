#pragma once
// Simulation configuration. Every value here is a display-model tuning parameter chosen to make the farm
// look believable over a day of streaming; none of them is a measured biological constant.

#include <cstdint>
#include <string>

namespace antfarm {

struct SimConfig {
	// ---- habitat geometry (grid cells; one cell is roughly a quarter of an ant's body length) ----
	int width = 960;
	int height = 540;
	float surfaceFraction = 0.155f;   ///< average depth of the soil surface from the top, as a fraction of height
	float surfaceRoughness = 5.0f;    ///< cells of relief along the initial surface
	int lidRows = 3;                  ///< rows under the lid that ants do not enter

	// ---- time ----
	int ticksPerSecond = 30;          ///< fixed simulation rate
	int decisionIntervalTicks = 5;    ///< movement re-evaluation cadence (staggered per ant)

	// ---- colony ----
	int minAnts = 350;
	int maxAnts = 650;
	float antLength = 4.2f;           ///< cells (≈16 px at 3840 px habitat width)
	float antWidth = 1.2f;
	float walkSpeed = 5.2f;           ///< cells per second, typical unloaded
	float loadedSpeedFactor = 0.78f;
	float turnRate = 4.0f;            ///< radians per second, maximum

	// ---- excavation and material ----
	int cellVolume = 256;             ///< integer volume units per cell (exact accounting)
	int pelletVolume = 256;           ///< volume units one worker carries per trip (one cell: about an ant head)
	float digSecondsPerPellet = 9.0f;  ///< work time for a pellet of reference-hardness soil
	float digReach = 1.6f;            ///< cells from the head to a face that can be worked
	int maxFacesConsidered = 10;
	int maxDiggersPerFace = 2;        ///< workers digging within 4 cells of one another
	int maxFaceOpenness = 8;          ///< open cells in the 5x5 around a face above which it is a wall, not a tip
	float siteThreshold = 0.25f;      ///< dig-site mark above which a face counts as an active site
	float branchChance = 0.004f;      ///< per look-around: a worker may start a branch from a flat wall
	float foundingChance = 2e-4f;     ///< per face evaluation: a surface worker starts a brand-new entrance
	float entranceSpacing = 150.0f;   ///< founding is strongly inhibited within this many cells of an entrance

	// ---- colony drives (design, not biology) ----
	float spacePerAnt = 3.2f;          ///< cells of nest volume the colony wants per worker at the start
	float expansionPerHour = 2400.0f;  ///< additional desired volume per simulated hour
	float maxExcavationFraction = 0.42f; ///< the colony stops wanting more space beyond this fraction of the substrate
	float foodTarget = 400.0f;         ///< units of stored food the colony tries to keep
	float eatChance = 0.05f;           ///< probability a resting worker eats one unit from the store

	// ---- task mix / individual variation ----
	float digWorkerFraction = 0.46f;
	float foragerFraction = 0.12f;
	float traitVariation = 0.18f;

	// ---- rest cycles (seconds) ----
	float activeMinutesMin = 9.0f, activeMinutesMax = 26.0f;
	float restMinutesMin = 2.5f, restMinutesMax = 9.0f;

	// ---- trails ----
	int trailCell = 2;               ///< trail grid resolution in cells
	float trailUpdateHz = 3.0f;
	float digTrailHalfLife = 180.0f; ///< seconds
	float foodTrailHalfLife = 420.0f;
	float siteHalfLife = 420.0f;     ///< excavation-site mark (recruits diggers to active faces)
	float siteDeposit = 3.0f;
	float trailDiffusion = 0.06f;    ///< per update, bounded < 0.2 for stability
	float trailDeposit = 0.6f;
	float trailMax = 8.0f;

	// ---- scoring weights (normalised terms) ----
	float wTask = 1.6f;
	float wTrail = 0.9f;
	float wPersistence = 1.1f;
	float wSoil = 0.7f;
	float wDirection = 1.8f;          ///< keeping the remembered digging direction
	float wSpaceDemand = 0.6f;
	float wVariation = 0.35f;
	float wCongestion = 1.0f;
	float wObstacle = 2.0f;
	float wTravel = 0.3f;
	float softmaxTemperature = 0.35f;

	// ---- food (keeper feeding: a defined habitat mechanism) ----
	float feedingIntervalHours = 3.0f;
	int feedingAmount = 260;
	int initialFood = 320;

	// ---- events ----
	int eventCapacity = 4096;

	/// Loads "key = value" lines; unknown keys are reported through `error`.
	bool loadFile(const std::string &path, std::string *error);
	bool set(const std::string &key, const std::string &value);
	/// Stable hash of every value (stored in checkpoints and diagnostics).
	uint64_t hash() const;
	std::string dump() const;
};

} // namespace antfarm
