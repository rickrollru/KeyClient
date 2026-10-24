#include "xc_renderer.h"

#include <base/log.h>

#include <engine/shared/config.h>

#include <atomic>

static std::atomic<bool> s_Available{false};

bool XcRender::IsAvailable()
{
	return s_Available.load(std::memory_order_relaxed);
}

#if defined(CONF_BACKEND_OPENGL_ES) || defined(CONF_HEADLESS_CLIENT)

// No module on OpenGL ES and headless builds: commands are skipped, effects fall back to IGraphics.
CXcRenderer::CXcRenderer() = default;
CXcRenderer::~CXcRenderer() = default;
bool CXcRenderer::RunCommand(const CCommandBuffer::SCommand *pCommand) { return XcRender::IsXcCommand(pCommand->m_Cmd); }
void CXcRenderer::OnPreSwap() {}
void CXcRenderer::Shutdown() {}

#else

#include <GL/glew.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

// ---------------------------------------------------------------------------------------------
// Shaders

static const char *const s_pShapeVertex = R"(#version 330 core
layout(location = 0) in vec2 a_Corner;
layout(location = 1) in vec4 a_PosSize;   // center, half size
layout(location = 2) in vec4 a_Shape;     // rotation, shape, param0, param1
layout(location = 3) in vec4 a_Edge;      // softness, outline width, glow radius, glow strength
layout(location = 4) in vec4 a_Color;
layout(location = 5) in vec4 a_Color2;
layout(location = 6) in vec4 a_Outline;

uniform vec4 u_Screen;  // left, top, right, bottom
uniform float u_Pixel;  // world units per pixel
uniform int u_UseTilt;  // 3D view: the tilted camera maps the world
uniform mat4 u_Tilt;

out vec2 v_Local;
flat out vec2 v_Half;
flat out vec4 v_Shape;
flat out vec4 v_Edge;
flat out vec4 v_Color;
flat out vec4 v_Color2;
flat out vec4 v_Outline;

void main()
{
	// room for the glow, the soft edge and anti aliasing
	float Margin = a_Edge.z + a_Edge.x + 2.0 * u_Pixel;
	// rings: half the line lies outside the radius
	if(int(a_Shape.y + 0.5) == 1)
		Margin += 0.5 * a_Shape.z;
	vec2 Extent = a_PosSize.zw + vec2(Margin);
	vec2 Local = a_Corner * Extent;
	float s = sin(a_Shape.x);
	float c = cos(a_Shape.x);
	vec2 World = a_PosSize.xy + vec2(Local.x * c - Local.y * s, Local.x * s + Local.y * c);
	vec2 Ndc = (World - u_Screen.xy) / (u_Screen.zw - u_Screen.xy) * 2.0 - 1.0;
	gl_Position = vec4(Ndc.x, -Ndc.y, 0.0, 1.0);
	if(u_UseTilt == 1)
		gl_Position = u_Tilt * vec4(World, 0.0, 1.0);

	v_Local = Local;
	v_Half = a_PosSize.zw;
	v_Shape = a_Shape;
	v_Edge = a_Edge;
	v_Color = a_Color;
	v_Color2 = a_Color2;
	v_Outline = a_Outline;
}
)";

static const char *const s_pShapeFragment = R"(#version 330 core
in vec2 v_Local;
flat in vec2 v_Half;
flat in vec4 v_Shape;
flat in vec4 v_Edge;
flat in vec4 v_Color;
flat in vec4 v_Color2;
flat in vec4 v_Outline;

uniform float u_Pixel;
uniform int u_BloomPass;
uniform float u_Threshold;

out vec4 FragColor;

const float PI = 3.14159265;

// distance to an ellipse, exact for circles, first order approximation otherwise
float SdEllipse(vec2 p, vec2 ab)
{
	if(abs(ab.x - ab.y) < 0.001)
		return length(p) - ab.x;
	vec2 q = p / ab;
	float k = length(q);
	if(k < 0.0001)
		return -min(ab.x, ab.y);
	vec2 Grad = p / (ab * ab) / k;
	return (k - 1.0) / max(length(Grad), 0.0001);
}

float SdRoundRect(vec2 p, vec2 b, float r)
{
	r = min(r, min(b.x, b.y));
	vec2 q = abs(p) - b + vec2(r);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// regular star with n points, m = inner radius ratio
float SdStar(vec2 p, float r, float n, float m)
{
	n = max(n, 3.0);
	float an = PI / n;
	float en = PI / mix(2.0, n, clamp(1.0 - m, 0.0, 1.0));
	en = max(en, an * 1.0001);
	vec2 acs = vec2(cos(an), sin(an));
	vec2 ecs = vec2(cos(en), sin(en));
	// point up
	p = vec2(p.x, -p.y);
	float bn = mod(atan(p.x, p.y), 2.0 * an) - an;
	p = length(p) * vec2(cos(bn), abs(sin(bn)));
	p -= r * acs;
	p += ecs * clamp(-dot(p, ecs), 0.0, r * acs.y / ecs.y);
	return length(p) * sign(p.x);
}

// glow falloff: smooth, and exactly zero at the glow radius so the quad edge never shows
float GlowFalloff(float d, float Radius)
{
	float f = clamp(1.0 - max(d, 0.0) / max(Radius, 0.0001), 0.0, 1.0);
	return f * f * f;
}

// isosceles triangle, apex at the top
float SdTriangle(vec2 p, vec2 Half)
{
	vec2 q = vec2(Half.x, 2.0 * Half.y);
	p.y += Half.y; // apex at origin, base at y = 2 * half
	p.x = abs(p.x);
	vec2 a = p - q * clamp(dot(p, q) / dot(q, q), 0.0, 1.0);
	vec2 b = p - q * vec2(clamp(p.x / q.x, 0.0, 1.0), 1.0);
	float s = -sign(q.y);
	vec2 d = min(vec2(dot(a, a), s * (p.x * q.y - p.y * q.x)), vec2(dot(b, b), s * (p.y - q.y)));
	return -sqrt(d.x) * sign(d.y);
}

void main()
{
	int Shape = int(v_Shape.y + 0.5);
	vec2 p = v_Local;
	float Softness = v_Edge.x;
	float OutlineWidth = v_Edge.y;
	float GlowRadius = v_Edge.z;
	float GlowStrength = v_Edge.w;

	float t = clamp(p.y / max(2.0 * v_Half.y, 0.0001) + 0.5, 0.0, 1.0);
	vec4 Fill = mix(v_Color, v_Color2, t);
	if(Shape == 0 && v_Shape.w > 0.0)
	{
		// radial gradient: color2 in the center, color at the edge
		float k = clamp(length(p / max(v_Half, vec2(0.0001))), 0.0, 1.0);
		Fill = mix(v_Color2, v_Color, pow(k, v_Shape.w));
	}

	float Alpha;
	vec3 Rgb;
	if(Shape == 6)
	{
		// soft gaussian dot
		float r = length(p / max(v_Half, vec2(0.0001)));
		Alpha = exp(-3.5 * r * r) * Fill.a;
		Rgb = Fill.rgb;
	}
	else
	{
		float d;
		if(Shape == 0)
			d = SdEllipse(p, v_Half);
		else if(Shape == 1)
			d = abs(SdEllipse(p, v_Half)) - 0.5 * v_Shape.z;
		else if(Shape == 2)
			d = SdRoundRect(p, v_Half, v_Shape.z);
		else if(Shape == 3)
			d = SdStar(p, v_Half.x, v_Shape.z, v_Shape.w);
		else if(Shape == 4)
			d = SdTriangle(p, v_Half);
		else
		{
			// capsule along x
			vec2 q = vec2(max(abs(p.x) - max(v_Half.x - v_Half.y, 0.0), 0.0), p.y);
			d = length(q) - v_Half.y;
		}

		// screen space anti aliasing, widened by the requested softness
		float Aa = max(fwidth(d), u_Pixel * 0.5);
		float Width = Aa + Softness;
		float Coverage = clamp(0.5 - d / Width, 0.0, 1.0);

		Rgb = Fill.rgb;
		float FillAlpha = Fill.a;
		if(OutlineWidth < 0.0)
		{
			// glow only: light that is full on the shape and fades out around it
			float Glow = GlowStrength * GlowFalloff(d, GlowRadius);
			Alpha = Glow * Fill.a;
		}
		else if(OutlineWidth > 0.0)
		{
			float Inner = clamp(0.5 - (d + OutlineWidth) / Width, 0.0, 1.0);
			Rgb = mix(v_Outline.rgb, Fill.rgb, Inner);
			FillAlpha = mix(v_Outline.a, Fill.a, Inner);
		}
		if(OutlineWidth >= 0.0)
			Alpha = Coverage * FillAlpha;

		if(OutlineWidth >= 0.0 && GlowRadius > 0.0 && GlowStrength > 0.0)
		{
			float Glow = GlowStrength * GlowFalloff(d, GlowRadius) * (1.0 - Coverage);
			Alpha += Glow * Fill.a;
		}
	}

	if(Shape == 1 && v_Shape.w != 0.0)
	{
		// half ring: param1 > 0 keeps the lower (front) half, < 0 the upper (back) half
		float Side = sign(v_Shape.w) * p.y;
		Alpha *= clamp(0.5 + Side / max(fwidth(p.y), 0.0001), 0.0, 1.0);
	}

	if(u_BloomPass != 0)
	{
		// premultiplied, only the bright part goes into the bloom buffer
		vec3 c = Rgb * Alpha;
		float Luma = max(c.r, max(c.g, c.b));
		float Keep = clamp((Luma - u_Threshold) / max(1.0 - u_Threshold, 0.0001), 0.0, 1.0);
		FragColor = vec4(c * Keep, 1.0);
	}
	else
	{
		FragColor = vec4(Rgb, Alpha);
	}
}
)";

