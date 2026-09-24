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


func feed(events: PackedFloat32Array, walkers: int) -> void:
	if playback == null:
		return
	synth.set_walkers(walkers)
	synth.push_events(events)
	synth.fill(playback)
