class_name FarmView
extends Node3D
## The presentation: a shallow 3D view of the soil slab between two panes of glass. It only reads the
## simulation (terrain texture, interpolated ant transforms, events) and never changes it.

const CELL := 0.01            ## world units per grid cell
const ANT_Z := 0.0035         ## ants walk on the glass-pressed soil face
const GLASS_Z := 0.03
const FRAME_W := 0.12
const FRAME_MARGIN_CELLS := 3.0
const MAX_GRAINS := 480

var sim: AntFarmSim
var grid := Vector2i(960, 540)
var camera: Camera3D
var soil_material: ShaderMaterial
var terrain_image: Image
var terrain_texture: ImageTexture
var terrain_bytes := PackedByteArray()
var ants_mm: MultiMesh
var loads_mm: MultiMesh
var food_mm: MultiMesh
var grains_mm: MultiMesh
var key_light: DirectionalLight3D

var _terrain_timer := 0.0
var _food_timer := 0.0
var _decor_rng := RandomNumberGenerator.new()
# loose grains: position (grid), velocity, life, resting flag
var _gp := PackedVector2Array()
var _gv := PackedVector2Array()
var _gl := PackedFloat32Array()
var _gs := PackedByteArray()
var _grain_buffer := PackedFloat32Array()


func setup(p_sim: AntFarmSim) -> void:
	sim = p_sim
	grid = sim.get_grid_size()
	_decor_rng.seed = hash(sim.get_seed()) ^ 0x5EED
	for c in get_children():
		c.queue_free()
	_build_environment()
	_build_soil()
	_build_frame_and_glass()
	_build_ants()
	_build_food_and_grains()
	_update_terrain(true)
	_update_food()
	_fit_camera()
	get_viewport().size_changed.connect(_fit_camera)


func _build_environment() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.03, 0.028, 0.026)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.66, 0.63, 0.6)
	env.ambient_light_energy = 0.32
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.tonemap_exposure = 1.0
	env.tonemap_white = 6.0
	env.adjustment_enabled = true
	env.adjustment_contrast = 1.04
	env.adjustment_saturation = 1.02
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	key_light = DirectionalLight3D.new()
	key_light.light_color = Color(1.0, 0.95, 0.87)
	key_light.light_energy = 1.05
	key_light.shadow_enabled = true
	key_light.shadow_blur = 1.2
	key_light.shadow_bias = 0.02
	key_light.shadow_normal_bias = 0.4
	key_light.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	key_light.directional_shadow_max_distance = 40.0
	add_child(key_light)
	key_light.look_at_from_position(Vector3(-3.0, 4.0, 6.0), Vector3.ZERO, Vector3.UP)

	var fill := DirectionalLight3D.new()
	fill.light_color = Color(0.9, 0.92, 1.0)
	fill.light_energy = 0.2
	add_child(fill)
	fill.look_at_from_position(Vector3(5.0, -1.0, 6.0), Vector3.ZERO, Vector3.UP)

	camera = Camera3D.new()
	camera.fov = 16.0
	camera.near = 0.5
	camera.far = 200.0
	add_child(camera)
	camera.current = true


func _fit_camera() -> void:
	if camera == null:
		return
	var vp := get_viewport().get_visible_rect().size
	if vp.y <= 0:
		return
	var aspect := vp.x / vp.y
	var inner := Vector2(grid.x - 2.0 * FRAME_MARGIN_CELLS, grid.y - 2.0 * FRAME_MARGIN_CELLS) * CELL
	var outer := inner + Vector2(2.0 * FRAME_W, 2.0 * FRAME_W)
	# Fill the screen with the farm: the frame may be cropped a little at the edges, never the soil.
	var half_h: float = max(inner.y * 0.5 + FRAME_W * 0.45, (inner.x * 0.5 + FRAME_W * 0.45) / aspect)
	half_h = min(half_h, max(outer.y * 0.5, outer.x * 0.5 / aspect))
	var dist := half_h / tan(deg_to_rad(camera.fov * 0.5))
	camera.position = Vector3(0.0, 0.0, dist + GLASS_Z)
	camera.look_at(Vector3(0.0, 0.0, 0.0), Vector3.UP)