// 3D view: the sides of the blocks. One instance per cell of the grid, four faces of two triangles each;
// a face is only there between a block and air (or freeze). The face goes from the world plane into the depth.
// Two passes: the solid blocks, then the freeze as see-through ice. The output is premultiplied.
static const char *const s_pBlocksVertex = R"(#version 330 core
uniform mat4 u_Matrix;
uniform usampler2D u_Grid;
uniform ivec4 u_Cells; // first cell x, y, number of cells w, h
uniform vec2 u_ZRange; // the depth the sides span
uniform float u_FogDistance; // > 0: the fog grows with the distance to the eye
uniform int u_Pass; // 0 the blocks, 1 the freeze
out float v_Far;
out float v_Edge;
out float v_Depth; // 0 at the front edge, 1 at the back
out float v_U; // 0..1 along the edge of the face
out vec2 v_World;
flat out float v_Shade;
flat out int v_Type;
flat out int v_Face;
flat out vec2 v_Ao; // a wall stands at the start / the end of the edge
flat out vec4 v_MapColor; // a: the color is set

uint Cell(ivec2 Pos)
{
	if(Pos.x < 0 || Pos.y < 0 || Pos.x >= u_Cells.z || Pos.y >= u_Cells.w)
		return 1u;
	return texelFetch(u_Grid, Pos, 0).a & 3u;
}

bool Solid(ivec2 Pos)
{
	uint Type = Cell(Pos);
	return Type == 1u || Type == 2u;
}

void main()
{
	ivec2 Pos = ivec2(gl_InstanceID % u_Cells.z, gl_InstanceID / u_Cells.z);
	int Face = gl_VertexID / 6;
	int Corner = gl_VertexID % 6;
	const ivec2 aDir[4] = ivec2[4](ivec2(0, -1), ivec2(0, 1), ivec2(-1, 0), ivec2(1, 0)); // top, bottom, left, right
	uint Type = Cell(Pos);
	uint Next = Cell(Pos + aDir[Face]);
	bool Freeze = Type == 3u;
	// blocks: a face towards air and freeze; freeze: only towards air
	bool Show = Type != 0u && (u_Pass == 1) == Freeze && (Freeze ? Next == 0u : (Next == 0u || Next == 3u));
	if(!Show)
	{
		gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // no face here, outside the depth range
		v_Far = 0.0;
		v_Edge = 1.0;
		v_Depth = 0.0;
		v_U = 0.0;
		v_World = vec2(0.0);
		v_Shade = 0.0;
		v_Type = 0;
		v_Face = 0;
		v_Ao = vec2(0.0);
		v_MapColor = vec4(0.0);
		return;
	}
	const vec2 aStart[4] = vec2[4](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0));
	const vec2 aEnd[4] = vec2[4](vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
	const vec2 aCorner[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
	vec2 c = aCorner[Corner];
	vec2 World = (vec2(u_Cells.xy + Pos) + mix(aStart[Face], aEnd[Face], c.x)) * 32.0;
	gl_Position = u_Matrix * vec4(World, mix(u_ZRange.x, u_ZRange.y, c.y), 1.0);
	v_Far = u_FogDistance > 0.0 ? clamp(gl_Position.w / u_FogDistance, 0.0, 1.0) : c.y;
	v_Edge = u_FogDistance > 0.0 ? min(c.y, 1.0 - c.y) * 2.0 : c.y; // first person: both ends of the depth
	v_Depth = c.y;
	v_U = c.x;
	v_World = World;
	const float aShade[4] = float[4](1.06, 0.45, 0.7, 0.7);
	v_Shade = aShade[Face];
	v_Type = int(Type);
	v_Face = Face;
	// soft shadow where the face meets a wall: the corners next to it along the edge
	const ivec2 aAoStart[4] = ivec2[4](ivec2(-1, -1), ivec2(-1, 1), ivec2(-1, -1), ivec2(1, -1));
	const ivec2 aAoEnd[4] = ivec2[4](ivec2(1, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, 1));
	if(Freeze)
	{
		// ice: where the face ends (the next cell along the edge has no freeze face) instead of a wall shadow
		const ivec2 aAlong[4] = ivec2[4](ivec2(1, 0), ivec2(1, 0), ivec2(0, 1), ivec2(0, 1));
		ivec2 Before = Pos - aAlong[Face], After = Pos + aAlong[Face];
		bool GoesBefore = Cell(Before) == 3u && Cell(Before + aDir[Face]) == 0u;
		bool GoesAfter = Cell(After) == 3u && Cell(After + aDir[Face]) == 0u;
		v_Ao = vec2(GoesBefore ? 0.0 : 1.0, GoesAfter ? 0.0 : 1.0);
	}
	else
		v_Ao = vec2(Solid(Pos + aAoStart[Face]) ? 1.0 : 0.0, Solid(Pos + aAoEnd[Face]) ? 1.0 : 0.0);
	uvec4 Texel = texelFetch(u_Grid, Pos, 0);
	v_MapColor = vec4(vec3(Texel.rgb) / 255.0, (Texel.a & 4u) != 0u ? 1.0 : 0.0);
}
)";

static const char *const s_pBlocksFragment = R"(#version 330 core
in float v_Far;
in float v_Edge;
in float v_Depth;
in float v_U;
in vec2 v_World;
flat in float v_Shade;
flat in int v_Type;
flat in int v_Face;
flat in vec2 v_Ao;
flat in vec4 v_MapColor;
uniform vec4 u_Color;
uniform vec4 u_Color2;
uniform vec4 u_Color3;
uniform vec4 u_Fog;
out vec4 FragColor;

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main()
{
	bool Freeze = v_Type == 3;
	vec4 Color = v_Type == 2 ? u_Color2 : Freeze ? u_Color3 : u_Color;
	if(v_MapColor.a > 0.5)
		Color.rgb = Freeze ? mix(v_MapColor.rgb, u_Color3.rgb, 0.5) : v_MapColor.rgb;
	// a fine grain, so a long face is not one flat color
	float Grain = Freeze ? 0.0 : Hash(floor(v_World / 4.0) + vec2(v_Depth * 9.0, float(v_Face))) - 0.5;
	// the corner with a wall gets darker
	float Ao = Freeze ? 0.0 : v_Ao.x * (1.0 - smoothstep(0.0, 0.4, v_U)) + v_Ao.y * smoothstep(0.6, 1.0, v_U);
	vec3 Rgb = Color.rgb * v_Shade * mix(1.0, 0.75, v_Far) * (1.0 - 0.35 * Ao) * (1.0 + Grain * 0.06);
	// a light bevel along the front edge, a dark crease along the back one
	float Front = 1.0 - smoothstep(0.0, 0.06, v_Edge);
	Rgb += vec3(v_Face == 0 ? 0.12 : 0.07) * Front;
	Rgb *= 1.0 - 0.22 * smoothstep(0.9, 1.0, v_Depth);
	float Alpha = Color.a;
	if(Freeze)
	{
		// ice: brighter and more solid along its edges; across a long face only at its real ends, not at every
		// cell (that gave stripes)
		float Ends = max(v_Ao.x * (1.0 - smoothstep(0.0, 0.08, v_U)), v_Ao.y * smoothstep(0.92, 1.0, v_U));
		float Rim = max(Ends, 1.0 - smoothstep(0.0, 0.08, min(v_Depth, 1.0 - v_Depth)));
		Rgb += vec3(0.25) * Rim;
		Alpha = clamp(Alpha + 0.35 * Rim, 0.0, 1.0);
	}
	Rgb = mix(Rgb, u_Fog.rgb, u_Fog.a * v_Far);
	FragColor = vec4(Rgb * Alpha, Alpha);
}
)";

