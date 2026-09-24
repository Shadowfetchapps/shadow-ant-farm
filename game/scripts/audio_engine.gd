class_name AudioEngine
extends Node
## Streams the procedural synth (AntAudioSynth, in the extension) through an AudioStreamGenerator.

var synth := AntAudioSynth.new()
var player: AudioStreamPlayer
var playback: AudioStreamGeneratorPlayback


func setup(seed: int, grid_width: int, volumes: Dictionary) -> void:
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = 48000.0
	gen.buffer_length = 0.3
	player = AudioStreamPlayer.new()
	player.stream = gen
	player.bus = "Master"
	add_child(player)
	synth.configure(gen.mix_rate, seed, grid_width)
	for k in volumes:
		synth.set_category_volume(int(k), float(volumes[k]))
	player.play()
	playback = player.get_stream_playback()


## Anything that also wants the farm's sound (the live stream): called with each rendered block.
var tap: Callable
var _t0_usec := 0
var _frames_done := 0


func feed(events: PackedFloat32Array, walkers: int) -> void:
	if playback == null:
		return
	synth.set_walkers(walkers)
	synth.push_events(events)
	# Render exactly as much sound as real time has passed, once, and give the same samples to the speakers
	# and the stream (so the stream never depends on the sound device, and both hear the same thing).
	var now := Time.get_ticks_usec()
	if _t0_usec == 0:
		_t0_usec = now - 50000 # start with 50 ms in the speaker buffer
	var due := int((now - _t0_usec) * 48000 / 1000000) - _frames_done
	if due > 24000: # a long stall: skip ahead rather than play a burst
		_frames_done += due - 24000
		due = 24000
	if due <= 0:
		return
	var buf := synth.render(due)
	_frames_done += due
	var room := playback.get_frames_available()
	playback.push_buffer(buf if room >= buf.size() else buf.slice(0, room))
	if tap.is_valid():
		tap.call(buf)
