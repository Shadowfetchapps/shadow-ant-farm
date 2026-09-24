#include "ant_live_stream.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace godot;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kAudioRate = 48000;
constexpr size_t kAudioCap = size_t(kAudioRate) * 2; // one second of interleaved stereo samples

std::string toStd(const String &s)
{
	const CharString c = s.utf8();
	return std::string(c.get_data(), size_t(c.length()));
}

int64_t nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool writeAll(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		const ssize_t w = ::write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += w;
		n -= size_t(w);
	}
	return true;
}

/// Runs a program with optional stdin data; captures stdout. Returns the exit status (or -1).
int runCapture(const std::vector<std::string> &argv, const std::string &stdinData, std::string *out)
{
	int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1};
	if (pipe2(inPipe, O_CLOEXEC) != 0 || pipe2(outPipe, O_CLOEXEC) != 0)
		return -1;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
	posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	std::vector<char *> args;
	for (const std::string &a : argv)
		args.push_back(const_cast<char *>(a.c_str()));
	args.push_back(nullptr);
	pid_t pid = -1;
	const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	::close(inPipe[0]);
	::close(outPipe[1]);
	if (rc != 0) {
		::close(inPipe[1]);
		::close(outPipe[0]);
		return -1;
	}
	writeAll(inPipe[1], reinterpret_cast<const uint8_t *>(stdinData.data()), stdinData.size());
	::close(inPipe[1]);
	char buf[4096];
	for (ssize_t r; (r = ::read(outPipe[0], buf, sizeof buf)) != 0;) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (out)
			out->append(buf, size_t(r));
	}
	::close(outPipe[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::vector<std::string> keyAttrs(const std::string &dest)
{
	return {"application", "shadow-ant-farm", "destination", dest};
}

std::string lookupKey(const std::string &dest)
{
	std::vector<std::string> argv = {"secret-tool", "lookup"};
	for (const std::string &a : keyAttrs(dest))
		argv.push_back(a);
	std::string out;
	if (runCapture(argv, {}, &out) != 0)
		return {};
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
		out.pop_back();
	return out;
}

bool isNvencProblem(const std::string &line)
{
	static const char *needles[] = {"nvenc", "NVENC", "No capable devices", "CUDA", "libnvidia-encode", "Cannot load"};
	for (const char *n : needles)
		if (line.find(n) != std::string::npos)
			return true;
	return false;
}

} // namespace

struct AntLiveStream::Session {
	pid_t pid = -1;
	int videoFd = -1, audioFd = -1, errFd = -1;
	std::atomic<bool> alive{true};
};

AntLiveStream::AntLiveStream()
{
	// A write to a pipe whose reader (ffmpeg) has exited must return EPIPE, not kill the program.
	static bool once = [] {
		std::signal(SIGPIPE, SIG_IGN);
		return true;
	}();
	(void)once;
}

AntLiveStream::~AntLiveStream()
{
	stop();
}

bool AntLiveStream::start(const Dictionary &config)
{
	if (m_running.load())
		return true;
	if (m_supervisor.joinable())
		m_supervisor.join(); // a previous session that ended on its own (failed)
	m_destination = toStd(String(config.get("destination", "youtube")));
	m_server = toStd(String(config.get("server", "")));
	m_urlOverride = toStd(String(config.get("url_override", "")));
	m_width = std::clamp(int(config.get("width", 1280)), 320, 3840);
	m_height = std::clamp(int(config.get("height", 720)), 240, 2160);
	m_fps = std::clamp(int(config.get("fps", 30)), 10, 60);
	m_videoKbps = std::clamp(int(config.get("video_kbps", 4000)), 500, 20000);
	m_keyframeSeconds = std::clamp(int(config.get("keyframe_seconds", 2)), 1, 4);
	m_key.clear();
	{
		std::lock_guard<std::mutex> lk(m_statusMutex);
		m_restarts = 0;
		m_lastError.clear();
		m_kbps = m_outFps = 0;
		m_dropped = 0;
		m_liveSinceMs = 0;
	}
	if (!ffmpeg_available()) {
		setState("failed", "ffmpeg is not installed (sudo apt install ffmpeg).");
		return false;
	}
	if (m_urlOverride.empty()) {
		if (m_server.empty()) {
			setState("failed", "No server address is set for this destination.");
			return false;
		}
		m_key = lookupKey(m_destination);
		if (m_key.empty()) {
			setState("failed", "No stream key is saved for this destination. Paste it in the operator window and press Save key.");
			return false;
		}
	}
	std::string encoders;
	runCapture({"ffmpeg", "-hide_banner", "-encoders"}, {}, &encoders);
	m_encoder = encoders.find("h264_nvenc") != std::string::npos ? "h264_nvenc" : "libx264";
	{
		std::lock_guard<std::mutex> lk(m_frameMutex);
		m_frame.reset();
	}
	{
		std::lock_guard<std::mutex> lk(m_audioMutex);
		m_audio.clear();
		m_audioRead = 0;
	}
	m_running = true;
	setState("starting", "Connecting…");
	m_supervisor = std::thread([this] { supervise(); });
	return true;
}

void AntLiveStream::stop()
{
	if (!m_supervisor.joinable()) {
		m_running = false;
		return;
	}
	m_running = false;
	m_wake.notify_all();
	m_supervisor.join();
	setState("idle", "Stream ended.");
}

void AntLiveStream::push_video(const PackedByteArray &rgba, int width, int height)
{
	if (!m_running.load() || width != m_width || height != m_height || rgba.size() != int64_t(width) * height * 4)
		return;
	auto frame = std::make_shared<std::vector<uint8_t>>(size_t(rgba.size()));
	std::memcpy(frame->data(), rgba.ptr(), frame->size());
	std::lock_guard<std::mutex> lk(m_frameMutex);
	m_frame = std::move(frame);
}

void AntLiveStream::push_audio(const PackedVector2Array &frames)
{
	if (!m_running.load())
		return;
	std::lock_guard<std::mutex> lk(m_audioMutex);
	const Vector2 *f = frames.ptr();
	for (int64_t i = 0; i < frames.size(); ++i) {
		m_audio.push_back(int16_t(std::clamp(f[i].x, -1.0f, 1.0f) * 32767.0f));
		m_audio.push_back(int16_t(std::clamp(f[i].y, -1.0f, 1.0f) * 32767.0f));
	}
	// Keep at most a second queued: if the writer fell behind, drop the oldest audio rather than drift.
	const size_t queued = m_audio.size() - m_audioRead;
	if (queued > kAudioCap)
		m_audioRead += (queued - kAudioCap) & ~size_t(1);
	if (m_audioRead > kAudioCap) {
		m_audio.erase(m_audio.begin(), m_audio.begin() + std::ptrdiff_t(m_audioRead));
		m_audioRead = 0;
	}
}

void AntLiveStream::setState(const char *state, const std::string &message)
{
	std::lock_guard<std::mutex> lk(m_statusMutex);
	m_state = state;
	m_message = message;
}

std::string AntLiveStream::redact(const std::string &line) const
{
	std::string s = line;
	if (!m_key.empty())
		for (size_t p; (p = s.find(m_key)) != std::string::npos;)
			s.replace(p, m_key.size(), "<key>");
	return s;
}

void AntLiveStream::supervise()
{
	int backoff = 2, quickFails = 0;
	std::string base = !m_urlOverride.empty() ? m_urlOverride : m_server;
	std::string url = base;
	if (m_urlOverride.empty()) {
		while (!url.empty() && url.back() == '/')
			url.pop_back();
		url += "/" + m_key;
	}
	while (m_running.load()) {
		const bool wentLive = runSession(url, m_encoder);
		if (!m_running.load())
			break;
		std::string err;
		{
			std::lock_guard<std::mutex> lk(m_statusMutex);
			err = m_lastError;
			m_kbps = m_outFps = 0;
			m_liveSinceMs = 0;
		}
		if (!wentLive && m_encoder == "h264_nvenc" && isNvencProblem(err)) {
			m_encoder = "libx264"; // the GPU encoder is not usable here: carry on with the software encoder
			continue;
		}
		if (wentLive) {
			quickFails = 0;
			backoff = 2;
		} else if (++quickFails >= 5) {
			setState("failed", "The server keeps closing the connection straight away. Check the stream key and the server "
			                   "address for this destination." + (err.empty() ? std::string() : " (" + err + ")"));
			m_running = false;
			break;
		}
		{
			std::lock_guard<std::mutex> lk(m_statusMutex);
			++m_restarts;
		}
		setState("reconnecting", std::string(wentLive ? "Connection lost" : "Could not connect") + " — trying again in " +
		                             std::to_string(backoff) + " s." + (err.empty() ? std::string() : " (" + err + ")"));
		std::unique_lock<std::mutex> lk(m_wakeMutex);
		m_wake.wait_for(lk, std::chrono::seconds(backoff), [this] { return !m_running.load(); });
		backoff = std::min(backoff * 2, 30);
	}
}

bool AntLiveStream::runSession(const std::string &url, const std::string &encoder)
{
	int vPipe[2], aPipe[2], ePipe[2];
	if (pipe2(vPipe, O_CLOEXEC) != 0 || pipe2(aPipe, O_CLOEXEC) != 0 || pipe2(ePipe, O_CLOEXEC) != 0) {
		std::lock_guard<std::mutex> lk(m_statusMutex);
		m_lastError = "could not create pipes";
		return false;
	}
	const int g = m_fps * m_keyframeSeconds;
	const std::string kb = std::to_string(m_videoKbps) + "k", buf = std::to_string(m_videoKbps * 2) + "k";
	std::vector<std::string> argv = {
		"ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostats", "-progress", "pipe:2", "-stats_period", "1",
		"-thread_queue_size", "64", "-f", "rawvideo", "-pix_fmt", "rgba", "-video_size",
		std::to_string(m_width) + "x" + std::to_string(m_height), "-framerate", std::to_string(m_fps), "-i", "pipe:0",
		"-thread_queue_size", "512", "-f", "s16le", "-ar", std::to_string(kAudioRate), "-ac", "2", "-i", "pipe:3",
		"-map", "0:v", "-map", "1:a", "-vf", "format=yuv420p"};
	if (encoder == "h264_nvenc") {
		for (const char *a : {"-c:v", "h264_nvenc", "-preset", "p5", "-tune", "hq", "-rc", "cbr", "-profile:v", "high", "-bf", "0"})
			argv.push_back(a);
	} else {
		for (const char *a : {"-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high", "-bf", "0", "-sc_threshold", "0"})
			argv.push_back(a);
	}
	for (const std::string &a : std::vector<std::string>{"-b:v", kb, "-maxrate", kb, "-bufsize", buf, "-g", std::to_string(g),
	                                                     "-keyint_min", std::to_string(g), "-c:a", "aac", "-b:a", "128k", "-ar",
	                                                     std::to_string(kAudioRate), "-ac", "2", "-f", "flv", url})
		argv.push_back(a);

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, vPipe[0], 0);
	posix_spawn_file_actions_adddup2(&fa, aPipe[0], 3);
	posix_spawn_file_actions_adddup2(&fa, ePipe[1], 2);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	std::vector<char *> args;
	for (const std::string &a : argv)
		args.push_back(const_cast<char *>(a.c_str()));
	args.push_back(nullptr);
	auto session = std::make_unique<Session>();
	const int rc = posix_spawnp(&session->pid, "ffmpeg", &fa, nullptr, args.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	::close(vPipe[0]);
	::close(aPipe[0]);
	::close(ePipe[1]);
	if (rc != 0) {
		::close(vPipe[1]);
		::close(aPipe[1]);
		::close(ePipe[0]);
		std::lock_guard<std::mutex> lk(m_statusMutex);
		m_lastError = "could not start ffmpeg";
		return false;
	}
	session->videoFd = vPipe[1];
	session->audioFd = aPipe[1];
	session->errFd = ePipe[0];
	{
		std::lock_guard<std::mutex> lk(m_statusMutex);
		m_lastError.clear();
		m_encoderInUse = encoder;
	}
	Session *s = session.get();
	std::thread vw([this, s] { videoWriter(s); });
	std::thread aw([this, s] { audioWriter(s); });

	bool wentLive = false;
	int64_t lastTotal = 0;
	std::string pending;
	char chunk[4096];
	int64_t stopRequestedMs = 0;
	bool termSent = false, killSent = false;
	for (;;) {
		// Stopping: close the inputs so ffmpeg flushes and exits; terminate it if it does not in time. The process
		// is only reaped after this loop, so its PID cannot have been reused while it is signalled here.
		if (!m_running.load() || !s->alive.load()) {
			if (stopRequestedMs == 0) {
				stopRequestedMs = nowMs();
				s->alive = false;
			}
			const int64_t waited = nowMs() - stopRequestedMs;
			if (waited > 3000 && !termSent) {
				::kill(s->pid, SIGTERM);
				termSent = true;
			}
			if (waited > 6000 && !killSent) {
				::kill(s->pid, SIGKILL);
				killSent = true;
			}
		}
		pollfd pfd{s->errFd, POLLIN, 0};
		const int pr = ::poll(&pfd, 1, 200);
		if (pr < 0 && errno != EINTR)
			break;
		if (pr <= 0)
			continue;
		const ssize_t r = ::read(s->errFd, chunk, sizeof chunk);
		if (r == 0)
			break;
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			break;
		}
		pending.append(chunk, size_t(r));
		for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
			std::string line = pending.substr(0, nl);
			pending.erase(0, nl + 1);
			while (!line.empty() && line.back() == '\r')
				line.pop_back();
			// Progress lines are "key=value" with a lower-case key; everything else is a message from ffmpeg.
			const size_t eq = line.find('=');
			bool kv = eq != std::string::npos && eq > 0;
			for (size_t i = 0; kv && i < eq; ++i)
				kv = (line[i] >= 'a' && line[i] <= 'z') || (line[i] >= '0' && line[i] <= '9') || line[i] == '_';
			if (kv) {
				const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
				std::lock_guard<std::mutex> lk(m_statusMutex);
				if (k == "bitrate")
					m_kbps = std::atof(v.c_str());
				else if (k == "fps")
					m_outFps = std::atof(v.c_str());
				else if (k == "drop_frames")
					m_dropped = std::atoll(v.c_str());
				else if (k == "total_size") {
					const int64_t total = std::atoll(v.c_str());
					if (total > lastTotal && total > 4096 && !wentLive) {
						wentLive = true;
						m_liveSinceMs = nowMs();
						m_state = "live";
						m_message = "Live — sending to the server.";
					}
					lastTotal = std::max(lastTotal, total);
				}
			} else if (!line.empty()) {
				std::lock_guard<std::mutex> lk(m_statusMutex);
				m_lastError = redact(line);
			}
		}
	}
	s->alive = false;
	vw.join();
	aw.join();
	::close(s->errFd);
	int status = 0;
	while (waitpid(s->pid, &status, 0) < 0 && errno == EINTR) {
	}
	return wentLive;
}

