// Tune zones in plain words. A tune zone is a number painted on the tune layer; the server settings of the map
// say what changes inside it ("tune_zone 3 gravity 0.2"), zone 0 is the whole map ("tune gravity 0.2"). This
// window reads and writes exactly these commands, so the map stays a normal DDNet map; every change is one step
// of Ctrl+Z in the server settings history.
#include "xeditor.h"

#include <base/math.h>
#include <base/str.h>

#include <engine/font_icons.h>

#include <game/client/components/xclient/ui/lang.h>
#include <game/client/components/xclient/ui/theme.h>
#include <game/client/components/xclient/ui/widgets.h>
#include <game/editor/editor.h>
#include <game/editor/editor_actions.h>
#include <game/editor/mapitems/layer_group.h>
#include <game/editor/mapitems/layer_tune.h>
#include <game/gamecore.h>

#include <engine/external/imgui/imgui.h>
#include <engine/external/imgui/imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace XcUi;

namespace
{
enum
{
	GROUP_MOVE = 0,
	GROUP_JUMP,
	GROUP_HOOK,
	GROUP_WORLD,
	GROUP_WEAPONS,
	GROUP_DELAYS,
	GROUP_PLAYERS,
	NUM_GROUPS,
};
enum
{
	UNIT_PLAIN = 0,
	UNIT_BLOCKS, // 32 units = 1 block
	UNIT_MS,
	UNIT_SECONDS,
	UNIT_TOGGLE,
};
struct STuneInfo
{
	const char *m_pScript;
	int m_Group;
	int m_Unit;
	const char *m_pNameEn;
	const char *m_pNameRu;
	const char *m_pHelpEn;
	const char *m_pHelpRu;
};

// every parameter that does something, in the order a mapper looks for it
const STuneInfo s_aTunes[] = {
	{"ground_control_speed", GROUP_MOVE, UNIT_PLAIN, "Run speed", "Скорость бега", "The top speed when running on the ground. More is faster. Default 10.", "Предельная скорость бега по земле. Больше — быстрее. Обычно 10."},
	{"ground_control_accel", GROUP_MOVE, UNIT_PLAIN, "Run acceleration", "Разгон на земле", "How fast the tee reaches the run speed. Small values feel heavy, like a truck.", "Как быстро ти набирает скорость бега. Маленькое значение — тяжёлый, как грузовик."},
	{"ground_friction", GROUP_MOVE, UNIT_PLAIN, "Ground friction", "Трение земли", "How fast the tee stops when you let go. 0.5 stops at once; close to 1 slides like ice.", "Как быстро ти останавливается, когда отпустил кнопку. 0.5 — сразу, ближе к 1 — скользит как по льду."},
	{"air_control_speed", GROUP_MOVE, UNIT_PLAIN, "Air speed", "Скорость в воздухе", "The top speed you can steer to in the air. Default 5.", "Предельная скорость, которую можно набрать стрелками в воздухе. Обычно 5."},
	{"air_control_accel", GROUP_MOVE, UNIT_PLAIN, "Air control", "Управление в воздухе", "How much the arrows steer in the air. 0: no control in the air at all.", "Насколько стрелки управляют ти в воздухе. 0 — в воздухе не повернуть совсем."},
	{"air_friction", GROUP_MOVE, UNIT_PLAIN, "Air resistance", "Сопротивление воздуха", "How much speed stays every moment in the air. 0.95 is normal; lower is like water, 1 never slows down.", "Сколько скорости остаётся каждый миг в воздухе. 0.95 — обычно, меньше — как в воде, 1 — не тормозит никогда."},
	{"ground_jump_impulse", GROUP_JUMP, UNIT_PLAIN, "Jump from the ground", "Прыжок с земли", "How high the first jump goes. Default 13.2; 20 is a super jump.", "Высота первого прыжка. Обычно 13.2, 20 — супер-прыжок."},
	{"air_jump_impulse", GROUP_JUMP, UNIT_PLAIN, "Double jump", "Двойной прыжок", "How high the jump in the air goes. Default 12.", "Высота прыжка в воздухе. Обычно 12."},
	{"hook_length", GROUP_HOOK, UNIT_BLOCKS, "Hook length", "Длина хука", "How far the hook reaches. Default 380 (about 12 blocks).", "Как далеко достаёт хук. Обычно 380 (примерно 12 блоков)."},
	{"hook_fire_speed", GROUP_HOOK, UNIT_PLAIN, "Hook flight speed", "Скорость полёта хука", "How fast the hook flies out. Default 80.", "Как быстро вылетает хук. Обычно 80."},
	{"hook_drag_accel", GROUP_HOOK, UNIT_PLAIN, "Hook pull", "Сила тяги хука", "How hard the hook pulls the tee to the wall. Default 3.", "Как сильно хук тянет ти к стене. Обычно 3."},
	{"hook_drag_speed", GROUP_HOOK, UNIT_PLAIN, "Hook pull speed", "Скорость тяги хука", "The top speed of the hook pull. Default 15.", "Предельная скорость, с которой тянет хук. Обычно 15."},
	{"hook_duration", GROUP_HOOK, UNIT_SECONDS, "Hold a player", "Удержание игрока", "How many seconds the hook holds another player. Default 1.25.", "Сколько секунд хук держит другого игрока. Обычно 1.25."},
	{"gravity", GROUP_WORLD, UNIT_PLAIN, "Gravity", "Гравитация", "How fast the tee falls. Default 0.5; 0.15 is the moon, 0 is space, negative pulls up.", "Как быстро ти падает. Обычно 0.5; 0.15 — луна, 0 — космос, минус — тянет вверх."},
	{"ground_elasticity_x", GROUP_WORLD, UNIT_PLAIN, "Bounce off walls", "Отскок от стен", "0: no bounce, 1: the tee bounces back from walls with all its speed.", "0 — не отскакивает, 1 — ти отлетает от стен со всей скоростью."},
	{"ground_elasticity_y", GROUP_WORLD, UNIT_PLAIN, "Bounce off floors", "Отскок от пола", "0: no bounce, 1: a trampoline, the tee bounces from the floor and the ceiling.", "0 — не отскакивает, 1 — батут: ти прыгает от пола и потолка."},
	{"velramp_start", GROUP_WORLD, UNIT_PLAIN, "Speed limit starts", "Начало ограничения скорости", "From this speed very fast movement starts to slow down. Default 550.", "С этой скорости очень быстрое движение начинает притормаживать. Обычно 550."},
	{"velramp_range", GROUP_WORLD, UNIT_PLAIN, "Speed limit range", "Ширина ограничения", "Over how much speed the limit grows. Default 2000.", "На каком отрезке скорости растёт ограничение. Обычно 2000."},
	{"velramp_curvature", GROUP_WORLD, UNIT_PLAIN, "Speed limit strength", "Сила ограничения", "How hard very high speed is cut. Default 1.4; 1 means no limit.", "Насколько сильно режется очень большая скорость. Обычно 1.4, 1 — без ограничения."},
	{"jetpack_strength", GROUP_WEAPONS, UNIT_PLAIN, "Jetpack", "Джетпак", "How strong the pistol pushes with the jetpack on. Default 400.", "Насколько сильно толкает пистолет, когда включён джетпак. Обычно 400."},
	{"explosion_strength", GROUP_WEAPONS, UNIT_PLAIN, "Grenade push", "Сила взрыва гранаты", "How far an explosion throws the tee (rocket jumps). Default 6.", "Как далеко взрыв отбрасывает ти (гранатные прыжки). Обычно 6."},
	{"shotgun_strength", GROUP_WEAPONS, UNIT_PLAIN, "Shotgun pull", "Сила дробовика", "How hard the shotgun pulls a tee to the shooter. Default 10.", "Как сильно дробовик притягивает ти к стрелку. Обычно 10."},
	{"hammer_strength", GROUP_WEAPONS, UNIT_PLAIN, "Hammer power", "Сила молотка", "How far the hammer throws another tee. Default 1.", "Как далеко молоток отбрасывает другого ти. Обычно 1."},
	{"laser_reach", GROUP_WEAPONS, UNIT_BLOCKS, "Laser reach", "Дальность лазера", "How far the laser (rifle) goes. Default 800 (25 blocks).", "Как далеко бьёт лазер (винтовка). Обычно 800 (25 блоков)."},
	{"laser_bounce_num", GROUP_WEAPONS, UNIT_PLAIN, "Laser bounces", "Отскоки лазера", "How many times the laser bounces off walls.", "Сколько раз лазер отскакивает от стен."},
	{"laser_bounce_delay", GROUP_WEAPONS, UNIT_MS, "Laser bounce pause", "Пауза при отскоке", "How long the laser waits at every bounce, in milliseconds.", "Сколько лазер ждёт на каждом отскоке, в миллисекундах."},
	{"laser_bounce_cost", GROUP_WEAPONS, UNIT_PLAIN, "Laser bounce cost", "Цена отскока", "How much reach the laser loses at every bounce.", "Сколько дальности лазер теряет на каждом отскоке."},
	{"grenade_speed", GROUP_WEAPONS, UNIT_PLAIN, "Grenade speed", "Скорость гранаты", "How fast a grenade flies. Default 1000.", "Как быстро летит граната. Обычно 1000."},
	{"grenade_curvature", GROUP_WEAPONS, UNIT_PLAIN, "Grenade fall", "Падение гранаты", "How much a grenade falls on its way. 0: flies straight.", "Насколько граната падает в полёте. 0 — летит прямо."},
	{"grenade_lifetime", GROUP_WEAPONS, UNIT_SECONDS, "Grenade lifetime", "Время жизни гранаты", "Seconds before a grenade explodes in the air.", "Через сколько секунд граната взрывается в воздухе."},
	{"gun_speed", GROUP_WEAPONS, UNIT_PLAIN, "Pistol bullet speed", "Скорость пули", "How fast a pistol bullet flies. Default 2200.", "Как быстро летит пуля пистолета. Обычно 2200."},
	{"gun_curvature", GROUP_WEAPONS, UNIT_PLAIN, "Pistol bullet fall", "Падение пули", "How much a bullet falls on its way. 0: straight.", "Насколько пуля падает в полёте. 0 — прямо."},
	{"gun_lifetime", GROUP_WEAPONS, UNIT_SECONDS, "Pistol bullet lifetime", "Время жизни пули", "Seconds a bullet flies before it disappears.", "Сколько секунд пуля летит, пока не исчезнет."},
	{"shotgun_speed", GROUP_WEAPONS, UNIT_PLAIN, "Shotgun speed", "Скорость дробовика", "How fast the shotgun shot flies. Default 2750.", "Как быстро летит выстрел дробовика. Обычно 2750."},
	{"shotgun_curvature", GROUP_WEAPONS, UNIT_PLAIN, "Shotgun fall", "Падение дробовика", "How much the shotgun shot falls.", "Насколько падает выстрел дробовика."},
	{"hammer_fire_delay", GROUP_DELAYS, UNIT_MS, "Hammer (miss)", "Молоток (мимо)", "Pause between hammer hits into the air, in milliseconds. Default 125.", "Пауза между ударами молотка в пустоту, мс. Обычно 125."},
	{"hammer_hit_fire_delay", GROUP_DELAYS, UNIT_MS, "Hammer (hit)", "Молоток (попал)", "Pause after the hammer hits a tee, in milliseconds. Default 320.", "Пауза после удара по ти, мс. Обычно 320."},
	{"gun_fire_delay", GROUP_DELAYS, UNIT_MS, "Pistol", "Пистолет", "Pause between pistol shots, in milliseconds. Default 125.", "Пауза между выстрелами пистолета, мс. Обычно 125."},
	{"shotgun_fire_delay", GROUP_DELAYS, UNIT_MS, "Shotgun", "Дробовик", "Pause between shotgun shots, in milliseconds. Default 500.", "Пауза между выстрелами дробовика, мс. Обычно 500."},
	{"grenade_fire_delay", GROUP_DELAYS, UNIT_MS, "Grenade", "Граната", "Pause between grenades, in milliseconds. Default 500.", "Пауза между гранатами, мс. Обычно 500."},
	{"laser_fire_delay", GROUP_DELAYS, UNIT_MS, "Laser", "Лазер", "Pause between laser shots, in milliseconds. Default 800.", "Пауза между выстрелами лазера, мс. Обычно 800."},
	{"ninja_fire_delay", GROUP_DELAYS, UNIT_MS, "Ninja", "Ниндзя", "Pause between ninja dashes, in milliseconds. Default 800.", "Пауза между рывками ниндзя, мс. Обычно 800."},
	{"player_collision", GROUP_PLAYERS, UNIT_TOGGLE, "Tees collide", "Ти сталкиваются", "On: tees bump into each other and can stand on each other. Off: they pass through.", "Вкл — ти толкаются и могут стоять друг на друге. Выкл — проходят насквозь."},
	{"player_hooking", GROUP_PLAYERS, UNIT_TOGGLE, "Hook players", "Хук по игрокам", "On: the hook can catch other tees. Off: it flies through them.", "Вкл — хук цепляет других ти. Выкл — пролетает сквозь них."},
};

const char *GroupName(int Group)
{
	static const char *s_apEn[] = {"Movement", "Jumps", "Hook", "World", "Weapons", "Weapon pauses", "Players"};
	static const char *s_apRu[] = {"Движение", "Прыжки", "Хук", "Мир", "Оружие", "Паузы оружия", "Игроки"};
	static_assert(std::size(s_apEn) == NUM_GROUPS && std::size(s_apRu) == NUM_GROUPS);
	return IsRussian() ? s_apRu[Group] : s_apEn[Group];
}

float DefaultValue(const char *pScript)
{
	for(int i = 0; i < CTuningParams::Num(); i++)
	{
		if(str_comp(CTuningParams::Name(i), pScript) == 0)
		{
			float Value = 0.0f;
			CTuningParams::DEFAULT.Get(i, &Value);
			return Value;
		}
	}
	return 0.0f;
}

const STuneInfo *FindInfo(const char *pScript)
{
	for(const STuneInfo &Info : s_aTunes)
		if(str_comp(Info.m_pScript, pScript) == 0)
			return &Info;
	return nullptr;
}

// 0.5 -> "0.5", 10 -> "10", 0.0333 -> "0.0333"
void FormatValue(char *pBuf, int Size, float Value)
{
	str_format(pBuf, Size, "%.4f", Value);
	char *pDot = (char *)str_find(pBuf, ".");
	if(pDot)
	{
		char *pEnd = pBuf + str_length(pBuf) - 1;
		while(pEnd > pDot && *pEnd == '0')
			*pEnd-- = '\0';
		if(pEnd == pDot)
			*pEnd = '\0';
	}
}

// ready zones: one click sets a few parameters
struct SPreset
{
	const char *m_pNameEn;
	const char *m_pNameRu;
	const char *m_pIcon;
	struct SValue
	{
		const char *m_pScript;
		float m_Value;
	} m_aValues[4];
};
const SPreset s_aPresets[] = {
	{"Moon", "Луна", "\xef\x86\x86", {{"gravity", 0.15f}, {nullptr, 0}}},
	{"Space", "Космос", "\xef\x84\xb5", {{"gravity", 0.0f}, {"air_friction", 1.0f}, {nullptr, 0}}},
	{"Water", "Вода", "\xef\x9d\xb3", {{"gravity", 0.12f}, {"air_friction", 0.85f}, {"air_control_accel", 0.8f}, {nullptr, 0}}},
	{"Ice", "Лёд", "\xef\x8b\x9c", {{"ground_friction", 0.97f}, {"ground_control_accel", 0.6f}, {nullptr, 0}}},
	{"Trampoline", "Батут", "\xef\x81\xbd", {{"ground_elasticity_y", 0.9f}, {nullptr, 0}}},
	{"Super jump", "Супер-прыжок", "\xef\x81\xa2", {{"ground_jump_impulse", 20.0f}, {"air_jump_impulse", 18.0f}, {nullptr, 0}}},
	{"Long hook", "Длинный хук", "\xef\x83\x81", {{"hook_length", 800.0f}, {"hook_fire_speed", 120.0f}, {nullptr, 0}}},
	{"Fast run", "Быстрый бег", "\xef\x9c\x8c", {{"ground_control_speed", 20.0f}, {"ground_control_accel", 4.0f}, {nullptr, 0}}},
	{"Ghosts", "Призраки", "\xef\x9b\xa2", {{"player_collision", 0.0f}, {"player_hooking", 0.0f}, {nullptr, 0}}},
};
} // namespace

