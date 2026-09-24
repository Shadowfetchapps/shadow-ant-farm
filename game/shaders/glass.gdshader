// The front pane. Almost invisible: a faint sheet reflection of the room, very light dust and a few old
// smudges, strongest towards the edges where the eye expects glass. Kept subtle so the capture stays clean.
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_back, specular_schlick_ggx, unshaded;

uniform float reflection_strength = 0.045;
uniform float dust_strength = 0.035;
uniform float seed = 0.0;

float hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031 + seed * 0.011);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}
float vnoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}

void fragment() {
	vec2 uv = UV;
	// A broad, soft reflection of a window off to the upper left, falling off across the pane.
	float band = smoothstep(0.55, 0.0, abs((uv.x * 0.8 + uv.y * 0.55) - 0.32)) * smoothstep(1.2, 0.2, uv.x + uv.y);
	float sheen = band * reflection_strength;
	// Edge falloff: glass reads more towards the frame.
	float edge = 1.0 - smoothstep(0.0, 0.08, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));
	// Fine dust specks and faint wipe marks.
	float dust = step(0.99993, hash(floor(uv * vec2(3840.0, 2160.0) * 0.5))) * dust_strength * 3.0;
	float smudge = smoothstep(0.62, 0.9, vnoise(uv * vec2(6.0, 3.5) + 11.0)) * vnoise(uv * vec2(90.0, 14.0)) * 0.018;
	float a = sheen + edge * 0.012 + dust + smudge;
	ALBEDO = mix(vec3(0.92, 0.95, 1.0), vec3(1.0), dust / max(a, 1e-4));
	ALPHA = clamp(a, 0.0, 0.25);
}
