#pragma once
// Streams the farm's own rendered frames and synthesised sound to an RTMP/RTMPS server through ffmpeg.
// No screen capture is involved: the presentation hands over frames of its stream camera and the audio it
// just rendered. ffmpeg runs as a child process (NVENC when available, x264 otherwise), is fed at a steady
// frame and sample rate by two writer threads, and is restarted with back-off when the connection drops.
// Stream keys live in the desktop keyring (secret-tool) and never pass through scripts, settings or logs.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace godot {

class AntLiveStream : public RefCounted {
	GDCLASS(AntLiveStream, RefCounted)

public:
	AntLiveStream();
	~AntLiveStream() override;

	/// Starts streaming. Keys: destination ("youtube", "x", "custom"), server, width, height, fps,
	/// video_kbps, keyframe_seconds, url_override (a full URL including the key, for tests; skips the keyring).
	bool start(const Dictionary &config);
	void stop();
	bool is_active() const { return m_running.load(); }
	/// Latest frame of the stream camera (RGBA8, width × height as configured); frames of another size are ignored.
	void push_video(const PackedByteArray &rgba, int width, int height);
	/// Audio just rendered for the farm (48 kHz stereo, -1..1).
	void push_audio(const PackedVector2Array &frames);
	/// state ("idle", "starting", "live", "reconnecting", "failed"), message, kbps, fps, uptime_seconds,
	/// dropped_frames, restarts, encoder.
	Dictionary get_status() const;

	static bool store_key(const String &destination, const String &key);
	static bool has_key(const String &destination);
	static bool clear_key(const String &destination);
	static bool ffmpeg_available();

protected:
	static void _bind_methods();

private:
	struct Session;
	void supervise();
	bool runSession(const std::string &url, const std::string &encoder);
	void videoWriter(Session *s);
	void audioWriter(Session *s);
	void setState(const char *state, const std::string &message);
	std::string redact(const std::string &line) const;

	// configuration
	std::string m_destination, m_server, m_urlOverride, m_key;
	int m_width = 1280, m_height = 720, m_fps = 30, m_videoKbps = 4000, m_keyframeSeconds = 2;
	std::string m_encoder; // "h264_nvenc" or "libx264"

	std::atomic<bool> m_running{false};
	std::thread m_supervisor;
	std::mutex m_wakeMutex;
	std::condition_variable m_wake;

	// latest frame and audio ring
	std::mutex m_frameMutex;
	std::shared_ptr<const std::vector<uint8_t>> m_frame;
	std::mutex m_audioMutex;
	std::vector<int16_t> m_audio; // interleaved stereo, bounded
	size_t m_audioRead = 0;

	// status
	mutable std::mutex m_statusMutex;
	std::string m_state = "idle", m_message, m_lastError, m_encoderInUse;
	double m_kbps = 0, m_outFps = 0;
	int64_t m_dropped = 0;
	int m_restarts = 0;
	int64_t m_liveSinceMs = 0;
};

} // namespace godot
