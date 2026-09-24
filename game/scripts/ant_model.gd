class_name AntModel
extends RefCounted
## Procedural worker-ant mesh (seen from above against the glass): mandibles, head, geniculate antennae,
## mesosoma, petiole node, gaster and six two-segment legs. One unit long, head towards +X, back towards
## +Z. CUSTOM0 carries (part, weight along the limb, attachment x, attachment y) for the gait shader.

const PART_BODY := 10.0
const PART_GASTER := 9.0
const PART_HEAD := 11.0

static var _cache: Dictionary = {}


static func build(detail: int = 1) -> ArrayMesh:
	if _cache.has(detail):
		return _cache[detail]
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	st.set_custom_format(0, SurfaceTool.CUSTOM_RGBA_FLOAT)
	var seg := 10 if detail > 0 else 6
	var ring := 7 if detail > 0 else 5
	# Body segments: centre, radii, colour tint, part id.
	var chitin := Color(0.045, 0.03, 0.022)
	var chitin_red := Color(0.075, 0.038, 0.024)
	_ellipsoid(st, Vector3(0.355, 0.0, 0.05), Vector3(0.095, 0.083, 0.058), chitin, PART_HEAD, seg, ring)
	_ellipsoid(st, Vector3(0.155, 0.0, 0.058), Vector3(0.125, 0.052, 0.055), chitin_red, PART_BODY, seg, ring)
	_ellipsoid(st, Vector3(0.235, 0.0, 0.07), Vector3(0.05, 0.045, 0.04), chitin_red, PART_BODY, seg, ring)  # pronotum hump
	_ellipsoid(st, Vector3(0.0, 0.0, 0.055), Vector3(0.034, 0.03, 0.045), chitin, PART_BODY, seg, ring)      # petiole node
	_ellipsoid(st, Vector3(-0.235, 0.0, 0.07), Vector3(0.2, 0.125, 0.1), chitin, PART_GASTER, seg + 2, ring)
	# Mandibles: two short curved blades in front of the head.
	for side in [-1.0, 1.0]:
		var a := Vector3(0.43, 0.035 * side, 0.04)
		var b := Vector3(0.49, 0.012 * side, 0.035)
		_limb(st, a, b, 0.013, 0.007, chitin_red, 12.0, a, 0.0, 1.0, 5)
	# Antennae: scape then funiculus (elbowed), from the front of the head.
	for side in [-1.0, 1.0]:
		var base := Vector3(0.41, 0.035 * side, 0.07)
		var elbow := Vector3(0.49, 0.125 * side, 0.08)
		var tip := Vector3(0.62, 0.16 * side, 0.07)
		var part := 6.0 if side < 0 else 7.0
		_limb(st, base, elbow, 0.011, 0.009, chitin, part, base, 0.0, 0.55, 5)
		_limb(st, elbow, tip, 0.009, 0.011, chitin, part, base, 0.55, 1.0, 5)
	# Legs: coxa on the mesosoma, femur out, tibia and tarsus down to the glass. 0,2,4 left; 1,3,5 right.
	var attach := [0.215, 0.155, 0.095]
	var knee := [Vector2(0.30, 0.15), Vector2(0.15, 0.2), Vector2(0.02, 0.18)]
	var foot := [Vector2(0.43, 0.23), Vector2(0.14, 0.31), Vector2(-0.2, 0.3)]
	for pair in 3:
		for s in 2:
			var side := -1.0 if s == 0 else 1.0
			var idx := float(pair * 2 + s)
			var a := Vector3(attach[pair], 0.035 * side, 0.045)
			var k := Vector3(knee[pair].x, knee[pair].y * side, 0.075)
			var f := Vector3(foot[pair].x, foot[pair].y * side, 0.012)
			_limb(st, a, k, 0.02, 0.016, chitin, idx, a, 0.0, 0.5, 6)
			_limb(st, k, f, 0.015, 0.008, chitin, idx, a, 0.5, 1.0, 6)
	var mesh := st.commit()
	_cache[detail] = mesh
	return mesh


static func build_pellet() -> ArrayMesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	st.set_custom_format(0, SurfaceTool.CUSTOM_RGBA_FLOAT)
	_ellipsoid(st, Vector3.ZERO, Vector3(0.5, 0.42, 0.4), Color(1, 1, 1), 0.0, 9, 6)
	return st.commit()


static func _ellipsoid(st: SurfaceTool, c: Vector3, r: Vector3, col: Color, part: float, seg: int, ring: int) -> void:
	var verts := []
	for i in ring + 1:
		var v := float(i) / ring
		var phi := v * PI
		var row := []
		for j in seg + 1:
			var u := float(j) / seg
			var th := u * TAU
			var p := Vector3(cos(phi), sin(phi) * cos(th), sin(phi) * sin(th))
			row.append(c + Vector3(p.x * r.x, p.y * r.y, p.z * r.z))
		verts.append(row)
	var inv := Vector3(1.0 / (r.x * r.x), 1.0 / (r.y * r.y), 1.0 / (r.z * r.z))
	for i in ring:
		for j in seg:
			var q := [verts[i][j], verts[i + 1][j], verts[i + 1][j + 1], verts[i][j + 1]]
			for k in [0, 1, 2, 0, 2, 3]:
				_vtx(st, q[k], ((q[k] - c) * inv).normalized(), col, part, 0.0, c)


static func _limb(st: SurfaceTool, a: Vector3, b: Vector3, ra: float, rb: float, col: Color, part: float, anchor: Vector3, wa: float, wb: float, sides: int) -> void:
	var axis := (b - a).normalized()
	var ref := Vector3(0, 0, 1) if abs(axis.z) < 0.9 else Vector3(1, 0, 0)
	var u := axis.cross(ref).normalized()
	var v := axis.cross(u).normalized()
	for i in sides:
		var t0 := TAU * float(i) / sides
		var t1 := TAU * float(i + 1) / sides
		var d0 := u * cos(t0) + v * sin(t0)
		var d1 := u * cos(t1) + v * sin(t1)
		var p00 := a + d0 * ra
		var p01 := a + d1 * ra
		var p10 := b + d0 * rb
		var p11 := b + d1 * rb
		_vtx(st, p00, d0, col, part, wa, anchor)
		_vtx(st, p10, d0, col, part, wb, anchor)
		_vtx(st, p11, d1, col, part, wb, anchor)
		_vtx(st, p00, d0, col, part, wa, anchor)
		_vtx(st, p11, d1, col, part, wb, anchor)
		_vtx(st, p01, d1, col, part, wa, anchor)


static func _vtx(st: SurfaceTool, p: Vector3, n: Vector3, col: Color, part: float, w: float, anchor: Vector3) -> void:
	st.set_normal(n)
	st.set_color(col)
	st.set_custom(0, Color(part, w, anchor.x, anchor.y))
	st.add_vertex(p)
