// Soil pellets and food carried in the mandibles, and seeds on the feeding stone / in stores.
// INSTANCE_CUSTOM.x: 1 soil, 2 food; .y: random.
shader_type spatial;
render_mode cull_disabled;

varying vec2 v_custom;

void vertex() {
	v_custom = INSTANCE_CUSTOM.xy;
}

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void fragment() {
	float kind = v_custom.x;
	float r = v_custom.y;
	if (kind > 1.5) {
		// seed / crumb: pale, slightly waxy
		ALBEDO = mix(vec3(0.78, 0.70, 0.52), vec3(0.88, 0.80, 0.62), r);
		ROUGHNESS = 0.45;
		SPECULAR = 0.5;
	} else {
		// a packed crumb of moist soil
		float g = hash(floor(UV * 8.0) + r * 13.0);
		ALBEDO = mix(vec3(0.34, 0.25, 0.17), vec3(0.52, 0.40, 0.27), r) * (0.85 + 0.3 * g);
		ROUGHNESS = 0.9;
		SPECULAR = 0.25;
	}
}
