extends Node
## Shadow Ant Farm entry point.
##
## Command line (after `--`, or directly on the installed launcher):
##   --seed N              start a new colony from this seed (default: a fresh random seed each launch)
##   --resume              continue the newest valid checkpoint (falls back to a new colony)
##   --load FILE           continue a specific checkpoint file
##   --windowed            run in a window instead of full screen
##   --operator            open the operator window at start (F2 toggles it)
##   --fps 30|60           frame-rate preset
##   --config FILE         simulation tuning file (key = value)
##   --capture DIR         accelerated visual inspection: render stills at --capture-hours and exit
##   --capture-hours LIST  comma-separated simulated hours (default 0,1,4,8,12,18,24)
##   --capture-size WxH    still size (default 3840x2160)
##   --soak HOURS          real-time soak: logs health every minute, stills at 0/1/4/8/12/18/24 h, report
##   --bench SECONDS       measure frame times, then exit (--uncapped: no vsync/frame cap)
##   --render-size WxH     render the farm offscreen at this size (shown scaled in the window)
##   --headless-test HOURS run the core checks through the extension without rendering, then exit
##   --run-seconds N       run normally for N real seconds, then save and quit (automation)

const CHECKPOINT_INTERVAL := 600.0
const MAX_TICKS_PER_FRAME := 60  ## up to 2 s of catch-up if the compositor throttles frames (hidden window)

var sim := AntFarmSim.new()
var view: FarmView
var audio: AudioEngine
var store: CheckpointStore
var operator: OperatorWindow
var args := {}
var settings := {}

var _since_checkpoint := 0.0
var _since_status := 0.0
var _since_hourly := 0.0
var _walkers := 0
var _last_usec := 0
var _capture_viewport: SubViewport
var _capture_targets: Array[float] = []
var _capture_dir := ""
var _capture_wait := -1
var _soak: Dictionary = {}
var _bench: Dictionary = {}
var _frame_times := PackedFloat32Array()
var _bench_d := PackedFloat32Array()
var _soak_deltas := PackedFloat32Array()
var _stills: Array = []


func _ready() -> void:
	args = _parse_args()
	AppLog.open(OS.get_user_data_dir().path_join("logs"))
	AppLog.info("Shadow Ant Farm %s starting (Godot %s), args %s" % [ProjectSettings.get_setting("application/config/version"), Engine.get_version_info().string, str(args)])
	store = CheckpointStore.new(OS.get_user_data_dir().path_join("checkpoints"))
	_load_settings()
	if args.has("headless-test"):
		_run_headless_test(float(args["headless-test"]))
		return
	get_tree().auto_accept_quit = false
	_configure_window()
	if not _start_colony():
		AppLog.error("could not start a colony: %s" % sim.get_last_error())
		get_tree().quit(1)
		return
	_build_presentation()
	if args.has("capture"):
		_begin_capture()
	elif args.has("soak"):
		_begin_soak(float(args["soak"]))
	elif args.has("bench"):
		if args.has("uncapped"):
			Engine.max_fps = 0
			DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
		_bench = {"until": Time.get_ticks_msec() / 1000.0 + float(args["bench"]) + 3.0, "start": Time.get_ticks_msec() / 1000.0 + 3.0,
			"sim0": sim.get_sim_seconds(), "wall0": Time.get_ticks_msec() / 1000.0}
	if args.has("operator"):
		_toggle_operator()
	if args.has("run-seconds"):
		get_tree().create_timer(float(args["run-seconds"]), true, false, true).timeout.connect(_quit_saving)


# ---- start-up -----------------------------------------------------------------------------------

func _parse_args() -> Dictionary:
	var out := {}
	var list := OS.get_cmdline_user_args()
	if list.is_empty():
		list = OS.get_cmdline_args()
	var i := 0
	while i < list.size():
		var a: String = list[i]
		if a.begins_with("--"):
			var key := a.substr(2)
			var val: Variant = true
			if key.contains("="):
				val = key.get_slice("=", 1)
				key = key.get_slice("=", 0)
			elif i + 1 < list.size() and not String(list[i + 1]).begins_with("--"):
				val = list[i + 1]
				i += 1
			out[key] = val
		i += 1
	return out


