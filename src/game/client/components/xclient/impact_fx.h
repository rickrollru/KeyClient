#ifndef GAME_CLIENT_COMPONENTS_XCLIENT_IMPACT_FX_H
#define GAME_CLIENT_COMPONENTS_XCLIENT_IMPACT_FX_H

#include "xc_particles.h"

#include <base/color.h>
#include <base/vmath.h>

#include <game/client/component.h>

#include <unordered_map>

// Bullet hits: sparks and dust where a bullet or a laser hits a block, a small flash where a shot starts.
// The client does not get "hit" events, so the shots are followed while they are drawn: a bullet that
// is gone and had a block right in front of it has hit that block; a laser hits where it ends.
class CXcImpactFx : public CComponent
{
public:
	enum
	{
		IMPACT_OFF = 0,
		IMPACT_SPARKS,
		IMPACT_DUST,
		IMPACT_STARS,
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnMapLoad() override { OnReset(); }
	void OnRender() override;

	// called by CItems for every drawn bullet, Age in seconds since it was shot, Speed in units per second
	void Projectile(int ItemId, vec2 Pos, vec2 Vel, int Weapon, int Owner, float Age, float Speed);
	// called by CItems for every drawn rifle or shotgun laser
	void Laser(vec2 From, vec2 To, int StartTick, int Owner, int Type);

private:
	struct SShot
	{
		vec2 m_Pos;
		vec2 m_Vel;
		int m_Weapon;
		int m_Owner;
		int m_Frame;
		float m_Speed; // units per second
	};
	std::unordered_map<int, SShot> m_Shots;
	struct SLaser
	{
		vec2 m_To;
		int m_Frame;
	};
	std::unordered_map<long long, SLaser> m_Lasers;
	int m_Frame = 0;
	CXcParticles m_Particles;

	bool ShowFor(int Owner) const;
	bool ShowTargets(int Owner) const;
	ColorRGBA Color(int Weapon, int Owner) const;
	void Muzzle(vec2 Pos, vec2 Dir, int Weapon, int Owner);
	void Impact(vec2 Pos, vec2 Dir, vec2 Normal, int Weapon, int Owner);

public:
	// a shot or the hammer hit a player (the damage event: a place and a direction, no owner)
	void PlayerHit(vec2 Pos, vec2 Dir, int HitTee = -1);

private:
	// direction out of the block that was hit at Pos, coming from Before
	// the tee (not the owner) a shot passing from From to To hits, -1 for none
	int TeeOnLine(vec2 From, vec2 To, int Owner, vec2 *pHit) const;
	vec2 WallNormal(vec2 Pos, vec2 Before) const;
};

#endif