func _build_soil() -> void:
	terrain_image = Image.create(grid.x, grid.y, false, Image.FORMAT_RGBA8)
	terrain_texture = ImageTexture.create_from_image(terrain_image)
	soil_material = ShaderMaterial.new()
	soil_material.shader = load("res://shaders/soil.gdshader")
	soil_material.set_shader_parameter("terrain_nearest", terrain_texture)
	soil_material.set_shader_parameter("terrain_linear", terrain_texture)
	soil_material.set_shader_parameter("grid_size", Vector2(grid))
	soil_material.set_shader_parameter("seed", float(sim.get_seed() % 997))
	var quad := QuadMesh.new()
	quad.size = Vector2(grid.x * CELL, grid.y * CELL)
	var mi := MeshInstance3D.new()
	mi.mesh = quad
	mi.material_override = soil_material
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(mi)


func _build_frame_and_glass() -> void:
	var inner := Vector2(grid.x - 2.0 * FRAME_MARGIN_CELLS, grid.y - 2.0 * FRAME_MARGIN_CELLS) * CELL
	var metal := StandardMaterial3D.new()
	metal.albedo_color = Color(0.085, 0.078, 0.07)
	metal.metallic = 0.75
	metal.roughness = 0.38
	var depth := 0.09
	var z := GLASS_Z - depth * 0.5 + 0.012
	var bars := [
		[Vector3(0, inner.y * 0.5 + FRAME_W * 0.5, z), Vector3(inner.x + 2.0 * FRAME_W, FRAME_W, depth)],
		[Vector3(0, -inner.y * 0.5 - FRAME_W * 0.5, z), Vector3(inner.x + 2.0 * FRAME_W, FRAME_W, depth)],
		[Vector3(-inner.x * 0.5 - FRAME_W * 0.5, 0, z), Vector3(FRAME_W, inner.y, depth)],
		[Vector3(inner.x * 0.5 + FRAME_W * 0.5, 0, z), Vector3(FRAME_W, inner.y, depth)],
	]
	for b in bars:
		var bm := BoxMesh.new()
		bm.size = b[1]
		var mi := MeshInstance3D.new()
		mi.mesh = bm
		mi.material_override = metal
		mi.position = b[0]
		add_child(mi)
	# A soft chamfer highlight along the inner edge of the frame.
	var lip := StandardMaterial3D.new()
	lip.albedo_color = Color(0.2, 0.19, 0.17)
	lip.metallic = 0.9
	lip.roughness = 0.25
	for b in [[Vector3(0, inner.y * 0.5 + 0.004, GLASS_Z + 0.02), Vector3(inner.x, 0.008, 0.01)],
			[Vector3(0, -inner.y * 0.5 - 0.004, GLASS_Z + 0.02), Vector3(inner.x, 0.008, 0.01)],
			[Vector3(-inner.x * 0.5 - 0.004, 0, GLASS_Z + 0.02), Vector3(0.008, inner.y, 0.01)],
			[Vector3(inner.x * 0.5 + 0.004, 0, GLASS_Z + 0.02), Vector3(0.008, inner.y, 0.01)]]:
		var lm := BoxMesh.new()
		lm.size = b[1]
		var li := MeshInstance3D.new()
		li.mesh = lm
		li.material_override = lip
		li.position = b[0]
		li.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(li)

	var glass := QuadMesh.new()
	glass.size = inner
	var gm := ShaderMaterial.new()
	gm.shader = load("res://shaders/glass.gdshader")
	gm.set_shader_parameter("seed", float(sim.get_seed() % 991))
	var gi := MeshInstance3D.new()
	gi.mesh = glass
	gi.material_override = gm
	gi.position = Vector3(0, 0, GLASS_Z)
	gi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(gi)


func _build_ants() -> void:
	var n: int = sim.get_stats().get("ants", 0)
	var mat := ShaderMaterial.new()
	mat.shader = load("res://shaders/ant.gdshader")
	ants_mm = MultiMesh.new()
	ants_mm.transform_format = MultiMesh.TRANSFORM_3D
	ants_mm.use_custom_data = true
	ants_mm.mesh = AntModel.build(1)
	ants_mm.instance_count = n
	var ai := MultiMeshInstance3D.new()
	ai.multimesh = ants_mm
	ai.material_override = mat
	ai.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	ai.custom_aabb = AABB(Vector3(-10, -10, -1), Vector3(20, 20, 2))
	add_child(ai)

	var lmat := ShaderMaterial.new()
	lmat.shader = load("res://shaders/load.gdshader")
	loads_mm = MultiMesh.new()
	loads_mm.transform_format = MultiMesh.TRANSFORM_3D
	loads_mm.use_custom_data = true
	loads_mm.mesh = AntModel.build_pellet()
	loads_mm.instance_count = n
	loads_mm.visible_instance_count = 0
	var li := MultiMeshInstance3D.new()
	li.multimesh = loads_mm
	li.material_override = lmat
	li.custom_aabb = AABB(Vector3(-10, -10, -1), Vector3(20, 20, 2))
	add_child(li)