int CXcEditorUi::FindTuneSetting(int Zone, const char *pName, float *pValue) const
{
	const CEditorMap *pMap = m_pEditor->Map();
	for(int i = (int)pMap->m_vSettings.size() - 1; i >= 0; i--) // the last one wins, like on the server
	{
		const char *pCommand = pMap->m_vSettings[i].m_aCommand;
		char aCommand[32], aName[64];
		int CommandZone;
		float Value;
		if(Zone == 0)
		{
			if(std::sscanf(pCommand, "%31s %63s %f", aCommand, aName, &Value) == 3 && str_comp(aCommand, "tune") == 0 && str_comp(aName, pName) == 0)
			{
				if(pValue)
					*pValue = Value;
				return i;
			}
		}
		else if(std::sscanf(pCommand, "%31s %d %63s %f", aCommand, &CommandZone, aName, &Value) == 4 && str_comp(aCommand, "tune_zone") == 0 && CommandZone == Zone && str_comp(aName, pName) == 0)
		{
			if(pValue)
				*pValue = Value;
			return i;
		}
	}
	return -1;
}

int CXcEditorUi::FindTuneMessage(int Zone, bool Enter, char *pMessage, int MessageSize) const
{
	const CEditorMap *pMap = m_pEditor->Map();
	const char *pWanted = Enter ? "tune_zone_enter" : "tune_zone_leave";
	for(int i = (int)pMap->m_vSettings.size() - 1; i >= 0; i--)
	{
		const char *pCommand = pMap->m_vSettings[i].m_aCommand;
		char aCommand[32];
		int CommandZone, Offset = 0;
		if(std::sscanf(pCommand, "%31s %d %n", aCommand, &CommandZone, &Offset) >= 2 && Offset > 0 && str_comp(aCommand, pWanted) == 0 && CommandZone == Zone)
		{
			if(pMessage)
			{
				const char *pText = pCommand + Offset;
				str_copy(pMessage, pText, MessageSize);
				// "message" -> message
				const int Length = str_length(pMessage);
				if(Length >= 2 && pMessage[0] == '"' && pMessage[Length - 1] == '"')
				{
					pMessage[Length - 1] = '\0';
					mem_move(pMessage, pMessage + 1, Length - 1);
				}
			}
			return i;
		}
	}
	if(pMessage && MessageSize > 0)
		pMessage[0] = '\0';
	return -1;
}

