#include "impact_fx.h"

#include "fx_renderer.h"
#include "xc_helpers.h"

#include <base/math.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/gameclient.h>
#include <game/collision.h>

#include <cmath>

using namespace XcHelpers;

void CXcImpactFx::OnReset()
{
	m_Shots.clear();
	m_Lasers.clear();
	m_Particles.Clear();
}

bool CXcImpactFx::ShowFor(int Owner) const
{
	return g_Config.m_XcImpactFx != IMPACT_OFF && ShowTargets(Owner);
}

// the self / dummy / others choice of the card, also for the flash at the gun (it does not need the hits)
bool CXcImpactFx::ShowTargets(int Owner) const
{
	// shots of the map (turrets) count as others
	if(!in_range(Owner, MAX_CLIENTS - 1))
		return g_Config.m_XcImpactOthers;
	return XcHelpers::ShowFor(GameClient(), Owner, g_Config.m_XcImpactSelf, g_Config.m_XcImpactDummy, g_Config.m_XcImpactOthers);
}

ColorRGBA CXcImpactFx::Color(int Weapon, int Owner) const
{
	if(!g_Config.m_XcImpactWeaponColor)
		return EffectColor(GameClient(), g_Config.m_XcImpactColorMode, g_Config.m_XcImpactColor, Owner, random_float());
	switch(Weapon)
	{
	case WEAPON_GUN: return ColorRGBA(1.0f, 0.85f, 0.35f, 1.0f);
	case WEAPON_SHOTGUN: return ColorRGBA(1.0f, 0.6f, 0.25f, 1.0f);
	case WEAPON_LASER: return ColorRGBA(0.45f, 0.75f, 1.0f, 1.0f);
	default: return ColorRGBA(1.0f, 0.75f, 0.4f, 1.0f);
	}
}

int CXcImpactFx::TeeOnLine(vec2 From, vec2 To, int Owner, vec2 *pHit) const
{
	// the server does not tell about hits on tees in DDRace, so a shot that ends at a tee hit it
	int Best = -1;
	float BestDist = 1e9f;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == Owner || !GameClient()->m_Snap.m_aCharacters[i].m_Active)
			continue;
		const vec2 Tee = GameClient()->m_aClients[i].m_RenderPos;
		vec2 Closest;
		if(!closest_point_on_line(From, To, Tee, Closest) || distance(Closest, Tee) > 30.0f)
			continue;
		const float Dist = distance(From, Closest);
		if(Dist < BestDist)
		{
			BestDist = Dist;
			Best = i;
			*pHit = Tee + normalize(Closest - Tee + vec2(0.001f, 0.0f)) * 16.0f;
		}
	}
	return Best;
}

vec2 CXcImpactFx::WallNormal(vec2 Pos, vec2 Before) const
{
	// the side of the hit block the shot came from
	const vec2 Center((std::floor(Pos.x / 32.0f) + 0.5f) * 32.0f, (std::floor(Pos.y / 32.0f) + 0.5f) * 32.0f);
	const vec2 Away = Before - Center;
	if(std::abs(Away.x) > std::abs(Away.y))
		return vec2(Away.x > 0.0f ? 1.0f : -1.0f, 0.0f);
	return vec2(0.0f, Away.y > 0.0f ? 1.0f : -1.0f);
}