void AntLiveStream::videoWriter(Session *s)
{
	const size_t bytes = size_t(m_width) * size_t(m_height) * 4;
	const std::vector<uint8_t> black(bytes, 0);
	const auto period = std::chrono::nanoseconds(1000000000LL / m_fps);
	auto next = Clock::now();
	while (s->alive.load()) {
		std::this_thread::sleep_until(next);
		std::shared_ptr<const std::vector<uint8_t>> frame;
		{
			std::lock_guard<std::mutex> lk(m_frameMutex);
			frame = m_frame;
		}
		const uint8_t *data = frame ? frame->data() : black.data();
		if (!writeAll(s->videoFd, data, bytes))
			break;
		// Never skip ahead after a stall (ffmpeg connecting, a network hiccup): catch up by repeating the latest
		// frame, so the picture's timeline stays equal to real time, exactly like the sound's.
		next += period;
	}
	::close(s->videoFd);
	s->alive = false;
}

void AntLiveStream::audioWriter(Session *s)
{
	constexpr int chunkFrames = kAudioRate / 50; // 20 ms
	std::vector<int16_t> chunk(size_t(chunkFrames) * 2);
	const auto period = std::chrono::milliseconds(20);
	auto next = Clock::now();
	while (s->alive.load()) {
		std::this_thread::sleep_until(next);
		std::fill(chunk.begin(), chunk.end(), int16_t(0));
		{
			std::lock_guard<std::mutex> lk(m_audioMutex);
			const size_t avail = m_audio.size() - m_audioRead;
			const size_t take = std::min(avail, chunk.size());
			std::copy_n(m_audio.begin() + std::ptrdiff_t(m_audioRead), take, chunk.begin());
			m_audioRead += take;
		}
		if (!writeAll(s->audioFd, reinterpret_cast<const uint8_t *>(chunk.data()), chunk.size() * sizeof(int16_t)))
			break;
		next += period; // as for the picture: catch up after a stall, never skip, so both stay in step
	}
	::close(s->audioFd);
	s->alive = false;
}

