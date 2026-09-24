class_name OperatorWindow
extends Window
## A separate desktop window for the operator: status, volumes, frame-rate preset and the two
## destructive actions, each behind its own confirmation. Nothing here ever draws over the presentation.

signal save_requested
signal screenshot_requested
signal new_colony_requested
signal quit_requested
signal fps_changed(fps: int)
signal volume_changed(category: int, value: float)
signal master_changed(value: float)
signal live_settings_changed

const CATEGORIES := ["Room tone", "Digging", "Soil and spoil", "Footsteps", "Food"]

var _status: Label
var _confirm_new: Control
var _new_edit: LineEdit
var _new_button: Button
var _quit_button: Button
var _quit_armed_until := 0.0
var _fps_option: OptionButton
var _live: LiveStream
var _live_dest: OptionButton
var _live_server: LineEdit
var _live_server_label: Label
var _live_server_row: Control
var _live_key: LineEdit
var _live_key_status: Label
var _live_quality: OptionButton
var _live_auto: CheckBox
var _live_button: Button
var _live_status: RichTextLabel
var _live_help: Label


func _init() -> void:
	title = "Shadow Ant Farm — Operator"
	size = Vector2i(580, 900)
	min_size = Vector2i(480, 600)
	wrap_controls = false
	transient = false
	exclusive = false
	unresizable = false


func build(settings: Dictionary, live: LiveStream = null) -> void:
	_live = live
	var bg := PanelContainer.new()
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	bg.add_child(scroll)
	var margin := MarginContainer.new()
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for s in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + s, 14)
	scroll.add_child(margin)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	margin.add_child(box)

	var head := Label.new()
	head.text = "Shadow Ant Farm"
	head.add_theme_font_size_override("font_size", 22)
	box.add_child(head)

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 13)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.custom_minimum_size = Vector2(470, 0)
	box.add_child(_status)

	var row := HBoxContainer.new()
	box.add_child(row)
	var save := Button.new()
	save.text = "Save checkpoint now"
	save.pressed.connect(func(): save_requested.emit())
	row.add_child(save)
	var shot := Button.new()
	shot.text = "Save screenshot"
	shot.pressed.connect(func(): screenshot_requested.emit())
	row.add_child(shot)

	var fps_row := HBoxContainer.new()
	box.add_child(fps_row)
	var fl := Label.new()
	fl.text = "Frame rate"
	fl.custom_minimum_size.x = 150
	fps_row.add_child(fl)
	_fps_option = OptionButton.new()
	_fps_option.add_item("60 FPS", 60)
	_fps_option.add_item("30 FPS (lighter)", 30)
	_fps_option.select(0 if int(settings.get("fps", 60)) == 60 else 1)
	_fps_option.item_selected.connect(func(i): fps_changed.emit(_fps_option.get_item_id(i)))
	fps_row.add_child(_fps_option)

	if _live:
		_build_live(box)

	box.add_child(HSeparator.new())
	var vl := Label.new()
	vl.text = "Sound"
	box.add_child(vl)
	_slider(box, "Master", float(settings.get("master", 0.9)), func(v): master_changed.emit(v))
	var vols: Dictionary = settings.get("volumes", {})
	for i in CATEGORIES.size():
		var cat := i
		_slider(box, CATEGORIES[i], float(vols.get(str(i), [0.55, 0.8, 0.8, 0.35, 0.7][i])), func(v): volume_changed.emit(cat, v))

	box.add_child(HSeparator.new())
	var danger := Label.new()
	danger.text = "Colony"
	box.add_child(danger)
	var nb := Button.new()
	nb.text = "Start a new colony…"
	box.add_child(nb)
	_confirm_new = VBoxContainer.new()
	_confirm_new.visible = false
	box.add_child(_confirm_new)
	var warn := Label.new()
	warn.text = "This replaces the running colony with a fresh one (its last checkpoint is kept). Type NEW COLONY to confirm."
	warn.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	warn.custom_minimum_size = Vector2(470, 0)
	_confirm_new.add_child(warn)
	_new_edit = LineEdit.new()
	_new_edit.placeholder_text = "NEW COLONY"
	_confirm_new.add_child(_new_edit)
	var cr := HBoxContainer.new()
	_confirm_new.add_child(cr)
	_new_button = Button.new()
	_new_button.text = "Start new colony"
	_new_button.disabled = true
	cr.add_child(_new_button)
	var cancel := Button.new()
	cancel.text = "Cancel"
	cr.add_child(cancel)
	nb.pressed.connect(func():
		_confirm_new.visible = true
		_new_edit.text = ""
		_new_edit.grab_focus())
	_new_edit.text_changed.connect(func(t): _new_button.disabled = t.strip_edges().to_upper() != "NEW COLONY")
	cancel.pressed.connect(func(): _confirm_new.visible = false)
	_new_button.pressed.connect(func():
		_confirm_new.visible = false
		new_colony_requested.emit())

	_quit_button = Button.new()
	_quit_button.text = "Quit…"
	_quit_button.pressed.connect(_on_quit)
	box.add_child(_quit_button)

	var hint := Label.new()
	hint.text = "F2 in the farm window shows or hides this window. Closing it does not stop the farm."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.custom_minimum_size = Vector2(470, 0)
	hint.modulate = Color(1, 1, 1, 0.6)
	box.add_child(hint)
	close_requested.connect(func(): hide())