void CXcImpactFx::Muzzle(vec2 Pos, vec2 Dir, int Weapon, int Owner)
{
	const FxTune::CScope TuneScope(FxTune::IMPACT);
	const ColorRGBA Main = Color(Weapon, Owner);
	CXcParticles::SParticle Flash;
	Flash.m_Type = CXcParticles::EType::FLASH;
	Flash.m_Pos = Pos;
	Flash.m_LifeSpan = 0.1f;
	Flash.m_StartSize = 6.0f;
	Flash.m_EndSize = 22.0f;
	Flash.m_Color = ColorRGBA(mix(Main.r, 1.0f, 0.5f), mix(Main.g, 1.0f, 0.5f), mix(Main.b, 1.0f, 0.5f), 0.8f);
	m_Particles.Add(Flash);
	for(int i = 0; i < 3; i++)
	{
		CXcParticles::SParticle Spark;
		Spark.m_Type = CXcParticles::EType::SPARK;
		Spark.m_Pos = Pos;
		Spark.m_Vel = direction(angle(Dir) + random_float(-0.35f, 0.35f)) * random_float(160.0f, 300.0f);
		Spark.m_Friction = 0.02f;
		Spark.m_LifeSpan = random_float(0.08f, 0.16f);
		Spark.m_StartSize = random_float(3.0f, 5.0f);
		Spark.m_Color = Main;
		m_Particles.Add(Spark);
	}
}

void CXcImpactFx::Impact(vec2 Pos, vec2 Dir, vec2 Normal, int Weapon, int Owner)
{
	const FxTune::CScope TuneScope(FxTune::IMPACT);
	const ColorRGBA Main = Color(Weapon, Owner);
	const ColorRGBA Hot(mix(Main.r, 1.0f, 0.6f), mix(Main.g, 1.0f, 0.6f), mix(Main.b, 1.0f, 0.6f), 1.0f);
	const vec2 At = Pos + Normal * 2.0f;
	// the shot bounces off the wall: sparks fly around the reflected direction
	const vec2 Reflected = Dir - Normal * (2.0f * dot(Dir, Normal));
	const float BaseAngle = angle(normalize(Reflected * 0.6f + Normal * 0.4f));

	CXcParticles::SParticle Flash;
	Flash.m_Type = CXcParticles::EType::FLASH;
	Flash.m_Pos = At;
	Flash.m_LifeSpan = 0.14f;
	Flash.m_StartSize = 6.0f;
	Flash.m_EndSize = 30.0f;
	Flash.m_Color = Hot;
	m_Particles.Add(Flash);

	const bool Stars = g_Config.m_XcImpactFx == IMPACT_STARS;
	const int Num = Stars ? 5 : 8;
	for(int i = 0; i < Num; i++)
	{
		CXcParticles::SParticle Spark;
		Spark.m_Type = Stars ? CXcParticles::EType::STAR : CXcParticles::EType::SPARK;
		Spark.m_Pos = At;
		Spark.m_Vel = direction(BaseAngle + random_float(-1.0f, 1.0f)) * random_float(150.0f, 420.0f);
		Spark.m_Gravity = 900.0f;
		Spark.m_Friction = 0.1f;
		Spark.m_LifeSpan = random_float(0.2f, 0.4f);
		Spark.m_StartSize = Stars ? random_float(9.0f, 13.0f) : random_float(3.5f, 6.5f);
		Spark.m_Rot = random_float(0.0f, 2.0f * pi);
		Spark.m_RotSpeed = Stars ? random_float(-8.0f, 8.0f) : 0.0f;
		Spark.m_Color = i % 3 == 0 ? Hot : Main;
		m_Particles.Add(Spark);
	}
	if(g_Config.m_XcImpactFx == IMPACT_DUST)
	{
		// crumbs of the block and a puff of dust
		for(int i = 0; i < 4; i++)
		{
			CXcParticles::SParticle Crumb;
			Crumb.m_Type = CXcParticles::EType::SHARD;
			Crumb.m_Pos = At;
			Crumb.m_Vel = direction(angle(Normal) + random_float(-0.8f, 0.8f)) * random_float(90.0f, 220.0f);
			Crumb.m_Gravity = 1100.0f;
			Crumb.m_Friction = 0.3f;
			Crumb.m_LifeSpan = random_float(0.35f, 0.6f);
			Crumb.m_StartSize = random_float(4.0f, 7.0f);
			Crumb.m_EndSize = Crumb.m_StartSize;
			Crumb.m_Rot = random_float(0.0f, 2.0f * pi);
			Crumb.m_RotSpeed = random_float(-10.0f, 10.0f);
			const float Grey = random_float(0.35f, 0.55f);
			Crumb.m_Color = ColorRGBA(Grey, Grey * 0.95f, Grey * 0.9f, 1.0f);
			m_Particles.Add(Crumb);
		}
		CXcParticles::SParticle Dust;
		Dust.m_Type = CXcParticles::EType::DUST;
		Dust.m_Pos = At + Normal * 4.0f;
		Dust.m_Vel = Normal * 40.0f;
		Dust.m_Gravity = -20.0f;
		Dust.m_LifeSpan = 0.45f;
		Dust.m_StartSize = 10.0f;
		Dust.m_EndSize = 26.0f;
		Dust.m_Color = ColorRGBA(0.7f, 0.68f, 0.64f, 0.4f);
		m_Particles.Add(Dust);
	}
}

