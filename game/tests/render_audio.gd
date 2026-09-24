extends SceneTree
## Renders simulation-driven audio offline to a WAV file for level and content checks.
## godot --headless --path game --script res://tests/render_audio.gd -- <checkpoint> <seconds> <out.wav>

func _init() -> void:
	var a := OS.get_cmdline_user_args()
	var sim := AntFarmSim.new()
	if a.size() > 0 and a[0] != "new":
		sim.load_checkpoint(a[0])
	else:
		sim.start_new(0x5EED, "")
		sim.step_ticks(30 * 3600)
	var seconds := float(a[1]) if a.size() > 1 else 60.0
	var out := a[2] if a.size() > 2 else "/tmp/antfarm-audio.wav"
	var syn := AntAudioSynth.new()
	syn.configure(48000.0, sim.get_seed(), sim.get_grid_size().x)
	sim.poll_events(100000)
	var pcm := PackedByteArray()
	var frames_per_tick := 1600 # 48000 / 30
	var ticks := int(seconds * 30.0)
	var peak := 0.0
	var sum2 := 0.0
	var n := 0
	for t in ticks:
		sim.step_ticks(1)
		if t % 30 == 0:
			var st := sim.get_stats()
			var states: Dictionary = st["states"]
			syn.set_walkers(int(st["ants"]) - int(states.get("REST", 0)))
		syn.push_events(sim.poll_events(1024))
		var buf := syn.render(frames_per_tick)
		for v in buf:
			var l := clampf(v.x, -1.0, 1.0)
			var r := clampf(v.y, -1.0, 1.0)
			peak = max(peak, max(abs(l), abs(r)))
			sum2 += l * l + r * r
			n += 2
			pcm.append_array(PackedByteArray([0, 0, 0, 0]))
			pcm.encode_s16(pcm.size() - 4, int(l * 32767.0))
			pcm.encode_s16(pcm.size() - 2, int(r * 32767.0))
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 48000
	wav.stereo = true
	wav.data = pcm
	wav.save_to_wav(out)
	var rms := sqrt(sum2 / max(1, n))
	print("audio: %.0f s, peak %.3f (%.1f dBFS), rms %.4f (%.1f dBFS), stats %s" % [seconds, peak, 20.0 * log(max(peak, 1e-9)) / log(10.0), rms, 20.0 * log(max(rms, 1e-9)) / log(10.0), str(syn.get_stats())])
	quit()