static const char *const s_pCopyFragment = R"(#version 330 core
in vec2 v_Uv;
uniform sampler2D u_Tex;
out vec4 FragColor;
void main()
{
	FragColor = texture(u_Tex, v_Uv);
}
)";

static const char *const s_pFullscreenVertex = R"(#version 330 core
out vec2 v_Uv;
void main()
{
	// one big triangle covering the viewport
	vec2 Pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_Uv = Pos;
	gl_Position = vec4(Pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

// dual Kawase filter, see "Bandwidth-Efficient Rendering" (Marius Bjorge, SIGGRAPH 2015)
static const char *const s_pDownFragment = R"(#version 330 core
in vec2 v_Uv;
uniform sampler2D u_Tex;
uniform vec2 u_HalfPixel;
out vec4 FragColor;
void main()
{
	vec4 Sum = texture(u_Tex, v_Uv) * 4.0;
	Sum += texture(u_Tex, v_Uv - u_HalfPixel);
	Sum += texture(u_Tex, v_Uv + u_HalfPixel);
	Sum += texture(u_Tex, v_Uv + vec2(u_HalfPixel.x, -u_HalfPixel.y));
	Sum += texture(u_Tex, v_Uv - vec2(u_HalfPixel.x, -u_HalfPixel.y));
	FragColor = Sum / 8.0;
}
)";

static const char *const s_pUpFragment = R"(#version 330 core
in vec2 v_Uv;
uniform sampler2D u_Tex;
uniform vec2 u_HalfPixel;
out vec4 FragColor;
void main()
{
	vec4 Sum = texture(u_Tex, v_Uv + vec2(-u_HalfPixel.x * 2.0, 0.0));
	Sum += texture(u_Tex, v_Uv + vec2(-u_HalfPixel.x, u_HalfPixel.y)) * 2.0;
	Sum += texture(u_Tex, v_Uv + vec2(0.0, u_HalfPixel.y * 2.0));
	Sum += texture(u_Tex, v_Uv + vec2(u_HalfPixel.x, u_HalfPixel.y)) * 2.0;
	Sum += texture(u_Tex, v_Uv + vec2(u_HalfPixel.x * 2.0, 0.0));
	Sum += texture(u_Tex, v_Uv + vec2(u_HalfPixel.x, -u_HalfPixel.y)) * 2.0;
	Sum += texture(u_Tex, v_Uv + vec2(0.0, -u_HalfPixel.y * 2.0));
	Sum += texture(u_Tex, v_Uv + vec2(-u_HalfPixel.x, -u_HalfPixel.y)) * 2.0;
	FragColor = Sum / 12.0;
}
)";

static const char *const s_pCompositeFragment = R"(#version 330 core
in vec2 v_Uv;
uniform sampler2D u_Tex;
uniform float u_Strength;
uniform vec4 u_Tint;
out vec4 FragColor;
void main()
{
	vec4 c = texture(u_Tex, v_Uv);
	c.rgb = mix(c.rgb, u_Tint.rgb, u_Tint.a);
	FragColor = vec4(c.rgb * u_Strength, 1.0);
}
)";

// neon outline: the captured sprites with a soft glow and a crisp rim around their silhouette
static const char *const s_pGlowFragment = R"(#version 330 core
in vec2 v_Uv;
uniform sampler2D u_Capture;
uniform sampler2D u_Blur;
uniform vec4 u_Color;
uniform vec4 u_RimColor;
uniform float u_Strength;
uniform float u_RimWidth;
uniform vec2 u_Texel;
uniform int u_FillMode;
uniform vec4 u_FillColor;
uniform vec4 u_FillColor2;
uniform float u_FillAlpha;
uniform float u_FillScale;
uniform float u_FillTime;
uniform float u_KeepShade;
out vec4 FragColor;

