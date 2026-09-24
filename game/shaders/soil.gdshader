// The soil slab pressed against the front glass. The simulation grid arrives as a small RGBA8 texture
// (material, remaining volume, moisture, signed distance to the tunnel boundary); everything finer than a
// cell — grains, pebbles, crumbly tunnel edges, the curved walls and dim back wall of each passage — is
// generated here, so tunnels read as carved into packed soil rather than cut out of a pixel grid.
shader_type spatial;
render_mode cull_back, depth_draw_opaque, specular_schlick_ggx;

uniform sampler2D terrain_nearest : filter_nearest, repeat_disable;
uniform sampler2D terrain_linear : filter_linear, repeat_disable;
uniform vec2 grid_size = vec2(960.0, 540.0);
uniform vec3 backdrop_top : source_color = vec3(0.36, 0.35, 0.33);
uniform vec3 backdrop_bottom : source_color = vec3(0.30, 0.285, 0.265);
uniform float tunnel_depth_dark = 0.11;
uniform float grain_contrast = 0.22;
uniform float seed = 0.0;

// ---- hashing / noise ------------------------------------------------------------------------
float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031 + seed * 0.013);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}
vec2 hash22(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973) + seed * 0.017);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.xx + p3.yz) * p3.zy);
}
float vnoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
	float s = 0.0, a = 0.5;
	for (int i = 0; i < 4; i++) {
		s += a * vnoise(p);
		p = p * 2.03 + 17.1;
		a *= 0.5;
	}
	return s;
}
// Cellular grains: distance to the nearest grain centre and that grain's id.
vec3 grains(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	float d1 = 8.0, d2 = 8.0;
	vec2 id = vec2(0.0);
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++) {
			vec2 g = vec2(float(x), float(y));
			vec2 o = hash22(i + g) * 0.8 + 0.1;
			float d = length(g + o - f);
			if (d < d1) {
				d2 = d1;
				d1 = d;
				id = i + g;
			} else if (d < d2) {
				d2 = d;
			}
		}
	return vec3(d1, d2 - d1, hash12(id * 1.37 + 3.1));
}

// ---- materials --------------------------------------------------------------------------------
// 0 air, 1 sand, 2 loam, 3 clay, 4 gravel, 5 stone, 6 root, 7 spoil
vec3 material_colour(int m, float v) {
	if (m == 1) return mix(vec3(0.36, 0.265, 0.165), vec3(0.45, 0.34, 0.21), v);  // sand
	if (m == 2) return mix(vec3(0.19, 0.13, 0.08), vec3(0.26, 0.18, 0.115), v);   // loam
	if (m == 3) return mix(vec3(0.28, 0.185, 0.12), vec3(0.34, 0.23, 0.15), v);   // clay
	if (m == 4) return mix(vec3(0.27, 0.235, 0.19), vec3(0.36, 0.31, 0.25), v);   // gravel
	if (m == 5) return mix(vec3(0.15, 0.13, 0.11), vec3(0.24, 0.21, 0.18), v);    // stone
	if (m == 6) return mix(vec3(0.14, 0.09, 0.055), vec3(0.2, 0.13, 0.08), v);    // root
	if (m == 7) return mix(vec3(0.29, 0.21, 0.135), vec3(0.38, 0.28, 0.18), v);   // spoil
	return vec3(0.24, 0.17, 0.11);
}

