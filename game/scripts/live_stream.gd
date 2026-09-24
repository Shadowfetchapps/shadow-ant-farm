class_name LiveStream
extends Node
## Going live straight from the farm. A second camera renders the farm offscreen at the stream size; its frames
## and the farm's own sound go to AntLiveStream (ffmpeg, in the extension). No screen capture is involved, so
## nothing else on the desktop can appear in the stream and it never depends on a window being shared.

const FPS := 30

const DESTINATIONS := {
	"youtube": {"name": "YouTube", "server": "rtmps://a.rtmps.youtube.com:443/live2", "keyframe": 2, "ask_server": false,
		"help": "YouTube Studio → Create → Go live → Stream → Stream settings → Stream key. Turn on Auto-start there and YouTube goes public by itself whenever the farm starts sending."},
	"x": {"name": "X", "server": "", "keyframe": 3, "ask_server": true,
		"help": "Media Studio → Producer → Sources: copy the RTMPS address and the stream key of your source (the address matters: sources live on different servers). Start the broadcast in Producer once the source shows connected."},
	"custom": {"name": "Custom RTMP", "server": "", "keyframe": 2, "ask_server": true,
		"help": "Any RTMP or RTMPS server: its address here, its stream key below."},
}
const QUALITIES := [
	{"label": "720p · 4 Mb/s (recommended)", "w": 1280, "h": 720, "kbps": 4000},
	{"label": "1080p · 7 Mb/s", "w": 1920, "h": 1080, "kbps": 7000},
]

var stream := AntLiveStream.new()
var view: FarmView
var settings: Dictionary
var _viewport: SubViewport
var _camera: Camera3D
var _size := Vector2i(1280, 720)
var _grab_accum := 0.0
var _last_state := "idle"


func setup(p_view: FarmView, p_settings: Dictionary) -> void:
	view = p_view
	settings = p_settings
	if not settings.has("live"):
		settings["live"] = {"destination": "youtube", "servers": {}, "quality": 0, "auto": false}


func config() -> Dictionary:
	return settings["live"]


func destination() -> String:
	return String(config().get("destination", "youtube"))


func server_for(dest: String) -> String:
	var servers: Dictionary = config().get("servers", {})
	var saved := String(servers.get(dest, ""))
	return saved if not saved.is_empty() else String(DESTINATIONS[dest]["server"])


func is_active() -> bool:
	return stream.is_active()


## Starts streaming to the saved destination (or to `url_override`, a full RTMP URL with its key, for tests).
func start(url_override := "") -> bool:
	if stream.is_active():
		return true
	var dest := destination()
	var q: Dictionary = QUALITIES[clampi(int(config().get("quality", 0)), 0, QUALITIES.size() - 1)]
	_size = Vector2i(int(q["w"]), int(q["h"]))
	_ensure_viewport()
	var ok := stream.start({
		"destination": dest, "server": server_for(dest), "url_override": url_override,
		"width": _size.x, "height": _size.y, "fps": FPS, "video_kbps": int(q["kbps"]),
		"keyframe_seconds": int(DESTINATIONS[dest]["keyframe"]),
	})
	var st := stream.get_status()
	var target: String = "a test URL" if not url_override.is_empty() else DESTINATIONS[dest]["name"]
	AppLog.info("live: start to %s (%dx%d@%d, %d kb/s): %s" % [target, _size.x, _size.y, FPS, int(q["kbps"]), st.get("message", "")])
	return ok


func stop() -> void:
	if stream.is_active() or String(stream.get_status().get("state", "idle")) != "idle":
		stream.stop()
		AppLog.info("live: stopped")
	if _viewport:
		_viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED


func status() -> Dictionary:
	return stream.get_status()


func push_audio(frames: PackedVector2Array) -> void:
	if stream.is_active():
		stream.push_audio(frames)


func _ensure_viewport() -> void:
	if _viewport == null:
		_viewport = SubViewport.new()
		_viewport.msaa_3d = Viewport.MSAA_4X
		_viewport.transparent_bg = false
		add_child(_viewport) # shares the farm's World3D (no world of its own)
		_camera = Camera3D.new()
		_camera.fov = view.camera.fov
		_camera.near = view.camera.near
		_camera.far = view.camera.far
		_viewport.add_child(_camera)
		_camera.current = true
	_viewport.size = _size
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	view.fit_camera_to(_camera, Vector2(_size))


func _process(delta: float) -> void:
	if not stream.is_active():
		var st := String(stream.get_status().get("state", "idle"))
		if st != _last_state:
			_last_state = st
			AppLog.info("live: %s — %s" % [st, stream.get_status().get("message", "")])
		return
	var state := String(stream.get_status().get("state", ""))
	if state != _last_state:
		_last_state = state
		AppLog.info("live: %s — %s" % [state, stream.get_status().get("message", "")])
	_grab_accum += delta
	if _grab_accum < 1.0 / FPS:
		return
	_grab_accum = fmod(_grab_accum, 1.0 / FPS)
	var img := _viewport.get_texture().get_image()
	if img == null:
		return
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	stream.push_video(img.get_data(), img.get_width(), img.get_height())