vec3 Hue(float h)
{
	return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float Noise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(Hash(i), Hash(i + vec2(1.0, 0.0)), f.x), mix(Hash(i + vec2(0.0, 1.0)), Hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// the pattern at a screen point, p is in tee widths
vec3 Pattern(vec2 p)
{
	float t = u_FillTime;
	if(u_FillMode == 1) // gradient that flows up
		return mix(u_FillColor.rgb, u_FillColor2.rgb, 0.5 + 0.5 * sin(p.y * 3.0 - t * 1.6));
	if(u_FillMode == 2) // rainbow
		return Hue(fract((p.x + p.y) * 0.6 - t * 0.25));
	if(u_FillMode == 3) // galaxy: dark sky, colored nebula and twinkling stars
	{
		vec2 q = p * 3.0 + vec2(t * 0.08, 0.0);
		float n = Noise(q) * 0.6 + Noise(q * 2.3 + 7.0) * 0.4;
		vec3 Sky = mix(u_FillColor.rgb * 0.18, u_FillColor2.rgb * 0.8, smoothstep(0.35, 0.85, n));
		vec2 Cell = floor(p * 14.0);
		float Star = step(0.93, Hash(Cell)) * (0.5 + 0.5 * sin(t * 3.0 + Hash(Cell + 3.0) * 6.28));
		vec2 f = fract(p * 14.0) - 0.5;
		Star *= smoothstep(0.25, 0.0, length(f));
		return Sky + vec3(Star);
	}
	if(u_FillMode == 4) // diagonal waves
	{
		float w = sin((p.x + p.y) * 9.0 - t * 3.0 + sin(p.y * 4.0 + t) * 1.5);
		return mix(u_FillColor.rgb, u_FillColor2.rgb, smoothstep(-0.2, 0.2, w));
	}
	// chrome: bands of light like a mirror, slowly sliding
	float b = sin(p.y * 7.0 + sin(p.x * 3.0 + t * 0.7) * 1.2 - t * 0.5);
	vec3 Dark = u_FillColor.rgb * 0.35;
	vec3 Light = mix(u_FillColor.rgb, vec3(1.0), 0.65);
	return mix(Dark, Light, smoothstep(-0.3, 0.6, b)) + vec3(pow(max(b, 0.0), 12.0)) * 0.6;
}

// DDNet blends with glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) for all channels, so on a
// transparent target the stored alpha is a*a and the color is premultiplied by a
float Coverage(vec2 Uv)
{
	return sqrt(clamp(texture(u_Capture, Uv).a, 0.0, 1.0));
}

void main()
{
	vec4 c = texture(u_Capture, v_Uv);
	float a = sqrt(clamp(c.a, 0.0, 1.0));

	float Rim = 0.0;
	if(u_RimWidth > 0.0)
	{
		float Dilated = 0.0;
		for(int i = 0; i < 12; i++)
		{
			float Angle = 6.2831853 * float(i) / 12.0;
			Dilated = max(Dilated, Coverage(v_Uv + vec2(cos(Angle), sin(Angle)) * u_RimWidth * u_Texel));
		}
		Rim = clamp(Dilated - a, 0.0, 1.0);
	}
	float Glow = clamp(texture(u_Blur, v_Uv).a * u_Strength, 0.0, 1.0);

	// light only outside the silhouette, the sprites themselves stay untouched.
	// Part of the light also covers the background, so the color stays visible on bright maps.
	float GlowA = Glow * u_Color.a;
	float RimA = Rim * u_RimColor.a;
	vec3 Light = u_Color.rgb * GlowA + u_RimColor.rgb * RimA;
	float Cover = clamp(GlowA * 0.55 + RimA * 0.9, 0.0, 1.0) * (1.0 - a);
	vec3 Body = c.rgb;
	if(u_FillMode > 0 && a > 0.001)
	{
		// the pattern over the tee, lit by the skin's own light and dark parts
		vec3 Src = c.rgb / a;
		float Lum = dot(Src, vec3(0.299, 0.587, 0.114));
		vec3 Fill = Pattern(gl_FragCoord.xy / max(u_FillScale, 1.0)) * mix(1.0, clamp(Lum * 1.7, 0.0, 1.5), u_KeepShade);
		Body = mix(Src, Fill, u_FillAlpha) * a;
	}
	FragColor = vec4(Body + Light * (1.0 - a), a + Cover);
}
)";

// ---------------------------------------------------------------------------------------------
// GL helpers

static unsigned CompileShader(GLenum Type, const char *pSource, const char *pName)
{
	const GLuint Shader = glCreateShader(Type);
	glShaderSource(Shader, 1, &pSource, nullptr);
	glCompileShader(Shader);
	GLint Ok = 0;
	glGetShaderiv(Shader, GL_COMPILE_STATUS, &Ok);
	if(!Ok)
	{
		char aLog[1024];
		glGetShaderInfoLog(Shader, sizeof(aLog), nullptr, aLog);
		log_error("xclient/render", "failed to compile shader '%s': %s", pName, aLog);
		glDeleteShader(Shader);
		return 0;
	}
	return Shader;
}

static unsigned LinkProgram(const char *pVertex, const char *pFragment, const char *pName)
{
	const unsigned Vertex = CompileShader(GL_VERTEX_SHADER, pVertex, pName);
	const unsigned Fragment = CompileShader(GL_FRAGMENT_SHADER, pFragment, pName);
	if(!Vertex || !Fragment)
	{
		if(Vertex)
			glDeleteShader(Vertex);
		if(Fragment)
			glDeleteShader(Fragment);
		return 0;
	}
	const GLuint Program = glCreateProgram();
	glAttachShader(Program, Vertex);
	glAttachShader(Program, Fragment);
	glLinkProgram(Program);
	glDeleteShader(Vertex);
	glDeleteShader(Fragment);
	GLint Ok = 0;
	glGetProgramiv(Program, GL_LINK_STATUS, &Ok);
	if(!Ok)
	{
		char aLog[1024];
		glGetProgramInfoLog(Program, sizeof(aLog), nullptr, aLog);
		log_error("xclient/render", "failed to link program '%s': %s", pName, aLog);
		glDeleteProgram(Program);
		return 0;
	}
	return Program;
}

// Everything our passes touch, so DDNet's cached state stays valid.
class CGlStateGuard
{
	GLint m_Program, m_Vao, m_ArrayBuffer, m_ActiveTexture, m_Texture, m_Sampler;
	GLint m_DrawFbo, m_ReadFbo;
	GLint m_aViewport[4], m_aScissorBox[4];
	GLboolean m_Scissor, m_Blend, m_DepthTest, m_CullFace;
	GLint m_BlendSrcRgb, m_BlendDstRgb, m_BlendSrcAlpha, m_BlendDstAlpha, m_BlendEqRgb, m_BlendEqAlpha;
	GLfloat m_aClearColor[4];

public:
	CGlStateGuard()
	{
		glGetIntegerv(GL_CURRENT_PROGRAM, &m_Program);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_Vao);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_ArrayBuffer);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &m_ActiveTexture);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_Texture);
		glGetIntegerv(GL_SAMPLER_BINDING, &m_Sampler);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_DrawFbo);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_ReadFbo);
		glGetIntegerv(GL_VIEWPORT, m_aViewport);
		glGetIntegerv(GL_SCISSOR_BOX, m_aScissorBox);
		m_Scissor = glIsEnabled(GL_SCISSOR_TEST);
		m_Blend = glIsEnabled(GL_BLEND);
		m_DepthTest = glIsEnabled(GL_DEPTH_TEST);
		m_CullFace = glIsEnabled(GL_CULL_FACE);
		glGetIntegerv(GL_BLEND_SRC_RGB, &m_BlendSrcRgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &m_BlendDstRgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_BlendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &m_BlendDstAlpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_BlendEqRgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_BlendEqAlpha);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, m_aClearColor);

		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
	}

	~CGlStateGuard()
	{
		glUseProgram(m_Program);
		glBindVertexArray(m_Vao);
		glBindBuffer(GL_ARRAY_BUFFER, m_ArrayBuffer);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_Texture);
		glBindSampler(0, m_Sampler);
		glActiveTexture(m_ActiveTexture);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_DrawFbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ReadFbo);
		glViewport(m_aViewport[0], m_aViewport[1], m_aViewport[2], m_aViewport[3]);
		glScissor(m_aScissorBox[0], m_aScissorBox[1], m_aScissorBox[2], m_aScissorBox[3]);
		if(m_Scissor)
			glEnable(GL_SCISSOR_TEST);
		else
			glDisable(GL_SCISSOR_TEST);
		if(m_Blend)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);
		if(m_DepthTest)
			glEnable(GL_DEPTH_TEST);
		if(m_CullFace)
			glEnable(GL_CULL_FACE);
		glBlendFuncSeparate(m_BlendSrcRgb, m_BlendDstRgb, m_BlendSrcAlpha, m_BlendDstAlpha);
		glBlendEquationSeparate(m_BlendEqRgb, m_BlendEqAlpha);
		glClearColor(m_aClearColor[0], m_aClearColor[1], m_aClearColor[2], m_aClearColor[3]);
	}

	GLint DrawFbo() const { return m_DrawFbo; }
	const GLint *Viewport() const { return m_aViewport; }
};

// ---------------------------------------------------------------------------------------------

CXcRenderer::CXcRenderer() = default;

CXcRenderer::~CXcRenderer()
{
	// GL objects are released in Shutdown() on the render thread
	s_Available = false;
}