void CXcEditorUi::SetTuneParam(int Zone, const char *pName, const float *pValue)
{
	CEditorMap *pMap = m_pEditor->Map();
	CEditorHistory &History = pMap->m_ServerSettingsHistory;
	int &Selected = pMap->m_MapSettingsCommandContext.m_CommandSelectedIndex;
	auto &vSettings = pMap->m_vSettings;
	const int Index = FindTuneSetting(Zone, pName, nullptr);
	if(!pValue)
	{
		if(Index < 0)
			return;
		History.RecordAction(std::make_shared<CEditorCommandAction>(pMap, CEditorCommandAction::EType::DELETE, &Selected, Index, vSettings[Index].m_aCommand));
		vSettings.erase(vSettings.begin() + Index);
		Selected = -1;
		pMap->OnModify();
		return;
	}
	char aValue[32], aCommand[256];
	FormatValue(aValue, sizeof(aValue), *pValue);
	if(Zone == 0)
		str_format(aCommand, sizeof(aCommand), "tune %s %s", pName, aValue);
	else
		str_format(aCommand, sizeof(aCommand), "tune_zone %d %s %s", Zone, pName, aValue);
	if(Index >= 0)
	{
		if(str_comp(vSettings[Index].m_aCommand, aCommand) == 0)
			return;
		History.RecordAction(std::make_shared<CEditorCommandAction>(pMap, CEditorCommandAction::EType::EDIT, &Selected, Index, vSettings[Index].m_aCommand, aCommand));
		str_copy(vSettings[Index].m_aCommand, aCommand);
	}
	else
	{
		vSettings.emplace_back(aCommand);
		History.RecordAction(std::make_shared<CEditorCommandAction>(pMap, CEditorCommandAction::EType::ADD, &Selected, (int)vSettings.size() - 1, aCommand));
	}
	pMap->OnModify();
}

