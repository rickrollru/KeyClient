#include "cape.h"

#include "fx_renderer.h"
#include "sprite_fx.h"
#include "xc_helpers.h"

#include <base/math.h>

#include <engine/client.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cmath>

using namespace XcHelpers;

static constexpr float STEP = 1.0f / 120.0f;

static ColorRGBA Shaded(const ColorRGBA &Color, float Shade, float Alpha)
{
	return ColorRGBA(std::clamp(Color.r * Shade, 0.0f, 1.0f), std::clamp(Color.g * Shade, 0.0f, 1.0f), std::clamp(Color.b * Shade, 0.0f, 1.0f), Alpha);
}

static ColorRGBA ConfigColor(unsigned Value)
{
	return color_cast<ColorRGBA>(ColorHSLA(Value));
}

// a thick line as one quad, inside an open QuadsBegin
static void DrawSegment(IGraphics *pGraphics, vec2 a, vec2 b, float Width, const ColorRGBA &Color, float Overlap = 0.35f)
{
	const vec2 Dir = b - a;
	const float Len = length(Dir);
	if(Len < 0.001f)
		return;
	const vec2 Side = vec2(-Dir.y, Dir.x) / Len * (Width * 0.5f);
	const vec2 Along = Dir / Len * (Width * Overlap); // overlaps the next segment so corners stay closed
	pGraphics->SetColor(Color);
	const IGraphics::CFreeformItem Item(a - Along + Side, b + Along + Side, a - Along - Side, b + Along - Side);
	pGraphics->QuadsDrawFreeform(&Item, 1);
}

// a filled circle as a fan of freeform quads, inside an open QuadsBegin
static void DrawDisc(IGraphics *pGraphics, vec2 Center, float Radius, const ColorRGBA &Color, int Segments = 10)
{
	pGraphics->SetColor(Color);
	for(int i = 0; i < Segments; i += 2)
	{
		const float a0 = 2.0f * pi * i / Segments, a1 = 2.0f * pi * (i + 1) / Segments, a2 = 2.0f * pi * (i + 2) / Segments;
		const IGraphics::CFreeformItem Item(Center, Center + direction(a0) * Radius, Center + direction(a1) * Radius, Center + direction(a2) * Radius);
		pGraphics->QuadsDrawFreeform(&Item, 1);
	}
}

CXcCape::CXcCape() :
	m_Pictures("xclient/capes", 512, 256, false)
{
	m_Front.m_pCape = this;
	for(vec2 &Pos : m_aPreviewPos)
		Pos = vec2(-1e6f, -1e6f);
}

void CXcCape::OnInit()
{
	m_Pictures.Init(Graphics(), Storage(), Kernel()->RequestInterface<IEngine>());
}

void CXcCape::OnShutdown()
{
	m_Pictures.Shutdown();
}

void CXcCape::OnReset()
{
	for(SState &State : m_aStates)
	{
		State.m_Cloth.m_Valid = false;
		for(STail &Tail : State.m_aTails)
			Tail.m_Valid = false;
		State.m_HasLast = false;
		State.m_DrawFront = false;
	}
}

bool CXcCape::LooksLeft(int ClientId) const
{
	// the player you control looks where the mouse is, the others where the server says
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK && ClientId == GameClient()->m_aLocalIds[g_Config.m_ClDummy])
		return GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy].x < 0.0f;
	const float Angle = GameClient()->m_aClients[ClientId].m_RenderCur.m_Angle / 256.0f;
	return std::cos(Angle) < 0.0f;
}

void CXcCape::UpdateMotion(SState &State, vec2 Pos, float Dt, bool Jumped) const
{
	const float React = g_Config.m_XcCapeReact / 100.0f;
	vec2 Vel = State.m_Vel;
	if(State.m_HasLast && Dt > 0.0f && distance(Pos, State.m_LastPos) < 200.0f)
		Vel = (Pos - State.m_LastPos) / Dt;
	// a sharp kick upwards is a jump: the cloth snaps down and out
	if(!Jumped && State.m_HasLast && Vel.y < -350.0f && Vel.y - State.m_Vel.y < -600.0f)
		Jumped = true;
	if(Jumped && React > 0.0f)
	{
		for(int r = 1; r < ROWS; r++)
			State.m_Cloth.m_aSpinePrev[r] -= vec2(State.m_Back * 1.5f, 4.0f) * ((float)r / (ROWS - 1)) * React;
		for(STail &Tail : State.m_aTails)
			for(int i = 1; i < TAIL_POINTS; i++)
				Tail.m_aPrev[i] -= vec2(State.m_Back * 1.0f, 3.0f) * ((float)i / (TAIL_POINTS - 1)) * React;
	}
	State.m_Vel += (Vel - State.m_Vel) * SmoothFactor(12.0f, Dt);
	State.m_LastPos = Pos;
	State.m_HasLast = true;
}

float CXcCape::ColumnLength(int Col) const
{
	const float u = 2.0f * Col / (COLS - 1) - 1.0f;
	switch(g_Config.m_XcCapeShape)
	{
	case SHAPE_POINTED: return 1.0f - 0.32f * std::abs(u);
	case SHAPE_RAGGED:
	{
		static const float s_aLengths[COLS] = {0.9f, 1.04f, 0.84f, 1.0f, 0.88f};
		return s_aLengths[Col];
	}
	default: return 1.0f;
	}
}

void CXcCape::Collide(vec2 &Point, vec2 &Prev) const
{
	// lies on the ground and slides along walls: a point inside a block moves out over the nearest edge
	if(!Collision()->CheckPoint(Point))
		return;
	const float Left = std::floor(Point.x / 32.0f) * 32.0f - 0.5f;
	const float Top = std::floor(Point.y / 32.0f) * 32.0f - 0.5f;
	const vec2 aCandidates[] = {vec2(Point.x, Top), vec2(Point.x, Top + 33.0f), vec2(Left, Point.y), vec2(Left + 33.0f, Point.y)};
	float Best = 1e9f;
	vec2 Out = Point;
	for(const vec2 &Candidate : aCandidates)
		if(!Collision()->CheckPoint(Candidate) && distance(Candidate, Point) < Best)
		{
			Best = distance(Candidate, Point);
			Out = Candidate;
		}
	Point = Out;
	Prev = mix(Prev, Point, 0.5f); // friction on the ground
}