Dictionary AntLiveStream::get_status() const
{
	Dictionary d;
	std::lock_guard<std::mutex> lk(m_statusMutex);
	d["state"] = String(m_state.c_str());
	d["message"] = String::utf8(m_message.c_str());
	d["last_error"] = String::utf8(m_lastError.c_str());
	d["kbps"] = m_kbps;
	d["fps"] = m_outFps;
	d["dropped_frames"] = m_dropped;
	d["restarts"] = m_restarts;
	d["encoder"] = String(m_encoderInUse.c_str());
	d["uptime_seconds"] = m_liveSinceMs > 0 ? double(nowMs() - m_liveSinceMs) / 1000.0 : 0.0;
	return d;
}

bool AntLiveStream::store_key(const String &destination, const String &key)
{
	const std::string k = toStd(key.strip_edges());
	if (k.empty())
		return false;
	std::vector<std::string> argv = {"secret-tool", "store", "--label=Shadow Ant Farm stream key (" + toStd(destination) + ")"};
	for (const std::string &a : keyAttrs(toStd(destination)))
		argv.push_back(a);
	return runCapture(argv, k, nullptr) == 0;
}

bool AntLiveStream::has_key(const String &destination)
{
	return !lookupKey(toStd(destination)).empty();
}

bool AntLiveStream::clear_key(const String &destination)
{
	std::vector<std::string> argv = {"secret-tool", "clear"};
	for (const std::string &a : keyAttrs(toStd(destination)))
		argv.push_back(a);
	return runCapture(argv, {}, nullptr) == 0;
}

