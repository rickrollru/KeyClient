#include "xeditor_solver.h"

#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <game/collision.h>
#include <game/gamecore.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <cmath>
#include <deque>
#include <queue>
#include <thread>
#include <unordered_set>

namespace
{
// a decision every few ticks: often enough for gores timing, few enough to search
constexpr int STEP_TICKS = 5;
// near freeze every 2 ticks: dropping into a narrow shaft lined with freeze needs that precision
constexpr int FINE_STEP_TICKS = 2;
constexpr int MAX_TICKS = 50 * 60 * 10; // ten minutes
constexpr int FREEZE_TICKS = 50 * 3; // sv_freeze_delay
constexpr float FAR = 1e9f;

// the part of the character core that changes while it moves, small enough to keep many of them
struct SState
{
	vec2 m_Pos;
	vec2 m_Vel;
	vec2 m_HookPos;
	vec2 m_HookDir;
	vec2 m_HookTeleBase;
	int m_HookTick;
	int m_HookState;
	int m_Jumped;
	int m_JumpedTotal;
	int m_Jumps;
	bool m_NewHook;
	bool m_LeftWall;
	int m_Colliding;
	CNetObj_PlayerInput m_Input;
	int m_TeleCheckpoint = 0; // the last tele checkpoint passed (not part of the core)
	int m_FreezeTicks = 0; // frozen for so long still: no input
	int m_FreezeTouch = 0; // the soft check: ticks in a row the middle of the tee was in freeze
};

struct SNode
{
	SState m_State;
	int m_Parent;
	XcSolver::SStep m_Step;
	int m_Steps; // ticks since the start
	int m_Stage; // hints passed
};

void Save(const CCharacterCore &Core, SState &State)
{
	State.m_Pos = Core.m_Pos;
	State.m_Vel = Core.m_Vel;
	State.m_HookPos = Core.m_HookPos;
	State.m_HookDir = Core.m_HookDir;
	State.m_HookTeleBase = Core.m_HookTeleBase;
	State.m_HookTick = Core.m_HookTick;
	State.m_HookState = Core.m_HookState;
	State.m_Jumped = Core.m_Jumped;
	State.m_JumpedTotal = Core.m_JumpedTotal;
	State.m_Jumps = Core.m_Jumps;
	State.m_NewHook = Core.m_NewHook;
	State.m_LeftWall = Core.m_LeftWall;
	State.m_Colliding = Core.m_Colliding;
	State.m_Input = Core.m_Input;
}

void Load(CCharacterCore &Core, const SState &State)
{
	Core.m_Pos = State.m_Pos;
	Core.m_Vel = State.m_Vel;
	Core.m_HookPos = State.m_HookPos;
	Core.m_HookDir = State.m_HookDir;
	Core.m_HookTeleBase = State.m_HookTeleBase;
	Core.m_HookTick = State.m_HookTick;
	Core.m_HookState = State.m_HookState;
	Core.m_Jumped = State.m_Jumped;
	Core.m_JumpedTotal = State.m_JumpedTotal;
	Core.m_Jumps = State.m_Jumps;
	Core.m_NewHook = State.m_NewHook;
	Core.m_LeftWall = State.m_LeftWall;
	Core.m_Colliding = State.m_Colliding;
	Core.m_Input = State.m_Input;
}

class CSearch
{
public:
	explicit CSearch(XcSolver::CJob &Job) :
		m_Job(Job) {}

	void Run();

private:
	XcSolver::CJob &m_Job;
	CCollision m_Collision;
	// the way through free cells, in pixels: to each hint and the last one to the finish
	std::vector<std::vector<float>> m_vvFields;
	std::vector<float> m_vRest; // from reaching target k on: the way left after it

	int Tile(const std::vector<CTile> &vTiles, int x, int y) const
	{
		if(vTiles.empty() || x < 0 || y < 0 || x >= m_Job.m_Width || y >= m_Job.m_Height)
			return 0;
		return vTiles[y * m_Job.m_Width + x].m_Index;
	}
	// death and deep freeze end a try; freeze only freezes: the tee flies on and an unfreeze tile or 3 s
	// out of the freeze free it (maps lead through freeze into unfreeze)
	static bool Deadly(int Index) { return Index == TILE_DEATH || Index == TILE_DFREEZE; }
	static bool Freezes(int Index) { return Index == TILE_FREEZE || Index == TILE_LFREEZE; }
	static bool Unfreezes(int Index) { return Index == TILE_UNFREEZE || Index == TILE_DUNFREEZE; }
	bool FreezeAt(vec2 Pos) const
	{
		const int x = (int)std::floor(Pos.x / 32.0f), y = (int)std::floor(Pos.y / 32.0f);
		return Freezes(Tile(m_Job.m_vGame, x, y)) || Freezes(Tile(m_Job.m_vFront, x, y));
	}
	bool UnfreezeAt(vec2 Pos) const
	{
		const int x = (int)std::floor(Pos.x / 32.0f), y = (int)std::floor(Pos.y / 32.0f);
		return Unfreezes(Tile(m_Job.m_vGame, x, y)) || Unfreezes(Tile(m_Job.m_vFront, x, y));
	}
	bool DeadlyAt(vec2 Pos) const
	{
		const int x = (int)std::floor(Pos.x / 32.0f), y = (int)std::floor(Pos.y / 32.0f);
		return Deadly(Tile(m_Job.m_vGame, x, y)) || Deadly(Tile(m_Job.m_vFront, x, y));
	}
	bool FinishAt(vec2 Pos) const
	{
		const int x = (int)std::floor(Pos.x / 32.0f), y = (int)std::floor(Pos.y / 32.0f);
		return Tile(m_Job.m_vGame, x, y) == TILE_FINISH || Tile(m_Job.m_vFront, x, y) == TILE_FINISH;
	}
	float Field(int Target, vec2 Pos) const
	{
		const int x = std::clamp((int)(Pos.x / 32.0f), 0, m_Job.m_Width - 1), y = std::clamp((int)(Pos.y / 32.0f), 0, m_Job.m_Height - 1);
		return m_vvFields[Target][y * m_Job.m_Width + x];
	}
	float Heuristic(vec2 Pos, int Stage) const
	{
		float Value = Field(Stage, Pos);
		// a hint in a wall or behind one: straight to it
		if(Value >= FAR && Stage < (int)m_Job.m_vHints.size())
			Value = distance(Pos, m_Job.m_vHints[Stage]);
		return Value >= FAR ? FAR : Value + m_vRest[Stage];
	}
	void BuildField(std::vector<float> &vField, int Hint);