void CXcCape::SimulateCloth(SState &State, vec2 Pos, float Scale, float Dt, float Time, bool Collide) const
{
	// The middle line of the cape is a chain (verlet). The cloth is built around it with the width always
	// across the line, so in this side view the cape never folds into a thin edge; ripples run over it.
	SCloth &Cloth = State.m_Cloth;
	const bool Royal = g_Config.m_XcCapeShape == SHAPE_ROYAL;
	const float Width = g_Config.m_XcCapeWidth / 100.0f;
	const float Length = (Royal ? 62.0f : 52.0f) * Scale * g_Config.m_XcCapeLength / 100.0f;
	const float Segment = Length / (ROWS - 1);
	const float TopWidth = (Royal ? 20.0f : 14.0f) * Width * Scale;
	const float BottomWidth = (Royal ? 38.0f : 30.0f) * Width * Scale;
	const float Wind = g_Config.m_XcCapeWind / 100.0f;
	const float React = g_Config.m_XcCapeReact / 100.0f;
	const float Speed = std::min(length(State.m_Vel) / 900.0f, 1.5f) * React;
	// the shoulders: a bit behind the middle of the body, a little above it
	const vec2 Neck = Pos + vec2(State.m_Back * 11.0f, -8.0f) * Scale;
	vec2 *pSpine = Cloth.m_aSpine;
	vec2 *pPrev = Cloth.m_aSpinePrev;

	if(!Cloth.m_Valid || distance(pSpine[0], Neck) > 300.0f)
	{
		// starts hanging behind the tee (also after a teleport)
		for(int r = 0; r < ROWS; r++)
		{
			pSpine[r] = Neck + normalize(vec2(State.m_Back * 0.5f, 1.0f)) * Segment * r;
			pPrev[r] = pSpine[r];
		}
		Cloth.m_Valid = true;
		Cloth.m_Accumulator = 0.0f;
	}
	Cloth.m_Accumulator = std::min(Cloth.m_Accumulator + Dt, 0.1f);
	const vec2 Start = pSpine[0];
	const int NumSteps = (int)(Cloth.m_Accumulator / STEP);
	int Steps = 0;
	while(Cloth.m_Accumulator >= STEP)
	{
		Cloth.m_Accumulator -= STEP;
		Steps++;
		// the shoulders follow the tee smoothly between the frames
		pSpine[0] = mix(Start, Neck, (float)Steps / std::max(NumSteps, 1));
		pPrev[0] = pSpine[0];
		for(int r = 1; r < ROWS; r++)
		{
			const float t = (float)r / (ROWS - 1);
			vec2 Force(0.0f, 1500.0f * Scale);
			// the cloth likes to fly a bit backwards and flutters, more at the free end and at speed
			Force.x += State.m_Back * 1100.0f * Scale;
			const float Flutter = Wind * t * Scale * (1.0f + Speed * 1.5f);
			Force.x += std::sin(Time * 2.3f + r * 0.7f) * 260.0f * Flutter;
			Force.y += std::sin(Time * 3.1f + r * 1.1f) * 180.0f * Flutter;
			// the hook pulls the tee, the cape streams away from it
			Force -= State.m_Pull * 1600.0f * React * t * Scale;
			const vec2 Velocity = (pSpine[r] - pPrev[r]) * 0.97f;
			pPrev[r] = pSpine[r];
			pSpine[r] += Velocity + Force * STEP * STEP;
		}
		for(int Iteration = 0; Iteration < 4; Iteration++)
		{
			for(int r = 1; r < ROWS; r++)
			{
				const vec2 Delta = pSpine[r] - pSpine[r - 1];
				const float Dist = length(Delta);
				if(Dist <= 0.0001f)
					continue;
				const vec2 Fix = Delta * ((Dist - Segment) / Dist);
				if(r == 1)
					pSpine[r] -= Fix; // the shoulders do not move
				else
				{
					pSpine[r] -= Fix * 0.5f;
					pSpine[r - 1] += Fix * 0.5f;
				}
			}
		}
		if(Collide)
		{
			for(int r = 1; r < ROWS; r++)
				this->Collide(pSpine[r], pPrev[r]);
			// cloth does not stretch: a point stuck behind a corner pulls no further than a bit over its length
			for(int r = 1; r < ROWS; r++)
			{
				const vec2 Delta = pSpine[r] - pSpine[r - 1];
				const float Dist = length(Delta);
				if(Dist > Segment * 1.15f)
					pSpine[r] = pSpine[r - 1] + Delta * (Segment * 1.15f / Dist);
			}
		}
	}

	// the cloth around the line: across it, as wide as the tee faces away (turning folds it over), with ripples
	auto SpineAt = [&](float s, vec2 *pDir) {
		const float f = std::clamp(s, 0.0f, 1.0f) * (ROWS - 1);
		const int i = std::min((int)f, ROWS - 2);
		const float u = f - i;
		*pDir = normalize(pSpine[i + 1] - pSpine[i]);
		return mix(pSpine[i], pSpine[i + 1], u);
	};
	for(int r = 0; r < ROWS; r++)
	{
		const float t = (float)r / (ROWS - 1);
		for(int c = 0; c < COLS; c++)
		{
			const float u = 2.0f * c / (COLS - 1) - 1.0f;
			const float s = t * ColumnLength(c);
			vec2 Dir;
			const vec2 Center = SpineAt(s, &Dir);
			const vec2 Normal(-Dir.y, Dir.x);
			const float Half = mix(TopWidth, BottomWidth, s) * 0.5f;
			const float Ripple = std::sin(Time * 5.0f + r * 0.9f + c * 1.7f) * (0.6f + Speed) * Scale * s * Wind;
			Cloth.m_aPoints[r][c] = Center + Normal * (u * Half * State.m_Back) + Dir * Ripple + Normal * Ripple * 0.5f;
		}
	}
}

float CXcCape::WaveAt(float t, float Time)
{
	// a band of light slides from the shoulders to the free end every two seconds
	if(!g_Config.m_XcCapeWave)
		return 0.0f;
	const float Phase = std::fmod(Time * 0.55f, 1.5f) - 0.25f;
	const float d = t - Phase;
	return std::exp(-d * d / 0.012f);
}

void CXcCape::GlowLine(const vec2 *pPoints, int Num, const ColorRGBA *pColors, float Width, float Strength, bool Preview) const
{
	if(Num < 2)
		return;
	const float Reach = FxGlow::g_Current.m_Radius;
	const float Bright = FxGlow::g_Current.m_Strength * Strength;
	auto Hot = [](const ColorRGBA &c, float Amount) { return ColorRGBA(mix(c.r, 1.0f, Amount), mix(c.g, 1.0f, Amount), mix(c.b, 1.0f, Amount), c.a); };
	CXcSprites &Sprites = GameClient()->m_XcSprites;
	if(Sprites.Enabled())
	{
		// soft light: glow sprites at even steps along the line and a thin hot core
		if(Bright > 0.0f)
		{
			Sprites.Begin(CXcSprites::SPRITE_GLOW, true);
			const float Step = std::max(Width * 1.2f, 2.0f);
			float Carry = 0.0f;
			for(int i = 0; i + 1 < Num; i++)
			{
				const float Segment = distance(pPoints[i], pPoints[i + 1]);
				float d = Carry;
				for(; d < Segment; d += Step)
				{
					const float u = d / std::max(Segment, 0.001f);
					const ColorRGBA &a = pColors[i], &b = pColors[i + 1];
					const ColorRGBA c(mix(a.r, b.r, u), mix(a.g, b.g, u), mix(a.b, b.b, u), mix(a.a, b.a, u));
					Sprites.Draw(mix(pPoints[i], pPoints[i + 1], u), Width * 6.0f * Reach, 0.0f, c.WithMultipliedAlpha(0.16f * std::min(Bright, 2.0f)));
				}
				Carry = d - Segment;
			}
			Sprites.End();
		}
		Graphics()->TextureClear();
		Graphics()->BlendAdditive();
		Graphics()->QuadsBegin();
		for(int i = 0; i + 1 < Num; i++)
			DrawSegment(Graphics(), pPoints[i], pPoints[i + 1], Width, Hot(pColors[i], 0.45f).WithMultipliedAlpha(0.9f), 0.0f); // added light: overlaps would be bright dots
		Graphics()->QuadsEnd();
		Graphics()->BlendNormal();
		return;
	}
	// classic look: shapes of the render module with their glow and bloom
	std::vector<SFxShape> vShapes;
	for(int i = 0; i + 1 < Num; i++)
		CFxRenderer::AppendGlowing(vShapes, FxShape::Line(pPoints[i], pPoints[i + 1], Width, Hot(pColors[i], 0.35f)), Width * 3.0f, 0.6f * Bright);
	GameClient()->m_FxRenderer.Draw(vShapes, !Preview);
}

