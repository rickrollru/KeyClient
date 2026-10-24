// The map generator's bot: finds a way through a part of a map on the real DDNet physics (the character core
// of the game), running, jumping and hooking; touching freeze or death ends a try. Only in the editor, offline.
#ifndef GAME_CLIENT_COMPONENTS_XCLIENT_EDITOR_XEDITOR_SOLVER_H
#define GAME_CLIENT_COMPONENTS_XCLIENT_EDITOR_XEDITOR_SOLVER_H

#include <base/vmath.h>

#include <game/mapitems.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace XcSolver
{
struct SStep
{
	int m_Direction; // -1, 0, 1
	bool m_Jump; // pressed at the start of the step
	int m_Hook; // 0 released, 1 held, 2 pressed again
	vec2 m_Target; // where the mouse points (relative)
	int m_Ticks = 5; // how long the step lasts
};

struct SResult
{
	bool m_Found = false;
	std::vector<vec2> m_vPath; // positions of the tee along the way
	std::vector<SStep> m_vSteps; // the inputs, one per step
	int m_Ticks = 0; // how long the way takes (50 ticks a second)
	int m_Hooks = 0; // hooks used
	int m_Jumps = 0; // jumps used
	int m_Explored = 0; // tries looked at
	vec2 m_Closest = vec2(0.0f, 0.0f); // where it got closest to the goal when not found

	// the heat map: tries spent in each cell (many tries: a hard place)
	int m_Width = 0;
	int m_Height = 0;
	std::vector<uint16_t> m_vHeat;
	// how much room the way has at each point: cells to the nearest wall or freeze (1: a one cell gap)
	std::vector<uint8_t> m_vRoom;
	// tower skips: cells of frozen tees the way stood on or hooked
	std::vector<vec2> m_vTowerSpots;
};

// a search running in a thread: copies the tiles first, so the map can change meanwhile
class CJob
{
public:
	std::atomic<bool> m_Done = false;
	std::atomic<bool> m_Cancel = false;
	std::atomic<int> m_Progress = 0; // tries looked at so far
	// live view while it searches: the best way so far and the places it tried (under the mutex)
	std::mutex m_LiveMutex;
	std::vector<vec2> m_vLivePath;
	std::vector<vec2> m_vLiveTried;
	std::atomic<int> m_LiveTicks = 0; // game time of the best way so far
	SResult m_Result;
	bool m_HasStart = false;
	bool m_HasGoal = false;

	int m_Width = 0;
	int m_Height = 0;
	std::vector<CTile> m_vGame;
	std::vector<CTile> m_vFront;
	std::vector<CTeleTile> m_vTele; // empty without a tele layer
	vec2 m_Start;
	std::vector<vec2> m_vHints; // points the way has to pass, in this order, before the finish
	int m_MaxTries = 200000;
	// tower skip check: frozen tees lie in freeze pits up to this many cells deep (0: off)
	int m_TowerDepth = 0;
};

// a search from the spawn (or start line) to the finish line of the layers; the tiles are copied
std::shared_ptr<CJob> Start(const CTile *pGame, const CTile *pFront, const CTeleTile *pTele, int Width, int Height, int MaxTries, const std::vector<vec2> &vHints = {}, int TowerDepth = 0);
} // namespace XcSolver

#endif
