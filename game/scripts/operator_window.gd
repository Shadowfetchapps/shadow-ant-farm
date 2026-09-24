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

const CATEGORIES := ["Room tone", "Digging", "Soil and spoil", "Footsteps", "Food"]

var _status: Label
var _confirm_new: Control
var _new_edit: LineEdit
var _new_button: Button
var _quit_button: Button
var _quit_armed_until := 0.0
var _fps_option: OptionButton


func _init() -> void:
	title = "Shadow Ant Farm — Operator"
	size = Vector2i(560, 780)
	min_size = Vector2i(460, 640)
	wrap_controls = false
	transient = false
	exclusive = false
	unresizable = false


func build(settings: Dictionary) -> void:
	var bg := PanelContainer.new()
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	var margin := MarginContainer.new()
	for s in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + s, 14)
	bg.add_child(margin)
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