void CXcEditorUi::SetTuneMessage(int Zone, bool Enter, const char *pMessage)
{
	CEditorMap *pMap = m_pEditor->Map();
	CEditorHistory &History = pMap->m_ServerSettingsHistory;
	int &Selected = pMap->m_MapSettingsCommandContext.m_CommandSelectedIndex;
	auto &vSettings = pMap->m_vSettings;
	const int Index = FindTuneMessage(Zone, Enter, nullptr, 0);
	if(!pMessage[0])
	{
		if(Index >= 0)
		{
			History.RecordAction(std::make_shared<CEditorCommandAction>(pMap, CEditorCommandAction::EType::DELETE, &Selected, Index, vSettings[Index].m_aCommand));
			vSettings.erase(vSettings.begin() + Index);
			Selected = -1;
			pMap->OnModify();
		}
		return;
	}
	char aCommand[256];
	str_format(aCommand, sizeof(aCommand), "%s %d \"%s\"", Enter ? "tune_zone_enter" : "tune_zone_leave", Zone, pMessage);
	if(Index >= 0)
	{
		if(str_comp(vSettings[Index].m_aCommand, aCommand) == 0)
			return;
		History.RecordAction(std::make_shared<CEditorCommandAction>(pMap, CEditorCommandAction::EType::EDIT, &Selected, Index, vSettings[Index].m_aCommand, aCommand));
		str_copy(vSettings[Index].m_aCommand, aCommand);
	}
	else
	{
		vSettings.emplace_back(aCommand);
		History.RecordAction(std::make_shared<CEditorCommandAction>(pMap, CEditorCommandAction::EType::ADD, &Selected, (int)vSettings.size() - 1, aCommand));
	}
	pMap->OnModify();
}