func _load_settings() -> void:
	var cf := ConfigFile.new()
	if cf.load("user://settings.cfg") == OK:
		settings["fps"] = cf.get_value("display", "fps", 60)
		settings["master"] = cf.get_value("audio", "master", 0.9)
		settings["volumes"] = cf.get_value("audio", "volumes", {})
	if args.has("fps"):
		settings["fps"] = int(args["fps"])


func _save_settings() -> void:
	var cf := ConfigFile.new()
	cf.set_value("display", "fps", settings.get("fps", 60))
	cf.set_value("audio", "master", settings.get("master", 0.9))
	cf.set_value("audio", "volumes", settings.get("volumes", {}))
	cf.save("user://settings.cfg")


func _configure_window() -> void:
	var win := get_window()
	win.title = "Shadow Ant Farm"
	if args.has("windowed") or args.has("capture"):
		win.mode = Window.MODE_WINDOWED
		win.size = Vector2i(1600, 900)
	else:
		win.mode = Window.MODE_FULLSCREEN
	# The capture image stays clean: no cursor over the farm, no overlays, the screen never blanks.
	Input.mouse_mode = Input.MOUSE_MODE_HIDDEN
	DisplayServer.screen_set_keep_on(true)
	_apply_fps(int(settings.get("fps", 60)))


func _apply_fps(fps: int) -> void:
	fps = 30 if fps == 30 else 60
	settings["fps"] = fps
	Engine.max_fps = fps
	AppLog.info("frame-rate preset %d" % fps)


func _start_colony() -> bool:
	if args.has("load"):
		if sim.load_checkpoint(String(args["load"])):
			AppLog.info("loaded %s (seed %016x, %.2f h)" % [String(args["load"]), sim.get_seed(), sim.get_sim_seconds() / 3600.0])
			return true
		AppLog.warn("could not load %s: %s" % [String(args["load"]), sim.get_last_error()])
	if args.has("resume") and not args.has("seed"):
		var p := store.resume_latest(sim)
		if not p.is_empty():
			AppLog.info("resumed %s (seed %016x, %.2f h)" % [p.get_file(), sim.get_seed(), sim.get_sim_seconds() / 3600.0])
			return true
		AppLog.info("no usable checkpoint; starting a new colony")
	var seed := 0
	if args.has("seed"):
		seed = String(args["seed"]).to_int() if not String(args["seed"]).begins_with("0x") else String(args["seed"]).substr(2).hex_to_int()
	var cfg := String(args.get("config", ""))
	if not sim.start_new(seed, cfg):
		return false
	AppLog.info("new colony, seed %016x" % sim.get_seed())
	return true


func _build_presentation() -> void:
	view = FarmView.new()
	if args.has("capture") or args.has("render-size"):
		var sz := String(args.get("capture-size", args.get("render-size", "3840x2160"))).split("x")
		_capture_viewport = SubViewport.new()
		_capture_viewport.size = Vector2i(int(sz[0]), int(sz[1]))
		_capture_viewport.msaa_3d = Viewport.MSAA_4X
		_capture_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		add_child(_capture_viewport)
		_capture_viewport.add_child(view)
		var tr := TextureRect.new()
		tr.texture = _capture_viewport.get_texture()
		tr.set_anchors_preset(Control.PRESET_FULL_RECT)
		tr.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		tr.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		add_child(tr)
	else:
		add_child(view)
	view.setup(sim)
	audio = AudioEngine.new()
	add_child(audio)
	var vols: Dictionary = settings.get("volumes", {})
	var v := {}
	for k in vols:
		v[int(k)] = float(vols[k])
	audio.setup(sim.get_seed(), sim.get_grid_size().x, v)
	audio.synth.set_master_volume(float(settings.get("master", 0.9)))
	if args.has("capture") or args.has("bench"):
		audio.synth.set_master_volume(0.0)