func _build_food_and_grains() -> void:
	var lmat := ShaderMaterial.new()
	lmat.shader = load("res://shaders/load.gdshader")
	food_mm = MultiMesh.new()
	food_mm.transform_format = MultiMesh.TRANSFORM_3D
	food_mm.use_custom_data = true
	food_mm.mesh = AntModel.build_pellet()
	food_mm.instance_count = 400
	food_mm.visible_instance_count = 0
	var fi := MultiMeshInstance3D.new()
	fi.multimesh = food_mm
	fi.material_override = lmat
	fi.custom_aabb = AABB(Vector3(-10, -10, -1), Vector3(20, 20, 2))
	add_child(fi)

	grains_mm = MultiMesh.new()
	grains_mm.transform_format = MultiMesh.TRANSFORM_3D
	grains_mm.use_custom_data = true
	grains_mm.mesh = AntModel.build_pellet()
	grains_mm.instance_count = MAX_GRAINS
	grains_mm.visible_instance_count = 0
	var gi := MultiMeshInstance3D.new()
	gi.multimesh = grains_mm
	gi.material_override = lmat
	gi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	gi.custom_aabb = AABB(Vector3(-10, -10, -1), Vector3(20, 20, 2))
	add_child(gi)
	_grain_buffer.resize(MAX_GRAINS * 16)


## Called every rendered frame with the interpolation factor and the events polled since last frame.
func refresh(delta: float, alpha: float, events: PackedFloat32Array) -> void:
	if sim == null:
		return
	_terrain_timer -= delta
	if _terrain_timer <= 0.0:
		_terrain_timer = 0.25
		_update_terrain(false)
	_food_timer -= delta
	if _food_timer <= 0.0:
		_food_timer = 2.0
		_update_food()
	var buf := sim.build_ant_buffer(alpha, CELL, ANT_Z)
	if buf.size() == ants_mm.instance_count * 16:
		ants_mm.buffer = buf
	var lb := sim.build_load_buffer(alpha, CELL, ANT_Z)
	if lb.size() == loads_mm.instance_count * 16:
		loads_mm.buffer = lb
		loads_mm.visible_instance_count = sim.get_load_count()
	_spawn_grains(events)
	_update_grains(delta)


func _update_terrain(force: bool) -> void:
	var bytes := sim.take_terrain_update(force)
	if bytes.is_empty():
		return
	terrain_bytes = bytes
	terrain_image.set_data(grid.x, grid.y, false, Image.FORMAT_RGBA8, bytes)
	terrain_texture.update(terrain_image)


func _is_solid(x: float, y: float) -> bool:
	var ix := int(x)
	var iy := int(y)
	if ix < 0 or iy < 0 or ix >= grid.x or iy >= grid.y or terrain_bytes.is_empty():
		return true
	return (terrain_bytes[(iy * grid.x + ix) * 4] & 8) == 0


func _grid_to_world(p: Vector2, z: float) -> Vector3:
	return Vector3((p.x - grid.x * 0.5) * CELL, (grid.y * 0.5 - p.y) * CELL, z)


func _update_food() -> void:
	var food := sim.get_food()
	if food.size() < 3:
		return
	var buf := PackedFloat32Array()
	buf.resize(food_mm.instance_count * 16)
	var n := 0
	var rng := RandomNumberGenerator.new()
	# Seeds on the feeding spot (placed on the soil surface, stable from one refresh to the next).
	var station_count := int(clamp(food[2] / 5.0, 0, 70))
	rng.seed = 911
	for i in station_count:
		var gx := food[0] + rng.randf_range(-5.5, 5.5)
		var gy := food[1] + 0.2
		while not _is_solid(gx, gy + 0.6) and gy < grid.y - 2:
			gy += 1.0
		gy -= rng.randf_range(0.0, 0.8) + float(i % 3) * 0.35
		n = _put_seed(buf, n, Vector2(gx, gy), rng.randf())
	# Stores underground: small heaps where carriers left food.
	var k := 3
	while k + 2 < food.size() and n < food_mm.instance_count:
		var count := int(clamp(food[k + 2] / 3.0, 1, 14))
		rng.seed = int(food[k] * 131.0 + food[k + 1] * 7.0)
		for i in count:
			var p := Vector2(food[k] + rng.randf_range(-1.6, 1.6), food[k + 1] + rng.randf_range(-0.6, 0.9))
			if not _is_solid(p.x, p.y):
				n = _put_seed(buf, n, p, rng.randf())
		k += 3
	food_mm.buffer = buf
	food_mm.visible_instance_count = n


