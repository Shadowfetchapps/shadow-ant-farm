#pragma once
// The excavatable habitat: a grid of cells between two glass panes. Soil starts intact; only ants
// standing at an exposed face can remove it, pellet by pellet, and every removed unit is accounted for.

#include "antfarm/config.hpp"
#include "antfarm/rng.hpp"

#include <cstdint>
#include <vector>

namespace antfarm {

enum class Material : uint8_t {
	Air = 0,   ///< open space above the original surface
	Sand = 1,
	Loam = 2,
	Clay = 3,
	Gravel = 4, ///< diggable but hard
	Stone = 5,  ///< cannot be excavated
	Root = 6,   ///< cannot be excavated (ants work around root fragments)
	Spoil = 7,  ///< loose excavated soil deposited on the surface
};

inline bool isDiggable(Material m)
{
	return m == Material::Sand || m == Material::Loam || m == Material::Clay || m == Material::Gravel;
}

/// A tiny RLE-free binary writer/reader for checkpoints.
struct ByteWriter;
struct ByteReader;

class World {
public:
	void generate(const SimConfig &cfg, uint64_t terrainSeed);

	int width() const { return m_w; }
	int height() const { return m_h; }
	int index(int x, int y) const { return y * m_w + x; }
	bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < m_w && y < m_h; }

	Material material(int i) const { return Material(m_material[i]); }
	/// Remaining volume of the cell (0 = open, cellVolume = intact).
	uint16_t remaining(int i) const { return m_remaining[i]; }
	bool isOpen(int i) const { return m_remaining[i] == 0; }
	bool isOpen(int x, int y) const { return inBounds(x, y) && m_remaining[index(x, y)] == 0 && y >= m_lidRows; }
	bool isSolid(int x, int y) const { return !inBounds(x, y) || m_remaining[index(x, y)] != 0; }
	uint8_t hardness(int i) const { return m_hardness[i]; }
	uint8_t moisture(int i) const { return m_moisture[i]; }
	bool wasOriginallyAir(int i) const { return m_origAir[i] != 0; }
	/// Cell is part of the surface-air region (above the original ground), where ants walk on the soil/glass.
	bool isAboveGround(int x, int y) const { return inBounds(x, y) && m_origAir[index(x, y)] != 0; }
	int originalSurface(int x) const { return m_surface[x]; }
	/// Current top solid cell of a column (including spoil). Cached per column.
	int groundTop(int x) const { return m_top[size_t(x)]; }

	/// A face is intact, excavatable material with at least one open 4-neighbour.
	bool isFace(int x, int y) const;
	bool isExcavatable(int x, int y) const;

	/// Removes up to `amount` volume units from a cell; returns what was removed. Marks the chunk dirty.
	int excavate(int x, int y, int amount);
	/// Adds loose material on the surface at column x; returns true on success (it always finds room).
	void depositSpoil(int x, int volume);
	/// Packs one cell's worth of carried soil into an open underground cell (backfilling a dead end).
	bool backfill(int x, int y, int volume);
	/// Moves spoil down slopes steeper than the angle of repose (a few cells per call).
	void relaxSpoil(int iterations);
	/// Columns where spoil may not be dumped or slump to (entrance sites).
	void setProtectedColumns(const std::vector<int> &centers, int radius);
	bool isProtected(int x) const { return x >= 0 && x < m_w && m_protected[size_t(x)] != 0; }

	// Accounting (volume units).
	int64_t excavatedVolume() const { return m_excavated; }
	int64_t depositedVolume() const { return m_deposited; }
	int64_t pendingSpoil() const;
	int64_t diggableTotal() const { return m_diggableTotal; }
	int64_t openUndergroundCells() const { return m_openUnderground; }

	// Dirty-chunk tracking for renderers (chunkSize cells square).
	static constexpr int kChunk = 64;
	int chunksX() const { return (m_w + kChunk - 1) / kChunk; }
	int chunksY() const { return (m_h + kChunk - 1) / kChunk; }
	const std::vector<uint8_t> &dirtyChunks() const { return m_dirty; }
	void clearDirty();
	void markDirty(int x, int y);

	int cellVolume() const { return m_cellVolume; }
	const std::vector<uint8_t> &materials() const { return m_material; }
	const std::vector<uint16_t> &remainingVolumes() const { return m_remaining; }
	const std::vector<uint8_t> &moistures() const { return m_moisture; }

	void save(ByteWriter &w) const;
	bool load(ByteReader &r);

	uint64_t checksum() const;

private:
	void setOpen(int i);
	int scanTop(int x) const;
	void refreshTop(int x) { m_top[size_t(x)] = scanTop(x); }

	int m_w = 0, m_h = 0, m_lidRows = 3, m_cellVolume = 256;
	std::vector<uint8_t> m_material;
	std::vector<uint16_t> m_remaining;
	std::vector<uint8_t> m_hardness;
	std::vector<uint8_t> m_moisture;
	std::vector<uint8_t> m_origAir;
	std::vector<int> m_surface;
	std::vector<int> m_spoilAccum; ///< per column, volume waiting to form a spoil cell
	std::vector<uint8_t> m_protected;     ///< no dumping (entrance and its rim)
	std::vector<uint8_t> m_protectedCore; ///< no slumping either (the hole itself); derived from m_protected's centres
	std::vector<int> m_top;
	std::vector<uint8_t> m_dirty;
	int64_t m_excavated = 0;
	int64_t m_deposited = 0;
	int64_t m_diggableTotal = 0;
	int64_t m_openUnderground = 0;
};

} // namespace antfarm