bool CXcRenderer::Init()
{
	m_ShapeProgram = LinkProgram(s_pShapeVertex, s_pShapeFragment, "shape");
	m_DownProgram = LinkProgram(s_pFullscreenVertex, s_pDownFragment, "kawase_down");
	m_UpProgram = LinkProgram(s_pFullscreenVertex, s_pUpFragment, "kawase_up");
	m_CompositeProgram = LinkProgram(s_pFullscreenVertex, s_pCompositeFragment, "composite");
	m_GlowProgram = LinkProgram(s_pFullscreenVertex, s_pGlowFragment, "glow");
	m_BlocksProgram = LinkProgram(s_pBlocksVertex, s_pBlocksFragment, "blocks3d");
	m_CopyProgram = LinkProgram(s_pFullscreenVertex, s_pCopyFragment, "copy");
	if(!m_ShapeProgram || !m_DownProgram || !m_UpProgram || !m_CompositeProgram || !m_GlowProgram || !m_BlocksProgram || !m_CopyProgram)
		return false;
	m_LocBlocksMatrix = glGetUniformLocation(m_BlocksProgram, "u_Matrix");
	m_LocBlocksGrid = glGetUniformLocation(m_BlocksProgram, "u_Grid");
	m_LocBlocksCells = glGetUniformLocation(m_BlocksProgram, "u_Cells");
	m_LocBlocksZRange = glGetUniformLocation(m_BlocksProgram, "u_ZRange");
	m_LocBlocksFogDistance = glGetUniformLocation(m_BlocksProgram, "u_FogDistance");
	m_LocBlocksColor = glGetUniformLocation(m_BlocksProgram, "u_Color");
	m_LocBlocksColor2 = glGetUniformLocation(m_BlocksProgram, "u_Color2");
	m_LocBlocksColor3 = glGetUniformLocation(m_BlocksProgram, "u_Color3");
	m_LocBlocksPass = glGetUniformLocation(m_BlocksProgram, "u_Pass");
	m_LocBlocksFog = glGetUniformLocation(m_BlocksProgram, "u_Fog");
	m_LocCopyTex = glGetUniformLocation(m_CopyProgram, "u_Tex");
	m_LocUseTilt = glGetUniformLocation(m_ShapeProgram, "u_UseTilt");
	m_LocTilt = glGetUniformLocation(m_ShapeProgram, "u_Tilt");
	m_LocGlowCapture = glGetUniformLocation(m_GlowProgram, "u_Capture");
	m_LocGlowBlur = glGetUniformLocation(m_GlowProgram, "u_Blur");
	m_LocGlowColor = glGetUniformLocation(m_GlowProgram, "u_Color");
	m_LocGlowRimColor = glGetUniformLocation(m_GlowProgram, "u_RimColor");
	m_LocGlowStrength = glGetUniformLocation(m_GlowProgram, "u_Strength");
	m_LocGlowRimWidth = glGetUniformLocation(m_GlowProgram, "u_RimWidth");
	m_LocGlowTexel = glGetUniformLocation(m_GlowProgram, "u_Texel");
	m_LocGlowFillMode = glGetUniformLocation(m_GlowProgram, "u_FillMode");
	m_LocGlowFillColor = glGetUniformLocation(m_GlowProgram, "u_FillColor");
	m_LocGlowFillColor2 = glGetUniformLocation(m_GlowProgram, "u_FillColor2");
	m_LocGlowFillAlpha = glGetUniformLocation(m_GlowProgram, "u_FillAlpha");
	m_LocGlowFillScale = glGetUniformLocation(m_GlowProgram, "u_FillScale");
	m_LocGlowFillTime = glGetUniformLocation(m_GlowProgram, "u_FillTime");
	m_LocGlowKeepShade = glGetUniformLocation(m_GlowProgram, "u_KeepShade");

	m_LocScreen = glGetUniformLocation(m_ShapeProgram, "u_Screen");
	m_LocPixel = glGetUniformLocation(m_ShapeProgram, "u_Pixel");
	m_LocBloomPass = glGetUniformLocation(m_ShapeProgram, "u_BloomPass");
	m_LocThreshold = glGetUniformLocation(m_ShapeProgram, "u_Threshold");
	m_LocDownTex = glGetUniformLocation(m_DownProgram, "u_Tex");
	m_LocDownHalfPixel = glGetUniformLocation(m_DownProgram, "u_HalfPixel");
	m_LocUpTex = glGetUniformLocation(m_UpProgram, "u_Tex");
	m_LocUpHalfPixel = glGetUniformLocation(m_UpProgram, "u_HalfPixel");
	m_LocCompTex = glGetUniformLocation(m_CompositeProgram, "u_Tex");
	m_LocCompStrength = glGetUniformLocation(m_CompositeProgram, "u_Strength");
	m_LocCompTint = glGetUniformLocation(m_CompositeProgram, "u_Tint");

	GLint OldVao, OldBuffer;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &OldVao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &OldBuffer);

	// unit quad as a triangle strip, instanced per shape
	static const float s_aCorners[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
	glGenVertexArrays(1, &m_ShapeVao);
	glBindVertexArray(m_ShapeVao);
	glGenBuffers(1, &m_QuadVbo);
	glBindBuffer(GL_ARRAY_BUFFER, m_QuadVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(s_aCorners), s_aCorners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

	glGenBuffers(1, &m_InstanceVbo);
	glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVbo);
	glBufferData(GL_ARRAY_BUFFER, 1024 * sizeof(XcRender::SShapeGpu), nullptr, GL_STREAM_DRAW);
	for(int i = 0; i < 6; i++)
	{
		glEnableVertexAttribArray(1 + i);
		glVertexAttribPointer(1 + i, 4, GL_FLOAT, GL_FALSE, sizeof(XcRender::SShapeGpu), (void *)(uintptr_t)(i * 4 * sizeof(float)));
		glVertexAttribDivisor(1 + i, 1);
	}

	glGenVertexArrays(1, &m_EmptyVao);

	glGenSamplers(1, &m_Sampler);
	glSamplerParameteri(m_Sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(m_Sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(m_Sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(m_Sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindVertexArray(OldVao);
	glBindBuffer(GL_ARRAY_BUFFER, OldBuffer);

	if(glGetError() != GL_NO_ERROR)
	{
		log_error("xclient/render", "OpenGL error while creating buffers");
		return false;
	}
	log_info("xclient/render", "render module ready (OpenGL 3.3, SDF shapes, bloom, blur)");
	return true;
}

void CXcRenderer::Shutdown()
{
	s_Available = false;
	if(!m_Initialized)
		return;
	DestroyTargets();
	glDeleteProgram(m_ShapeProgram);
	glDeleteProgram(m_DownProgram);
	glDeleteProgram(m_UpProgram);
	glDeleteProgram(m_CompositeProgram);
	glDeleteProgram(m_GlowProgram);
	glDeleteProgram(m_BlocksProgram);
	glDeleteProgram(m_CopyProgram);
	if(m_BlocksGrid)
		glDeleteTextures(1, &m_BlocksGrid);
	m_BlocksGrid = 0;
	glDeleteBuffers(1, &m_QuadVbo);
	glDeleteBuffers(1, &m_InstanceVbo);
	glDeleteVertexArrays(1, &m_ShapeVao);
	glDeleteVertexArrays(1, &m_EmptyVao);
	glDeleteSamplers(1, &m_Sampler);
	if(m_pFrameFence)
	{
		glDeleteSync((GLsync)m_pFrameFence);
		m_pFrameFence = nullptr;
	}
	m_Initialized = false;
}

void CXcRenderer::OnPreSwap()
{
	if(m_Capturing)
	{
		// a capture was never ended, never leave DDNet drawing into our texture
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_CaptureSavedDraw);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_CaptureSavedRead);
		m_Capturing = false;
	}
	if(m_Initialized)
	{
		// low latency: do not let the driver queue up frames. Before this frame is shown, wait until the
		// GPU finished the previous one, so input reaches the screen at most one frame later
		if(m_pFrameFence)
		{
			if(g_Config.m_XcLowLatency)
				glClientWaitSync((GLsync)m_pFrameFence, GL_SYNC_FLUSH_COMMANDS_BIT, 50 * 1000 * 1000);
			glDeleteSync((GLsync)m_pFrameFence);
			m_pFrameFence = nullptr;
		}
		if(g_Config.m_XcLowLatency)
			m_pFrameFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}
	if(m_Initialized || m_Failed)
		return;
	// first frame: the context is current on this thread now
	CGlStateGuard Guard;
	m_Initialized = Init();
	m_Failed = !m_Initialized;
	while(glGetError() != GL_NO_ERROR)
	{
	}
	if(m_Failed)
		log_warn("xclient/render", "render module disabled, effects use the regular renderer");
	s_Available = m_Initialized;
}

bool CXcRenderer::RunCommand(const CCommandBuffer::SCommand *pCommand)
{
	if(pCommand->m_Cmd == CCommandBuffer::CMD_SWAP)
	{
		OnPreSwap();
		return false; // swap itself is handled by the SDL fragment
	}
	if(!XcRender::IsXcCommand(pCommand->m_Cmd))
		return false;
	if(!m_Initialized)
		return true;

	switch(pCommand->m_Cmd)
	{
	case XcRender::CMD_XC_DRAW_SHAPES: DrawShapes(static_cast<const XcRender::SCommand_DrawShapes *>(pCommand)); break;
	case XcRender::CMD_XC_BLUR_SCREEN: BlurScreen(static_cast<const XcRender::SCommand_BlurScreen *>(pCommand)); break;
	case XcRender::CMD_XC_CAPTURE_BEGIN: CaptureBegin(static_cast<const XcRender::SCommand_CaptureBegin *>(pCommand)); break;
	case XcRender::CMD_XC_CAPTURE_END: CaptureEnd(static_cast<const XcRender::SCommand_CaptureEnd *>(pCommand)); break;
	case XcRender::CMD_XC_DRAW_BLOCKS3D: DrawBlocks3D(static_cast<const XcRender::SCommand_DrawBlocks3D *>(pCommand)); break;
	}
	return true;
}

static void CreateTarget(int Width, int Height, GLenum InternalFormat, GLenum Type, unsigned &Fbo, unsigned &Texture)
{
	glGenTextures(1, &Texture);
	glBindTexture(GL_TEXTURE_2D, Texture);
	glTexImage2D(GL_TEXTURE_2D, 0, InternalFormat, Width, Height, 0, GL_RGBA, Type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &Fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, Fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, Texture, 0);
}

void CXcRenderer::EnsureTargets(int Width, int Height)
{
	if(Width == m_ViewportW && Height == m_ViewportH && m_aBloom[0].m_Fbo)
		return;
	DestroyTargets();
	m_ViewportW = Width;
	m_ViewportH = Height;
	m_Capture.m_Width = Width;
	m_Capture.m_Height = Height;
	for(int i = 0; i < MAX_LEVELS; i++)
	{
		// bloom starts at half resolution, the blur chain at full resolution
		STarget &Bloom = m_aBloom[i];
		Bloom.m_Width = std::max(Width >> (i + 1), 1);
		Bloom.m_Height = std::max(Height >> (i + 1), 1);
		CreateTarget(Bloom.m_Width, Bloom.m_Height, GL_RGBA16F, GL_HALF_FLOAT, Bloom.m_Fbo, Bloom.m_Texture);

		if(i == 0)
			CreateTarget(Width, Height, GL_RGBA8, GL_UNSIGNED_BYTE, m_Capture.m_Fbo, m_Capture.m_Texture);
		STarget &Blur = m_aBlur[i];
		Blur.m_Width = std::max(Width >> i, 1);
		Blur.m_Height = std::max(Height >> i, 1);
		CreateTarget(Blur.m_Width, Blur.m_Height, GL_RGBA8, GL_UNSIGNED_BYTE, Blur.m_Fbo, Blur.m_Texture);
	}
}

void CXcRenderer::DestroyTargets()
{
	for(STarget *pTargets : {m_aBloom, m_aBlur})
	{
		for(int i = 0; i < MAX_LEVELS; i++)
		{
			if(pTargets[i].m_Fbo)
				glDeleteFramebuffers(1, &pTargets[i].m_Fbo);
			if(pTargets[i].m_Texture)
				glDeleteTextures(1, &pTargets[i].m_Texture);
			pTargets[i] = STarget();
		}
	}
	if(m_Blocks.m_Fbo)
		glDeleteFramebuffers(1, &m_Blocks.m_Fbo);
	if(m_Blocks.m_Texture)
		glDeleteTextures(1, &m_Blocks.m_Texture);
	m_Blocks = STarget();
	if(m_BlocksDepth)
		glDeleteRenderbuffers(1, &m_BlocksDepth);
	m_BlocksDepth = 0;
	if(m_Capture.m_Fbo)
		glDeleteFramebuffers(1, &m_Capture.m_Fbo);
	if(m_Capture.m_Texture)
		glDeleteTextures(1, &m_Capture.m_Texture);
	m_Capture = STarget();
	m_ViewportW = 0;
	m_ViewportH = 0;
}

void CXcRenderer::UploadAndDraw(const XcRender::SShapeGpu *pShapes, int Num)
{
	if(Num <= 0)
		return;
	glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVbo);
	// orphan the old storage so the driver does not have to wait for the previous draw
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)Num * sizeof(XcRender::SShapeGpu), nullptr, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)Num * sizeof(XcRender::SShapeGpu), pShapes);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, Num);
}