bool AntLiveStream::ffmpeg_available()
{
	return runCapture({"ffmpeg", "-hide_banner", "-version"}, {}, nullptr) == 0;
}

void AntLiveStream::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("start", "config"), &AntLiveStream::start);
	ClassDB::bind_method(D_METHOD("stop"), &AntLiveStream::stop);
	ClassDB::bind_method(D_METHOD("is_active"), &AntLiveStream::is_active);
	ClassDB::bind_method(D_METHOD("push_video", "rgba", "width", "height"), &AntLiveStream::push_video);
	ClassDB::bind_method(D_METHOD("push_audio", "frames"), &AntLiveStream::push_audio);
	ClassDB::bind_method(D_METHOD("get_status"), &AntLiveStream::get_status);
	ClassDB::bind_static_method("AntLiveStream", D_METHOD("store_key", "destination", "key"), &AntLiveStream::store_key);
	ClassDB::bind_static_method("AntLiveStream", D_METHOD("has_key", "destination"), &AntLiveStream::has_key);
	ClassDB::bind_static_method("AntLiveStream", D_METHOD("clear_key", "destination"), &AntLiveStream::clear_key);
	ClassDB::bind_static_method("AntLiveStream", D_METHOD("ffmpeg_available"), &AntLiveStream::ffmpeg_available);
}