void CXcImpactFx::PlayerHit(vec2 Pos, vec2 Dir, int HitTee)
{
	if(g_Config.m_XcHitPlayerFx == 0)
		return;
	const FxTune::CScope TuneScope(FxTune::IMPACT);
	const float Size = g_Config.m_XcHitPlayerSize / 100.0f;
	const ColorRGBA Main = EffectColor(GameClient(), g_Config.m_XcHitPlayerColorMode, g_Config.m_XcHitPlayerColor, HitTee);
	if(g_Config.m_XcHitPlayerFx == 1)
	{
		// a ring that pops on the tee
		CXcParticles::SParticle Ring;
		Ring.m_Type = CXcParticles::EType::RING;
		Ring.m_Pos = Pos;
		Ring.m_LifeSpan = 0.3f;
		Ring.m_StartSize = 10.0f * Size;
		Ring.m_EndSize = 46.0f * Size;
		Ring.m_Thickness = 4.0f * Size;
		Ring.m_Color = Main;
		m_Particles.Add(Ring);
		return;
	}
	// sparks or stars flying on in the direction of the hit
	const bool Stars = g_Config.m_XcHitPlayerFx == 3;
	const float BaseAngle = angle(length(Dir) > 0.0f ? Dir : vec2(0.0f, -1.0f));
	for(int i = 0; i < (Stars ? 4 : 7); i++)
	{
		CXcParticles::SParticle Particle;
		Particle.m_Type = Stars ? CXcParticles::EType::STAR : CXcParticles::EType::SPARK;
		Particle.m_Pos = Pos;
		Particle.m_Vel = direction(BaseAngle + random_float(-0.9f, 0.9f)) * random_float(140.0f, 380.0f) * std::sqrt(Size);
		Particle.m_Gravity = Stars ? 300.0f : 800.0f;
		Particle.m_LifeSpan = random_float(0.25f, 0.45f);
		Particle.m_StartSize = (Stars ? random_float(11.0f, 16.0f) : random_float(5.0f, 8.0f)) * Size;
		Particle.m_Rot = random_float(0.0f, 2.0f * pi);
		Particle.m_RotSpeed = Stars ? random_float(-6.0f, 6.0f) : 0.0f;
		Particle.m_Color = i % 3 == 0 ? ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f) : Main;
		m_Particles.Add(Particle);
	}
}

void CXcImpactFx::Projectile(int ItemId, vec2 Pos, vec2 Vel, int Weapon, int Owner, float Age, float Speed)
{
	auto It = m_Shots.find(ItemId);
	if(It == m_Shots.end())
	{
		// a new bullet: a flash at the gun if it was just shot (an old one may only have come into view)
		if(g_Config.m_XcImpactMuzzle && Age < 0.08f && Weapon != WEAPON_GRENADE && ShowTargets(Owner) && length(Vel) > 0.001f)
			Muzzle(Pos, normalize(Vel), Weapon, Owner);
		m_Shots[ItemId] = {Pos, Vel, Weapon, Owner, m_Frame, Speed};
		return;
	}
	It->second = {Pos, Vel, Weapon, Owner, m_Frame, Speed};
}