void CXcRenderer::DrawFullscreen(unsigned Program, unsigned Texture)
{
	(void)Program;
	glBindTexture(GL_TEXTURE_2D, Texture);
	glBindVertexArray(m_EmptyVao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

void CXcRenderer::ScissorScaled(const int *pRegion, int FullWidth, const STarget &Target)
{
	const float Scale = (float)Target.m_Width / FullWidth;
	const int x = (int)std::floor(pRegion[0] * Scale) - 2;
	const int y = (int)std::floor(pRegion[1] * Scale) - 2;
	const int w = (int)std::ceil(pRegion[2] * Scale) + 4;
	const int h = (int)std::ceil(pRegion[3] * Scale) + 4;
	glScissor(x, y, w, h);
}

void CXcRenderer::BlurChain(STarget *pTargets, int NumLevels, float Offset, int FullWidth, const int *pRegion)
{
	glDisable(GL_BLEND);
	glUseProgram(m_DownProgram);
	glUniform1i(m_LocDownTex, 0);
	for(int i = 1; i < NumLevels; i++)
	{
		const STarget &Src = pTargets[i - 1];
		const STarget &Dst = pTargets[i];
		glBindFramebuffer(GL_FRAMEBUFFER, Dst.m_Fbo);
		glViewport(0, 0, Dst.m_Width, Dst.m_Height);
		if(pRegion)
		{
			// the region plus a small border, so the filter never reads data from older frames
			glEnable(GL_SCISSOR_TEST);
			ScissorScaled(pRegion, FullWidth, Dst);
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		glUniform2f(m_LocDownHalfPixel, 0.5f * Offset / Src.m_Width, 0.5f * Offset / Src.m_Height);
		DrawFullscreen(m_DownProgram, Src.m_Texture);
	}
	glUseProgram(m_UpProgram);
	glUniform1i(m_LocUpTex, 0);
	for(int i = NumLevels - 1; i > 0; i--)
	{
		const STarget &Src = pTargets[i];
		const STarget &Dst = pTargets[i - 1];
		glBindFramebuffer(GL_FRAMEBUFFER, Dst.m_Fbo);
		glViewport(0, 0, Dst.m_Width, Dst.m_Height);
		if(pRegion)
			ScissorScaled(pRegion, FullWidth, Dst);
		glUniform2f(m_LocUpHalfPixel, 0.5f * Offset / Src.m_Width, 0.5f * Offset / Src.m_Height);
		DrawFullscreen(m_UpProgram, Src.m_Texture);
	}
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
}

float CXcRenderer::BlurSpread(int Levels)
{
	// how far the blur reaches in full resolution pixels (dual Kawase, starting at half resolution)
	return 2.5f * (float)(1 << (Levels + 1));
}

bool CXcRenderer::WorldToRegion(const float *pScreen, const float *pWorld, int Width, int Height, float Margin, int *pRegion)
{
	const float ScreenW = pScreen[2] - pScreen[0];
	const float ScreenH = pScreen[3] - pScreen[1];
	if(ScreenW == 0.0f || ScreenH == 0.0f)
		return false;
	const int Left = std::max((int)((pWorld[0] - pScreen[0]) / ScreenW * Width - Margin), 0);
	const int Right = std::min((int)((pWorld[2] - pScreen[0]) / ScreenW * Width + Margin), Width);
	// OpenGL counts rows from the bottom
	const int Bottom = std::max((int)(Height - (pWorld[3] - pScreen[1]) / ScreenH * Height - Margin), 0);
	const int Top = std::min((int)(Height - (pWorld[1] - pScreen[1]) / ScreenH * Height + Margin), Height);
	if(Right <= Left || Top <= Bottom)
		return false;
	pRegion[0] = Left;
	pRegion[1] = Bottom;
	pRegion[2] = Right - Left;
	pRegion[3] = Top - Bottom;
	return true;
}

void CXcRenderer::DrawShapes(const XcRender::SCommand_DrawShapes *pCommand)
{
	if(pCommand->m_NumAlpha <= 0 && pCommand->m_NumAdditive <= 0)
		return;

	CGlStateGuard Guard;
	const GLint *pViewport = Guard.Viewport();
	const int Width = pViewport[2];
	const int Height = pViewport[3];
	if(Width <= 0 || Height <= 0)
		return;

	const float ScreenW = pCommand->m_ScreenRight - pCommand->m_ScreenLeft;
	const float Pixel = std::abs(ScreenW) / Width;

	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glUseProgram(m_ShapeProgram);
	glUniform4f(m_LocScreen, pCommand->m_ScreenLeft, pCommand->m_ScreenTop, pCommand->m_ScreenRight, pCommand->m_ScreenBottom);
	glUniform1f(m_LocPixel, Pixel);
	glUniform1i(m_LocBloomPass, 0);
	glUniform1i(m_LocUseTilt, pCommand->m_Tilt ? 1 : 0);
	if(pCommand->m_Tilt)
		glUniformMatrix4fv(m_LocTilt, 1, GL_FALSE, pCommand->m_aTilt);
	glBindVertexArray(m_ShapeVao);

	// regular alpha blending (dust, weather, solid shapes)
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	UploadAndDraw(pCommand->m_pAlpha, pCommand->m_NumAlpha);

	// additive (sparks, rings, stars, glow)
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
	UploadAndDraw(pCommand->m_pAdditive, pCommand->m_NumAdditive);

	const XcRender::SDrawSettings &Settings = pCommand->m_Settings;
	if(Settings.m_BloomStrength <= 0.0f || pCommand->m_NumAdditive <= 0 || Settings.m_Quality <= 0)
		return;

	// bloom: additive shapes again into the half resolution buffer, blur, add on top
	EnsureTargets(Width, Height);
	const int Levels = Settings.m_Quality >= 2 ? 5 : 3;

	// only work on the part of the screen around the glowing shapes
	float MinX = 1e9f, MinY = 1e9f, MaxX = -1e9f, MaxY = -1e9f;
	for(int i = 0; i < pCommand->m_NumAdditive; i++)
	{
		const XcRender::SShapeGpu &Shape = pCommand->m_pAdditive[i];
		const float RingLine = (int)(Shape.m_Shape + 0.5f) == XcRender::SHAPE_RING ? 0.5f * Shape.m_Param0 : 0.0f;
		const float Extent = (std::max(Shape.m_HalfW, Shape.m_HalfH) + RingLine + Shape.m_GlowRadius + Shape.m_Softness) * 1.42f;
		MinX = std::min(MinX, Shape.m_X - Extent);
		MinY = std::min(MinY, Shape.m_Y - Extent);
		MaxX = std::max(MaxX, Shape.m_X + Extent);
		MaxY = std::max(MaxY, Shape.m_Y + Extent);
	}
	const float aScreen[4] = {pCommand->m_ScreenLeft, pCommand->m_ScreenTop, pCommand->m_ScreenRight, pCommand->m_ScreenBottom};
	const float aWorld[4] = {MinX, MinY, MaxX, MaxY};
	int aRegion[4] = {0, 0, Width, Height}; // 3D view: the world is not a rectangle on the screen any more
	if(!pCommand->m_Tilt && !WorldToRegion(aScreen, aWorld, Width, Height, BlurSpread(Levels), aRegion))
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, m_aBloom[0].m_Fbo);
	glViewport(0, 0, m_aBloom[0].m_Width, m_aBloom[0].m_Height);
	glEnable(GL_SCISSOR_TEST);
	ScissorScaled(aRegion, Width, m_aBloom[0]);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUniform1i(m_LocBloomPass, 1);
	glUniform1f(m_LocThreshold, Settings.m_BloomThreshold);
	glUniform1f(m_LocPixel, Pixel * 2.0f);
	glBlendFunc(GL_ONE, GL_ONE);
	glBindVertexArray(m_ShapeVao);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, pCommand->m_NumAdditive); // instance data is still in the buffer

	glBindSampler(0, m_Sampler);
	BlurChain(m_aBloom, Levels, 1.5f, Width, aRegion);

	glBindFramebuffer(GL_FRAMEBUFFER, Guard.DrawFbo());
	glViewport(pViewport[0], pViewport[1], pViewport[2], pViewport[3]);
	glEnable(GL_SCISSOR_TEST);
	glScissor(pViewport[0] + aRegion[0], pViewport[1] + aRegion[1], aRegion[2], aRegion[3]);
	glUseProgram(m_CompositeProgram);
	glUniform1i(m_LocCompTex, 0);
	glUniform1f(m_LocCompStrength, Settings.m_BloomStrength);
	glUniform4f(m_LocCompTint, 0.0f, 0.0f, 0.0f, 0.0f);
	// add light, keep the destination alpha
	glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
	DrawFullscreen(m_CompositeProgram, m_aBloom[0].m_Texture);
}

void CXcRenderer::BlurScreen(const XcRender::SCommand_BlurScreen *pCommand)
{
	CGlStateGuard Guard;
	const GLint *pViewport = Guard.Viewport();
	const int Width = pViewport[2];
	const int Height = pViewport[3];
	if(Width <= 0 || Height <= 0)
		return;
	EnsureTargets(Width, Height);

	// copy the current frame (resolves multisampling too)
	glBindFramebuffer(GL_READ_FRAMEBUFFER, Guard.DrawFbo());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_aBlur[0].m_Fbo);
	glBlitFramebuffer(pViewport[0], pViewport[1], pViewport[0] + Width, pViewport[1] + Height, 0, 0, Width, Height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, m_Sampler);
	const int Levels = std::clamp(pCommand->m_Settings.m_Passes + 1, 2, (int)MAX_LEVELS);
	BlurChain(m_aBlur, Levels, pCommand->m_Settings.m_Offset, Width);

	glBindFramebuffer(GL_FRAMEBUFFER, Guard.DrawFbo());
	glViewport(pViewport[0], pViewport[1], pViewport[2], pViewport[3]);
	glDisable(GL_BLEND);
	glUseProgram(m_CompositeProgram);
	glUniform1i(m_LocCompTex, 0);
	glUniform1f(m_LocCompStrength, 1.0f);
	const ColorRGBA &Tint = pCommand->m_Settings.m_Tint;
	glUniform4f(m_LocCompTint, Tint.r, Tint.g, Tint.b, Tint.a);
	const XcRender::SBlurSettings &Settings = pCommand->m_Settings;
	if(Settings.m_NumRects <= 0)
	{
		DrawFullscreen(m_CompositeProgram, m_aBlur[0].m_Texture);
		return;
	}
	// only behind some panels (the glass chat): the same blurred frame, cut by a scissor per rect
	glEnable(GL_SCISSOR_TEST);
	for(int i = 0; i < std::min(Settings.m_NumRects, (int)XcRender::SBlurSettings::MAX_RECTS); i++)
	{
		const float *pRect = Settings.m_aaRects[i];
		const int x0 = (int)std::floor(pRect[0] * Width), x1 = (int)std::ceil(pRect[2] * Width);
		const int y0 = (int)std::floor((1.0f - pRect[3]) * Height), y1 = (int)std::ceil((1.0f - pRect[1]) * Height);
		if(x1 <= x0 || y1 <= y0)
			continue;
		glScissor(pViewport[0] + x0, pViewport[1] + y0, x1 - x0, y1 - y0);
		DrawFullscreen(m_CompositeProgram, m_aBlur[0].m_Texture);
	}
}