func _build_live(box: VBoxContainer) -> void:
	box.add_child(HSeparator.new())
	var head := Label.new()
	head.text = "Live stream"
	head.add_theme_font_size_override("font_size", 18)
	box.add_child(head)
	var note := Label.new()
	note.text = "Streams the farm's own picture and sound (no screen sharing): nothing else on your desktop can appear in it."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 470
	note.modulate = Color(1, 1, 1, 0.7)
	box.add_child(note)

	var cfg := _live.config()
	var row := HBoxContainer.new()
	box.add_child(row)
	var l := Label.new()
	l.text = "Stream to"
	l.custom_minimum_size.x = 150
	row.add_child(l)
	_live_dest = OptionButton.new()
	for id in LiveStream.DESTINATIONS:
		_live_dest.add_item(LiveStream.DESTINATIONS[id]["name"])
		_live_dest.set_item_metadata(_live_dest.item_count - 1, id)
	_live_dest.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(_live_dest)

	var srow := HBoxContainer.new()
	_live_server_row = srow
	box.add_child(srow)
	_live_server_label = Label.new()
	_live_server_label.text = "Server (RTMPS)"
	_live_server_label.custom_minimum_size.x = 150
	srow.add_child(_live_server_label)
	_live_server = LineEdit.new()
	_live_server.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_live_server.placeholder_text = "rtmps://…"
	srow.add_child(_live_server)

	var krow := HBoxContainer.new()
	box.add_child(krow)
	var kl := Label.new()
	kl.text = "Stream key"
	kl.custom_minimum_size.x = 150
	krow.add_child(kl)
	_live_key = LineEdit.new()
	_live_key.secret = true
	_live_key.placeholder_text = "Paste the stream key"
	_live_key.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	krow.add_child(_live_key)
	var save_key := Button.new()
	save_key.text = "Save key"
	krow.add_child(save_key)
	_live_key_status = Label.new()
	_live_key_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_live_key_status.custom_minimum_size.x = 470
	_live_key_status.modulate = Color(1, 1, 1, 0.75)
	box.add_child(_live_key_status)

	var qrow := HBoxContainer.new()
	box.add_child(qrow)
	var ql := Label.new()
	ql.text = "Quality"
	ql.custom_minimum_size.x = 150
	qrow.add_child(ql)
	_live_quality = OptionButton.new()
	for q in LiveStream.QUALITIES:
		_live_quality.add_item(q["label"])
	_live_quality.select(clampi(int(cfg.get("quality", 0)), 0, LiveStream.QUALITIES.size() - 1))
	_live_quality.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	qrow.add_child(_live_quality)

	_live_auto = CheckBox.new()
	_live_auto.text = "Go live automatically whenever the farm starts"
	_live_auto.button_pressed = bool(cfg.get("auto", false))
	box.add_child(_live_auto)

	_live_button = Button.new()
	_live_button.text = "Go live"
	_live_button.custom_minimum_size.y = 44
	box.add_child(_live_button)
	_live_status = RichTextLabel.new()
	_live_status.bbcode_enabled = true
	_live_status.fit_content = true
	_live_status.custom_minimum_size.x = 470
	box.add_child(_live_status)
	_live_help = Label.new()
	_live_help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_live_help.custom_minimum_size.x = 470
	_live_help.modulate = Color(1, 1, 1, 0.6)
	box.add_child(_live_help)

	var dest := _live.destination()
	for i in _live_dest.item_count:
		if _live_dest.get_item_metadata(i) == dest:
			_live_dest.select(i)
	_show_destination(dest)

	_live_dest.item_selected.connect(func(i):
		var id: String = _live_dest.get_item_metadata(i)
		_live.config()["destination"] = id
		_show_destination(id)
		live_settings_changed.emit())
	_live_server.text_changed.connect(func(t):
		var servers: Dictionary = _live.config().get("servers", {})
		servers[_live.destination()] = t.strip_edges()
		_live.config()["servers"] = servers
		live_settings_changed.emit())
	_live_quality.item_selected.connect(func(i):
		_live.config()["quality"] = i
		live_settings_changed.emit())
	_live_auto.toggled.connect(func(on):
		_live.config()["auto"] = on
		live_settings_changed.emit())
	save_key.pressed.connect(func():
		var key := _live_key.text.strip_edges()
		_live_key.text = ""
		if key.is_empty():
			_live_key_status.text = "Paste the key first."
		elif AntLiveStream.store_key(_live.destination(), key):
			_live_key_status.text = "Key saved in your desktop keyring."
		else:
			_live_key_status.text = "The key could not be saved (is secret-tool installed? sudo apt install libsecret-tools)."
		_update_key_status())
	_live_button.pressed.connect(func():
		if _live.is_active():
			_live.stop()
		else:
			_live.start()
		set_live_status(_live.status()))