// Soil colour with soft, irregular boundaries between layers: the four nearest cells are blended with
// narrow smooth weights at domain-warped positions, so no grid staircase shows.
vec3 blended_soil(vec2 cellp, vec2 grid, float tone, out float wet_out) {
	vec2 warp = vec2(fbm(cellp * 0.13 + 1.7), fbm(cellp * 0.13 + 9.2)) - 0.5;
	vec2 wp = cellp + warp * 5.0 + (vec2(vnoise(cellp * 0.9), vnoise(cellp * 0.9 + 4.0)) - 0.5) * 1.6;
	vec2 b = floor(wp - 0.5);
	vec2 f = fract(wp - 0.5);
	vec3 acc = vec3(0.0);
	float wet = 0.0;
	for (int k = 0; k < 4; k++) {
		vec2 o = vec2(float(k & 1), float(k >> 1));
		vec2 uv = (clamp(b + o, vec2(0.0), grid - 1.0) + 0.5) / grid;
		vec4 t = texture(terrain_nearest, uv);
		int mm = int(t.r * 255.0 + 0.5) / 32;
		if (mm == 0) mm = 2;
		float w = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
		acc += material_colour(mm, tone) * w;
		wet += t.b * w;
	}
	wet_out = wet;
	return acc;
}

struct Cell {
	int material;
	bool surface_air;
	bool open;
	float moisture;
};
Cell cell_at(vec2 uv) {
	vec4 t = texture(terrain_nearest, uv);
	int r = int(t.r * 255.0 + 0.5);
	Cell c;
	c.material = r / 32;
	c.surface_air = (r & 16) != 0;
	c.open = (r & 8) != 0;
	c.moisture = t.b;
	return c;
}
float sdf_at(vec2 uv) {
	return (texture(terrain_linear, uv).a * 255.0 - 128.0) / 12.0;
}