void CXcRenderer::CaptureBegin(const XcRender::SCommand_CaptureBegin *pCommand)
{
	if(m_Capturing)
		return;
	GLint aViewport[4];
	glGetIntegerv(GL_VIEWPORT, aViewport);
	const int Width = aViewport[2];
	const int Height = aViewport[3];
	if(Width <= 0 || Height <= 0)
		return;
	const float aScreen[4] = {pCommand->m_ScreenLeft, pCommand->m_ScreenTop, pCommand->m_ScreenRight, pCommand->m_ScreenBottom};
	// the world rect already has room for the glow, a few pixels more for the filter taps
	if(pCommand->m_Tilt)
	{
		// 3D view: the world rect is no rectangle on the screen, take all of it
		m_aCaptureRegion[0] = m_aCaptureRegion[1] = 0;
		m_aCaptureRegion[2] = Width;
		m_aCaptureRegion[3] = Height;
	}
	else if(!WorldToRegion(aScreen, pCommand->m_aWorldRect, Width, Height, 8.0f, m_aCaptureRegion))
		return;

	// targets may be created here, keep DDNet's bindings intact while doing so
	{
		CGlStateGuard Guard;
		EnsureTargets(Width, Height);
	}

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_CaptureSavedDraw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_CaptureSavedRead);
	GLint aScissorBox[4];
	glGetIntegerv(GL_SCISSOR_BOX, aScissorBox);
	const GLboolean Scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLfloat aClearColor[4];
	glGetFloatv(GL_COLOR_CLEAR_VALUE, aClearColor);

	// from now on DDNet draws into our texture
	glBindFramebuffer(GL_FRAMEBUFFER, m_Capture.m_Fbo);
	glEnable(GL_SCISSOR_TEST);
	glScissor(m_aCaptureRegion[0], m_aCaptureRegion[1], m_aCaptureRegion[2], m_aCaptureRegion[3]);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glClearColor(aClearColor[0], aClearColor[1], aClearColor[2], aClearColor[3]);
	glScissor(aScissorBox[0], aScissorBox[1], aScissorBox[2], aScissorBox[3]);
	if(!Scissor)
		glDisable(GL_SCISSOR_TEST);
	m_Capturing = true;
}