# ---- running ------------------------------------------------------------------------------------

func _process(delta: float) -> void:
	if not sim.is_running():
		return
	var t0 := Time.get_ticks_usec()
	if _capture_viewport and args.has("capture"):
		_capture_step()
	else:
		# Real time comes from the wall clock, not the frame delta: when the compositor throttles a hidden
		# window the engine's delta is capped, but the colony must keep to real time.
		var now_usec := Time.get_ticks_usec()
		var real_dt := delta if _last_usec == 0 else (now_usec - _last_usec) / 1000000.0
		_last_usec = now_usec
		sim.advance(real_dt, MAX_TICKS_PER_FRAME)
	var events := sim.poll_events(1024)
	view.refresh(delta, sim.get_alpha(), events)
	audio.feed(events, _walkers)

	_since_status += delta
	if _since_status >= 1.0:
		_since_status = 0.0
		var st := sim.get_stats()
		var states: Dictionary = st.get("states", {})
		_walkers = int(st.get("ants", 0)) - int(states.get("REST", 0)) - int(states.get("EXCAVATE", 0)) - int(states.get("GROOM", 0))
		if operator and operator.visible:
			operator.set_status(_status_text(st))
	if args.has("capture"):
		return
	_since_checkpoint += delta
	if _since_checkpoint >= CHECKPOINT_INTERVAL:
		_since_checkpoint = 0.0
		_checkpoint()
	_since_hourly += delta
	if _since_hourly >= 3600.0:
		_since_hourly = 0.0
		AppLog.info("hourly: " + _status_text(sim.get_stats()).replace("\n", "; "))
	_frame_times.append(delta)
	if _frame_times.size() > 3600:
		_frame_times = _frame_times.slice(_frame_times.size() - 3600)
	if not _soak.is_empty():
		_soak_step(delta, Time.get_ticks_usec() - t0)
	if not _bench.is_empty():
		_bench_step(delta)


func _status_text(st: Dictionary) -> String:
	var h := float(st.get("seconds", 0.0)) / 3600.0
	return "Seed %016x   simulated %d h %02d min\nAnts %d   excavated %.2f %%   deepest %d cells   entrances %d\nCarrying soil %d   food on the stone %d   stored %d\nFPS %d   dropped ticks %d   stuck recoveries %d\nMemory %.0f MB   log %s" % [
		sim.get_seed(), int(h), int(fmod(h * 60.0, 60.0)), st.get("ants", 0), float(st.get("excavated_fraction", 0.0)) * 100.0,
		st.get("max_depth", 0), st.get("entrances", 0), st.get("carrying_soil", 0), st.get("station_food", 0), st.get("stored_food", 0),
		Engine.get_frames_per_second(), st.get("dropped_ticks", 0), st.get("stuck_recoveries", 0),
		OS.get_static_memory_usage() / 1048576.0, AppLog.path()]


func _checkpoint() -> String:
	var p := store.save(sim)
	if not p.is_empty():
		AppLog.info("checkpoint %s" % p.get_file())
	return p


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_F2:
			_toggle_operator()
		elif event.keycode == KEY_F11:
			var w := get_window()
			w.mode = Window.MODE_WINDOWED if w.mode == Window.MODE_FULLSCREEN else Window.MODE_FULLSCREEN


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_quit_saving()


func _quit_saving() -> void:
	if sim.is_running() and not args.has("capture") and not args.has("bench"):
		_checkpoint()
	_save_settings()
	AppLog.info("quit")
	get_tree().quit()


func _toggle_operator() -> void:
	if operator == null:
		operator = OperatorWindow.new()
		add_child(operator)
		operator.build(settings)
		operator.save_requested.connect(func(): _checkpoint())
		operator.screenshot_requested.connect(_save_screenshot)
		operator.quit_requested.connect(_quit_saving)
		operator.fps_changed.connect(func(f): _apply_fps(f); _save_settings())
		operator.master_changed.connect(func(v):
			settings["master"] = v
			audio.synth.set_master_volume(v)
			_save_settings())
		operator.volume_changed.connect(func(c, v):
			var vols: Dictionary = settings.get("volumes", {})
			vols[str(c)] = v
			settings["volumes"] = vols
			audio.synth.set_category_volume(c, v)
			_save_settings())
		operator.new_colony_requested.connect(_new_colony)
		operator.popup_centered()
		operator.set_status(_status_text(sim.get_stats()))
	elif operator.visible:
		operator.hide()
	else:
		operator.show()


