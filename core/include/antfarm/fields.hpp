#pragma once
// Local information fields that ants sense: task-specific trails (deposited by the relevant action,
// decaying and diffusing over simulated time, bounded) and a navigation cache over *discovered* space.

#include "antfarm/world.hpp"

#include <cstdint>
#include <vector>

namespace antfarm {

class TrailField {
public:
	void init(int worldW, int worldH, int cellSize, float halfLifeSeconds, float diffusion, float maxValue);
	void deposit(float x, float y, float amount);
	float sample(float x, float y) const;
	/// Advances decay/diffusion by `dt` seconds of simulated time. Diffusion only spreads through open space
	/// (`open` is one byte per trail cell, maintained by the simulation as cells are excavated).
	void update(const std::vector<uint8_t> &open, const std::vector<int> &openList, float dt);
	int cellSize() const { return m_cell; }
	int w() const { return m_w; }
	int h() const { return m_h; }
	const std::vector<float> &values() const { return m_v; }
	std::vector<float> &values() { return m_v; }
	float total() const;

private:
	int m_w = 0, m_h = 0, m_cell = 2;
	float m_halfLife = 120, m_diffusion = 0.05f, m_max = 8;
	std::vector<float> m_v, m_tmp;
};

/// Breadth-first distance (in cells) from every open cell to the open air above the ground. It only
/// covers space that is actually open, i.e. what the colony has discovered by digging.
class ExitDistance {
public:
	void compute(const World &world);
	/// Distance at a cell, or kUnreachable.
	uint16_t at(int x, int y) const
	{
		if (x < 0 || y < 0 || x >= m_w || y >= m_h)
			return kUnreachable;
		return m_d[size_t(y * m_w + x)];
	}
	static constexpr uint16_t kUnreachable = 0xFFFF;
	int maxDistance() const { return m_max; }
	std::vector<uint16_t> &raw() { return m_d; }
	const std::vector<uint16_t> &raw() const { return m_d; }
	void restore(int w, int h, int maxD)
	{
		m_w = w;
		m_h = h;
		m_max = maxD;
	}

private:
	int m_w = 0, m_h = 0, m_max = 0;
	std::vector<uint16_t> m_d;
	std::vector<int> m_queue;
};

} // namespace antfarm
