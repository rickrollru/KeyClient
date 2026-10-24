#include "xeditor_solver.h"

#include <base/math.h>
#include <base/mem.h>

#include <game/collision.h>
#include <game/gamecore.h>

#include <algorithm>
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
constexpr int MAX_TICKS = 50 * 60; // a minute
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
	static bool Deadly(int Index) { return Index == TILE_DEATH || Index == TILE_FREEZE || Index == TILE_DFREEZE || Index == TILE_LFREEZE; }
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
	// like the server (GetMapIndices): the tiles the middle of the tee passes from the last tick to this one
	bool Passes(vec2 From, vec2 To, bool (CSearch::*pfnTest)(vec2) const) const
	{
		const float Distance = distance(From, To);
		const int Num = (int)(Distance / 8.0f) + 1;
		for(int i = 0; i <= Num; i++)
			if((this->*pfnTest)(mix(From, To, (float)i / Num)))
				return true;
		return false;
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
	while(!Queue.empty())
	{
		const int i = Queue.front();
		Queue.pop_front();
		const int x = i % w, y = i / w;
		const int aDx[] = {1, -1, 0, 0}, aDy[] = {0, 0, 1, -1};
		for(int d = 0; d < 4; d++)
		{
			const int nx = x + aDx[d], ny = y + aDy[d];
			if(nx < 0 || ny < 0 || nx >= w || ny >= h || !Free(nx, ny))
				continue;
			const int n = ny * w + nx;
			if(vDistance[n] <= vDistance[i] + 32.0f)
				continue;
			vDistance[n] = vDistance[i] + 32.0f;
			Queue.push_back(n);
		}
	}
}

void CSearch::Run()
{
	XcSolver::SResult &Result = m_Job.m_Result;
	m_Collision.XcInitTiles(m_Job.m_vGame.data(), m_Job.m_vFront.empty() ? nullptr : m_Job.m_vFront.data(), m_Job.m_Width, m_Job.m_Height);
	m_vNearFreeze.assign((size_t)m_Job.m_Width * m_Job.m_Height, 0);
	for(int y = 0; y < m_Job.m_Height; y++)
		for(int x = 0; x < m_Job.m_Width; x++)
			if(Deadly(Tile(m_Job.m_vGame, x, y)) || Deadly(Tile(m_Job.m_vFront, x, y)))
				for(int dy = -2; dy <= 2; dy++)
					for(int dx = -2; dx <= 2; dx++)
						if(x + dx >= 0 && y + dy >= 0 && x + dx < m_Job.m_Width && y + dy < m_Job.m_Height)
							m_vNearFreeze[(y + dy) * m_Job.m_Width + x + dx] = 1;
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
	if(Heuristic(m_Job.m_Start, 0) >= FAR)
	{
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
	float BestDistance = FAR;
	int BestNode = 0;
	int Goal = -1;

	while(!Open.empty() && Result.m_Explored < m_Job.m_MaxTries && !m_Job.m_Cancel)
	{
		const int Index = Open.top().second;
		Open.pop();
		Result.m_Explored++;
		const SNode Node = vNodes[Index];
		if((Result.m_Explored & 255) == 0)
		{
			m_Job.m_Progress = Result.m_Explored;
			// the live view: the way to the best place so far, and a sample of the tried places
			std::vector<vec2> vPath;
			for(int i = BestNode; i >= 0; i = vNodes[i].m_Parent)
				vPath.push_back(vNodes[i].m_State.m_Pos);
			std::reverse(vPath.begin(), vPath.end());
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
		for(const XcSolver::SStep &Step : vSteps)
		{
			if(Step.m_Hook == 1 && !Hooking)
				continue; // nothing to hold
			Load(Core, Node.m_State);
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
				const vec2 PrevPos = Core.m_Pos;
				Core.Tick(true);
				Core.Move();
				Core.Quantize();
				if(Core.m_Pos.x < 0.0f || Core.m_Pos.y < 0.0f || Core.m_Pos.x >= m_Job.m_Width * 32.0f || Core.m_Pos.y >= m_Job.m_Height * 32.0f || Passes(PrevPos, Core.m_Pos, &CSearch::DeadlyAt))
					Dead = true;
				else if(Passes(PrevPos, Core.m_Pos, &CSearch::FinishAt))
					Done = true;
			}
			if(Dead)
				continue;
			// the same place, speed and hook as a try before: nothing new
			SState New;
			Save(Core, New);
			uint64_t Key = (uint64_t)(int)(New.m_Pos.x / 4.0f) & 0xffff;
			Key = Key << 16 | ((uint64_t)(int)(New.m_Pos.y / 4.0f) & 0xffff);
			Key = Key << 8 | ((uint64_t)(int)(New.m_Vel.x / 2.0f + 128.0f) & 0xff);
			Key = Key << 8 | ((uint64_t)(int)(New.m_Vel.y / 2.0f + 128.0f) & 0xff);
			Key = Key << 4 | (uint64_t)(New.m_HookState & 0xf);
			Key = Key << 2 | (uint64_t)(New.m_Jumped & 3);
			// a hint reached: the next one is the target
			int Stage = Node.m_Stage;
			while(Stage < (int)m_Job.m_vHints.size() && distance(New.m_Pos, m_Job.m_vHints[Stage]) < 48.0f)
				Stage++;
			Key ^= (uint64_t)Stage * 0x9e3779b97f4a7c15ull;
			if(New.m_HookState == HOOK_GRABBED)
				Key ^= ((uint64_t)(int)(New.m_HookPos.x / 16.0f) * 2654435761ull) ^ ((uint64_t)(int)(New.m_HookPos.y / 16.0f) * 40503ull);
			if(!Seen.insert(Key).second)
				continue;
			{
				const int cx = std::clamp((int)(New.m_Pos.x / 32.0f), 0, m_Job.m_Width - 1), cy = std::clamp((int)(New.m_Pos.y / 32.0f), 0, m_Job.m_Height - 1);
				const int Cell = (cy * m_Job.m_Width + cx) * 4 + std::min(Stage, 3);
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
			// mostly towards the finish, a little for being quick
			Open.push({Distance + Child.m_Steps * 5.0f, ChildIndex});
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
	m_Collision.Unload();
}
} // namespace

std::shared_ptr<XcSolver::CJob> XcSolver::Start(const CTile *pGame, const CTile *pFront, int Width, int Height, int MaxTries, const std::vector<vec2> &vHints)
{
	auto pJob = std::make_shared<CJob>();
	pJob->m_vHints = vHints;
	pJob->m_Width = Width;
	pJob->m_Height = Height;
	pJob->m_vGame.assign(pGame, pGame + (size_t)Width * Height);
	if(pFront)
		pJob->m_vFront.assign(pFront, pFront + (size_t)Width * Height);
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