func _new_colony() -> void:
	_checkpoint()
	var fresh := AntFarmSim.new()
	if not fresh.start_new(0, String(args.get("config", ""))):
		AppLog.warn("new colony failed: %s" % fresh.get_last_error())
		return
	sim = fresh
	AppLog.info("operator started a new colony, seed %016x" % sim.get_seed())
	view.get_viewport().size_changed.disconnect(view._fit_camera)
	view.setup(sim)
	audio.synth.configure(48000.0, sim.get_seed(), sim.get_grid_size().x)


func _save_screenshot() -> void:
	var dir := OS.get_user_data_dir().path_join("screenshots")
	DirAccess.make_dir_recursive_absolute(dir)
	var img := get_viewport().get_texture().get_image()
	var p := dir.path_join("farm-%s.png" % Time.get_datetime_string_from_system(false, true).replace(":", "-"))
	img.save_png(p)
	AppLog.info("screenshot %s" % p)


# ---- visual inspection (accelerated) ------------------------------------------------------------

func _begin_capture() -> void:
	_capture_dir = String(args["capture"])
	DirAccess.make_dir_recursive_absolute(_capture_dir)
	for h in String(args.get("capture-hours", "0,1,4,8,12,18,24")).split(","):
		_capture_targets.append(float(h))
	_capture_targets.sort()
	AppLog.info("capture mode: %s at hours %s" % [_capture_dir, str(_capture_targets)])


func _capture_step() -> void:
	if _capture_targets.is_empty():
		return
	var target := _capture_targets[0] * 3600.0
	if _capture_wait < 0 and sim.get_sim_seconds() + 1e-6 < target:
		_capture_viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
		var start := Time.get_ticks_usec()
		var tps := sim.get_ticks_per_second()
		while sim.get_sim_seconds() + 1e-6 < target and Time.get_ticks_usec() - start < 250000:
			var need := int(ceil((target - sim.get_sim_seconds()) * tps))
			sim.step_ticks(min(need, 300))
		return
	if _capture_wait < 0:
		_capture_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		_capture_wait = 20 # let terrain, food and grains settle; several ticks of real-time motion
		view._update_terrain(true)
		view._update_food()
	sim.advance(1.0 / 60.0, 2)
	_capture_wait -= 1
	if _capture_wait == 0:
		var img := _capture_viewport.get_texture().get_image()
		var name := "farm_%05.2fh_t%09d_seed%016x.png" % [_capture_targets[0], sim.get_tick(), sim.get_seed()]
		img.save_png(_capture_dir.path_join(name))
		var st := sim.get_stats()
		AppLog.info("captured %s  excavated %.2f%%  depth %d  entrances %d" % [name, float(st["excavated_fraction"]) * 100.0, st["max_depth"], st["entrances"]])
		_capture_targets.remove_at(0)
		_capture_wait = -1
		if _capture_targets.is_empty():
			get_tree().quit()


# ---- soak and bench -----------------------------------------------------------------------------

func _begin_soak(hours: float) -> void:
	var dir := OS.get_user_data_dir().path_join("soak")
	DirAccess.make_dir_recursive_absolute(dir)
	var stamp := Time.get_datetime_string_from_system(false, true).replace(":", "-")
	_soak = {
		"hours": hours, "start_ms": Time.get_ticks_msec(), "start_sim": sim.get_sim_seconds(), "dir": dir,
		"file": dir.path_join("soak-%s.jsonl" % stamp), "minute": 0.0, "stamp": stamp,
		"worst_frame": 0.0, "min_minute_fps": 1e9, "max_mem": 0.0,
	}
	_stills = [0.0, 1.0, 4.0, 8.0, 12.0, 18.0, 24.0]
	_soak_deltas = PackedFloat32Array()
	AppLog.info("soak started: %.1f real hours, log %s" % [hours, _soak["file"]])


