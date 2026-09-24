// Worker ants (MultiMesh). The gait is driven by the simulation: INSTANCE_CUSTOM.x is the stride phase,
// which advances with distance walked (legs never slide), .y packs the animation and the load, .z the
// speed relative to a normal walk and .w a per-worker random value.
shader_type spatial;
render_mode cull_disabled, specular_schlick_ggx;

uniform float time_offset = 0.0;
uniform vec3 tint : source_color = vec3(1.0);

varying float v_part;

vec2 rot(vec2 p, float a) {
	float c = cos(a), s = sin(a);
	return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

void vertex() {
	float gait = INSTANCE_CUSTOM.x;
	float code = INSTANCE_CUSTOM.y;
	float anim = mod(code, 8.0);          // 0 walk, 1 dig, 2 groom, 3 rest, 4 pick up, 5 deposit, 6 probe
	float load = floor(code / 8.0);       // 0 none, 1 soil, 2 food
	float speed = INSTANCE_CUSTOM.z;
	float rnd = INSTANCE_CUSTOM.w;
	float t = TIME + time_offset + rnd * 37.0;
	float part = CUSTOM0.x;
	float w = CUSTOM0.y;
	vec2 anchor = CUSTOM0.zw;
	v_part = part;
	vec3 v = VERTEX;

	if (part < 5.5) {
		// Legs: alternating tripod gait (L1 R2 L3 against R1 L2 R3).
		int idx = int(part + 0.5);
		float side = (idx % 2 == 0) ? -1.0 : 1.0;
		float group = (idx == 0 || idx == 3 || idx == 4) ? 0.0 : 3.14159;
		float amp = 0.38 * clamp(speed * 1.4, 0.0, 1.0);
		float ph = gait + group;
		float swing = sin(ph) * amp;
		float lift = max(0.0, cos(ph)) * amp * 0.12;
		if (anim > 0.5 && anim < 1.5) { // digging: front legs rake, others brace
			if (idx < 2) swing += sin(t * 11.0) * 0.25;
		} else if (anim > 1.5 && anim < 2.5) { // grooming: front legs sweep over the head and antennae
			if (idx < 2) swing += -side * (0.6 + 0.35 * sin(t * 7.0));
		} else if (anim > 2.5 && anim < 3.5) { // resting: legs drawn in a little
			v.xy = anchor + (v.xy - anchor) * mix(1.0, 0.82, w);
		} else if (speed < 0.05) { // standing: an occasional shift of weight
			swing += sin(t * 0.9 + float(idx)) * 0.04;
		}
		v.xy = anchor + rot(v.xy - anchor, -swing * side * w);
		v.z += lift * w;
	} else if (part < 7.5) {
		// Antennae: constant probing, faster when searching or digging.
		float side = part < 6.5 ? -1.0 : 1.0;
		float rate = (anim > 5.5 || anim > 0.5 && anim < 1.5) ? 9.0 : 5.0;
		float a = 0.22 * sin(t * rate + side) + 0.12 * sin(t * 2.3 + rnd * 5.0);
		if (anim > 2.5 && anim < 3.5) a = 0.05 * sin(t * 0.7) - 0.25 * side;
		if (anim > 1.5 && anim < 2.5) a += 0.3 * sin(t * 7.0) * side;
		v.xy = anchor + rot(v.xy - anchor, a * w * side);
	} else if (part > 11.5) {
		// Mandibles open and close while digging.
		if (anim > 0.5 && anim < 1.5) v.y *= 1.0 + 0.8 * max(0.0, sin(t * 11.0));
	} else if (part > 8.5 && part < 9.5) {
		// Gaster sways slightly with the stride; lifts when resting.
		v.y += sin(gait) * 0.012 * clamp(speed, 0.0, 1.0);
		if (anim > 2.5 && anim < 3.5) v.z += 0.01;
	}
	if (part > 10.5 && part < 11.5 && anim > 0.5 && anim < 1.5) {
		v.x += 0.012 * sin(t * 11.0); // head bobbing against the face
	}
	VERTEX = v;
}

void fragment() {
	vec3 base = COLOR.rgb * tint;
	ALBEDO = base;
	ROUGHNESS = 0.32;
	SPECULAR = 0.55;
	// Fine setae and the waxy cuticle give a soft sheen at grazing angles.
	RIM = 0.25;
	RIM_TINT = 0.4;
	CLEARCOAT = 0.35;
	CLEARCOAT_ROUGHNESS = 0.25;
}