void CXcCape::RenderStrip(const vec2 *pCenter, const float *pHalf, int Num, const ColorRGBA *pSegColors, const float *pWave, int Look, float Scale, float Alpha, bool Preview) const
{
	// a band along the points (scarf ends and the wrap), segment colors already hold the stripes
	std::vector<vec2> vLeft(Num), vRight(Num);
	for(int i = 0; i < Num; i++)
	{
		const vec2 Prev = pCenter[i > 0 ? i - 1 : i], Next = pCenter[i + 1 < Num ? i + 1 : i];
		const vec2 Dir = normalize(Next - Prev);
		const vec2 Normal(-Dir.y, Dir.x);
		vLeft[i] = pCenter[i] + Normal * pHalf[i];
		vRight[i] = pCenter[i] - Normal * pHalf[i];
	}
	auto Quad = [&](int i, float From, float To, const ColorRGBA &a0, const ColorRGBA &a1, const ColorRGBA &b0, const ColorRGBA &b1) {
		// From/To: -1 left edge .. +1 right edge
		const vec2 p0 = mix(vRight[i], vLeft[i], (From + 1.0f) * 0.5f), p1 = mix(vRight[i], vLeft[i], (To + 1.0f) * 0.5f);
		const vec2 q0 = mix(vRight[i + 1], vLeft[i + 1], (From + 1.0f) * 0.5f), q1 = mix(vRight[i + 1], vLeft[i + 1], (To + 1.0f) * 0.5f);
		Graphics()->SetColor4(a0, b0, a1, b1);
		const IGraphics::CFreeformItem Item(p0, q0, p1, q1);
		Graphics()->QuadsDrawFreeform(&Item, 1);
	};
	Graphics()->TextureClear();
	if(Look == LOOK_CLOTH)
	{
		Graphics()->QuadsBegin();
		for(int i = 0; i + 1 < Num; i++)
		{
			// dark border, then the knitted body: every row a bit lighter or darker
			const ColorRGBA Border = Shaded(pSegColors[i], 0.3f, Alpha);
			const float Grow = 1.3f * Scale / std::max(pHalf[i], 0.1f);
			Quad(i, -1.0f - Grow, 1.0f + Grow, Border, Border, Border, Border);
		}
		for(int i = 0; i + 1 < Num; i++)
		{
			const float Rib = (i % 2 ? 0.88f : 1.0f);
			const float t = (float)i / std::max(Num - 2, 1);
			const ColorRGBA Light = Shaded(pSegColors[i], Rib * (1.0f - 0.15f * t) * (1.0f + 0.5f * pWave[i]), Alpha);
			const ColorRGBA Dark = Shaded(pSegColors[i], Rib * 0.8f * (1.0f - 0.15f * t), Alpha);
			Quad(i, -1.0f, 1.0f, Dark, Light, Dark, Light);
		}
		Graphics()->QuadsEnd();
	}
	else
	{
		if(Look == LOOK_NEON)
		{
			// dark cloth under the light
			Graphics()->QuadsBegin();
			for(int i = 0; i + 1 < Num; i++)
			{
				const ColorRGBA Dark = Shaded(pSegColors[i], 0.16f, Alpha * 0.9f);
				Quad(i, -1.0f, 1.0f, Dark, Dark, Dark, Dark);
			}
			Graphics()->QuadsEnd();
		}
		// light: brighter at the edges in the energy look (alpha blended, keeps its color on bright maps),
		// a soft inner glow lit up by the wave on the dark neon cloth
		if(Look == LOOK_NEON)
			Graphics()->BlendAdditive();
		Graphics()->QuadsBegin();
		for(int i = 0; i + 1 < Num; i++)
		{
			const float Base = Look == LOOK_ENERGY ? 0.32f : 0.08f;
			const float a = Alpha * (Base + 0.55f * pWave[i]), b = Alpha * (Base + 0.55f * pWave[i + 1]);
			const float Edge = Look == LOOK_ENERGY ? 1.6f : 1.0f;
			const ColorRGBA &c = pSegColors[i];
			Quad(i, -1.0f, 0.0f, c.WithAlpha(a * Edge), c.WithAlpha(a), c.WithAlpha(b * Edge), c.WithAlpha(b));
			Quad(i, 0.0f, 1.0f, c.WithAlpha(a), c.WithAlpha(a * Edge), c.WithAlpha(b), c.WithAlpha(b * Edge));
		}
		Graphics()->QuadsEnd();
		Graphics()->BlendNormal();
	}
	// glowing edges along both sides
	std::vector<ColorRGBA> vColors(Num);
	for(int i = 0; i < Num; i++)
		vColors[i] = pSegColors[std::min(i, Num - 2)].WithAlpha(Alpha * (0.7f + 0.3f * pWave[i]));
	const float Strength = Look == LOOK_CLOTH ? 0.3f : (Look == LOOK_NEON ? 1.0f : 0.7f);
	const float Width = (Look == LOOK_CLOTH ? 1.0f : 1.6f) * Scale;
	GlowLine(vLeft.data(), Num, vColors.data(), Width, Strength, Preview);
	GlowLine(vRight.data(), Num, vColors.data(), Width, Strength, Preview);
}

