#include "antfarm/world.hpp"

#include "antfarm/bytes.hpp"

#include <algorithm>
#include <cmath>

namespace antfarm {

namespace {

uint8_t clampByte(float v)
{
	return uint8_t(std::clamp(v, 0.0f, 255.0f));
}

} // namespace

void World::generate(const SimConfig &cfg, uint64_t terrainSeed)
{
	m_w = cfg.width;
	m_h = cfg.height;
	m_lidRows = cfg.lidRows;
	m_cellVolume = cfg.cellVolume;
	const size_t n = size_t(m_w) * size_t(m_h);
	m_material.assign(n, uint8_t(Material::Air));
	m_remaining.assign(n, 0);
	m_hardness.assign(n, 0);
	m_moisture.assign(n, 0);
	m_origAir.assign(n, 1);
	m_surface.assign(size_t(m_w), 0);
	m_spoilAccum.assign(size_t(m_w), 0);
	m_protected.assign(size_t(m_w), 0);
	m_protectedCore.assign(size_t(m_w), 0);
	m_dirty.assign(size_t(chunksX() * chunksY()), 1);
	m_excavated = m_deposited = m_diggableTotal = m_openUnderground = 0;

	Pcg32 rng(terrainSeed, 7);
	const uint32_t s0 = rng.next(), s1 = rng.next(), s2 = rng.next(), s3 = rng.next(), s4 = rng.next(), s5 = rng.next();

	// Soil character of this session: how sandy, how much clay, how many stones.
	const float sandiness = rng.range(0.25f, 0.8f);
	const float clayiness = rng.range(0.15f, 0.65f);
	const float gravelBands = rng.range(0.0f, 1.0f);
	const float moistBase = rng.range(0.35f, 0.7f);
	const float topsoilDepth = rng.range(10.0f, 26.0f);

	// Surface profile: gentle undulation plus fine relief, never touching the lid.
	const float surfaceY = cfg.surfaceFraction * float(m_h);
	for (int x = 0; x < m_w; ++x) {
		const float broad = (fbm(float(x) / 140.0f, 0.5f, s0, 3) - 0.5f) * 2.0f;
		const float fine = (fbm(float(x) / 18.0f, 3.5f, s1, 2) - 0.5f) * 2.0f;
		float y = surfaceY + broad * cfg.surfaceRoughness + fine * 1.4f;
		m_surface[size_t(x)] = std::clamp(int(std::lround(y)), m_lidRows + 20, m_h - 40);
	}

	// Strata: a stack of layers of random thickness below the topsoil, their boundaries undulating and
	// gently tilted, so the section reads as deposited soil rather than blotches.
	struct Layer {
		float bottom;
		Material m;
	};
	std::vector<Layer> layers;
	{
		float acc = topsoilDepth;
		Material prev = Material::Loam;
		while (acc < float(m_h) + 60.0f) {
			const float t = rng.range(14.0f, 58.0f);
			Material m = prev;
			for (int tries = 0; tries < 3 && m == prev; ++tries) {
				const float r = rng.uniform();
				m = r < sandiness * 0.75f ? Material::Sand : (r < sandiness * 0.75f + clayiness * 0.45f ? Material::Clay : Material::Loam);
			}
			acc += t;
			layers.push_back({acc, m});
			prev = m;
		}
	}
	const float tilt = rng.range(-14.0f, 14.0f);
	for (int y = 0; y < m_h; ++y) {
		for (int x = 0; x < m_w; ++x) {
			const int i = index(x, y);
			const int surf = m_surface[size_t(x)];
			if (y < surf)
				continue;
			m_origAir[size_t(i)] = 0;
			const float depth = float(y - surf);
			const float warp = (fbm(float(x) / 110.0f, float(y) / 90.0f, s2, 3) - 0.5f) * 16.0f + (float(x) / float(m_w) - 0.5f) * tilt;
			const float d = depth + warp;
			const float fx = float(x), fy = float(y);

			Material m;
			if (d < topsoilDepth)
				m = Material::Loam;
			else {
				m = layers.back().m;
				for (const Layer &l : layers)
					if (d < l.bottom) {
						m = l.m;
						break;
					}
				// Thin, elongated lenses of a different soil inside a layer.
				const float lens = fbm(fx / 95.0f, fy / 9.0f, s4, 3);
				const float lensKind = fbm(fx / 300.0f, fy / 200.0f, s3, 2);
				if (lens > 0.76f - clayiness * 0.05f)
					m = (m == Material::Sand) ? (lensKind > 0.5f ? Material::Clay : Material::Loam) : Material::Sand;
				// Thin, discontinuous gravel lenses (not continuous bands).
				const float band = std::fabs(std::sin((d + warp * 0.6f) / 31.0f + fbm(fx / 200.0f, 0.3f, s5, 2) * 6.0f));
				const float patch = fbm(fx / 38.0f, fy / 9.0f, s5 ^ 0x77u, 3);
				if (gravelBands > 0.3f && band > 0.992f && patch > 0.58f)
					m = Material::Gravel;
			}
			m_material[size_t(i)] = uint8_t(m);
			m_remaining[size_t(i)] = uint16_t(m_cellVolume);

			// Moisture rises with depth, with darker damp patches; the top few cells are drier.
			const float damp = fbm(fx / 70.0f, fy / 45.0f, s1 ^ 0x55u, 3);
			float moist = moistBase * 120.0f + std::min(depth, 300.0f) * 0.25f + (damp - 0.5f) * 140.0f;
			if (depth < 6)
				moist *= 0.55f + depth * 0.07f;
			m_moisture[size_t(i)] = clampByte(moist);

			float hard;
			switch (m) {
			case Material::Sand: hard = 55; break;
			case Material::Loam: hard = 85; break;
			case Material::Clay: hard = 150; break;
			case Material::Gravel: hard = 185; break;
			default: hard = 100; break;
			}
			hard *= 0.82f + 0.36f * fbm(fx / 25.0f, fy / 25.0f, s0 ^ 0xA5u, 2);
			// Damp soil is more cohesive and easier to shape; very dry clay is hard.
			const float mm = float(m_moisture[size_t(i)]) / 255.0f;
			if (m == Material::Clay)
				hard *= 1.25f - 0.5f * mm;
			else
				hard *= 1.08f - 0.25f * mm;
			m_hardness[size_t(i)] = clampByte(hard);
		}
	}

	// Stones: small irregular pebbles that cannot be excavated.
	const int stones = int(rng.range(70.0f, 170.0f) * float(m_w * m_h) / (960.0f * 540.0f));
	for (int s = 0; s < stones; ++s) {
		const int cx = int(rng.below(uint32_t(m_w)));
		const int cy = int(rng.range(float(m_surface[size_t(cx)] + 4), float(m_h - 2)));
		const float rx = rng.range(0.9f, 3.6f), ry = rx * rng.range(0.55f, 1.0f);
		const float ang = rng.range(0.0f, 3.14159f);
		const float ca = std::cos(ang), sa = std::sin(ang);
		const int r = int(std::ceil(std::max(rx, ry))) + 1;
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx) {
				const int x = cx + dx, y = cy + dy;
				if (!inBounds(x, y) || m_origAir[size_t(index(x, y))])
					continue;
				const float u = (float(dx) * ca + float(dy) * sa) / rx;
				const float v = (-float(dx) * sa + float(dy) * ca) / ry;
				const float edge = 1.0f + (hashUnit(x, y, s2) - 0.5f) * 0.35f;
				if (u * u + v * v <= edge) {
					m_material[size_t(index(x, y))] = uint8_t(Material::Stone);
					m_hardness[size_t(index(x, y))] = 255;
				}
			}
	}