	// teleports: where each number comes out, and the cells of the ins (for the distances)
	std::vector<std::vector<vec2>> m_vvTeleOuts; // [number - 1]
	std::vector<std::vector<vec2>> m_vvCheckOuts; // [number - 1]
	std::vector<std::vector<int>> m_vvTeleIns; // [number - 1] cells of normal and evil ins
	void BuildTeles();
	// without hints of the mapper: a route through the free cells that keeps to the middle of the ways and away
	// from freeze, with a hint every few cells along it; the search goes from one to the next
	void AutoHints();
	const CTeleTile *TeleAt(vec2 Pos) const
	{
		if(m_Job.m_vTele.empty())
			return nullptr;
		const int x = (int)std::floor(Pos.x / 32.0f), y = (int)std::floor(Pos.y / 32.0f);
		if(x < 0 || y < 0 || x >= m_Job.m_Width || y >= m_Job.m_Height)
			return nullptr;
		const CTeleTile &Tile = m_Job.m_vTele[y * m_Job.m_Width + x];
		return Tile.m_Type ? &Tile : nullptr;
	}
	// one tick along the way of the middle of the tee, like the server: freeze, teleports, the finish
	void HandleTiles(vec2 From, CCharacterCore &Core, int &TeleCheckpoint, int &FreezeTicks, bool &Unfrozen, bool &Dead, bool &Done) const;

	// the report
	enum
	{
		END_DEATH = 0, // a death or deep freeze tile
		END_OUT, // out of the map
		END_THAW, // thawed out by waiting after freeze
		END_STUCK, // came to rest in freeze
		NUM_ENDS,
	};
	mutable int m_aEnds[NUM_ENDS] = {};
	mutable bool m_TouchedFreeze = false; // the soft check: this tick passed freeze
	void Log(const char *pFormat, ...);
	void LogGrid(vec2 Center, const std::vector<vec2> &vPath) const;
	std::vector<uint8_t> m_vNearFreeze; // freeze or death within two cells
	bool NearFreeze(vec2 Pos) const
	{
		const int x = std::clamp((int)(Pos.x / 32.0f), 0, m_Job.m_Width - 1), y = std::clamp((int)(Pos.y / 32.0f), 0, m_Job.m_Height - 1);
		return m_vNearFreeze[y * m_Job.m_Width + x] != 0;
	}
};

// Hint -1: from the finish line
void CSearch::BuildField(std::vector<float> &vDistance, int Hint)
{
	const int w = m_Job.m_Width, h = m_Job.m_Height;
	vDistance.assign((size_t)w * h, FAR);
	std::deque<int> Queue;
	if(Hint >= 0)
	{
		const vec2 Pos = m_Job.m_vHints[Hint];
		const int i = std::clamp((int)(Pos.y / 32.0f), 0, h - 1) * w + std::clamp((int)(Pos.x / 32.0f), 0, w - 1);
		vDistance[i] = 0.0f;
		Queue.push_back(i);
	}
	else
		for(int i = 0; i < w * h; i++)
			if(m_Job.m_vGame[i].m_Index == TILE_FINISH || (!m_Job.m_vFront.empty() && m_Job.m_vFront[i].m_Index == TILE_FINISH))
			{
				vDistance[i] = 0.0f;
				Queue.push_back(i);
			}
	auto Free = [&](int x, int y) {
		const int Game = Tile(m_Job.m_vGame, x, y), Front = Tile(m_Job.m_vFront, x, y);
		return Game != TILE_SOLID && Game != TILE_NOHOOK && !Deadly(Game) && !Deadly(Front);
	};
	// through freeze is possible but much harder
	auto Cost = [&](int x, int y) { return Freezes(Tile(m_Job.m_vGame, x, y)) || Freezes(Tile(m_Job.m_vFront, x, y)) ? 32.0f * 400.0f : 32.0f; };
	while(!Queue.empty())
	{
		const int i = Queue.front();
		Queue.pop_front();
		const int x = i % w, y = i / w;
		// the way back through a teleport: from its out to every in of the number
		if(!m_Job.m_vTele.empty() && m_Job.m_vTele[i].m_Type == TILE_TELEOUT && m_Job.m_vTele[i].m_Number > 0)
			for(int In : m_vvTeleIns[m_Job.m_vTele[i].m_Number - 1])
				if(vDistance[In] > vDistance[i] + 32.0f)
				{
					vDistance[In] = vDistance[i] + 32.0f;
					Queue.push_back(In);
				}
		const int aDx[] = {1, -1, 0, 0}, aDy[] = {0, 0, 1, -1};
		for(int d = 0; d < 4; d++)
		{
			const int nx = x + aDx[d], ny = y + aDy[d];
			if(nx < 0 || ny < 0 || nx >= w || ny >= h || !Free(nx, ny))
				continue;
			const int n = ny * w + nx;
			const float Next = vDistance[i] + Cost(nx, ny);
			if(vDistance[n] <= Next)
				continue;
			vDistance[n] = Next;
			Queue.push_back(n);
		}
	}
}

void CSearch::BuildTeles()
{
	m_vvTeleOuts.assign(256, {});
	m_vvCheckOuts.assign(256, {});
	m_vvTeleIns.assign(256, {});
	for(size_t i = 0; i < m_Job.m_vTele.size(); i++)
	{
		const CTeleTile &Tile = m_Job.m_vTele[i];
		if(Tile.m_Number == 0)
			continue;
		const vec2 Center((i % m_Job.m_Width) * 32.0f + 16.0f, (i / m_Job.m_Width) * 32.0f + 16.0f);
		if(Tile.m_Type == TILE_TELEOUT)
			m_vvTeleOuts[Tile.m_Number - 1].push_back(Center);
		else if(Tile.m_Type == TILE_TELECHECKOUT)
			m_vvCheckOuts[Tile.m_Number - 1].push_back(Center);
		else if(Tile.m_Type == TILE_TELEIN || Tile.m_Type == TILE_TELEINEVIL)
			m_vvTeleIns[Tile.m_Number - 1].push_back((int)i);
	}
}

void CSearch::HandleTiles(vec2 From, CCharacterCore &Core, int &TeleCheckpoint, int &FreezeTicks, bool &Unfrozen, bool &Dead, bool &Done) const
{
	const vec2 To = Core.m_Pos;
	const int Num = (int)(distance(From, To) / 8.0f) + 1;
	for(int i = 0; i <= Num; i++)
	{
		const vec2 Pos = mix(From, To, (float)i / Num);
		if(DeadlyAt(Pos))
		{
			Dead = true;
			m_aEnds[END_DEATH]++;
			return;
		}
		if(UnfreezeAt(Pos))
		{
			Unfrozen = FreezeTicks > 0;
			FreezeTicks = 0;
		}
		else if(FreezeAt(Pos))
		{
			if(m_Job.m_Forgiving)
				m_TouchedFreeze = true;
			else
				FreezeTicks = FREEZE_TICKS;
		}
		if(FinishAt(Pos))
		{
			Done = true;
			return;
		}
		const CTeleTile *pTele = TeleAt(Pos);
		if(!pTele || pTele->m_Number == 0)
			continue;
		const int Number = pTele->m_Number;
		auto ResetHook = [&]() {
			Core.SetHookedPlayer(-1);
			Core.m_HookState = HOOK_RETRACTED;
			Core.m_HookPos = Core.m_Pos;
		};
		switch(pTele->m_Type)
		{
		case TILE_TELECHECK:
			TeleCheckpoint = Number;
			break;
		case TILE_TELEIN:
		case TILE_TELEINEVIL:
			// the server picks one of the outs at random, the bot takes the first
			if(!m_vvTeleOuts[Number - 1].empty())
			{
				Core.m_Pos = m_vvTeleOuts[Number - 1][0];
				if(pTele->m_Type == TILE_TELEINEVIL)
					Core.m_Vel = vec2(0.0f, 0.0f);
				ResetHook();
				return;
			}
			break;
		case TILE_TELECHECKIN:
		case TILE_TELECHECKINEVIL:
		{
			// back to the last checkpoint that has an out, else to the spawn
			vec2 Target = m_Job.m_Start;
			for(int k = TeleCheckpoint - 1; k >= 0; k--)
				if(!m_vvCheckOuts[k].empty())
				{
					Target = m_vvCheckOuts[k][0];
					break;
				}
			Core.m_Pos = Target;
			if(pTele->m_Type == TILE_TELECHECKINEVIL)
				Core.m_Vel = vec2(0.0f, 0.0f);
			ResetHook();
			return;
		}
		default:
			break;
		}
	}
}

void CSearch::Log(const char *pFormat, ...)
{
	char aBuf[512];
	va_list Args;
	va_start(Args, pFormat);
	std::vsnprintf(aBuf, sizeof(aBuf), pFormat, Args);
	va_end(Args);
	m_Job.m_Result.m_Log += aBuf;
}

// the cells around a place as text: # wall, N unhookable, F freeze, D death, u unfreeze, T tele in, O tele out,
// S start, E finish, h hint, * the way of the bot, @ the place
void CSearch::LogGrid(vec2 Center, const std::vector<vec2> &vPath) const
{
	const int cx = (int)(Center.x / 32.0f), cy = (int)(Center.y / 32.0f);
	std::vector<uint8_t> vOnPath((size_t)m_Job.m_Width * m_Job.m_Height, 0);
	for(const vec2 &Pos : vPath)
	{
		const int x = (int)(Pos.x / 32.0f), y = (int)(Pos.y / 32.0f);
		if(x >= 0 && y >= 0 && x < m_Job.m_Width && y < m_Job.m_Height)
			vOnPath[y * m_Job.m_Width + x] = 1;
	}
	std::string &Out = m_Job.m_Result.m_Log;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "cells %d..%d x, %d..%d y\n", cx - 35, cx + 35, cy - 18, cy + 18);
	Out += aBuf;
	for(int y = cy - 18; y <= cy + 18; y++)
	{
		for(int x = cx - 35; x <= cx + 35; x++)
		{
			const int g = Tile(m_Job.m_vGame, x, y), f = Tile(m_Job.m_vFront, x, y);
			const CTeleTile *pTele = TeleAt(vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f));
			bool Hint = false;
			for(const vec2 &H : m_Job.m_vHints)
				if((int)(H.x / 32.0f) == x && (int)(H.y / 32.0f) == y)
					Hint = true;
			char c = '.';
			if(x == cx && y == cy)
				c = '@';
			else if(x < 0 || y < 0 || x >= m_Job.m_Width || y >= m_Job.m_Height)
				c = ' ';
			else if(vOnPath[y * m_Job.m_Width + x])
				c = '*';
			else if(Hint)
				c = 'h';
			else if(g == TILE_SOLID)
				c = '#';
			else if(g == TILE_NOHOOK)
				c = 'N';
			else if(Freezes(g) || Freezes(f))
				c = 'F';
			else if(Deadly(g) || Deadly(f))
				c = 'D';
			else if(Unfreezes(g) || Unfreezes(f))
				c = 'u';
			else if(g == TILE_FINISH || f == TILE_FINISH)
				c = 'E';
			else if(g == TILE_START || f == TILE_START)
				c = 'S';
			else if(pTele && (pTele->m_Type == TILE_TELEIN || pTele->m_Type == TILE_TELEINEVIL))
				c = 'T';
			else if(pTele && pTele->m_Type == TILE_TELEOUT)
				c = 'O';
			Out += c;
		}
		Out += '\n';
	}
}