void CXcCape::RenderCloth(const SState &State, float Scale, int ClientId, float Alpha, float Time, bool Preview)
{
	const FxGlow::CScope GlowScope(FxGlow::CAPE);
	const SCloth &Cloth = State.m_Cloth;
	const int Look = g_Config.m_XcCapeLook;
	const int Shape = g_Config.m_XcCapeShape;
	const int Pattern = g_Config.m_XcCapePattern;
	const float Opacity = Alpha * g_Config.m_XcCapeAlpha / 100.0f;
	const bool Ragged = Shape == SHAPE_RAGGED;
	const bool Picture = Pattern == PATTERN_PICTURE && m_Pictures.HasFrames();
	const float Width = g_Config.m_XcCapeWidth / 100.0f;
	const bool Royal = Shape == SHAPE_ROYAL;
	const float TopWidth = (Royal ? 20.0f : 14.0f) * Width * Scale;
	const float BottomWidth = (Royal ? 38.0f : 30.0f) * Width * Scale;
	const float TimeOffset = Time + (ClientId < 0 ? 0.0f : ClientId * 0.37f);

	// colors and the light wave along the length
	ColorRGBA aOuter[ROWS], aInner[ROWS], aLight[ROWS];
	float aWave[ROWS];
	const ColorRGBA OwnInner = ConfigColor(g_Config.m_XcCapeInnerColor);
	for(int r = 0; r < ROWS; r++)
	{
		const float t = (float)r / (ROWS - 1);
		const ColorRGBA Main = EffectColor(GameClient(), g_Config.m_XcCapeColorMode, g_Config.m_XcCapeColor, ClientId, t * 0.35f, 1.0f);
		const ColorRGBA Inner = g_Config.m_XcCapeInnerMode ? OwnInner : Shaded(Main, 0.45f, 1.0f);
		aLight[r] = Main;
		aOuter[r] = Main;
		aInner[r] = Inner;
		if(Pattern == PATTERN_GRADIENT)
			aOuter[r] = ColorRGBA(mix(Main.r, Inner.r, t * 0.85f), mix(Main.g, Inner.g, t * 0.85f), mix(Main.b, Inner.b, t * 0.85f), 1.0f);
		if(Picture)
			aOuter[r] = aInner[r] = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
		aWave[r] = WaveAt(t, TimeOffset);
	}

	// folds: where the cloth is squeezed sideways it gets darker, stretched parts catch the light
	float aShade[ROWS][COLS];
	for(int r = 0; r < ROWS; r++)
	{
		const float t = (float)r / (ROWS - 1);
		const float Rest = mix(TopWidth, BottomWidth, t) / (COLS - 1);
		for(int c = 0; c < COLS; c++)
		{
			float Stretch = 0.0f;
			int Count = 0;
			if(c > 0)
			{
				Stretch += distance(Cloth.m_aPoints[r][c], Cloth.m_aPoints[r][c - 1]);
				Count++;
			}
			if(c + 1 < COLS)
			{
				Stretch += distance(Cloth.m_aPoints[r][c], Cloth.m_aPoints[r][c + 1]);
				Count++;
			}
			const float Ratio = std::clamp(Stretch / std::max(Count * Rest, 0.001f), 0.0f, 1.2f);
			// the side towards the tee is a little darker
			const float Side = (2.0f * c / (COLS - 1) - 1.0f) * State.m_Back;
			aShade[r][c] = (0.62f + 0.38f * std::min(Ratio, 1.0f)) * (1.0f - 0.22f * t) * (0.92f + 0.08f * Side);
		}
	}

	// the corners of a cell; ragged capes end in teeth
	auto Corners = [&](int r, int c, vec2 *pOut) {
		pOut[0] = Cloth.m_aPoints[r][c];
		pOut[1] = Cloth.m_aPoints[r][c + 1];
		pOut[2] = Cloth.m_aPoints[r + 1][c];
		pOut[3] = Cloth.m_aPoints[r + 1][c + 1];
		if(Ragged && r + 2 == ROWS)
		{
			const vec2 Tip = (pOut[2] + pOut[3]) * 0.5f + ((pOut[2] + pOut[3]) * 0.5f - (pOut[0] + pOut[1]) * 0.5f) * 0.3f;
			pOut[2] = pOut[3] = Tip;
		}
	};
	// the edge of the cloth as one line: down the left side, along the bottom, up the right side
	std::vector<vec2> vEdge;
	std::vector<float> vEdgeT;
	for(int r = 0; r < ROWS; r++)
	{
		vEdge.push_back(Cloth.m_aPoints[r][0]);
		vEdgeT.push_back((float)r / (ROWS - 1));
	}
	for(int c = 0; c + 1 < COLS; c++)
	{
		vec2 aCell[4];
		Corners(ROWS - 2, c, aCell);
		if(Ragged)
		{
			vEdge.push_back(aCell[2]);
			vEdgeT.push_back(1.05f);
		}
		vEdge.push_back(Cloth.m_aPoints[ROWS - 1][c + 1]);
		vEdgeT.push_back(1.0f);
	}
	for(int r = ROWS - 2; r >= 0; r--)
	{
		vEdge.push_back(Cloth.m_aPoints[r][COLS - 1]);
		vEdgeT.push_back((float)r / (ROWS - 1));
	}
	if(Ragged)
	{
		// the teeth go down from the cell corners: rebuild the bottom of the line through the tips
		vEdge.clear();
		vEdgeT.clear();
		for(int r = 0; r + 1 < ROWS; r++)
		{
			vEdge.push_back(Cloth.m_aPoints[r][0]);
			vEdgeT.push_back((float)r / (ROWS - 1));
		}
		for(int c = 0; c + 1 < COLS; c++)
		{
			vec2 aCell[4];
			Corners(ROWS - 2, c, aCell);
			vEdge.push_back(aCell[2]);
			vEdgeT.push_back(1.0f);
			vEdge.push_back(Cloth.m_aPoints[ROWS - 2][c + 1]);
			vEdgeT.push_back((float)(ROWS - 2) / (ROWS - 1));
		}
		for(int r = ROWS - 3; r >= 0; r--)
		{
			vEdge.push_back(Cloth.m_aPoints[r][COLS - 1]);
			vEdgeT.push_back((float)r / (ROWS - 1));
		}
	}

	if(Look == LOOK_CLOTH)
	{
		// a thin dark border first, the cloth over it
		const ColorRGBA Dark = Shaded(aOuter[ROWS / 2], Picture ? 0.25f : 0.32f, Opacity);
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		for(size_t i = 0; i + 1 < vEdge.size(); i++)
			DrawSegment(Graphics(), vEdge[i], vEdge[i + 1], 2.8f * Scale, Dark);
		Graphics()->QuadsEnd();
	}

	// the cloth; the light body is alpha blended so it keeps its color on bright maps, the glow adds light over it
	const bool Additive = false;
	if(Picture)
		Graphics()->TextureSet(m_Pictures.CurrentFrame());
	else
		Graphics()->TextureClear();
	if(Additive)
		Graphics()->BlendAdditive();
	Graphics()->QuadsBegin();
	for(int r = 0; r + 1 < ROWS; r++)
	{
		const bool Stripe = Pattern == PATTERN_STRIPES && (r / 2) % 2 == 1;
		for(int c = 0; c + 1 < COLS; c++)
		{
			vec2 aCell[4];
			Corners(r, c, aCell);
			// while the tee turns around the cape folds over and shows its inner side
			const bool Inside = std::abs(State.m_Back) < 0.35f;
			const ColorRGBA *pColors = Inside || Stripe ? aInner : aOuter;
			ColorRGBA aCorner[4];
			for(int k = 0; k < 4; k++)
			{
				const int rr = r + k / 2, cc = c + k % 2;
				float Shade = aShade[rr][cc];
				float CornerAlpha = Opacity;
				if(Look == LOOK_CLOTH)
					Shade *= (Inside && Picture ? 0.45f : 1.0f) * (1.0f + 0.45f * aWave[rr]);
				else if(Look == LOOK_NEON)
				{
					Shade *= Picture ? 0.35f : 0.18f;
					CornerAlpha *= 0.9f;
				}
				else
				{
					// energy: thin light in the middle, bright at the edges and where the wave runs
					const bool Edge = cc == 0 || cc == COLS - 1 || rr == ROWS - 1;
					Shade = 1.0f;
					CornerAlpha *= (Inside ? 0.24f : 0.34f) + (Edge ? 0.3f : 0.0f) + 0.6f * aWave[rr];
					CornerAlpha *= 1.0f - 0.3f * (float)rr / (ROWS - 1);
				}
				aCorner[k] = Shaded(pColors[rr], Shade, CornerAlpha);
			}
			Graphics()->SetColor4(aCorner[0], aCorner[1], aCorner[2], aCorner[3]);
			if(Picture)
			{
				const float u0 = (float)c / (COLS - 1), u1 = (float)(c + 1) / (COLS - 1);
				const float v0 = (float)r / (ROWS - 1), v1 = (float)(r + 1) / (ROWS - 1);
				const float Mid = (u0 + u1) * 0.5f;
				if(Ragged && r + 2 == ROWS)
					Graphics()->QuadsSetSubsetFree(u0, v0, u1, v0, Mid, 1.0f, Mid, 1.0f);
				else
					Graphics()->QuadsSetSubsetFree(u0, v0, u1, v0, u0, v1, u1, v1);
			}
			const IGraphics::CFreeformItem Item(aCell[0], aCell[1], aCell[2], aCell[3]);
			Graphics()->QuadsDrawFreeform(&Item, 1);
		}
	}
	Graphics()->QuadsEnd();
	if(Additive)
		Graphics()->BlendNormal();

	if(Look == LOOK_NEON)
	{
		// the wave lights the dark cloth from inside
		Graphics()->TextureClear();
		Graphics()->BlendAdditive();
		Graphics()->QuadsBegin();
		for(int r = 0; r + 1 < ROWS; r++)
			for(int c = 0; c + 1 < COLS; c++)
			{
				vec2 aCell[4];
				Corners(r, c, aCell);
				const ColorRGBA Top = aLight[r].WithAlpha(Opacity * (0.06f + 0.45f * aWave[r]));
				const ColorRGBA Bottom = aLight[r + 1].WithAlpha(Opacity * (0.06f + 0.45f * aWave[r + 1]));
				Graphics()->SetColor4(Top, Top, Bottom, Bottom);
				const IGraphics::CFreeformItem Item(aCell[0], aCell[1], aCell[2], aCell[3]);
				Graphics()->QuadsDrawFreeform(&Item, 1);
			}
		Graphics()->QuadsEnd();
		Graphics()->BlendNormal();
	}

	if(Look == LOOK_ENERGY && GameClient()->m_XcSprites.Enabled() && FxGlow::g_Current.m_Strength > 0.0f)
	{
		// soft light filling the cloth
		CXcSprites &Sprites = GameClient()->m_XcSprites;
		Sprites.Begin(CXcSprites::SPRITE_GLOW, true);
		for(int r = 1; r < ROWS; r += 2)
			for(int c = 0; c < COLS; c++)
				Sprites.Draw(Cloth.m_aPoints[r][c], BottomWidth * 0.55f * FxGlow::g_Current.m_Radius, 0.0f, aLight[r].WithAlpha(Opacity * (0.05f + 0.12f * aWave[r]) * std::min(FxGlow::g_Current.m_Strength, 2.0f)));
		Sprites.End();
	}

	// the glowing edge, in the trim color when the cape has an edge
	const int Trim = g_Config.m_XcCapeTrim;
	std::vector<ColorRGBA> vEdgeColors(vEdge.size());
	const ColorRGBA TrimColor = ConfigColor(g_Config.m_XcCapeTrimColor);
	for(size_t i = 0; i < vEdge.size(); i++)
	{
		const float t = std::min(vEdgeT[i], 1.0f);
		const int r = std::clamp((int)std::round(t * (ROWS - 1)), 0, ROWS - 1);
		const ColorRGBA Base = Trim == 1 ? TrimColor : aLight[r];
		vEdgeColors[i] = Base.WithAlpha(Opacity * (0.75f + 0.25f * aWave[r]));
	}
	const float EdgeStrength = Look == LOOK_CLOTH ? (Trim == 1 ? 0.5f : 0.25f) : (Look == LOOK_NEON ? 1.0f : 0.8f);
	const float EdgeWidth = (Look == LOOK_CLOTH ? (Trim == 1 ? 2.0f : 1.0f) : 1.8f) * Scale;
	GlowLine(vEdge.data(), (int)vEdge.size(), vEdgeColors.data(), EdgeWidth, EdgeStrength, Preview);

	// fur: along the bottom and the collar of the royal cape
	if(Trim != 2 && !Royal)
		return;
	const ColorRGBA Fur(0.96f, 0.94f, 0.9f, Opacity);
	const ColorRGBA FurShade(0.78f, 0.76f, 0.72f, Opacity);
	const ColorRGBA Spot(0.08f, 0.08f, 0.1f, Opacity);
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	if(Trim == 2)
	{
		for(int c = 0; c < COLS; c++)
		{
			const vec2 p = Cloth.m_aPoints[ROWS - 1][c];
			DrawDisc(Graphics(), p, 3.6f * Scale, FurShade);
			DrawDisc(Graphics(), p - vec2(0.0f, 0.6f) * Scale, 3.0f * Scale, Fur);
			if(c + 1 < COLS)
			{
				const vec2 m = (p + Cloth.m_aPoints[ROWS - 1][c + 1]) * 0.5f;
				DrawDisc(Graphics(), m, 3.2f * Scale, Fur);
				if(c % 2 == 0)
					DrawDisc(Graphics(), m + vec2(0.0f, 0.6f) * Scale, 0.9f * Scale, Spot, 6);
			}
		}
	}
	// the collar: a roll of fur over the shoulders
	for(int c = 0; c < COLS; c++)
	{
		const vec2 p = mix(Cloth.m_aPoints[0][c], Cloth.m_aPoints[1][c], 0.3f);
		DrawDisc(Graphics(), p, (Royal ? 5.2f : 3.8f) * Scale, FurShade);
		DrawDisc(Graphics(), p - vec2(0.0f, 0.8f) * Scale, (Royal ? 4.4f : 3.2f) * Scale, Fur);
		if(Royal && c % 2 == 1)
			DrawDisc(Graphics(), p + vec2(0.0f, 1.0f) * Scale, 1.1f * Scale, Spot, 6);
	}
	Graphics()->QuadsEnd();
}