void CXcRenderer::CaptureEnd(const XcRender::SCommand_CaptureEnd *pCommand)
{
	if(!m_Capturing)
		return;
	m_Capturing = false;

	GLint ActiveTexture, Unit1Texture, Unit1Sampler;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &ActiveTexture);
	glActiveTexture(GL_TEXTURE1);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &Unit1Texture);
	glGetIntegerv(GL_SAMPLER_BINDING, &Unit1Sampler);
	glActiveTexture(ActiveTexture);

	{
		CGlStateGuard Guard;
		const GLint *pViewport = Guard.Viewport();
		const int Width = pViewport[2];
		const int Height = pViewport[3];
		const XcRender::SGlowSettings &Settings = pCommand->m_Settings;

		// glow: silhouette down to half resolution, then blurred as wide as asked
		// (only the pattern: no blur needed, the glow strength is 0)
		glActiveTexture(GL_TEXTURE0);
		glBindSampler(0, m_Sampler);
		if(Settings.m_DoGlow)
		{
			const int Levels = std::clamp((int)std::round(std::log2(std::max(Settings.m_Radius, 5.0f) / 2.5f)) - 1, 1, (int)MAX_GLOW_LEVELS);
			glBindFramebuffer(GL_FRAMEBUFFER, m_aBloom[0].m_Fbo);
			glViewport(0, 0, m_aBloom[0].m_Width, m_aBloom[0].m_Height);
			glEnable(GL_SCISSOR_TEST);
			ScissorScaled(m_aCaptureRegion, Width, m_aBloom[0]);
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glDisable(GL_BLEND);
			glUseProgram(m_DownProgram);
			glUniform1i(m_LocDownTex, 0);
			glUniform2f(m_LocDownHalfPixel, 0.5f / Width, 0.5f / Height);
			DrawFullscreen(m_DownProgram, m_Capture.m_Texture);
			BlurChain(m_aBloom, Levels + 1, 1.5f, Width, m_aCaptureRegion);
		}

		// back to DDNet's framebuffer: glow and outline, then the sprites on top
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_CaptureSavedDraw);
		glViewport(pViewport[0], pViewport[1], pViewport[2], pViewport[3]);
		glEnable(GL_SCISSOR_TEST);
		glScissor(pViewport[0] + m_aCaptureRegion[0], pViewport[1] + m_aCaptureRegion[1], m_aCaptureRegion[2], m_aCaptureRegion[3]);
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glUseProgram(m_GlowProgram);
		glUniform1i(m_LocGlowCapture, 0);
		glUniform1i(m_LocGlowBlur, 1);
		glUniform4f(m_LocGlowColor, Settings.m_Color.r, Settings.m_Color.g, Settings.m_Color.b, Settings.m_Color.a);
		glUniform4f(m_LocGlowRimColor, Settings.m_RimColor.r, Settings.m_RimColor.g, Settings.m_RimColor.b, Settings.m_RimColor.a);
		glUniform1f(m_LocGlowStrength, Settings.m_Strength);
		glUniform1f(m_LocGlowRimWidth, Settings.m_RimWidth);
		glUniform2f(m_LocGlowTexel, 1.0f / Width, 1.0f / Height);
		if(!Settings.m_DoGlow)
		{
			glUniform1f(m_LocGlowStrength, 0.0f);
			glUniform1f(m_LocGlowRimWidth, 0.0f);
		}
		glUniform1i(m_LocGlowFillMode, Settings.m_FillMode);
		glUniform4f(m_LocGlowFillColor, Settings.m_FillColor.r, Settings.m_FillColor.g, Settings.m_FillColor.b, Settings.m_FillColor.a);
		glUniform4f(m_LocGlowFillColor2, Settings.m_FillColor2.r, Settings.m_FillColor2.g, Settings.m_FillColor2.b, Settings.m_FillColor2.a);
		glUniform1f(m_LocGlowFillAlpha, Settings.m_FillAlpha);
		glUniform1f(m_LocGlowFillScale, Settings.m_FillScale);
		glUniform1f(m_LocGlowFillTime, Settings.m_FillTime);
		glUniform1f(m_LocGlowKeepShade, Settings.m_KeepShade);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_aBloom[0].m_Texture);
		glBindSampler(1, m_Sampler);
		glActiveTexture(GL_TEXTURE0);
		DrawFullscreen(m_GlowProgram, m_Capture.m_Texture);
	}

	// the guard restored our capture framebuffer, hand DDNet its own back
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_CaptureSavedDraw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_CaptureSavedRead);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, Unit1Texture);
	glBindSampler(1, Unit1Sampler);
	glActiveTexture(ActiveTexture);
}

// 3D view: the sides of the blocks go into their own target with a depth buffer, then over the frame
void CXcRenderer::DrawBlocks3D(const XcRender::SCommand_DrawBlocks3D *pCommand)
{
	CGlStateGuard Guard;
	const GLint *pViewport = Guard.Viewport();
	const int Width = pViewport[2];
	const int Height = pViewport[3];
	if(Width <= 0 || Height <= 0 || pCommand->m_W <= 0 || pCommand->m_H <= 0)
		return;
	EnsureTargets(Width, Height);
	if(!m_Blocks.m_Fbo)
	{
		CreateTarget(Width, Height, GL_RGBA8, GL_UNSIGNED_BYTE, m_Blocks.m_Fbo, m_Blocks.m_Texture);
		m_Blocks.m_Width = Width;
		m_Blocks.m_Height = Height;
		glGenRenderbuffers(1, &m_BlocksDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_BlocksDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, Width, Height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_BlocksDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	// the cells as an integer texture
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	if(!m_BlocksGrid)
		glGenTextures(1, &m_BlocksGrid);
	glBindTexture(GL_TEXTURE_2D, m_BlocksGrid);
	// the backend keeps an alignment of 1 for its uploads (the glyphs of the text), put back what was there
	GLint UnpackAlignment;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &UnpackAlignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if(pCommand->m_W > m_BlocksGridW || pCommand->m_H > m_BlocksGridH)
	{
		m_BlocksGridW = std::max(pCommand->m_W, m_BlocksGridW);
		m_BlocksGridH = std::max(pCommand->m_H, m_BlocksGridH);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, m_BlocksGridW, m_BlocksGridH, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pCommand->m_W, pCommand->m_H, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, pCommand->m_pGrid);
	glPixelStorei(GL_UNPACK_ALIGNMENT, UnpackAlignment);

	glBindFramebuffer(GL_FRAMEBUFFER, m_Blocks.m_Fbo);
	glViewport(0, 0, Width, Height);
	// first person: the sky behind the blocks covers the frame
	if(pCommand->m_aSky[3] > 0.0f)
		glClearColor(pCommand->m_aSky[0], pCommand->m_aSky[1], pCommand->m_aSky[2], pCommand->m_aSky[3]);
	else
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0);
	GLboolean DepthMask;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &DepthMask);
	glDepthMask(GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);
	glUseProgram(m_BlocksProgram);
	glUniformMatrix4fv(m_LocBlocksMatrix, 1, GL_FALSE, pCommand->m_aMatrix);
	glUniform1i(m_LocBlocksGrid, 0);
	glUniform4i(m_LocBlocksCells, pCommand->m_X, pCommand->m_Y, pCommand->m_W, pCommand->m_H);
	glUniform2f(m_LocBlocksZRange, pCommand->m_ZFrom, pCommand->m_ZTo);
	glUniform1f(m_LocBlocksFogDistance, pCommand->m_FogDistance);
	glUniform4fv(m_LocBlocksColor, 1, pCommand->m_aColor);
	glUniform4fv(m_LocBlocksColor2, 1, pCommand->m_aColor2);
	glUniform4fv(m_LocBlocksColor3, 1, pCommand->m_aColor3);
	glUniform4fv(m_LocBlocksFog, 1, pCommand->m_aFog);
	glBindVertexArray(m_EmptyVao);
	glUniform1i(m_LocBlocksPass, 0);
	glDrawArraysInstanced(GL_TRIANGLES, 0, 24, pCommand->m_W * pCommand->m_H);
	// the freeze over the blocks behind it, see-through: blended, without writing the depth
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glUniform1i(m_LocBlocksPass, 1);
	glDrawArraysInstanced(GL_TRIANGLES, 0, 24, pCommand->m_W * pCommand->m_H);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(DepthMask);

	// over the frame (premultiplied), the frame keeps its alpha
	glBindFramebuffer(GL_FRAMEBUFFER, Guard.DrawFbo());
	glViewport(pViewport[0], pViewport[1], pViewport[2], pViewport[3]);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
	glUseProgram(m_CopyProgram);
	glUniform1i(m_LocCopyTex, 0);
	glBindSampler(0, m_Sampler);
	DrawFullscreen(m_CopyProgram, m_Blocks.m_Texture);
}

#endif