void fragment() {
	vec2 cellp = UV * grid_size;
	vec2 px = 1.0 / grid_size;
	float sdf = sdf_at(UV);
	// Crumbly, sub-cell boundary: the smooth distance field broken up at grain scale.
	float rough = (fbm(cellp * 0.9) - 0.5) * 0.45 + (vnoise(cellp * 4.0) - 0.5) * 0.2;
	float e = sdf + rough;
	float aa = max(fwidth(e), 0.02);
	float solid = smoothstep(-aa, aa, e);

	Cell here = cell_at(UV);
	// Gradient of the distance field: points into the soil.
	vec2 g = vec2(sdf_at(UV + vec2(px.x, 0.0)) - sdf_at(UV - vec2(px.x, 0.0)), sdf_at(UV + vec2(0.0, px.y)) - sdf_at(UV - vec2(0.0, px.y)));
	vec2 gn = length(g) > 1e-4 ? normalize(g) : vec2(0.0, 1.0);

	// Which material shows: the cell's own, or (for smoothed edges spilling into open cells) the soil beside it.
	Cell mat = here;
	if (here.open && here.surface_air) {
		mat = cell_at(UV + gn * px * 1.5);
		if (mat.material == 0) mat.material = 7;
	}
	int m = mat.material;
	if (m == 0) m = 2;

	// ---- the soil face against the glass --------------------------------------------------------
	vec3 gr = grains(cellp * 3.4);                       // grains about a quarter of a cell across
	float mottling = fbm(cellp * 0.09) ;
	float tone = clamp(mottling * 0.9 + (gr.z - 0.5) * 0.55, 0.0, 1.0);
	float wet = 0.0;
	vec3 soil = blended_soil(cellp, grid_size, tone, wet);
	// individual grains: quartz glints, dark mineral grains, shadowed gaps between grains
	float quartz = step(0.93, gr.z) * (m == 1 || m == 7 || m == 4 ? 1.0 : 0.35);
	float darkg = step(gr.z, 0.06);
	soil = mix(soil, vec3(0.62, 0.58, 0.50), quartz * 0.45);
	soil = mix(soil, vec3(0.06, 0.05, 0.04), darkg * 0.45);
	soil *= 1.0 - grain_contrast * smoothstep(0.08, 0.0, gr.y);  // gaps between grains
	if (m == 4) { // gravel: pebbles with their own shading
		vec3 pb = grains(cellp * 1.1);
		float pebble = smoothstep(0.42, 0.30, pb.x);
		vec3 pc = mix(vec3(0.25, 0.22, 0.18), vec3(0.40, 0.36, 0.30), pb.z);
		soil = mix(soil, pc * (0.8 + 0.4 * (1.0 - pb.x)), pebble);
	}
	if (m == 5) { // embedded stones: smooth, rounded, darker than the soil around them
		float sd = texture(terrain_linear, UV).a; // unused channel of interest is the stone's own extent
		soil = material_colour(5, 0.3 + 0.5 * vnoise(cellp * 0.6)) * (0.8 + 0.3 * vnoise(cellp * 2.5));
		soil = mix(soil, soil * vec3(1.05, 0.95, 0.85), 0.5);
	}
	if (m == 6) { // root fibres
		float fib = abs(sin(cellp.x * 2.3 + vnoise(cellp * 0.8) * 5.0));
		soil *= 0.8 + 0.3 * fib;
	}
	if (m == 7) { // loose spoil: lighter, looser, more air between grains
		soil *= 0.92 + 0.16 * vnoise(cellp * 1.7);
		soil *= 1.0 - 0.18 * smoothstep(0.12, 0.0, gr.y);
	}
	soil *= 1.0 - 0.28 * wet;
	soil = mix(soil, soil * vec3(0.92, 0.95, 1.0), wet * 0.3);

	// ---- inside a passage: its curved walls and the dim back wall ---------------------------------
	float inside = clamp(-sdf, 0.0, 4.0);
	vec3 back = material_colour(m, 0.35 + 0.3 * fbm(cellp * 0.35)) * (1.0 - 0.3 * wet);
	vec3 backg = grains(cellp * 2.2);
	back *= 0.85 + 0.3 * backg.z;
	back *= 1.0 - 0.25 * smoothstep(0.1, 0.0, backg.y);
	// crumbs lying on the passage floor (where the soil is below)
	float floorside = clamp(gn.y, 0.0, 1.0) * smoothstep(1.6, 0.3, inside);
	float crumbs = step(0.72, grains(cellp * 4.1).z) * floorside;
	back = mix(back, soil * 0.9, crumbs * 0.6);
	float depthShade = mix(0.45, tunnel_depth_dark, smoothstep(0.05, 1.3, inside));
	// light enters through the glass from the upper left: lower walls catch it, upper walls are shadowed
	float wallLight = dot(-gn, normalize(vec2(-0.45, -0.9)));
	float wallBand = smoothstep(2.0, 0.2, inside);
	depthShade *= 1.0 + 0.35 * wallLight * wallBand;
	vec3 tunnel = back * depthShade;

	// ---- surface air: the far pane and the room behind the farm ----------------------------------
	vec3 air = mix(backdrop_top, backdrop_bottom, smoothstep(0.0, 0.25, UV.y));
	air *= 0.96 + 0.04 * vnoise(UV * vec2(9.0, 5.0) + 3.0);
	float airness = (here.open && here.surface_air) ? 1.0 : 0.0;
	vec3 openColour = mix(tunnel, air, airness);

	vec3 col = mix(openColour, soil, solid);

	// Relief for the lights: grains stand slightly proud of the glass-pressed face, and the tunnel lip
	// rounds over into the passage.
	float h = solid * (0.6 + 0.4 * smoothstep(0.0, 0.2, gr.x)) ;
	float lip = smoothstep(-1.2, 0.6, e) ;
	vec2 dh = vec2(dFdx(h + lip * 1.5), dFdy(h + lip * 1.5));
	vec3 n = normalize(vec3(-dh.x * 1.8, dh.y * 1.8, 1.0));
	NORMAL_MAP = n * 0.5 + 0.5;
	NORMAL_MAP_DEPTH = 1.0;

	ALBEDO = col;
	ROUGHNESS = mix(0.95, mix(0.8, 0.55, wet), solid);
	if (m == 5) ROUGHNESS = mix(ROUGHNESS, 0.45, solid);
	SPECULAR = 0.35;
	AO = mix(mix(0.55, 1.0, airness), 1.0, solid);
	AO_LIGHT_AFFECT = 0.35;
}