void CXcCape::EmitSparks(SState &State, float Scale, int ClientId, float Dt)
{
	// sparks fly off the free edge, more at speed
	const float Speed = length(State.m_Vel);
	const float Rate = 28.0f * std::clamp((Speed - 250.0f) / 600.0f, 0.0f, 1.5f) * g_Config.m_XcCapeReact / 100.0f;
	if(Rate <= 0.0f)
	{
		State.m_SparkCarry = 0.0f;
		return;
	}
	State.m_SparkCarry += Rate * Dt;
	while(State.m_SparkCarry >= 1.0f)
	{
		State.m_SparkCarry -= 1.0f;
		vec2 From;
		ColorRGBA Color;
		const bool Cape = State.m_Cloth.m_Valid && (!State.m_aTails[0].m_Valid || random_float() < 0.6f);
		if(Cape)
		{
			const int c = (int)(random_float() * COLS) % COLS;
			const int r = ROWS - 1 - (int)(random_float() * 3.0f);
			From = State.m_Cloth.m_aPoints[r][c];
			Color = EffectColor(GameClient(), g_Config.m_XcCapeColorMode, g_Config.m_XcCapeColor, ClientId, 0.3f, 1.0f);
		}
		else if(State.m_aTails[0].m_Valid)
		{
			const STail &Tail = State.m_aTails[random_float() < 0.5f || !State.m_aTails[1].m_Valid ? 0 : 1];
			From = Tail.m_aPoints[TAIL_POINTS - 1 - (int)(random_float() * 3.0f)];
			Color = ScarfColor(ClientId, 1.0f);
		}
		else
			return;
		CXcParticles::SParticle Spark;
		Spark.m_Type = CXcParticles::EType::SPARK;
		Spark.m_Pos = From;
		Spark.m_Vel = -State.m_Vel * 0.15f + direction(random_float() * 2.0f * pi) * (40.0f + random_float() * 60.0f);
		Spark.m_Gravity = 300.0f;
		Spark.m_Friction = 0.3f;
		Spark.m_LifeSpan = 0.35f + random_float() * 0.35f;
		Spark.m_StartSize = (5.0f + random_float() * 4.0f) * Scale;
		Spark.m_EndSize = 0.0f;
		Spark.m_Color = ColorRGBA(mix(Color.r, 1.0f, 0.4f), mix(Color.g, 1.0f, 0.4f), mix(Color.b, 1.0f, 0.4f), 1.0f);
		m_Particles.Add(Spark);
	}
}

