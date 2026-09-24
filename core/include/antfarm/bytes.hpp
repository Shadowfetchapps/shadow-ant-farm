#pragma once
// Minimal little-endian binary serialization for checkpoints (host is x86_64; the format is versioned).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace antfarm {

struct ByteWriter {
	std::vector<uint8_t> data;

	template<typename T> void pod(const T &v)
	{
		const auto *p = reinterpret_cast<const uint8_t *>(&v);
		data.insert(data.end(), p, p + sizeof(T));
	}
	template<typename T> void vec(const std::vector<T> &v)
	{
		pod<uint64_t>(v.size());
		const auto *p = reinterpret_cast<const uint8_t *>(v.data());
		data.insert(data.end(), p, p + v.size() * sizeof(T));
	}
	void str(const std::string &s)
	{
		pod<uint64_t>(s.size());
		data.insert(data.end(), s.begin(), s.end());
	}
};

struct ByteReader {
	const uint8_t *p = nullptr;
	size_t n = 0, pos = 0;
	bool ok = true;

	ByteReader(const uint8_t *data, size_t size) : p(data), n(size) {}

	template<typename T> T pod()
	{
		T v{};
		if (pos + sizeof(T) > n) {
			ok = false;
			return v;
		}
		std::memcpy(&v, p + pos, sizeof(T));
		pos += sizeof(T);
		return v;
	}
	template<typename T> bool vec(std::vector<T> &v, size_t maxElements = size_t(1) << 28)
	{
		const uint64_t count = pod<uint64_t>();
		if (!ok || count > maxElements || pos + count * sizeof(T) > n) {
			ok = false;
			return false;
		}
		v.resize(count);
		std::memcpy(v.data(), p + pos, count * sizeof(T));
		pos += count * sizeof(T);
		return true;
	}
	std::string str()
	{
		const uint64_t count = pod<uint64_t>();
		if (!ok || pos + count > n) {
			ok = false;
			return {};
		}
		std::string s(reinterpret_cast<const char *>(p + pos), count);
		pos += count;
		return s;
	}
};

} // namespace antfarm
