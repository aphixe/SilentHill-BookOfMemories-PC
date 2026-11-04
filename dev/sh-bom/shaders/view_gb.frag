#version 450
#extension GL_GOOGLE_include_directive : require
#include "view_common.glsl"
// F16F16F16F16 read as two U8U8U8U8 columns a pixel (a G-buffer's two words, Silent Hill: Book of Memories' lighting), from an
// integer copy of the target: its raw words survive, NaN patterns included, which a float read would normalise. One word for
// the whole view, at the target's own texels, so a pass at a higher render scale reads the G-buffer at that scale.
layout(set = 2, binding = 0) uniform usampler2D src;
void main() {
	uvec2 P = uvec2(gl_FragCoord.xy), sp = min(uvec2((P.x / (2u * u.scale)) * u.scale + P.x % u.scale, P.y), uvec2(u.tgt_w * u.scale - 1u, u.tgt_h * u.scale_y - 1u));
	uvec4 c = texelFetch(src, ivec2(sp), 0);
	// a NaN pattern reads as the canonical one, its sign kept, as the guest-memory path's f2h gives it
	for (int k = 0; k < 4; k++) if ((c[k] & 0x7c00u) == 0x7c00u && (c[k] & 0x3ffu) != 0u) c[k] = (c[k] & 0x8000u) | 0x7e00u;
	uint w = (u.off_bytes & 4u) != 0u ? (c.b | (c.a << 16u)) : (c.r | (c.g << 16u));
	vec4 r; for (int k = 0; k < 4; k++) r[k] = float((w >> (8u * uint(k))) & 255u) / 255.0;
	o = swizzle(r, u.swz);
}