func _soak_step(delta: float, _work_us: int) -> void:
	var elapsed_h := (Time.get_ticks_msec() - int(_soak["start_ms"])) / 3600000.0
	_soak_deltas.append(delta)
	_soak["worst_frame"] = max(float(_soak["worst_frame"]), delta)
	if not _stills.is_empty() and elapsed_h >= float(_stills[0]):
		var vp_tex := _capture_viewport.get_texture() if _capture_viewport else get_viewport().get_texture()
		vp_tex.get_image().save_png(String(_soak["dir"]).path_join("soak-%s-%05.2fh.png" % [_soak["stamp"], float(_stills[0])]))
		_stills.remove_at(0)
	_soak["minute"] = float(_soak["minute"]) + delta
	if float(_soak["minute"]) >= 60.0:
		_soak["minute"] = 0.0
		var d := _soak_deltas
		var sorted := d.duplicate()
		sorted.sort()
		var fps: float = d.size() / max(0.001, _sum(d))
		var st := sim.get_stats()
		var mem := OS.get_static_memory_usage() / 1048576.0
		_soak["min_minute_fps"] = min(float(_soak["min_minute_fps"]), fps)
		_soak["max_mem"] = max(float(_soak["max_mem"]), mem)
		var rec := {
			"real_hours": elapsed_h, "sim_hours": sim.get_sim_seconds() / 3600.0, "fps": fps,
			"p99_frame_ms": sorted[int(sorted.size() * 0.99)] * 1000.0 if sorted.size() > 0 else 0.0,
			"worst_frame_ms": sorted[sorted.size() - 1] * 1000.0 if sorted.size() > 0 else 0.0,
			"static_mem_mb": mem, "video_mem_mb": Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / 1048576.0,
			"sim_mem_mb": sim.get_memory_bytes() / 1048576.0, "dropped_ticks": sim.get_dropped_ticks(),
			"excavated_pct": float(st["excavated_fraction"]) * 100.0, "carrying_soil": st["carrying_soil"],
			"stuck_recoveries": st["stuck_recoveries"], "audio": audio.synth.get_stats(),
		}
		var f := FileAccess.open(String(_soak["file"]), FileAccess.READ_WRITE if FileAccess.file_exists(String(_soak["file"])) else FileAccess.WRITE)
		if f:
			f.seek_end()
			f.store_line(JSON.stringify(rec))
			f.close()
		_soak_deltas = PackedFloat32Array()
	if elapsed_h >= float(_soak["hours"]):
		var summary := {
			"requested_real_hours": _soak["hours"], "elapsed_real_hours": elapsed_h,
			"simulated_hours": (sim.get_sim_seconds() - float(_soak["start_sim"])) / 3600.0,
			"min_minute_fps": _soak["min_minute_fps"], "worst_frame_ms": float(_soak["worst_frame"]) * 1000.0,
			"max_static_mem_mb": _soak["max_mem"], "dropped_ticks": sim.get_dropped_ticks(),
			"completed": elapsed_h >= float(_soak["hours"]),
		}
		var f := FileAccess.open(String(_soak["dir"]).path_join("soak-%s-summary.json" % _soak["stamp"]), FileAccess.WRITE)
		if f:
			f.store_string(JSON.stringify(summary, "  "))
			f.close()
		AppLog.info("soak finished: " + JSON.stringify(summary))
		_soak = {}
		_quit_saving()