vec2 CXcCape::KnotPos(vec2 Pos, float Scale, float Back) const
{
	return Pos + vec2(Back * 17.0f, 8.0f) * Scale;
}

ColorRGBA CXcCape::ScarfColor(int ClientId, float t) const
{
	return EffectColor(GameClient(), g_Config.m_XcScarfColorMode, g_Config.m_XcScarfColor, ClientId, t * 0.3f, 1.0f);
}

void CXcCape::SimulateTail(STail &Tail, const SState &State, int Index, vec2 Anchor, float Scale, float Dt, float Time, bool Collide) const
{
	const float Length = (Index == 0 ? 40.0f : 32.0f) * Scale * g_Config.m_XcScarfLength / 100.0f;
	const float Segment = Length / (TAIL_POINTS - 1);
	const float Wind = g_Config.m_XcScarfWind / 100.0f;
	const float React = g_Config.m_XcCapeReact / 100.0f;
	const float Speed = std::min(length(State.m_Vel) / 900.0f, 1.5f) * React;
	const float Phase = Index * 1.9f;
	if(!Tail.m_Valid || distance(Tail.m_aPoints[0], Anchor) > 300.0f)
	{
		for(int i = 0; i < TAIL_POINTS; i++)
		{
			Tail.m_aPoints[i] = Anchor + normalize(vec2(State.m_Back * (0.6f + Index * 0.3f), 1.0f)) * Segment * i;
			Tail.m_aPrev[i] = Tail.m_aPoints[i];
		}
		Tail.m_Valid = true;
		Tail.m_Accumulator = 0.0f;
	}
	Tail.m_Accumulator = std::min(Tail.m_Accumulator + Dt, 0.1f);
	const vec2 Start = Tail.m_aPoints[0];
	const int NumSteps = (int)(Tail.m_Accumulator / STEP);
	int Steps = 0;
	while(Tail.m_Accumulator >= STEP)
	{
		Tail.m_Accumulator -= STEP;
		Steps++;
		Tail.m_aPoints[0] = mix(Start, Anchor, (float)Steps / std::max(NumSteps, 1));
		Tail.m_aPrev[0] = Tail.m_aPoints[0];
		for(int i = 1; i < TAIL_POINTS; i++)
		{
			const float t = (float)i / (TAIL_POINTS - 1);
			vec2 Force(0.0f, 1500.0f * Scale);
			Force.x += State.m_Back * (900.0f + Index * 250.0f) * Scale;
			const float Flutter = Wind * t * Scale * (1.0f + Speed * 1.5f);
			Force.x += std::sin(Time * 2.6f + i * 0.8f + Phase) * 280.0f * Flutter;
			Force.y += std::sin(Time * 3.4f + i * 1.2f + Phase) * 200.0f * Flutter;
			Force -= State.m_Pull * 1400.0f * React * t * Scale;
			const vec2 Velocity = (Tail.m_aPoints[i] - Tail.m_aPrev[i]) * 0.97f;
			Tail.m_aPrev[i] = Tail.m_aPoints[i];
			Tail.m_aPoints[i] += Velocity + Force * STEP * STEP;
		}
		for(int Iteration = 0; Iteration < 4; Iteration++)
		{
			for(int i = 1; i < TAIL_POINTS; i++)
			{
				const vec2 Delta = Tail.m_aPoints[i] - Tail.m_aPoints[i - 1];
				const float Dist = length(Delta);
				if(Dist <= 0.0001f)
					continue;
				const vec2 Fix = Delta * ((Dist - Segment) / Dist);
				if(i == 1)
					Tail.m_aPoints[i] -= Fix;
				else
				{
					Tail.m_aPoints[i] -= Fix * 0.5f;
					Tail.m_aPoints[i - 1] += Fix * 0.5f;
				}
			}
		}
		if(Collide)
		{
			for(int i = 1; i < TAIL_POINTS; i++)
				this->Collide(Tail.m_aPoints[i], Tail.m_aPrev[i]);
			for(int i = 1; i < TAIL_POINTS; i++)
			{
				const vec2 Delta = Tail.m_aPoints[i] - Tail.m_aPoints[i - 1];
				const float Dist = length(Delta);
				if(Dist > Segment * 1.15f)
					Tail.m_aPoints[i] = Tail.m_aPoints[i - 1] + Delta * (Segment * 1.15f / Dist);
			}
		}
	}
}