	// Root fragments: a few thin, meandering fibres descending from the surface or floating as pieces.
	const int roots = int(rng.range(2.0f, 7.0f));
	for (int r = 0; r < roots; ++r) {
		float x = rng.range(20.0f, float(m_w - 20));
		float y = float(m_surface[size_t(int(x))]) + rng.range(-1.0f, 30.0f);
		float ang = 1.5708f + rng.range(-0.5f, 0.5f);
		const int len = int(rng.range(25.0f, 150.0f));
		for (int k = 0; k < len; ++k) {
			const int ix = int(x), iy = int(y);
			if (inBounds(ix, iy) && !m_origAir[size_t(index(ix, iy))]) {
				m_material[size_t(index(ix, iy))] = uint8_t(Material::Root);
				m_hardness[size_t(index(ix, iy))] = 255;
			}
			ang += rng.range(-0.18f, 0.18f);
			ang = std::clamp(ang, 0.5f, 2.64f);
			x += std::cos(ang) * 0.9f;
			y += std::sin(ang) * 0.9f;
			if (y >= float(m_h - 3) || x < 2 || x >= float(m_w - 2))
				break;
		}
	}

	m_top.assign(size_t(m_w), 0);
	for (int x = 0; x < m_w; ++x)
		refreshTop(x);
	// Keep a solid floor and walls: the bottom rows and the side columns are part of the frame.
	for (int i = 0; i < m_w * m_h; ++i)
		if (!m_origAir[size_t(i)] && isDiggable(Material(m_material[size_t(i)])))
			m_diggableTotal += m_cellVolume;
}

int World::scanTop(int x) const
{
	for (int y = m_lidRows; y < m_h; ++y)
		if (m_remaining[size_t(index(x, y))] != 0)
			return y;
	return m_h;
}

