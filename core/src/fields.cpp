#include "antfarm/fields.hpp"

#include <algorithm>
#include <cmath>

namespace antfarm {

void TrailField::init(int worldW, int worldH, int cellSize, float halfLifeSeconds, float diffusion, float maxValue)
{
	m_cell = std::max(1, cellSize);
	m_w = (worldW + m_cell - 1) / m_cell;
	m_h = (worldH + m_cell - 1) / m_cell;
	m_halfLife = std::max(1.0f, halfLifeSeconds);
	m_diffusion = std::clamp(diffusion, 0.0f, 0.2f); // explicit scheme is stable for alpha <= 0.25
	m_max = maxValue;
	m_v.assign(size_t(m_w * m_h), 0.0f);
	m_tmp.assign(m_v.size(), 0.0f);
}

void TrailField::deposit(float x, float y, float amount)
{
	const int cx = int(x) / m_cell, cy = int(y) / m_cell;
	if (cx < 0 || cy < 0 || cx >= m_w || cy >= m_h)
		return;
	float &v = m_v[size_t(cy * m_w + cx)];
	v = std::min(m_max, v + amount);
}

float TrailField::sample(float x, float y) const
{
	const int cx = int(x) / m_cell, cy = int(y) / m_cell;
	if (cx < 0 || cy < 0 || cx >= m_w || cy >= m_h)
		return 0.0f;
	return m_v[size_t(cy * m_w + cx)];
}

void TrailField::update(const std::vector<uint8_t> &open, const std::vector<int> &openList, float dt)
{
	// Only open cells ever hold trail (deposits happen where workers stand), so only they are updated.
	const float decay = std::exp2(-dt / m_halfLife);
	const float a = m_diffusion;
	m_tmp.resize(openList.size());
	for (size_t k = 0; k < openList.size(); ++k) {
		const size_t i = size_t(openList[k]);
		const int cx = int(i) % m_w, cy = int(i) / m_w;
		const float c = m_v[i];
		float flux = 0.0f;
		if (a > 0.0f) {
			if (cx > 0 && open[i - 1])
				flux += m_v[i - 1] - c;
			if (cx + 1 < m_w && open[i + 1])
				flux += m_v[i + 1] - c;
			if (cy > 0 && open[i - size_t(m_w)])
				flux += m_v[i - size_t(m_w)] - c;
			if (cy + 1 < m_h && open[i + size_t(m_w)])
				flux += m_v[i + size_t(m_w)] - c;
		}
		const float v = (c + a * flux) * decay;
		m_tmp[k] = v < 1e-4f ? 0.0f : std::min(v, m_max);
	}
	for (size_t k = 0; k < openList.size(); ++k)
		m_v[size_t(openList[k])] = m_tmp[k];
}

float TrailField::total() const
{
	float s = 0;
	for (float v : m_v)
		s += v;
	return s;
}

void ExitDistance::compute(const World &world)
{
	m_w = world.width();
	m_h = world.height();
	m_d.assign(size_t(m_w * m_h), kUnreachable);
	m_queue.clear();
	m_queue.reserve(size_t(m_w * m_h / 4));
	// Sources: open cells above the ground (the surface air). Their distance is 0.
	for (int y = 0; y < m_h; ++y)
		for (int x = 0; x < m_w; ++x)
			if (world.isOpen(x, y) && world.isAboveGround(x, y)) {
				m_d[size_t(y * m_w + x)] = 0;
				m_queue.push_back(y * m_w + x);
			}
	m_max = 0;
	for (size_t head = 0; head < m_queue.size(); ++head) {
		const int i = m_queue[head];
		const int x = i % m_w, y = i / m_w;
		const uint16_t d = m_d[size_t(i)];
		const int nx[4] = {x - 1, x + 1, x, x};
		const int ny[4] = {y, y, y - 1, y + 1};
		for (int k = 0; k < 4; ++k) {
			if (!world.isOpen(nx[k], ny[k]))
				continue;
			const size_t j = size_t(ny[k] * m_w + nx[k]);
			if (m_d[j] != kUnreachable)
				continue;
			m_d[j] = uint16_t(d + 1);
			m_max = std::max<int>(m_max, d + 1);
			m_queue.push_back(int(j));
		}
	}
}

} // namespace antfarm