void CSearch::AutoHints()
{
	const int w = m_Job.m_Width, h = m_Job.m_Height;
	auto Blocks = [&](int x, int y) {
		if(x < 0 || y < 0 || x >= w || y >= h)
			return true;
		const int g = Tile(m_Job.m_vGame, x, y), f = Tile(m_Job.m_vFront, x, y);
		return g == TILE_SOLID || g == TILE_NOHOOK || Deadly(g) || Deadly(f) || Freezes(g) || Freezes(f);
	};
	// room of each free cell: cells to the nearest wall or freeze, up to 3
	std::vector<uint8_t> vRoom((size_t)w * h, 0);
	for(int y = 0; y < h; y++)
		for(int x = 0; x < w; x++)
		{
			if(Blocks(x, y))
				continue;
			int Room = 3;
			for(int r = 1; r < 3 && Room == 3; r++)
				for(int dy = -r; dy <= r && Room == 3; dy++)
					for(int dx = -r; dx <= r; dx++)
						if((std::abs(dx) == r || std::abs(dy) == r) && Blocks(x + dx, y + dy))
						{
							Room = r;
							break;
						}
			vRoom[y * w + x] = (uint8_t)Room;
		}
	// distances from the finish, narrow cells cost more so the route keeps to the middle
	std::vector<float> vDistance((size_t)w * h, FAR);
	std::deque<int> Queue;
	for(int i = 0; i < w * h; i++)
		if(m_Job.m_vGame[i].m_Index == TILE_FINISH || (!m_Job.m_vFront.empty() && m_Job.m_vFront[i].m_Index == TILE_FINISH))
		{
			vDistance[i] = 0.0f;
			Queue.push_back(i);
		}
	auto Cost = [&](int n) {
		const int x = n % w, y = n / w;
		const int g = Tile(m_Job.m_vGame, x, y), f = Tile(m_Job.m_vFront, x, y);
		if(g == TILE_SOLID || g == TILE_NOHOOK || Deadly(g) || Deadly(f))
			return FAR;
		// through freeze only when there is no other way (some maps lead through freeze into unfreeze)
		if(Freezes(g) || Freezes(f))
			return 32.0f * 400.0f;
		return 32.0f * (vRoom[n] == 1 ? 4.0f : vRoom[n] == 2 ? 1.6f : 1.0f);
	};
	while(!Queue.empty())
	{
		const int i = Queue.front();
		Queue.pop_front();
		const int x = i % w, y = i / w;
		if(!m_Job.m_vTele.empty() && m_Job.m_vTele[i].m_Type == TILE_TELEOUT && m_Job.m_vTele[i].m_Number > 0)
			for(int In : m_vvTeleIns[m_Job.m_vTele[i].m_Number - 1])
				if(vDistance[In] > vDistance[i] + 32.0f)
				{
					vDistance[In] = vDistance[i] + 32.0f;
					Queue.push_back(In);
				}
		const int aDx[] = {1, -1, 0, 0}, aDy[] = {0, 0, 1, -1};
		for(int d = 0; d < 4; d++)
		{
			const int nx = x + aDx[d], ny = y + aDy[d];
			if(nx < 0 || ny < 0 || nx >= w || ny >= h)
				continue;
			const int n = ny * w + nx;
			const float c = Cost(n);
			if(c >= FAR || vDistance[n] <= vDistance[i] + c)
				continue;
			vDistance[n] = vDistance[i] + c;
			Queue.push_back(n);
		}
	}
	// walk down from the spawn to the finish
	int Cell = std::clamp((int)(m_Job.m_Start.y / 32.0f), 0, h - 1) * w + std::clamp((int)(m_Job.m_Start.x / 32.0f), 0, w - 1);
	if(vDistance[Cell] >= FAR)
		return;
	std::vector<int> vRoute;
	for(int Guard = 0; Guard < w * h && vDistance[Cell] > 0.0f; Guard++)
	{
		vRoute.push_back(Cell);
		const int x = Cell % w, y = Cell / w;
		int Next = -1;
		float Best = vDistance[Cell];
		const int aDx[] = {1, -1, 0, 0}, aDy[] = {0, 0, 1, -1};
		for(int d = 0; d < 4; d++)
		{
			const int nx = x + aDx[d], ny = y + aDy[d];
			if(nx >= 0 && ny >= 0 && nx < w && ny < h && vDistance[ny * w + nx] < Best)
			{
				Best = vDistance[ny * w + nx];
				Next = ny * w + nx;
			}
		}
		if(Next < 0)
		{
			// a teleport in: go on at its out
			if(!m_Job.m_vTele.empty() && (m_Job.m_vTele[Cell].m_Type == TILE_TELEIN || m_Job.m_vTele[Cell].m_Type == TILE_TELEINEVIL) && m_Job.m_vTele[Cell].m_Number > 0 && !m_vvTeleOuts[m_Job.m_vTele[Cell].m_Number - 1].empty())
			{
				const vec2 Out = m_vvTeleOuts[m_Job.m_vTele[Cell].m_Number - 1][0];
				Next = (int)(Out.y / 32.0f) * w + (int)(Out.x / 32.0f);
			}
			else
				break;
		}
		Cell = Next;
	}
	// a hint every 6 cells of the route; each hint keeps its own distances, so a big map gets fewer of them
	const int MaxHints = std::clamp(50000000 / std::max(w * h, 1), 16, 400);
	const int Spacing = std::max(6, (int)vRoute.size() / MaxHints + 1);
	for(size_t i = Spacing; i + Spacing / 2 < vRoute.size(); i += Spacing)
		m_Job.m_vHints.push_back(vec2((vRoute[i] % w) * 32.0f + 16.0f, (vRoute[i] / w) * 32.0f + 16.0f));
	m_Job.m_Result.m_vAutoHints = m_Job.m_vHints;
}