func _show_destination(id: String) -> void:
	var d: Dictionary = LiveStream.DESTINATIONS[id]
	var ask := bool(d["ask_server"])
	_live_server_row.visible = ask
	_live_server.text = _live.server_for(id) if ask else ""
	_live_help.text = d["help"]
	_update_key_status()


func _update_key_status() -> void:
	var dest := _live.destination()
	var name: String = LiveStream.DESTINATIONS[dest]["name"]
	if AntLiveStream.has_key(dest):
		_live_key_status.text = "A %s key is saved. Paste a new one only to replace it." % name
	else:
		_live_key_status.text = "No %s key saved yet." % name


func set_live_status(st: Dictionary) -> void:
	if _live_status == null:
		return
	var state := String(st.get("state", "idle"))
	_live_button.text = "End stream" if _live.is_active() else "Go live"
	var up := int(st.get("uptime_seconds", 0.0))
	match state:
		"live":
			_live_status.text = "[color=#6ee07a][b]● LIVE[/b][/color]  %02d:%02d:%02d · %.1f Mb/s · %s" % [
				up / 3600, (up / 60) % 60, up % 60, float(st.get("kbps", 0.0)) / 1000.0, st.get("encoder", "")]
		"starting":
			_live_status.text = "[color=#f5c451]Connecting…[/color]"
		"reconnecting":
			_live_status.text = "[color=#f5c451]%s[/color]" % String(st.get("message", "")).xml_escape()
		"failed":
			_live_status.text = "[color=#ff8a8e][b]Not live.[/b] %s[/color]" % String(st.get("message", "")).xml_escape()
		_:
			_live_status.text = "Not live." if String(st.get("message", "")).is_empty() else String(st.get("message", "")).xml_escape()


func _slider(parent: Control, label: String, value: float, cb: Callable) -> void:
	var r := HBoxContainer.new()
	parent.add_child(r)
	var l := Label.new()
	l.text = label
	l.custom_minimum_size.x = 150
	r.add_child(l)
	var s := HSlider.new()
	s.min_value = 0.0
	s.max_value = 1.0
	s.step = 0.01
	s.value = value
	s.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	s.value_changed.connect(cb)
	r.add_child(s)


func _on_quit() -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if now < _quit_armed_until:
		quit_requested.emit()
		return
	_quit_armed_until = now + 5.0
	_quit_button.text = "Press again within 5 s to save and quit"


func _process(_d: float) -> void:
	if _quit_armed_until > 0.0 and Time.get_ticks_msec() / 1000.0 > _quit_armed_until:
		_quit_armed_until = 0.0
		_quit_button.text = "Quit…"


func set_status(text: String) -> void:
	if _status:
		_status.text = text