bool World::isExcavatable(int x, int y) const
{
	if (!inBounds(x, y) || x < 1 || x >= m_w - 1 || y >= m_h - 1)
		return false;
	const int i = index(x, y);
	// Underground, packed spoil (backfill) can be dug out again; the loose mounds above ground are not dug.
	const Material m = Material(m_material[size_t(i)]);
	return m_remaining[size_t(i)] != 0 && !m_origAir[size_t(i)] && (isDiggable(m) || m == Material::Spoil);
}

bool World::isFace(int x, int y) const
{
	if (!isExcavatable(x, y))
		return false;
	return isOpen(x - 1, y) || isOpen(x + 1, y) || isOpen(x, y - 1) || isOpen(x, y + 1);
}

void World::markDirty(int x, int y)
{
	const int cx = x / kChunk, cy = y / kChunk;
	auto mark = [&](int ax, int ay) {
		if (ax >= 0 && ay >= 0 && ax < chunksX() && ay < chunksY())
			m_dirty[size_t(ay * chunksX() + ax)] = 1;
	};
	mark(cx, cy);
	// Neighbour chunks share a one-cell apron in the renderer.
	if (x % kChunk == 0)
		mark(cx - 1, cy);
	if (x % kChunk == kChunk - 1)
		mark(cx + 1, cy);
	if (y % kChunk == 0)
		mark(cx, cy - 1);
	if (y % kChunk == kChunk - 1)
		mark(cx, cy + 1);
}

void World::clearDirty()
{
	std::fill(m_dirty.begin(), m_dirty.end(), uint8_t(0));
}

void World::setOpen(int i)
{
	if (!m_origAir[size_t(i)])
		++m_openUnderground;
}

int World::excavate(int x, int y, int amount)
{
	if (!isExcavatable(x, y) || amount <= 0)
		return 0;
	const int i = index(x, y);
	const int take = std::min<int>(amount, m_remaining[size_t(i)]);
	m_remaining[size_t(i)] = uint16_t(m_remaining[size_t(i)] - take);
	m_excavated += take;
	if (m_remaining[size_t(i)] == 0) {
		setOpen(i);
		if (y <= m_top[size_t(x)])
			refreshTop(x);
	}
	markDirty(x, y);
	return take;
}

void World::depositSpoil(int x, int volume)
{
	x = std::clamp(x, 2, m_w - 3);
	// Never pour spoil into an entrance: slide to the nearest column whose top is at or above the original surface.
	for (int off = 0; off < m_w; ++off) {
		const int cand[2] = {x + off, x - off};
		bool placed = false;
		for (int c : cand) {
			if (c < 2 || c > m_w - 3)
				continue;
			if (!m_protected[size_t(c)] && groundTop(c) <= m_surface[size_t(c)]) {
				x = c;
				placed = true;
				break;
			}
		}
		if (placed)
			break;
	}
	m_spoilAccum[size_t(x)] += volume;
	m_deposited += volume;
	while (m_spoilAccum[size_t(x)] >= m_cellVolume) {
		const int top = groundTop(x);
		const int y = top - 1;
		if (y <= m_lidRows + 6)
			break; // pile reached the lid clearance: keep the volume pending (accounted for)
		const int i = index(x, y);
		m_material[size_t(i)] = uint8_t(Material::Spoil);
		m_remaining[size_t(i)] = uint16_t(m_cellVolume);
		m_hardness[size_t(i)] = 40;
		m_moisture[size_t(i)] = uint8_t(std::min<int>(255, m_moisture[size_t(index(x, std::min(top, m_h - 1)))] / 2 + 20));
		m_spoilAccum[size_t(x)] -= m_cellVolume;
		m_top[size_t(x)] = y;
		markDirty(x, y);
	}
}

bool World::backfill(int x, int y, int volume)
{
	// Packing a load into an underground dead end (the whole load fills the cell; volume is conserved).
	if (!inBounds(x, y) || x < 3 || x > m_w - 4 || y > m_h - 3 || volume != m_cellVolume)
		return false;
	const int i = index(x, y);
	if (m_remaining[size_t(i)] != 0 || m_origAir[size_t(i)])
		return false;
	m_material[size_t(i)] = uint8_t(Material::Spoil);
	m_remaining[size_t(i)] = uint16_t(volume);
	m_hardness[size_t(i)] = 60;
	m_deposited += volume;
	--m_openUnderground;
	if (y < m_top[size_t(x)])
		refreshTop(x);
	markDirty(x, y);
	return true;
}