void CSearch::Run()
{
	XcSolver::SResult &Result = m_Job.m_Result;
	// tower skip check: a frozen tee lies at the bottom of every shallow freeze pit; it stands like a block one
	// tee high that can be stood on and hooked (on Extreme two of them: a pit two cells deep)
	std::vector<uint8_t> vPlatform((size_t)m_Job.m_Width * m_Job.m_Height, 0);
	if(m_Job.m_TowerDepth > 0)
	{
		const int w = m_Job.m_Width;
		for(int x = 0; x < w; x++)
			for(int y = m_Job.m_Height - 1; y > 0; y--)
			{
				const int Below = m_Job.m_vGame[y * w + x].m_Index;
				if(Below != TILE_SOLID && Below != TILE_NOHOOK)
					continue;
				int Depth = 0;
				while(y - 1 - Depth >= 0 && (Freezes(Tile(m_Job.m_vGame, x, y - 1 - Depth)) || Freezes(Tile(m_Job.m_vFront, x, y - 1 - Depth))))
					Depth++;
				if(Depth == 0 || Depth > m_Job.m_TowerDepth)
					continue;
				for(int d = 0; d < Depth; d++)
				{
					const int i = (y - 1 - d) * w + x;
					m_Job.m_vGame[i].m_Index = TILE_SOLID;
					if(!m_Job.m_vFront.empty())
						m_Job.m_vFront[i].m_Index = 0;
					vPlatform[i] = 1;
				}
			}
	}
	m_Collision.XcInitTiles(m_Job.m_vGame.data(), m_Job.m_vFront.empty() ? nullptr : m_Job.m_vFront.data(), m_Job.m_Width, m_Job.m_Height);
	BuildTeles();
	m_vNearFreeze.assign((size_t)m_Job.m_Width * m_Job.m_Height, 0);
	for(int y = 0; y < m_Job.m_Height; y++)
		for(int x = 0; x < m_Job.m_Width; x++)
			if(Deadly(Tile(m_Job.m_vGame, x, y)) || Deadly(Tile(m_Job.m_vFront, x, y)) || Freezes(Tile(m_Job.m_vGame, x, y)) || Freezes(Tile(m_Job.m_vFront, x, y)))
				for(int dy = -2; dy <= 2; dy++)
					for(int dx = -2; dx <= 2; dx++)
						if(x + dx >= 0 && y + dy >= 0 && x + dx < m_Job.m_Width && y + dy < m_Job.m_Height)
							m_vNearFreeze[(y + dy) * m_Job.m_Width + x + dx] = 1;
	if(m_Job.m_vHints.empty())
		AutoHints();
	else
	{
		// a hint of the mapper in a wall or in freeze: moved to the nearest free cell
		for(vec2 &Hint : m_Job.m_vHints)
		{
			const int hx = (int)(Hint.x / 32.0f), hy = (int)(Hint.y / 32.0f);
			auto FreeCell = [&](int x, int y) {
				const int g = Tile(m_Job.m_vGame, x, y), f = Tile(m_Job.m_vFront, x, y);
				return x >= 0 && y >= 0 && x < m_Job.m_Width && y < m_Job.m_Height && g != TILE_SOLID && g != TILE_NOHOOK && !Deadly(g) && !Deadly(f) && !Freezes(g) && !Freezes(f);
			};
			if(FreeCell(hx, hy))
				continue;
			bool Moved = false;
			for(int r = 1; r <= 12 && !Moved; r++)
				for(int dy = -r; dy <= r && !Moved; dy++)
					for(int dx = -r; dx <= r && !Moved; dx++)
						if((std::abs(dx) == r || std::abs(dy) == r) && FreeCell(hx + dx, hy + dy))
						{
							Hint = vec2((hx + dx) * 32.0f + 16.0f, (hy + dy) * 32.0f + 16.0f);
							Moved = true;
						}
		}
	}
	const int NumHints = (int)m_Job.m_vHints.size();
	m_vvFields.resize(NumHints + 1);
	for(int k = 0; k < NumHints; k++)
		BuildField(m_vvFields[k], k);
	BuildField(m_vvFields[NumHints], -1);
	m_vRest.assign(NumHints + 1, 0.0f);
	for(int k = NumHints - 1; k >= 0; k--)
	{
		const float Next = Field(k + 1, m_Job.m_vHints[k]);
		m_vRest[k] = m_vRest[k + 1] + (Next >= FAR ? (k + 1 < NumHints ? distance(m_Job.m_vHints[k], m_Job.m_vHints[k + 1]) : 0.0f) : Next);
	}
	{
		int Finish = 0, TeleIns = 0;
		for(size_t i = 0; i < m_Job.m_vGame.size(); i++)
		{
			Finish += m_Job.m_vGame[i].m_Index == TILE_FINISH || (!m_Job.m_vFront.empty() && m_Job.m_vFront[i].m_Index == TILE_FINISH);
			TeleIns += !m_Job.m_vTele.empty() && (m_Job.m_vTele[i].m_Type == TILE_TELEIN || m_Job.m_vTele[i].m_Type == TILE_TELEINEVIL);
		}
		Log("map %d x %d cells, start at %.0f %.0f, finish cells %d, teleport ins %d\n", m_Job.m_Width, m_Job.m_Height, m_Job.m_Start.x / 32.0f, m_Job.m_Start.y / 32.0f, Finish, TeleIns);
		Log("hints %d (%s), tower check depth %d, max tries %d, %s check\n", NumHints, m_Job.m_Result.m_vAutoHints.empty() ? "set by the mapper" : "automatic route", m_Job.m_TowerDepth, m_Job.m_MaxTries, m_Job.m_Forgiving ? "soft" : "strict");
		Log("way through free cells from the start: %.0f cells\n", Heuristic(m_Job.m_Start, 0) >= FAR ? -1.0f : Heuristic(m_Job.m_Start, 0) / 32.0f);
	}
	if(Heuristic(m_Job.m_Start, 0) >= FAR)
	{
		Log("the finish cannot be reached through free cells from the start (walls, missing teleport outs?)\n");
		LogGrid(m_Job.m_Start, {});
		// the finish cannot be reached through free cells at all
		m_Collision.Unload();
		return;
	}

	CWorldCore World;
	CCharacterCore Core;
	Core.Init(&World, &m_Collision);
	Core.Reset();
	Core.m_Id = 0;
	World.m_apCharacters[0] = &Core;
	Core.m_Pos = m_Job.m_Start;
	mem_zero(&Core.m_Input, sizeof(Core.m_Input));
	Core.m_Input.m_TargetX = 1;

	// the ways a step can go: run left, stand, run right; jump or not; let the hook go, hold it or hook again
	// towards one of a few directions
	std::vector<XcSolver::SStep> vSteps;
	const float aAngles[] = {-90.0f, -60.0f, -120.0f, -30.0f, -150.0f, 0.0f, 180.0f, 45.0f, 135.0f};
	for(int Dir = -1; Dir <= 1; Dir++)
		for(int Jump = 0; Jump <= 1; Jump++)
		{
			vSteps.push_back({Dir, Jump != 0, 0, vec2(Dir == 0 ? 1.0f : (float)Dir, 0.0f) * 100.0f});
			vSteps.push_back({Dir, Jump != 0, 1, vec2(0.0f, 0.0f)});
			for(float Angle : aAngles)
				vSteps.push_back({Dir, Jump != 0, 2, direction(Angle * pi / 180.0f) * 100.0f});
		}

	std::vector<SNode> vNodes;
	vNodes.reserve(1 << 16);
	SNode Root;
	Save(Core, Root.m_State);
	Root.m_Parent = -1;
	Root.m_Step = {0, false, 0, vec2(100.0f, 0.0f)};
	Root.m_Steps = 0;
	Root.m_Stage = 0;
	vNodes.push_back(Root);

	using TEntry = std::pair<float, int>;
	std::priority_queue<TEntry, std::vector<TEntry>, std::greater<>> Open;
	Open.push({Heuristic(m_Job.m_Start, 0), 0});
	std::unordered_set<uint64_t> Seen;
	// a few dozen different states per cell are enough: more only digs deeper into the same dead end and
	// uses up the tries, then the search has to look elsewhere
	std::vector<uint16_t> vPerCell((size_t)m_Job.m_Width * m_Job.m_Height * 4, 0);
	// tiredness of a place: tries spent in each 4 x 4 cells make it less inviting, so the search climbs out of
	// a pocket that only looks close to the finish and goes on elsewhere
	const int RegionW = m_Job.m_Width / 4 + 1, RegionH = m_Job.m_Height / 4 + 1;
	std::vector<uint32_t> vRegionTries((size_t)RegionW * RegionH, 0);
	std::vector<uint16_t> vHeat((size_t)m_Job.m_Width * m_Job.m_Height, 0);
	auto Region = [&](vec2 Pos) {
		return std::clamp((int)(Pos.y / 128.0f), 0, RegionH - 1) * RegionW + std::clamp((int)(Pos.x / 128.0f), 0, RegionW - 1);
	};
	float BestDistance = FAR;
	int BestNode = 0;
	int Goal = -1;
	// the search goes on hint by hint: once a few tries reached the next hint, the tries behind it are dropped;
	// a hint that cannot be reached for long is skipped
	int MaxStage = 0, AtMaxStage = 0, SinceStage = 0, SkipStage = 0;
	// the tries left behind wait here: when every try ahead ends in a dead end, they come back
	std::vector<TEntry> vReserve;
	// tries before this hint go to the reserve as they come out of the queue (going through the whole queue
	// at every hint made the search slower and slower along the map)
	int DropStage = 0;
	auto DropBehind = [&]() {
		DropStage = MaxStage;
		std::fill(vRegionTries.begin(), vRegionTries.end(), 0);
	};

	while((!Open.empty() || !vReserve.empty()) && Result.m_Explored < m_Job.m_MaxTries && !m_Job.m_Cancel)
	{
		if(Open.empty())
		{
			for(const TEntry &Entry : vReserve)
				Open.push(Entry);
			vReserve.clear();
			DropStage = 0;
		}
		const int Index = Open.top().second;
		if(vNodes[Index].m_Stage < DropStage)
		{
			vReserve.push_back(Open.top());
			Open.pop();
			continue;
		}
		Open.pop();
		Result.m_Explored++;
		const SNode Node = vNodes[Index];
		vRegionTries[Region(Node.m_State.m_Pos)]++;
		if(++SinceStage > 25000 && SkipStage <= MaxStage && MaxStage < (int)m_Job.m_vHints.size())
		{
			// this hint does not work out: aim at the next one
			SkipStage = MaxStage + 1;
			SinceStage = 0;
			Log("try %d: hint %d not reached in 25000 tries, skipped (best place %.0f %.0f)\n", Result.m_Explored, MaxStage + 1, vNodes[BestNode].m_State.m_Pos.x / 32.0f, vNodes[BestNode].m_State.m_Pos.y / 32.0f);
		}
		{
			const int hx = std::clamp((int)(Node.m_State.m_Pos.x / 32.0f), 0, m_Job.m_Width - 1), hy = std::clamp((int)(Node.m_State.m_Pos.y / 32.0f), 0, m_Job.m_Height - 1);
			uint16_t &Heat = vHeat[hy * m_Job.m_Width + hx];
			if(Heat < 65535)
				Heat++;
		}
		if((Result.m_Explored & 255) == 0)
		{
			m_Job.m_Progress = Result.m_Explored;
			// the live view: the way to the best place so far, and a sample of the tried places
			std::vector<vec2> vPath;
			for(int i = BestNode; i >= 0; i = vNodes[i].m_Parent)
				vPath.push_back(vNodes[i].m_State.m_Pos);
			std::reverse(vPath.begin(), vPath.end());
			m_Job.m_LiveTicks = vNodes[BestNode].m_Steps;
			std::lock_guard<std::mutex> Lock(m_Job.m_LiveMutex);
			m_Job.m_vLivePath = std::move(vPath);
		}
		if((Result.m_Explored & 7) == 0)
		{
			std::lock_guard<std::mutex> Lock(m_Job.m_LiveMutex);
			if(m_Job.m_vLiveTried.size() < 20000)
				m_Job.m_vLiveTried.push_back(Node.m_State.m_Pos);
		}
		// far too many places kept: the memory would run out
		if(vNodes.size() > 2500000)
			break;
		if(Node.m_Steps >= MAX_TICKS)
			continue;
		const bool Hooking = Node.m_State.m_HookState != HOOK_IDLE && Node.m_State.m_HookState != HOOK_RETRACTED;
		// both jumps used up in the air: the steps with a jump do the same as without
		const vec2 Feet = Node.m_State.m_Pos + vec2(0.0f, 14.0f + 5.0f);
		const bool Grounded = m_Collision.CheckPoint(Feet.x - 7.0f, Feet.y) || m_Collision.CheckPoint(Feet.x + 7.0f, Feet.y);
		const bool CanJump = Grounded || !(Node.m_State.m_Jumped & 2);
		for(const XcSolver::SStep &Step : vSteps)
		{
			if(Step.m_Hook == 1 && !Hooking)
				continue; // nothing to hold
			if(Step.m_Jump && !CanJump)
				continue;
			if(Node.m_State.m_FreezeTicks > 0 && &Step != &vSteps[0])
				continue; // frozen: the input does nothing, one way is enough
			Load(Core, Node.m_State);
			int TeleCheckpoint = Node.m_State.m_TeleCheckpoint;
			int FreezeTicks = Node.m_State.m_FreezeTicks;
			int FreezeTouch = Node.m_State.m_FreezeTouch;
			bool Dead = false, Done = false;
			const int Ticks = NearFreeze(Node.m_State.m_Pos) ? FINE_STEP_TICKS : STEP_TICKS;
			for(int t = 0; t < Ticks && !Dead && !Done; t++)
			{
				CNetObj_PlayerInput &Input = Core.m_Input;
				Input.m_Direction = Step.m_Direction;
				Input.m_Jump = Step.m_Jump && t == 0 ? 1 : 0;
				if(Step.m_Hook == 0)
					Input.m_Hook = 0;
				else if(Step.m_Hook == 2)
					Input.m_Hook = t == 0 ? 0 : 1; // let go for a tick, then hook again
				if(Step.m_Hook != 1)
				{
					Input.m_TargetX = (int)Step.m_Target.x;
					Input.m_TargetY = (int)Step.m_Target.y;
				}
				if(FreezeTicks > 0)
				{
					// frozen: no running, jumping or hooking
					Input.m_Direction = 0;
					Input.m_Jump = 0;
					Input.m_Hook = 0;
				}
				const vec2 PrevPos = Core.m_Pos;
				Core.Tick(true);
				Core.Move();
				Core.Quantize();
				if(Core.m_Pos.x < 0.0f || Core.m_Pos.y < 0.0f || Core.m_Pos.x >= m_Job.m_Width * 32.0f || Core.m_Pos.y >= m_Job.m_Height * 32.0f)
				{
					Dead = true;
					m_aEnds[END_OUT]++;
				}
				else
				{
					const bool WasFrozen = FreezeTicks > 0;
					if(FreezeTicks > 0)
						FreezeTicks--;
					bool Unfrozen = false;
					m_TouchedFreeze = false;
					HandleTiles(PrevPos, Core, TeleCheckpoint, FreezeTicks, Unfrozen, Dead, Done);
					if(m_Job.m_Forgiving)
					{
						// the soft check: a short touch of freeze is forgiven
						FreezeTouch = m_TouchedFreeze ? FreezeTouch + 1 : 0;
						if(FreezeTouch >= 3 && !Unfrozen)
							FreezeTicks = FREEZE_TICKS;
					}
					// freeze is passed only when an unfreeze tile ends it on the way (maps made like that); thawing
					// out by waiting 3 s after falling through freeze is a skip, not the way
					if(!Dead && WasFrozen && FreezeTicks == 0 && !Unfrozen)
					{
						Dead = true;
						m_aEnds[END_THAW]++;
					}
					// frozen and come to rest in the freeze: stuck for good
					if(!Dead && FreezeTicks > 0 && length(Core.m_Vel) < 0.5f && FreezeAt(Core.m_Pos))
					{
						Dead = true;
						m_aEnds[END_STUCK]++;
					}
				}
			}
			if(Dead)
				continue;
			// the same place, speed and hook as a try before: nothing new
			SState New;
			Save(Core, New);
			New.m_TeleCheckpoint = TeleCheckpoint;
			New.m_FreezeTicks = FreezeTicks;
			New.m_FreezeTouch = FreezeTouch;
			uint64_t Key = (uint64_t)(int)(New.m_Pos.x / 4.0f) & 0xffff;
			Key = Key << 16 | ((uint64_t)(int)(New.m_Pos.y / 4.0f) & 0xffff);
			Key = Key << 8 | ((uint64_t)(int)(New.m_Vel.x / 2.0f + 128.0f) & 0xff);
			Key = Key << 8 | ((uint64_t)(int)(New.m_Vel.y / 2.0f + 128.0f) & 0xff);
			Key = Key << 4 | (uint64_t)(New.m_HookState & 0xf);
			Key = Key << 2 | (uint64_t)(New.m_Jumped & 3);
			Key ^= (uint64_t)New.m_TeleCheckpoint * 0xff51afd7ed558ccdull;
			Key ^= (uint64_t)(New.m_FreezeTicks / 10 + 1) * 0xc4ceb9fe1a85ec53ull;
			// a hint reached: the next one is the target
			// a skipped hint is skipped only for the tries that got as far as it, not for the ones far behind
			int Stage = Node.m_Stage >= SkipStage - 1 ? std::max(Node.m_Stage, std::min(SkipStage, (int)m_Job.m_vHints.size())) : Node.m_Stage;
			// a hint counts as passed when the tee is near it, or already farther along the way than it (it may fly
			// past a hint in a wide place without touching it)
			while(Stage < (int)m_Job.m_vHints.size() && (distance(New.m_Pos, m_Job.m_vHints[Stage]) < 64.0f || Field(Stage + 1, New.m_Pos) + 16.0f < Field(Stage + 1, m_Job.m_vHints[Stage])))
				Stage++;
			Key ^= (uint64_t)Stage * 0x9e3779b97f4a7c15ull;
			if(New.m_HookState == HOOK_GRABBED)
				Key ^= ((uint64_t)(int)(New.m_HookPos.x / 16.0f) * 2654435761ull) ^ ((uint64_t)(int)(New.m_HookPos.y / 16.0f) * 40503ull);
			if(!Seen.insert(Key).second)
				continue;
			{
				const int cx = std::clamp((int)(New.m_Pos.x / 32.0f), 0, m_Job.m_Width - 1), cy = std::clamp((int)(New.m_Pos.y / 32.0f), 0, m_Job.m_Height - 1);
				const int Cell = (cy * m_Job.m_Width + cx) * 4 + (Stage & 3);
				if(!Done && vPerCell[Cell] >= (NearFreeze(New.m_Pos) ? 160 : 40))
					continue;
				vPerCell[Cell]++;
			}
			SNode Child;
			Child.m_State = New;
			Child.m_Parent = Index;
			Child.m_Step = Step;
			Child.m_Step.m_Ticks = Ticks;
			Child.m_Steps = Node.m_Steps + Ticks;
			Child.m_Stage = Stage;
			vNodes.push_back(Child);
			const int ChildIndex = (int)vNodes.size() - 1;
			if(Done)
			{
				Goal = ChildIndex;
				break;
			}
			const float Distance = Heuristic(New.m_Pos, Stage);
			if(Distance >= FAR)
				continue;
			if(Distance < BestDistance)
			{
				BestDistance = Distance;
				BestNode = ChildIndex;
			}
			if(Stage > MaxStage)
			{
				Log("try %d: hint %d reached at %.0f %.0f, game time %.2f s\n", Result.m_Explored, Stage, New.m_Pos.x / 32.0f, New.m_Pos.y / 32.0f, Child.m_Steps / 50.0f);
				MaxStage = Stage;
				AtMaxStage = 0;
				SinceStage = 0;
			}
			if(Stage == MaxStage && ++AtMaxStage == 48)
				DropBehind();
			// mostly towards the finish, a little for being quick
			Open.push({Distance + Child.m_Steps * 5.0f + vRegionTries[Region(New.m_Pos)] * 4.0f, ChildIndex});
		}
		if(Goal >= 0)
			break;
	}

	const int Last = Goal >= 0 ? Goal : BestNode;
	Result.m_Found = Goal >= 0;
	Result.m_Closest = vNodes[Last].m_State.m_Pos;
	for(int i = Last; i >= 0; i = vNodes[i].m_Parent)
	{
		Result.m_vPath.push_back(vNodes[i].m_State.m_Pos);
		if(vNodes[i].m_Parent >= 0)
		{
			Result.m_vSteps.push_back(vNodes[i].m_Step);
			Result.m_Hooks += vNodes[i].m_Step.m_Hook == 2 ? 1 : 0;
			Result.m_Jumps += vNodes[i].m_Step.m_Jump ? 1 : 0;
		}
	}
	std::reverse(Result.m_vPath.begin(), Result.m_vPath.end());
	std::reverse(Result.m_vSteps.begin(), Result.m_vSteps.end());
	Result.m_Ticks = vNodes[Last].m_Steps;
	{
		Log("\nresult: %s after %d tries%s\n", Goal >= 0 ? "FINISHED" : "no way", Result.m_Explored, m_Job.m_Cancel ? " (stopped)" : "");
		Log("best place %.0f %.0f, game time %.2f s, hints passed %d of %d\n", vNodes[Last].m_State.m_Pos.x / 32.0f, vNodes[Last].m_State.m_Pos.y / 32.0f, vNodes[Last].m_Steps / 50.0f, vNodes[Last].m_Stage, (int)m_Job.m_vHints.size());
		Log("tries ended by: death tile %d, out of the map %d, thawed without unfreeze %d, stuck in freeze %d\n", m_aEnds[END_DEATH], m_aEnds[END_OUT], m_aEnds[END_THAW], m_aEnds[END_STUCK]);
		Log("\nthe cells around the best place:\n");
		LogGrid(vNodes[Last].m_State.m_Pos, Result.m_vPath);
		// the last steps of the best way
		std::vector<int> vChain;
		for(int i = Last; i >= 0; i = vNodes[i].m_Parent)
			vChain.push_back(i);
		std::reverse(vChain.begin(), vChain.end());
		Log("\nlast steps of the best way (tick: cell x y, speed x y, input: dir jump hook aim, frozen ticks):\n");
		for(size_t k = vChain.size() > 40 ? vChain.size() - 40 : 0; k < vChain.size(); k++)
		{
			const SNode &N = vNodes[vChain[k]];
			const char *apHook[] = {"release", "hold", "hook"};
			Log("%5d: %6.1f %6.1f  v %6.1f %6.1f  dir %2d jump %d %-7s aim %4.0f  frozen %d%s\n", N.m_Steps, N.m_State.m_Pos.x / 32.0f, N.m_State.m_Pos.y / 32.0f,
				N.m_State.m_Vel.x, N.m_State.m_Vel.y, N.m_Step.m_Direction, N.m_Step.m_Jump ? 1 : 0, apHook[std::clamp(N.m_Step.m_Hook, 0, 2)],
				angle(N.m_Step.m_Target) * 180.0f / pi, N.m_State.m_FreezeTicks, N.m_State.m_HookState == HOOK_GRABBED ? "  (hooked)" : "");
		}
	}
	Result.m_Width = m_Job.m_Width;
	Result.m_Height = m_Job.m_Height;
	Result.m_vHeat = std::move(vHeat);

	// room along the way: the nearest wall or freeze around each point, in cells (up to 4)
	auto Blocks = [&](int x, int y) {
		const int g = Tile(m_Job.m_vGame, x, y), f = Tile(m_Job.m_vFront, x, y);
		return g == TILE_SOLID || g == TILE_NOHOOK || Deadly(g) || Deadly(f) || Freezes(g) || Freezes(f);
	};
	for(const vec2 &Pos : Result.m_vPath)
	{
		const int px = (int)(Pos.x / 32.0f), py = (int)(Pos.y / 32.0f);
		int Room = 4;
		for(int r = 1; r <= 4 && Room == 4; r++)
			for(int dy = -r; dy <= r && Room == 4; dy++)
				for(int dx = -r; dx <= r; dx++)
					if((std::abs(dx) == r || std::abs(dy) == r) && Blocks(px + dx, py + dy))
					{
						Room = r;
						break;
					}
		Result.m_vRoom.push_back((uint8_t)Room);
	}

	// tower skips: where the way stood on a frozen tee or hooked one
	if(m_Job.m_TowerDepth > 0)
	{
		std::set<int> Spots;
		auto Platform = [&](vec2 Pos) {
			const int x = (int)std::floor(Pos.x / 32.0f), y = (int)std::floor(Pos.y / 32.0f);
			return x >= 0 && y >= 0 && x < m_Job.m_Width && y < m_Job.m_Height && vPlatform[y * m_Job.m_Width + x] ? y * m_Job.m_Width + x : -1;
		};
		for(int i = Last; i >= 0; i = vNodes[i].m_Parent)
		{
			const SState &State = vNodes[i].m_State;
			const int Stand = Platform(State.m_Pos + vec2(0.0f, 14.0f + 4.0f));
			if(Stand >= 0)
				Spots.insert(Stand);
			if(State.m_HookState == HOOK_GRABBED)
			{
				const int Hooked = Platform(State.m_HookPos);
				if(Hooked >= 0)
					Spots.insert(Hooked);
			}
		}
		for(int Spot : Spots)
			Result.m_vTowerSpots.push_back(vec2((Spot % m_Job.m_Width) * 32.0f + 16.0f, (Spot / m_Job.m_Width) * 32.0f + 16.0f));
	}
	m_Collision.Unload();
}
} // namespace