// select the tune layer (make it when there is none) and take a brush of this zone
void CXcEditorUi::PaintTuneZone(int Zone)
{
	CEditor &E = *m_pEditor;
	CEditorMap *pMap = E.Map();
	if(Zone <= 0)
		return;
	if(!pMap->m_pTuneLayer)
	{
		for(int g = 0; g < (int)pMap->m_vpGroups.size(); g++)
			if(pMap->m_vpGroups[g]->m_GameGroup)
				pMap->SelectLayer(0, g);
		E.m_QuickActionAddTuneLayer.Call();
	}
	for(int g = 0; g < (int)pMap->m_vpGroups.size(); g++)
		for(int l = 0; l < (int)pMap->m_vpGroups[g]->m_vpLayers.size(); l++)
			if(pMap->m_vpGroups[g]->m_vpLayers[l] == pMap->m_pTuneLayer)
				pMap->SelectLayer(l, g);
	if(!pMap->m_pTuneLayer)
		return;
	E.m_TuningNumber = Zone;
	E.m_pBrush->Clear();
	std::shared_ptr<CLayerTune> pBrush = std::make_shared<CLayerTune>(pMap, 1, 1);
	pBrush->m_pTiles[0].m_Index = TILE_TUNE;
	pBrush->m_pTuneTile[0].m_Number = Zone;
	pBrush->m_pTuneTile[0].m_Type = TILE_TUNE;
	pBrush->m_TuningNumber = Zone;
	str_copy(pBrush->m_aFilename, pMap->m_aFilename);
	E.m_pBrush->AddLayer(pBrush);
	E.m_XcTool = CEditor::XC_TOOL_BRUSH;
}

void CXcEditorUi::RenderTuneSummary()
{
	CEditor &E = *m_pEditor;
	const STheme &t = Theme();
	const int Zone = E.m_TuningNumber;
	char aTitle[64];
	str_format(aTitle, sizeof(aTitle), "%s %d", Tr("Zone"), Zone);
	ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
	const float Width = ImGui::GetContentRegionAvail().x;
	ImGui::PushFont(Fonts().m_pLabel);
	ImGui::TextColored(t.m_Text, "%s", aTitle);
	ImGui::PopFont();
	int Changed = 0;
	ImGui::PushFont(Fonts().m_pSmall);
	for(const STuneInfo &Info : s_aTunes)
	{
		float Value;
		if(FindTuneSetting(Zone, Info.m_pScript, &Value) < 0)
			continue;
		char aValue[32];
		FormatValue(aValue, sizeof(aValue), Value);
		ImGui::TextColored(t.m_TextSecondary, "%s", IsRussian() ? Info.m_pNameRu : Info.m_pNameEn);
		ImGui::SameLine(Width * 0.65f);
		ImGui::TextColored(t.m_Text, "%s", aValue);
		Changed++;
	}
	if(!Changed)
	{
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Width);
		ImGui::TextColored(t.m_TextMuted, "%s", Tr("Nothing changes in this zone yet: set it up below"));
		ImGui::PopTextWrapPos();
	}
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0.0f, S(2.0f)));
	// only a real click on the button: a click on a menu over the panel (the Map menu's "Bot: stop") opened it
	if(Button(Tr("Set up the tune zones..."), true, Width) && ImGui::IsItemHovered())
	{
		m_TuneRequest = true;
		m_TuneZone = Zone;
	}
}