void World::relaxSpoil(int iterations)
{
	// Loose material above the angle of repose slides to a lower neighbour (volume preserved exactly).
	for (int it = 0; it < iterations; ++it) {
		for (int x = 3; x < m_w - 3; ++x) {
			const int top = groundTop(x);
			if (top >= m_h || m_material[size_t(index(x, top))] != uint8_t(Material::Spoil))
				continue;
			for (int dir : {-1, 1}) {
				const int nx = x + dir;
				const int ntop = groundTop(nx);
				// Loose soil rests at roughly 27 degrees: it slides when the neighbour is two lower, or one
				// lower with the next column lower still. It may spread to the rim of an entrance but never
				// into the hole itself.
				const int nx2 = std::clamp(x + 2 * dir, 0, m_w - 1);
				const bool steep = ntop - top >= 2 || (ntop - top >= 1 && groundTop(nx2) - top >= 2);
				if (steep && ntop <= m_surface[size_t(nx)] && !m_protectedCore[size_t(nx)]) {
					const int from = index(x, top);
					const int to = index(nx, ntop - 1);
					m_material[size_t(to)] = uint8_t(Material::Spoil);
					m_remaining[size_t(to)] = uint16_t(m_cellVolume);
					m_hardness[size_t(to)] = m_hardness[size_t(from)];
					m_moisture[size_t(to)] = m_moisture[size_t(from)];
					m_material[size_t(from)] = uint8_t(Material::Air);
					m_remaining[size_t(from)] = 0;
					refreshTop(x);
					m_top[size_t(nx)] = ntop - 1;
					markDirty(x, top);
					markDirty(nx, ntop - 1);
					break;
				}
			}
		}
	}
}

void World::setProtectedColumns(const std::vector<int> &centers, int radius)
{
	std::fill(m_protected.begin(), m_protected.end(), uint8_t(0));
	m_protectedCore.assign(m_protected.size(), uint8_t(0));
	for (int c : centers)
		for (int x = c - radius; x <= c + radius; ++x)
			if (x >= 0 && x < m_w) {
				m_protected[size_t(x)] = 1;
				if (std::abs(x - c) <= 1)
					m_protectedCore[size_t(x)] = 1;
			}
}

int64_t World::pendingSpoil() const
{
	int64_t s = 0;
	for (int v : m_spoilAccum)
		s += v;
	return s;
}

uint64_t World::checksum() const
{
	uint64_t h = 1469598103934665603ULL;
	auto mix = [&h](const void *p, size_t n) {
		const auto *b = static_cast<const uint8_t *>(p);
		for (size_t i = 0; i < n; ++i) {
			h ^= b[i];
			h *= 1099511628211ULL;
		}
	};
	mix(m_material.data(), m_material.size());
	mix(m_remaining.data(), m_remaining.size() * 2);
	mix(m_spoilAccum.data(), m_spoilAccum.size() * sizeof(int));
	return h;
}

void World::save(ByteWriter &w) const
{
	w.pod<int32_t>(m_w);
	w.pod<int32_t>(m_h);
	w.pod<int32_t>(m_lidRows);
	w.pod<int32_t>(m_cellVolume);
	w.vec(m_material);
	w.vec(m_remaining);
	w.vec(m_hardness);
	w.vec(m_moisture);
	w.vec(m_origAir);
	w.vec(m_surface);
	w.vec(m_spoilAccum);
	w.pod(m_excavated);
	w.pod(m_deposited);
	w.pod(m_diggableTotal);
	w.pod(m_openUnderground);
}

bool World::load(ByteReader &r)
{
	m_w = r.pod<int32_t>();
	m_h = r.pod<int32_t>();
	m_lidRows = r.pod<int32_t>();
	m_cellVolume = r.pod<int32_t>();
	if (!r.ok || m_w <= 0 || m_h <= 0 || m_w > 8192 || m_h > 8192)
		return false;
	r.vec(m_material);
	r.vec(m_remaining);
	r.vec(m_hardness);
	r.vec(m_moisture);
	r.vec(m_origAir);
	r.vec(m_surface);
	r.vec(m_spoilAccum);
	m_excavated = r.pod<int64_t>();
	m_deposited = r.pod<int64_t>();
	m_diggableTotal = r.pod<int64_t>();
	m_openUnderground = r.pod<int64_t>();
	const size_t n = size_t(m_w) * size_t(m_h);
	if (!r.ok || m_material.size() != n || m_remaining.size() != n || m_surface.size() != size_t(m_w))
		return false;
	m_dirty.assign(size_t(chunksX() * chunksY()), 1);
	m_protected.assign(size_t(m_w), 0);
	m_protectedCore.assign(size_t(m_w), 0);
	m_top.assign(size_t(m_w), 0);
	for (int x = 0; x < m_w; ++x)
		refreshTop(x);
	return true;
}

} // namespace antfarm