std::shared_ptr<XcSolver::CJob> XcSolver::Start(const CTile *pGame, const CTile *pFront, const CTeleTile *pTele, int Width, int Height, int MaxTries, const std::vector<vec2> &vHints, int TowerDepth, bool Forgiving)
{
	auto pJob = std::make_shared<CJob>();
	pJob->m_vHints = vHints;
	pJob->m_TowerDepth = TowerDepth;
	pJob->m_Forgiving = Forgiving;
	pJob->m_Width = Width;
	pJob->m_Height = Height;
	pJob->m_vGame.assign(pGame, pGame + (size_t)Width * Height);
	if(pFront)
		pJob->m_vFront.assign(pFront, pFront + (size_t)Width * Height);
	if(pTele)
		pJob->m_vTele.assign(pTele, pTele + (size_t)Width * Height);
	pJob->m_MaxTries = MaxTries;
	// the spawn, or the start line when there is none
	int Spawn = -1, StartLine = -1;
	for(int i = 0; i < Width * Height; i++)
	{
		// the spawn, the start and the finish can be in the game layer or in the front layer
		const int Front = pFront ? pFront[i].m_Index : 0;
		auto IsSpawn = [](int Index) { return Index >= ENTITY_OFFSET + ENTITY_SPAWN && Index <= ENTITY_OFFSET + ENTITY_SPAWN_BLUE; };
		if(Spawn < 0 && (IsSpawn(pGame[i].m_Index) || IsSpawn(Front)))
			Spawn = i;
		if(StartLine < 0 && (pGame[i].m_Index == TILE_START || Front == TILE_START))
			StartLine = i;
		if(pGame[i].m_Index == TILE_FINISH || (pFront && pFront[i].m_Index == TILE_FINISH))
			pJob->m_HasGoal = true;
	}
	const int From = Spawn >= 0 ? Spawn : StartLine;
	pJob->m_HasStart = From >= 0;
	if(!pJob->m_HasStart || !pJob->m_HasGoal)
	{
		pJob->m_Done = true;
		return pJob;
	}
	pJob->m_Start = vec2((From % Width) * 32.0f + 16.0f, (From / Width) * 32.0f + 16.0f);
	std::thread([pJob]() {
		CSearch Search(*pJob);
		Search.Run();
		pJob->m_Done = true;
	}).detach();
	return pJob;
}