void CXcCape::RenderTails(const SState &State, float Scale, int ClientId, float Alpha, float Time, bool Preview) const
{
	const FxGlow::CScope GlowScope(FxGlow::CAPE);
	const ColorRGBA StripeColor = ConfigColor(g_Config.m_XcScarfStripeColor);
	const float TimeOffset = Time + (ClientId < 0 ? 0.0f : ClientId * 0.37f);
	// the second end hangs behind the first one
	for(int Index = g_Config.m_XcScarfTails - 1; Index >= 0; Index--)
	{
		const STail &Tail = State.m_aTails[Index];
		if(!Tail.m_Valid)
			continue;
		const float Behind = Index == 1 ? 0.8f : 1.0f;
		float aHalf[TAIL_POINTS], aWave[TAIL_POINTS];
		ColorRGBA aColors[TAIL_POINTS];
		for(int i = 0; i < TAIL_POINTS; i++)
		{
			const float t = (float)i / (TAIL_POINTS - 1);
			aHalf[i] = 4.6f * Scale * (1.0f - 0.12f * t);
			// the wave goes down the scarf a bit after the wrap
			aWave[i] = WaveAt(0.2f + t * 0.8f, TimeOffset + Index * 0.15f);
			const bool Stripe = g_Config.m_XcScarfStripes && i % 3 == 1;
			aColors[i] = Shaded(Stripe ? StripeColor : ScarfColor(ClientId, t), Behind, 1.0f);
		}
		RenderStrip(Tail.m_aPoints, aHalf, TAIL_POINTS, aColors, aWave, g_Config.m_XcScarfLook, Scale, Alpha, Preview);
		if(g_Config.m_XcScarfFringe)
		{
			// fringe: short threads at the end, glowing in the light looks
			const vec2 End = Tail.m_aPoints[TAIL_POINTS - 1];
			const vec2 Dir = normalize(End - Tail.m_aPoints[TAIL_POINTS - 2]);
			const vec2 Side(-Dir.y, Dir.x);
			const bool Light = g_Config.m_XcScarfLook != LOOK_CLOTH;
			const ColorRGBA Thread = Light ? ScarfColor(ClientId, 1.0f).WithAlpha(Alpha * 0.8f) : Shaded(ScarfColor(ClientId, 1.0f), 0.8f * Behind, Alpha);
			Graphics()->TextureClear();
			if(Light)
				Graphics()->BlendAdditive();
			Graphics()->QuadsBegin();
			for(int k = 0; k < 4; k++)
			{
				const float u = (k + 0.5f) / 4.0f;
				const vec2 Base = End + Side * (u * 2.0f - 1.0f) * aHalf[TAIL_POINTS - 1];
				const vec2 Tip = Base + Dir * 5.0f * Scale + Side * std::sin(u * 7.0f) * 0.8f * Scale;
				DrawSegment(Graphics(), Base, Tip, 1.4f * Scale, Thread);
			}
			Graphics()->QuadsEnd();
			Graphics()->BlendNormal();
		}
	}
}

void CXcCape::RenderWrap(vec2 Pos, float Scale, float Back, int ClientId, float Alpha, float Time, bool Preview) const
{
	// the band around the neck, bent down in the middle as it wraps around the round body
	const FxGlow::CScope GlowScope(FxGlow::CAPE);
	static constexpr int NUM = 11;
	const ColorRGBA StripeColor = ConfigColor(g_Config.m_XcScarfStripeColor);
	const int Look = g_Config.m_XcScarfLook;
	const float TimeOffset = Time + (ClientId < 0 ? 0.0f : ClientId * 0.37f);
	vec2 aCenter[NUM];
	float aHalf[NUM], aWave[NUM];
	ColorRGBA aColors[NUM];
	for(int i = 0; i < NUM; i++)
	{
		const float u = 2.0f * i / (NUM - 1) - 1.0f;
		aCenter[i] = Pos + vec2(u * 21.0f, 6.0f + 4.0f * (1.0f - u * u)) * Scale;
		aHalf[i] = 4.4f * Scale;
		// the light runs around the neck towards the knot
		aWave[i] = WaveAt(0.2f * (u * Back * 0.5f + 0.5f), TimeOffset);
		const bool Stripe = g_Config.m_XcScarfStripes && (i == 3 || i == 6);
		// the ends of the band go around the body, so they are darker
		const float Round = 1.0f - 0.3f * std::abs(u);
		aColors[i] = Shaded(Stripe ? StripeColor : ScarfColor(ClientId, (float)i / (NUM - 1)), Round, 1.0f);
	}
	RenderStrip(aCenter, aHalf, NUM, aColors, aWave, Look, Scale, Alpha, Preview);

	// the knot at the back where the ends come out
	const vec2 Knot = KnotPos(Pos, Scale, Back);
	const ColorRGBA KnotColor = ScarfColor(ClientId, 0.0f);
	if(Look == LOOK_CLOTH)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		DrawDisc(Graphics(), Knot, 6.0f * Scale, Shaded(KnotColor, 0.3f, Alpha), 12);
		DrawDisc(Graphics(), Knot, 4.8f * Scale, Shaded(KnotColor, 0.85f, Alpha), 12);
		DrawDisc(Graphics(), Knot - vec2(0.8f, 1.0f) * Scale, 2.4f * Scale, Shaded(KnotColor, 1.1f, Alpha), 8);
		Graphics()->QuadsEnd();
		return;
	}
	// a small glowing orb
	if(Look == LOOK_NEON)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		DrawDisc(Graphics(), Knot, 5.4f * Scale, Shaded(KnotColor, 0.16f, Alpha), 12);
		Graphics()->QuadsEnd();
	}
	std::vector<SFxShape> vShapes;
	const ColorRGBA Hot(mix(KnotColor.r, 1.0f, 0.4f), mix(KnotColor.g, 1.0f, 0.4f), mix(KnotColor.b, 1.0f, 0.4f), Alpha);
	if(Look == LOOK_NEON)
		CFxRenderer::AppendGlowing(vShapes, FxShape::Ring(Knot, vec2(5.0f, 5.0f) * Scale, 1.6f * Scale, Hot), 5.0f * Scale * FxGlow::g_Current.m_Radius, 0.7f * FxGlow::g_Current.m_Strength);
	else
		CFxRenderer::AppendGlowing(vShapes, FxShape::Circle(Knot, 4.6f * Scale, Hot.WithAlpha(Alpha * 0.8f)), 6.0f * Scale * FxGlow::g_Current.m_Radius, 0.7f * FxGlow::g_Current.m_Strength);
	GameClient()->m_FxRenderer.Draw(vShapes, !Preview);
}