func _bench_step(delta: float) -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if now < float(_bench["start"]):
		return
	_bench_d.append(delta)
	if now >= float(_bench["until"]):
		var d := _bench_d
		var s := d.duplicate()
		s.sort()
		var vp: Vector2 = Vector2(_capture_viewport.size) if _capture_viewport else get_viewport().get_visible_rect().size
		var report := "bench %dx%d preset %d: frames %d, mean %.2f ms (%.1f fps), p95 %.2f ms, p99 %.2f ms, worst %.2f ms, dropped ticks %d" % [
			vp.x, vp.y, int(settings.get("fps", 60)), d.size(), _sum(d) / d.size() * 1000.0, d.size() / _sum(d),
			s[int(s.size() * 0.95)] * 1000.0, s[int(s.size() * 0.99)] * 1000.0, s[s.size() - 1] * 1000.0, sim.get_dropped_ticks()]
		report += ", simulated %.1f s in %.1f s wall" % [sim.get_sim_seconds() - float(_bench["sim0"]), now - float(_bench["wall0"])]
		AppLog.info(report)
		print(report)
		get_tree().quit()


func _sum(a: PackedFloat32Array) -> float:
	var s := 0.0
	for v in a:
		s += v
	return s


# ---- headless checks through the extension ------------------------------------------------------

func _run_headless_test(hours: float) -> void:
	var ok := true
	var report := []
	var seed := 0x5A17
	var a := AntFarmSim.new()
	var b := AntFarmSim.new()
	a.start_new(seed, "")
	b.start_new(seed, "")
	var ticks := int(hours * 3600.0 * a.get_ticks_per_second())
	var t0 := Time.get_ticks_msec()
	var chunk := 3000
	var done := 0
	var ckpt := OS.get_user_data_dir().path_join("headless-test.antfarm")
	var c := AntFarmSim.new()
	var mid := -1
	while done < ticks:
		var n: int = min(chunk, ticks - done)
		a.step_ticks(n)
		b.step_ticks(n)
		done += n
		var st := a.get_stats()
		var conserved := int(st["excavated_volume"]) == int(st["deposited_volume"]) + int(st["carried_volume"])
		if not conserved:
			ok = false
			report.append("conservation broken at tick %d" % done)
			break
		if mid < 0 and done >= ticks / 2:
			mid = done
			if not (a.save_checkpoint(ckpt) and c.load_checkpoint(ckpt)):
				ok = false
				report.append("checkpoint round trip failed: %s" % c.get_last_error())
	# determinism: two instances, same seed
	if a.get_state_hash() != b.get_state_hash():
		ok = false
		report.append("same-seed runs diverged")
	# resume: the checkpoint from the midpoint continued to the end matches
	if c.is_running():
		c.step_ticks(ticks - mid)
		if c.get_state_hash() != a.get_state_hash():
			ok = false
			report.append("resumed run diverged")
	# fresh seeds
	var s1 := AntFarmSim.new()
	var s2 := AntFarmSim.new()
	s1.start_new(0, "")
	s2.start_new(0, "")
	if s1.get_seed() == s2.get_seed():
		ok = false
		report.append("two launches drew the same seed")
	# audio renders, stays bounded
	var syn := AntAudioSynth.new()
	syn.configure(48000.0, seed, 960)
	syn.push_events(a.poll_events(4096))
	var pcm := syn.render(48000 * 2)
	var peak := 0.0
	for v in pcm:
		peak = max(peak, max(abs(v.x), abs(v.y)))
	if peak >= 1.0 or pcm.size() != 96000:
		ok = false
		report.append("audio out of range (peak %.3f)" % peak)
	var terrain := a.take_terrain_update(true)
	if terrain.size() != a.get_grid_size().x * a.get_grid_size().y * 4:
		ok = false
		report.append("terrain buffer size wrong")
	var st2 := a.get_stats()
	var summary := "headless test %s: %.1f simulated h in %.1f s, excavated %.2f%%, hash %s, memory %.1f MB, audio peak %.3f" % [
		"PASSED" if ok else "FAILED", hours, (Time.get_ticks_msec() - t0) / 1000.0, float(st2["excavated_fraction"]) * 100.0,
		a.get_state_hash(), a.get_memory_bytes() / 1048576.0, peak]
	for r in report:
		AppLog.error(r)
	AppLog.info(summary)
	print(summary)
	DirAccess.remove_absolute(ckpt)
	get_tree().quit(0 if ok else 1)