void CXcEditorUi::RenderTuneZones()
{
	CEditor &E = *m_pEditor;
	CEditorMap *pMap = E.Map();
	const STheme &t = Theme();
	// opens once the mouse button is up, so the click that asked for it cannot land on a button inside
	if(m_TuneRequest && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		ImGui::OpenPopup("##xe_tune");
		m_TuneRequest = false;
		m_TuneMessagesZone = -1;
		m_TuneOpenTime = ImGui::GetTime();
	}
	const ImVec2 Display = ImGui::GetIO().DisplaySize;
	const float Width = std::min(S(980.0f), Display.x - S(40.0f));
	const float Height = std::min(S(680.0f), Display.y - S(60.0f));
	ImGui::SetNextWindowPos(ImVec2(Display.x * 0.5f, Display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(Width, Height));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(t.m_Card.x, t.m_Card.y, t.m_Card.z, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20.0f), S(18.0f)));
	if(!ImGui::BeginPopupModal("##xe_tune", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
		return;
	}
	ImFont *pSmall = Fonts().m_pSmall;
	ImFont *pLabel = Fonts().m_pLabel;
	// a double click or a touchpad tap must not close it the moment it appears
	const bool JustOpened = ImGui::GetTime() - m_TuneOpenTime < 0.25;
	ImGui::PushItemFlag(ImGuiItemFlags_Disabled, JustOpened);

	// the header
	Header(Tr("Tune zones"));
	Description(Tr("Paint a zone number on the tune layer, and inside it the physics changes: gravity, jumps, the hook... Zone 0 is the whole map. It is saved in the server settings of the map and works on every DDNet server."), Width - S(40.0f));
	ImGui::Dummy(ImVec2(0.0f, S(6.0f)));

	// zones: used on the tune layer or in the settings
	int aTiles[256] = {};
	if(pMap->m_pTuneLayer)
	{
		const CLayerTune *pTune = pMap->m_pTuneLayer.get();
		for(int i = 0; i < pTune->m_Width * pTune->m_Height; i++)
			if(pTune->m_pTiles[i].m_Index == TILE_TUNE)
				aTiles[pTune->m_pTuneTile[i].m_Number]++;
	}
	bool aUsed[256] = {};
	aUsed[0] = true;
	for(const auto &Setting : pMap->m_vSettings)
	{
		char aCommand[32];
		int Zone;
		if(std::sscanf(Setting.m_aCommand, "%31s %d", aCommand, &Zone) == 2 && str_startswith(aCommand, "tune_zone") && Zone >= 0 && Zone < 256)
			aUsed[Zone] = true;
	}
	for(int z = 1; z < 256; z++)
		aUsed[z] |= aTiles[z] > 0;
	aUsed[std::clamp(m_TuneZone, 0, 255)] = true;

	const float ListWidth = S(230.0f);
	const float BodyHeight = ImGui::GetContentRegionAvail().y - S(46.0f);
	ImGui::BeginChild("##tune_zones", ImVec2(ListWidth, BodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	{
		ImDrawList *pDraw = ImGui::GetWindowDrawList();
		const float RowWidth = ImGui::GetContentRegionAvail().x;
		const float RowHeight = S(44.0f);
		for(int z = 0; z < 256; z++)
		{
			if(!aUsed[z])
				continue;
			int Changed = 0;
			for(const STuneInfo &Info : s_aTunes)
				Changed += FindTuneSetting(z, Info.m_pScript, nullptr) >= 0;
			ImGui::PushID(z);
			const ImVec2 Min = ImGui::GetCursorScreenPos();
			const ImVec2 Max(Min.x + RowWidth, Min.y + RowHeight);
			if(ImGui::InvisibleButton("##zone", ImVec2(RowWidth, RowHeight)))
				m_TuneZone = z;
			const bool Selected = m_TuneZone == z;
			if(Selected)
				pDraw->AddRectFilled(Min, Max, Col(t.m_Accent, 0.2f), S(8.0f));
			else if(ImGui::IsItemHovered())
				pDraw->AddRectFilled(Min, Max, Col(t.m_Track, 0.6f), S(8.0f));
			// the number in a round badge
			char aNumber[8];
			str_format(aNumber, sizeof(aNumber), "%d", z);
			const ImVec2 Badge(Min.x + S(20.0f), Min.y + RowHeight * 0.5f);
			pDraw->AddCircleFilled(Badge, S(13.0f), Col(z == 0 ? t.m_Track : t.m_Accent, z == 0 ? 1.0f : 0.85f));
			const ImVec2 NumberSize = pLabel->CalcTextSizeA(pLabel->FontSize * 0.9f, FLT_MAX, 0.0f, aNumber);
			pDraw->AddText(pLabel, pLabel->FontSize * 0.9f, ImVec2(Badge.x - NumberSize.x * 0.5f, Badge.y - NumberSize.y * 0.5f), Col(z == 0 ? t.m_Text : t.m_OnAccent), aNumber);
			char aName[64], aSub[96];
			if(z == 0)
			{
				str_copy(aName, Tr("Whole map"));
				str_format(aSub, sizeof(aSub), "%s: %d", Tr("changes"), Changed);
			}
			else
			{
				str_format(aName, sizeof(aName), "%s %d", Tr("Zone"), z);
				str_format(aSub, sizeof(aSub), "%s: %d · %s: %d", Tr("blocks"), aTiles[z], Tr("changes"), Changed);
			}
			pDraw->AddText(pLabel, pLabel->FontSize, ImVec2(Min.x + S(42.0f), Min.y + S(6.0f)), Col(Selected ? t.m_Text : t.m_TextSecondary), aName);
			pDraw->AddText(pSmall, pSmall->FontSize * 0.9f, ImVec2(Min.x + S(42.0f), Min.y + S(24.0f)), Col(t.m_TextMuted), aSub);
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
		if(Button(Tr("+ New zone"), false, RowWidth))
		{
			for(int z = 1; z < 256; z++)
			{
				if(!aUsed[z])
				{
					m_TuneZone = z;
					break;
				}
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine(0.0f, S(16.0f));

	// the selected zone
	const int Zone = std::clamp(m_TuneZone, 0, 255);
	ImGui::BeginChild("##tune_params", ImVec2(0.0f, BodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	{
		const float Inner = ImGui::GetContentRegionAvail().x - S(8.0f);
		char aTitle[64];
		if(Zone == 0)
			str_copy(aTitle, Tr("Whole map"));
		else
			str_format(aTitle, sizeof(aTitle), "%s %d", Tr("Zone"), Zone);
		Title(aTitle);
		if(Zone > 0)
		{
			// paint it and see it
			if(Button(Tr("Paint this zone"), true, S(170.0f)))
			{
				PaintTuneZone(Zone);
				ImGui::CloseCurrentPopup();
			}
			if(ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Tr("Selects the tune layer (makes it when there is none) and gives you a brush of this zone"));
			ImGui::SameLine(0.0f, S(6.0f));
			const bool HasTiles = aTiles[Zone] > 0;
			ImGui::BeginDisabled(!HasTiles);
			if(Button(Tr("Show on the map"), false, S(170.0f)))
			{
				ivec2 Pos;
				pMap->m_pTuneLayer->GetPos(Zone, -1, Pos);
				if(Pos != ivec2(-1, -1))
					E.MapView()->SetWorldOffset({32.0f * (Pos.x + 0.5f), 32.0f * (Pos.y + 0.5f)});
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine(0.0f, S(6.0f));
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
		if(Button(Tr("Reset the zone"), false, S(150.0f)))
		{
			pMap->m_ServerSettingsHistory.BeginBulk();
			for(const STuneInfo &Info : s_aTunes)
				SetTuneParam(Zone, Info.m_pScript, nullptr);
			pMap->m_ServerSettingsHistory.EndBulk("Reset the tune zone");
		}
		ImGui::PopStyleColor();
		ImGui::Dummy(ImVec2(0.0f, S(6.0f)));

		// presets
		ParamLabel(Tr("Ready zones"));
		const float PresetWidth = (Inner - S(6.0f) * 2) / 3.0f;
		for(int i = 0; i < (int)std::size(s_aPresets); i++)
		{
			const SPreset &Preset = s_aPresets[i];
			if(i % 3)
				ImGui::SameLine(0.0f, S(6.0f));
			char aLabel[64];
			str_format(aLabel, sizeof(aLabel), "%s  %s", Preset.m_pIcon, IsRussian() ? Preset.m_pNameRu : Preset.m_pNameEn);
			ImGui::PushID(i);
			if(Button(aLabel, false, PresetWidth))
			{
				pMap->m_ServerSettingsHistory.BeginBulk();
				for(const auto &Value : Preset.m_aValues)
					if(Value.m_pScript)
						SetTuneParam(Zone, Value.m_pScript, &Value.m_Value);
				pMap->m_ServerSettingsHistory.EndBulk("Tune zone preset");
			}
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

		// messages when entering and leaving
		if(m_TuneMessagesZone != Zone)
		{
			FindTuneMessage(Zone, true, m_aTuneEnter, sizeof(m_aTuneEnter));
			FindTuneMessage(Zone, false, m_aTuneLeave, sizeof(m_aTuneLeave));
			m_TuneMessagesZone = Zone;
		}
		ParamLabel(Zone == 0 ? Tr("Messages when coming back to the normal area") : Tr("Messages in the chat"));
		const float Half = (Inner - S(8.0f)) * 0.5f;
		InputText("##enter", m_aTuneEnter, sizeof(m_aTuneEnter), Tr("When entering: «Welcome to the moon!»"), Half);
		if(ImGui::IsItemDeactivatedAfterEdit())
			SetTuneMessage(Zone, true, m_aTuneEnter);
		ImGui::SameLine(0.0f, S(8.0f));
		InputText("##leave", m_aTuneLeave, sizeof(m_aTuneLeave), Tr("When leaving: «Back to the earth»"), Half);
		if(ImGui::IsItemDeactivatedAfterEdit())
			SetTuneMessage(Zone, false, m_aTuneLeave);
		ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

		InputText("##filter", m_aTuneFilter, sizeof(m_aTuneFilter), Tr("Search a parameter"), Inner);
		ImGui::Dummy(ImVec2(0.0f, S(4.0f)));

		// the parameters by groups: the name, the explanation under it, the value on the right
		ImDrawList *pDraw = ImGui::GetWindowDrawList();
		const float ValueWidth = S(130.0f);
		for(int Group = 0; Group < NUM_GROUPS; Group++)
		{
			bool GroupShown = false;
			for(const STuneInfo &Info : s_aTunes)
			{
				if(Info.m_Group != Group)
					continue;
				const char *pName = IsRussian() ? Info.m_pNameRu : Info.m_pNameEn;
				const char *pHelp = IsRussian() ? Info.m_pHelpRu : Info.m_pHelpEn;
				if(m_aTuneFilter[0] && !str_utf8_find_nocase(pName, m_aTuneFilter) && !str_utf8_find_nocase(pHelp, m_aTuneFilter) && !str_find_nocase(Info.m_pScript, m_aTuneFilter))
					continue;
				if(!GroupShown)
				{
					ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
					ImGui::PushFont(pLabel);
					ImGui::TextColored(t.m_Accent, "%s", GroupName(Group));
					ImGui::PopFont();
					GroupShown = true;
				}
				const float Default = DefaultValue(Info.m_pScript);
				float Value = Default;
				const bool Changed = FindTuneSetting(Zone, Info.m_pScript, &Value) >= 0;
				ImGui::PushID(Info.m_pScript);
				const ImVec2 RowMin = ImGui::GetCursorScreenPos();
				const float TextWidth = Inner - ValueWidth - S(40.0f);
				// the text
				const ImVec2 HelpSize = pSmall->CalcTextSizeA(pSmall->FontSize * 0.92f, FLT_MAX, TextWidth, pHelp);
				const float RowHeight = std::max(S(40.0f), S(22.0f) + HelpSize.y + S(8.0f));
				pDraw->AddRectFilled(RowMin, ImVec2(RowMin.x + Inner, RowMin.y + RowHeight), Col(t.m_Track, Changed ? 0.55f : 0.25f), S(8.0f));
				if(Changed)
					pDraw->AddRectFilled(ImVec2(RowMin.x, RowMin.y + S(6.0f)), ImVec2(RowMin.x + S(3.0f), RowMin.y + RowHeight - S(6.0f)), Col(t.m_Accent), S(2.0f));
				pDraw->AddText(pLabel, pLabel->FontSize, ImVec2(RowMin.x + S(12.0f), RowMin.y + S(6.0f)), Col(Changed ? t.m_Text : t.m_TextSecondary), pName);
				pDraw->AddText(pSmall, pSmall->FontSize * 0.92f, ImVec2(RowMin.x + S(12.0f), RowMin.y + S(24.0f)), Col(t.m_TextMuted), pHelp, nullptr, TextWidth);
				// the value
				const float ControlX = RowMin.x + Inner - ValueWidth - S(34.0f);
				ImGui::SetCursorScreenPos(ImVec2(ControlX, RowMin.y + (RowHeight - S(26.0f)) * 0.5f));
				if(Info.m_Unit == UNIT_TOGGLE)
				{
					bool On = Value != 0.0f;
					if(Toggle("##on", &On))
					{
						const float NewValue = On ? 1.0f : 0.0f;
						SetTuneParam(Zone, Info.m_pScript, NewValue == Default ? nullptr : &NewValue);
					}
				}
				else
				{
					ImGui::SetNextItemWidth(ValueWidth);
					ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(t.m_Background.x, t.m_Background.y, t.m_Background.z, 0.8f));
					ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.0f));
					float Edit = Value;
					const float Speed = std::max(std::abs(Default), 1.0f) * 0.01f;
					char aFormat[48];
					if(Info.m_Unit == UNIT_BLOCKS)
						str_format(aFormat, sizeof(aFormat), "%%.0f (%.1f %s)", Value / 32.0f, Tr("bl."));
					else if(Info.m_Unit == UNIT_MS)
						str_format(aFormat, sizeof(aFormat), "%%.0f %s", Tr("ms"));
					else if(Info.m_Unit == UNIT_SECONDS)
						str_format(aFormat, sizeof(aFormat), "%%.2f %s", Tr("s"));
					else
						str_copy(aFormat, std::abs(Default) < 10.0f ? "%.3g" : "%.1f");
					ImGui::DragFloat("##value", &Edit, Speed, 0.0f, 0.0f, aFormat);
					HoldMouseWhileDragging();
					// the value is written when the drag or the typing ends: one step of Ctrl+Z
					static float s_Pending = 0.0f;
					if(ImGui::IsItemActive())
						s_Pending = Edit;
					if(ImGui::IsItemDeactivatedAfterEdit())
						SetTuneParam(Zone, Info.m_pScript, std::abs(s_Pending - Default) < 1e-6f ? nullptr : &s_Pending);
					ImGui::PopStyleVar();
					ImGui::PopStyleColor();
					if(ImGui::IsItemHovered())
					{
						char aDefault[32];
						FormatValue(aDefault, sizeof(aDefault), Default);
						ImGui::SetTooltip("%s · %s %s · %s", Info.m_pScript, Tr("default"), aDefault, Tr("drag or double click to type"));
					}
				}
				// back to the default
				if(Changed)
				{
					ImGui::SetCursorScreenPos(ImVec2(RowMin.x + Inner - S(30.0f), RowMin.y + (RowHeight - S(26.0f)) * 0.5f));
					if(IconButton("##reset", FontIcon::ARROW_ROTATE_LEFT, Tr("Back to the default"), false, true, S(26.0f)))
						SetTuneParam(Zone, Info.m_pScript, nullptr);
				}
				ImGui::SetCursorScreenPos(ImVec2(RowMin.x, RowMin.y + RowHeight + S(4.0f)));
				ImGui::Dummy(ImVec2(Inner, 0.0f));
				ImGui::PopID();
			}
		}
	}
	ImGui::EndChild();

	ImGui::Dummy(ImVec2(0.0f, S(6.0f)));
	if(Button(Tr("Done"), true, Width - S(40.0f)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_Typing))
		ImGui::CloseCurrentPopup();
	ImGui::PopItemFlag();
	ImGui::EndPopup();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(2);
}