void CXcImpactFx::Laser(vec2 From, vec2 To, int StartTick, int Owner, int Type)
{
	if((g_Config.m_XcImpactFx == IMPACT_OFF && !g_Config.m_XcImpactMuzzle && !g_Config.m_XcHitPlayerFx) || !ShowTargets(Owner) || distance(From, To) < 1.0f)
		return;
	const long long Key = ((long long)(Owner + 1) << 40) ^ ((long long)Type << 32) ^ (unsigned)StartTick;
	auto It = m_Lasers.find(Key);
	const bool New = It == m_Lasers.end();
	if(New && g_Config.m_XcImpactMuzzle && Client()->GameTick(g_Config.m_ClDummy) - StartTick < 3)
		Muzzle(From, normalize(To - From), WEAPON_LASER, Owner);
	// every new end point is a hit, when there is a block right there
	if(New || distance(It->second.m_To, To) > 1.0f)
	{
		const vec2 Dir = normalize(To - From);
		vec2 Col, Before, Hit;
		if(Collision()->IntersectLine(To - Dir * 4.0f, To + Dir * 6.0f, &Col, &Before))
		{
			if(g_Config.m_XcImpactFx != IMPACT_OFF)
				Impact(Col, Dir, WallNormal(Col, Before), WEAPON_LASER, Owner);
		}
		else if(g_Config.m_XcHitPlayerFx)
		{
			// a laser that stops in the air stopped on a tee
			const int Tee = TeeOnLine(To - Dir * 24.0f, To + Dir * 8.0f, Owner, &Hit);
			if(Tee >= 0)
				PlayerHit(Hit, Dir, Tee);
		}
	}
	m_Lasers[Key] = {To, m_Frame};
}

void CXcImpactFx::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		m_Shots.clear();
		m_Lasers.clear();
		return;
	}
	// bullets that were not drawn this frame are gone: a block right in front means they hit it
	for(auto It = m_Shots.begin(); It != m_Shots.end();)
	{
		const SShot &Shot = It->second;
		if(Shot.m_Frame == m_Frame)
		{
			++It;
			continue;
		}
		if(Shot.m_Weapon != WEAPON_GRENADE && ShowTargets(Shot.m_Owner) && length(Shot.m_Vel) > 0.001f)
		{
			// how far it can have flown since it was last drawn: a frame and a tick, with some room
			const float Reach = std::clamp(Shot.m_Speed * (Client()->RenderFrameTime() + 1.0f / Client()->GameTickSpeed()) * 1.5f, 48.0f, 400.0f);
			const vec2 Dir = normalize(Shot.m_Vel);
			vec2 Col, Before, Hit;
			const bool Wall = Collision()->IntersectLine(Shot.m_Pos, Shot.m_Pos + Dir * Reach, &Col, &Before);
			// a tee before the wall took the shot
			const int Tee = g_Config.m_XcHitPlayerFx ? TeeOnLine(Shot.m_Pos, Wall ? Col : Shot.m_Pos + Dir * Reach, Shot.m_Owner, &Hit) : -1;
			if(Tee >= 0)
				PlayerHit(Hit, Dir, Tee);
			else if(Wall && ShowFor(Shot.m_Owner))
				Impact(Col, Dir, WallNormal(Col, Before), Shot.m_Weapon, Shot.m_Owner);
		}
		It = m_Shots.erase(It);
	}
	for(auto It = m_Lasers.begin(); It != m_Lasers.end();)
		It = It->second.m_Frame == m_Frame ? std::next(It) : m_Lasers.erase(It);
	m_Frame++;

	m_Particles.Update(std::min(Client()->RenderFrameTime(), 0.1f));
	m_Particles.Render(GameClient()->m_FxRenderer);
}