func _put_seed(buf: PackedFloat32Array, n: int, p: Vector2, r: float) -> int:
	if n >= food_mm.instance_count:
		return n
	var w := _grid_to_world(p, ANT_Z + 0.002)
	var s := CELL * (0.9 + 0.4 * r)
	var a := r * TAU
	var o := n * 16
	buf[o + 0] = cos(a) * s
	buf[o + 1] = -sin(a) * s * 0.7
	buf[o + 2] = 0.0
	buf[o + 3] = w.x
	buf[o + 4] = sin(a) * s
	buf[o + 5] = cos(a) * s * 0.7
	buf[o + 6] = 0.0
	buf[o + 7] = w.y
	buf[o + 8] = 0.0
	buf[o + 9] = 0.0
	buf[o + 10] = s * 0.6
	buf[o + 11] = w.z
	buf[o + 12] = 2.0
	buf[o + 13] = r
	return n + 1


func _spawn_grains(events: PackedFloat32Array) -> void:
	var i := 0
	while i + 3 < events.size():
		var t := int(events[i])
		if t == 1 or t == 2:
			var count := 3 if t == 1 else 5
			for k in count:
				if _gp.size() >= MAX_GRAINS:
					break
				var p := Vector2(events[i + 1], events[i + 2]) + Vector2(_decor_rng.randf_range(-0.8, 0.8), _decor_rng.randf_range(-0.6, 0.2))
				if _is_solid(p.x, p.y):
					continue
				_gp.append(p)
				_gv.append(Vector2(_decor_rng.randf_range(-3.0, 3.0), _decor_rng.randf_range(-3.0, 1.0)))
				_gl.append(_decor_rng.randf_range(1.5, 3.5))
				_gs.append(0)
		i += 4


func _update_grains(delta: float) -> void:
	var n := _gp.size()
	var k := 0
	while k < n:
		_gl[k] -= delta
		if _gl[k] <= 0.0:
			_gp.remove_at(k)
			_gv.remove_at(k)
			_gl.remove_at(k)
			_gs.remove_at(k)
			n -= 1
			continue
		if _gs[k] == 0:
			var v := _gv[k] + Vector2(0.0, 60.0) * delta
			var p := _gp[k] + v * delta
			if _is_solid(p.x, p.y):
				# Roll a little down the slope, else settle.
				var side := 1.0 if _decor_rng.randf() < 0.5 else -1.0
				if not _is_solid(p.x + side, p.y) and not _is_solid(p.x + side, p.y + 1.0):
					p = _gp[k] + Vector2(side * 0.6, 0.4)
					v = Vector2(side * 2.0, 2.0)
				else:
					p = _gp[k]
					v = Vector2.ZERO
					_gs[k] = 1
			_gp[k] = p
			_gv[k] = v
		k += 1
	for g in n:
		var w := _grid_to_world(_gp[g], ANT_Z + 0.001)
		var s := CELL * 0.32
		var o := g * 16
		_grain_buffer[o + 0] = s
		_grain_buffer[o + 1] = 0.0
		_grain_buffer[o + 2] = 0.0
		_grain_buffer[o + 3] = w.x
		_grain_buffer[o + 4] = 0.0
		_grain_buffer[o + 5] = s
		_grain_buffer[o + 6] = 0.0
		_grain_buffer[o + 7] = w.y
		_grain_buffer[o + 8] = 0.0
		_grain_buffer[o + 9] = 0.0
		_grain_buffer[o + 10] = s
		_grain_buffer[o + 11] = w.z
		_grain_buffer[o + 12] = 1.0
		_grain_buffer[o + 13] = fmod(float(g) * 0.618, 1.0)
	grains_mm.buffer = _grain_buffer
	grains_mm.visible_instance_count = n
