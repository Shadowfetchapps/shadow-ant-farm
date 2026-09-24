#pragma once
// Godot-facing wrapper around the deterministic colony core. The presentation never changes the
// simulation: it only advances it on a fixed tick and reads positions, terrain and events back.

#include "antfarm/sim.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <future>
#include <memory>

namespace godot {

class AntFarmSim : public RefCounted {
	GDCLASS(AntFarmSim, RefCounted)

public:
	AntFarmSim();
	~AntFarmSim() override;
	void _notification(int what);

	/// Starts a new colony. seed 0 draws a fresh seed from the operating system.
	bool start_new(int64_t seed, const String &config_path);
	/// Loads a checkpoint written by save_checkpoint (config comes from the file).
	bool load_checkpoint(const String &path);
	/// Atomically writes a checkpoint (temporary file, fsync, rename).
	bool save_checkpoint(const String &path) const;
	String get_last_error() const { return m_error; }
	bool is_running() const { return m_sim != nullptr; }

	/// Advances by real elapsed time on the fixed tick; at most max_ticks per call (the rest is dropped and
	/// counted, so a stall never turns into a catch-up burst). Returns the ticks run.
	int advance(double real_dt, int max_ticks);
	/// Runs exactly n ticks (accelerated/headless use).
	int step_ticks(int n);
	double get_alpha() const;
	double get_sim_seconds() const;
	int64_t get_tick() const;
	int64_t get_seed() const;
	int64_t get_dropped_ticks() const { return m_droppedTicks; }
	Vector2i get_grid_size() const;
	int get_ticks_per_second() const;

	/// Terrain as RGBA8 (width x height): R material*32 | surface-air 16 | open 8, G remaining volume,
	/// B moisture, A signed distance to the open/solid boundary (128 + 12 per cell, solid positive).
	/// Only rebuilt when the world changed since the last call; returns an empty array otherwise.
	PackedByteArray take_terrain_update(bool force);

	/// Per-ant MultiMesh buffer (12 floats of transform + 4 custom) interpolated at alpha, in 3D units:
	/// x right, y up, z toward the viewer; one grid cell is cell_size units, the ants lie at depth z.
	PackedFloat32Array build_ant_buffer(double alpha, double cell_size, double z);
	/// Loads carried in the mandibles (same layout); count in get_load_count().
	PackedFloat32Array build_load_buffer(double alpha, double cell_size, double z);
	int get_load_count() const { return m_loadCount; }

	/// Events since the last poll: [type, x, y, strength] per event (grid coordinates).
	PackedFloat32Array poll_events(int max_events);
	/// Food on the feeding stone and in underground caches: [x, y, amount] triplets, station first.
	PackedFloat32Array get_food() const;
	PackedFloat32Array get_entrances() const;

	Dictionary get_stats() const;
	String get_config_dump() const;
	String get_state_hash() const;
	int64_t get_memory_bytes() const;

protected:
	static void _bind_methods();

private:
	struct TerrainSnapshot {
		int w = 0, h = 0, cellVolume = 256;
		std::vector<uint8_t> material, origAir, moisture;
		std::vector<uint16_t> remaining;
	};
	void startTerrainJob();
	std::vector<uint8_t> buildTerrain(const TerrainSnapshot &t);

	std::unique_ptr<antfarm::Simulation> m_sim;
	double m_accum = 0;
	int64_t m_droppedTicks = 0;
	uint64_t m_eventCursor = 0;
	int m_loadCount = 0;
	bool m_terrainDirty = true;
	std::future<std::vector<uint8_t>> m_terrainJob; ///< background distance-field build (touches only the buffers below)
	std::vector<double> m_distA, m_distB, m_f, m_d, m_z;
	std::vector<int> m_v;
	std::vector<float> m_sdf, m_sdfTmp;
	std::vector<uint8_t> m_rgba;
	std::vector<antfarm::Event> m_eventScratch;
	String m_error;
};

} // namespace godot