void CXcCape::OnRender()
{
	// the scarf used to be the second style of the cape
	if(g_Config.m_XcCape == 2)
	{
		g_Config.m_XcCape = 0;
		g_Config.m_XcScarf = 1;
	}
	const bool Online = Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK;
	const float Dt = std::min(Client()->RenderFrameTime(), 0.1f);
	m_Pictures.Update(g_Config.m_XcCape && g_Config.m_XcCapePattern == PATTERN_PICTURE ? g_Config.m_XcCapeFile : "", Dt, 1.0f);
	if((!g_Config.m_XcCape && !g_Config.m_XcScarf) || !Online)
	{
		OnReset();
		m_Particles.Clear();
		return;
	}
	const float Time = Client()->GlobalTime();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		SState &State = m_aStates[ClientId];
		State.m_DrawFront = false;
		const bool Cape = g_Config.m_XcCape && ShowFor(GameClient(), ClientId, g_Config.m_XcCapeSelf, g_Config.m_XcCapeDummy, g_Config.m_XcCapeOthers);
		const bool Scarf = g_Config.m_XcScarf && ShowFor(GameClient(), ClientId, g_Config.m_XcScarfSelf, g_Config.m_XcScarfDummy, g_Config.m_XcScarfOthers);
		const float Alpha = GameClient()->m_Snap.m_aCharacters[ClientId].m_Active ? PlayerAlpha(GameClient(), ClientId) : 0.0f;
		if(!Cape)
			State.m_Cloth.m_Valid = false;
		if(!Scarf)
			for(STail &Tail : State.m_aTails)
				Tail.m_Valid = false;
		if((!Cape && !Scarf) || Alpha <= 0.0f)
		{
			State.m_Cloth.m_Valid = false;
			for(STail &Tail : State.m_aTails)
				Tail.m_Valid = false;
			State.m_HasLast = false;
			continue;
		}
		const CGameClient::CClientData &Data = GameClient()->m_aClients[ClientId];
		const float Scale = Data.m_RenderInfo.m_Size / 64.0f;
		const vec2 Pos = Data.m_RenderPos;
		// turning around swings the cloth over to the other side instead of jumping
		const float BackTarget = LooksLeft(ClientId) ? 1.0f : -1.0f;
		State.m_Back += (BackTarget - State.m_Back) * SmoothFactor(10.0f, Dt);
		State.m_Pull = vec2(0.0f, 0.0f);
		if(Data.m_RenderCur.m_HookState == HOOK_GRABBED)
		{
			const vec2 Hook((float)Data.m_RenderCur.m_HookX, (float)Data.m_RenderCur.m_HookY);
			if(distance(Hook, Pos) > 1.0f)
				State.m_Pull = normalize(Hook - Pos);
		}
		UpdateMotion(State, Pos, Dt, false);
		if(Cape)
		{
			SimulateCloth(State, Pos, Scale, Dt, Time + ClientId * 1.7f, true);
			RenderCloth(State, Scale, ClientId, Alpha, Time, false);
		}
		if(Scarf)
		{
			for(int i = 0; i < g_Config.m_XcScarfTails; i++)
				SimulateTail(State.m_aTails[i], State, i, KnotPos(Pos, Scale, State.m_Back) + vec2(State.m_Back * 1.5f * i, 1.5f * i) * Scale, Scale, Dt, Time + ClientId * 1.7f, true);
			if(g_Config.m_XcScarfTails < MAX_TAILS)
				State.m_aTails[1].m_Valid = false;
			RenderTails(State, Scale, ClientId, Alpha, Time, false);
			State.m_DrawFront = true;
			State.m_FrontPos = Pos;
			State.m_FrontScale = Scale;
			State.m_FrontAlpha = Alpha;
		}
		if(g_Config.m_XcCapeSparks)
			EmitSparks(State, Scale, ClientId, Dt);
	}
	m_Particles.Update(Dt);
	m_Particles.Render(GameClient()->m_FxRenderer);
}

void CXcCape::RenderFront()
{
	if(!g_Config.m_XcScarf)
		return;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const SState &State = m_aStates[ClientId];
		if(State.m_DrawFront)
			RenderWrap(State.m_FrontPos, State.m_FrontScale, State.m_Back, ClientId, State.m_FrontAlpha, Client()->GlobalTime(), false);
	}
}

void CXcCapeFront::OnRender()
{
	if(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK)
		m_pCape->RenderFront();
}

CXcCape::SState &CXcCape::PreviewState(vec2 Pos, float Scale)
{
	// the same preview stays near the same place on the screen; a new place takes the slot used longest ago
	const float Now = Client()->GlobalTime();
	int Best = -1;
	for(int i = 0; i < NUM_PREVIEWS; i++)
		if(Now - m_aPreviewTime[i] < 0.5f && distance(m_aPreviewPos[i], Pos) < 40.0f * Scale)
			Best = i;
	if(Best < 0)
	{
		Best = 0;
		for(int i = 1; i < NUM_PREVIEWS; i++)
			if(m_aPreviewTime[i] < m_aPreviewTime[Best])
				Best = i;
		m_aPreviews[Best] = SState();
	}
	m_aPreviewPos[Best] = Pos;
	m_aPreviewTime[Best] = Now;
	return m_aPreviews[Best];
}

void CXcCape::RenderPreview(vec2 Pos, float TeeSize, bool LookingLeft, float Alpha, float Time)
{
	if(!g_Config.m_XcCape && !g_Config.m_XcScarf)
		return;
	const float Dt = std::min(Client()->RenderFrameTime(), 0.1f);
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		m_Pictures.Update(g_Config.m_XcCape && g_Config.m_XcCapePattern == PATTERN_PICTURE ? g_Config.m_XcCapeFile : "", Dt, 1.0f);
	const float Scale = TeeSize / 64.0f;
	SState &State = PreviewState(Pos, Scale);
	const float BackTarget = LookingLeft ? 1.0f : -1.0f;
	State.m_Back += (BackTarget - State.m_Back) * SmoothFactor(10.0f, Dt);
	State.m_Pull = vec2(0.0f, 0.0f);
	if(g_Config.m_XcCape)
	{
		SimulateCloth(State, Pos, Scale, Dt, Time, false);
		RenderCloth(State, Scale, -1, Alpha, Time, true);
	}
	if(g_Config.m_XcScarf)
	{
		for(int i = 0; i < g_Config.m_XcScarfTails; i++)
			SimulateTail(State.m_aTails[i], State, i, KnotPos(Pos, Scale, State.m_Back) + vec2(State.m_Back * 1.5f * i, 1.5f * i) * Scale, Scale, Dt, Time, false);
		RenderTails(State, Scale, -1, Alpha, Time, true);
	}
}

void CXcCape::RenderPreviewFront(vec2 Pos, float TeeSize, float Alpha)
{
	if(!g_Config.m_XcScarf)
		return;
	const float Scale = TeeSize / 64.0f;
	const SState &State = PreviewState(Pos, Scale);
	RenderWrap(Pos, Scale, State.m_Back, -1, Alpha, Client()->GlobalTime(), true);
}
