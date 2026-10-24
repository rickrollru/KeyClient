// The window of the shared editing (open a room, join a room, who is here) and the cursors of the others
// over the map.
#include "xeditor.h"

#include <game/client/components/xclient/ui/feedback.h>

#include <base/io.h>
#include <base/str.h>

#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>

#include <game/client/components/xclient/ui/lang.h>
#include <game/client/components/xclient/ui/theme.h>
#include <game/client/components/xclient/ui/widgets.h>
#include <game/editor/editor.h>
#include <game/editor/mapitems/layer_group.h>

#include <engine/external/imgui/imgui.h>

#include <algorithm>
#include <cmath>

using namespace XcUi;

static ImVec4 MemberColor(int Id, float Alpha = 1.0f)
{
	const unsigned Rgb = CXcCoop::MemberColor(Id);
	return ImVec4(((Rgb >> 16) & 0xff) / 255.0f, ((Rgb >> 8) & 0xff) / 255.0f, (Rgb & 0xff) / 255.0f, Alpha);
}

void CXcEditorUi::UpdateCoop()
{
	m_Coop.Update();
	if(m_Coop.TakeMapReplaced())
	{
		// pointers into the old map
		m_pAdjustImage = nullptr;
		m_pLastQuads = nullptr;
		m_pLastBrushLayer = nullptr;
		m_pEnvLast = nullptr;
		m_EnvSelPoint = -1;
		m_EnvDragging = false;
		m_RenameGroup = -1;
		m_RenameLayer = -1;
		for(SCaptureState &Capture : m_aCaptures)
			Capture = SCaptureState();
	}
	const std::string Notice = m_Coop.TakeNotice();
	if(!Notice.empty())
		ShowMessage(Notice.c_str(), "Edit together");
}

void CXcEditorUi::RenderCoop()
{
	const STheme &t = Theme();
	if(m_CoopRequest)
	{
		ImGui::OpenPopup("##xe_coop");
		m_CoopRequest = false;
		m_aCoopError[0] = '\0';
		str_format(m_aCoopPort, sizeof(m_aCoopPort), "%d", g_Config.m_XcCoopPort);
		str_copy(m_aCoopAddress, g_Config.m_XcCoopAddress);
	}
	const ImVec2 Display = ImGui::GetIO().DisplaySize;
	const float Width = std::min(S(520.0f), Display.x - S(40.0f));
	const float Inner = Width - S(40.0f);
	ImGui::SetNextWindowPos(ImVec2(Display.x * 0.5f, Display.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(Width, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(t.m_Card.x, t.m_Card.y, t.m_Card.z, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20.0f), S(18.0f)));
	if(ImGui::BeginPopupModal("##xe_coop", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
	{
		bool Close = false;
		Header(Tr("Edit together"));
		Description(Tr("Maps with friends over LAN or Radmin VPN. Every tab is shared, each player works in any tab. Blocks show up right away, layers, quads and envelopes a moment after you stop."), Inner);
		ImGui::Dummy(ImVec2(0.0f, S(8.0f)));

		ImDrawList *pDraw = ImGui::GetWindowDrawList();
		const int Role = m_Coop.Role();
		if(Role == CXcCoop::ROLE_NONE)
		{
			// open a room
			ParamLabel(Tr("Open a room with all your tabs"));
			const float PortWidth = S(90.0f);
			InputText("##coop_port", m_aCoopPort, sizeof(m_aCoopPort), Tr("Port"), PortWidth);
			ImGui::SameLine(0.0f, S(8.0f));
			if(Button(Tr("Open a room"), true, Inner - PortWidth - S(8.0f)))
			{
				const int Port = std::clamp(str_toint(m_aCoopPort), 1024, 65535);
				char aError[128];
				if(m_Coop.Host(Port, aError, sizeof(aError)))
				{
					g_Config.m_XcCoopPort = Port;
					m_aCoopError[0] = '\0';
				}
				else
					str_copy(m_aCoopError, Tr(aError));
			}
			ImGui::Dummy(ImVec2(0.0f, S(2.0f)));
			Description(Tr("Friends connect to your IP. In Radmin VPN it is the 26.x.x.x address next to your name. If Windows asks about the network, allow it."), Inner);
			ImGui::Dummy(ImVec2(0.0f, S(10.0f)));

			// join a room
			ParamLabel(Tr("Join a friend's room"));
			const float JoinWidth = S(130.0f);
			const bool Enter = InputText("##coop_address", m_aCoopAddress, sizeof(m_aCoopAddress), "26.12.34.56:8314", Inner - JoinWidth - S(8.0f)) && ImGui::IsKeyPressed(ImGuiKey_Enter);
			ImGui::SameLine(0.0f, S(8.0f));
			if((Button(Tr("Join"), false, JoinWidth) || Enter) && m_aCoopAddress[0])
			{
				char aError[128];
				if(m_Coop.Join(m_aCoopAddress, aError, sizeof(aError)))
				{
					str_copy(g_Config.m_XcCoopAddress, m_aCoopAddress);
					m_aCoopError[0] = '\0';
				}
				else
					str_copy(m_aCoopError, Tr(aError));
			}
			ImGui::Dummy(ImVec2(0.0f, S(2.0f)));
			Description(Tr("The maps of the room open in new tabs, your own tabs stay yours. A tab you open later is shared, a closed one closes for everyone."), Inner);
			if(m_aCoopError[0])
			{
				ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Inner);
				ImGui::TextUnformatted(m_aCoopError);
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
		}
		else
		{
			// the state of the session
			char aStatus[256];
			const int State = m_Coop.State();
			if(Role == CXcCoop::ROLE_HOST)
				str_format(aStatus, sizeof(aStatus), Tr("The room is open on port %d. Friends join with your IP and this port."), m_Coop.Port());
			else if(State == CXcCoop::STATE_CONNECTING)
				str_format(aStatus, sizeof(aStatus), Tr("Connecting to %s..."), m_Coop.Address());
			else if(State == CXcCoop::STATE_JOINING)
			{
				if(m_Coop.Progress() >= 0.0f)
					str_format(aStatus, sizeof(aStatus), Tr("Getting the map... %d%%"), (int)(m_Coop.Progress() * 100.0f));
				else
					str_copy(aStatus, Tr("Getting the map..."));
			}
			else
				str_format(aStatus, sizeof(aStatus), Tr("Connected to %s"), m_Coop.Address());
			{
				const ImVec2 Pos = ImGui::GetCursorScreenPos();
				const float Pulse = 0.6f + 0.4f * std::sin(ImGui::GetTime() * 4.0f);
				const ImVec4 Dot = State == CXcCoop::STATE_ONLINE ? ImVec4(0.24f, 0.84f, 0.64f, 1.0f) : ImVec4(1.0f, 0.75f, 0.25f, Pulse);
				ImFont *pLabel = Fonts().m_pLabel;
				pDraw->AddCircleFilled(ImVec2(Pos.x + S(5.0f), Pos.y + pLabel->FontSize * 0.5f), S(4.0f), Col(Dot));
				ImGui::SetCursorScreenPos(ImVec2(Pos.x + S(16.0f), Pos.y));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Inner - S(16.0f));
				ImGui::PushStyleColor(ImGuiCol_Text, t.m_Text);
				ImGui::TextUnformatted(aStatus);
				ImGui::PopStyleColor();
				ImGui::PopTextWrapPos();
			}
			ImGui::Dummy(ImVec2(0.0f, S(10.0f)));

			// who is here
			ParamLabel(Tr("In the room"));
			for(const CXcCoop::SMember &Member : m_Coop.Members())
			{
				const ImVec2 Min = ImGui::GetCursorScreenPos();
				const float Row = S(30.0f);
				PanelRect(pDraw, Min, ImVec2(Min.x + Inner, Min.y + Row), S(8.0f), t.m_Panel);
				pDraw->AddCircleFilled(ImVec2(Min.x + S(15.0f), Min.y + Row * 0.5f), S(6.0f), Col(MemberColor(Member.m_Id)));
				char aName[96];
				str_format(aName, sizeof(aName), "%s%s%s", Member.m_Name.c_str(), Member.m_Id == m_Coop.SelfId() ? Tr(" (you)") : "", Member.m_Id == 0 ? Tr(" · host") : "");
				ImFont *pLabel = Fonts().m_pLabel;
				pDraw->AddText(pLabel, pLabel->FontSize, ImVec2(Min.x + S(30.0f), Min.y + (Row - pLabel->FontSize) * 0.5f), Col(t.m_Text), aName);
				// the tab the member is in
				const uint32_t MemberMap = Member.m_Id == m_Coop.SelfId() ? m_Coop.MapId(m_pEditor->Map()) : (Member.m_HasCursor ? Member.m_MapId : 0);
				for(const auto &pMap : m_pEditor->m_vpMaps)
				{
					if(MemberMap == 0 || m_Coop.MapId(pMap.get()) != MemberMap)
						continue;
					ImFont *pSmall = Fonts().m_pSmall;
					const float TabWidth = pSmall->CalcTextSizeA(pSmall->FontSize, FLT_MAX, 0.0f, pMap->m_aDisplayName).x;
					pDraw->AddText(pSmall, pSmall->FontSize, ImVec2(Min.x + Inner - TabWidth - S(12.0f), Min.y + (Row - pSmall->FontSize) * 0.5f), Col(t.m_TextMuted), pMap->m_aDisplayName);
				}
				ImGui::Dummy(ImVec2(Inner, Row));
				ImGui::Dummy(ImVec2(0.0f, S(2.0f)));
			}
			if(m_Coop.Members().empty())
				Description(Tr("Nobody yet"), Inner);
			ImGui::Dummy(ImVec2(0.0f, S(8.0f)));
			if(Button(Role == CXcCoop::ROLE_HOST ? Tr("Close the room") : Tr("Leave the room"), false, Inner))
				m_Coop.Leave();
		}
		ImGui::Dummy(ImVec2(0.0f, S(6.0f)));
		if(Button(Tr("Close"), false, Inner) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_Typing))
			Close = true;
		if(Close)
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(2);
}

void CXcEditorUi::RenderCoopCursors()
{
	CEditor &E = *m_pEditor;
	const CEditorMap *pMap = E.Map();
	const uint32_t MapId = m_Coop.MapId(pMap);
	if(m_Coop.State() != CXcCoop::STATE_ONLINE || MapId == 0)
		return;
	// world (without parallax) -> pixels through the game group, like the editor does for the mouse
	const CLayerGroup *pGame = nullptr;
	for(const auto &pGroup : pMap->m_vpGroups)
		if(pGroup->m_GameGroup)
			pGame = pGroup.get();
	if(!pGame)
		return;
	const CScreenRect Rect = pGame->Mapping();
	if(Rect.Width() <= 0.0f || Rect.Height() <= 0.0f)
		return;
	const ImVec2 Display = ImGui::GetIO().DisplaySize;
	const float k = UiToPixels();
	const ImVec2 ClipMin(m_View.x * k, m_View.y * k);
	const ImVec2 ClipMax((m_View.x + m_View.w) * k, (m_View.y + m_View.h) * k);
	ImDrawList *pDraw = ImGui::GetBackgroundDrawList();
	pDraw->PushClipRect(ClipMin, ClipMax, true);
	ImFont *pSmall = Fonts().m_pSmall;
	const float Time = E.Client()->GlobalTime();
	for(const CXcCoop::SMember &Member : m_Coop.Members())
	{
		// only the ones in this tab
		if(Member.m_Id == m_Coop.SelfId() || !Member.m_HasCursor || Member.m_MapId != MapId || Member.m_HasTee)
			continue;
		const ImVec2 Pos((Member.m_Cursor.x - Rect.m_TopLeft.x) / Rect.Width() * Display.x, (Member.m_Cursor.y - Rect.m_TopLeft.y) / Rect.Height() * Display.y);
		const float Alpha = Time - Member.m_LastSeen > 20.0f ? 0.45f : 1.0f;
		const ImVec4 Color = MemberColor(Member.m_Id, Alpha);
		// an arrow like the system cursor
		const float s = S(1.0f);
		const ImVec2 aArrow[] = {
			Pos,
			ImVec2(Pos.x, Pos.y + 17.0f * s),
			ImVec2(Pos.x + 4.5f * s, Pos.y + 13.0f * s),
			ImVec2(Pos.x + 8.0f * s, Pos.y + 20.0f * s),
			ImVec2(Pos.x + 11.0f * s, Pos.y + 18.5f * s),
			ImVec2(Pos.x + 7.5f * s, Pos.y + 12.0f * s),
			ImVec2(Pos.x + 13.0f * s, Pos.y + 12.0f * s),
		};
		pDraw->AddConcavePolyFilled(aArrow, (int)std::size(aArrow), Col(Color));
		pDraw->AddPolyline(aArrow, (int)std::size(aArrow), Col(ImVec4(0.04f, 0.04f, 0.06f, 0.9f * Alpha)), ImDrawFlags_Closed, 1.2f * s);

		// the name and the layer in a pill
		char aLabel[128];
		str_copy(aLabel, Member.m_Name.c_str());
		if(Member.m_Group >= 0 && Member.m_Group < (int)pMap->m_vpGroups.size())
		{
			const auto &vpLayers = pMap->m_vpGroups[Member.m_Group]->m_vpLayers;
			if(Member.m_Layer >= 0 && Member.m_Layer < (int)vpLayers.size())
			{
				const char *pLayerName = vpLayers[Member.m_Layer]->m_aName;
				char aLayer[32];
				if(!pLayerName[0])
				{
					str_format(aLayer, sizeof(aLayer), "#%d", Member.m_Layer);
					pLayerName = aLayer;
				}
				str_append(aLabel, " · ");
				str_append(aLabel, pLayerName);
			}
		}
		const ImVec2 TextSize = pSmall->CalcTextSizeA(pSmall->FontSize, FLT_MAX, 0.0f, aLabel);
		const ImVec2 PillMin(Pos.x + 12.0f * s, Pos.y + 18.0f * s);
		const ImVec2 PillMax(PillMin.x + TextSize.x + S(14.0f), PillMin.y + TextSize.y + S(6.0f));
		pDraw->AddRectFilled(PillMin, PillMax, Col(Color), S(6.0f));
		const float Light = Color.x * 0.3f + Color.y * 0.59f + Color.z * 0.11f;
		pDraw->AddText(pSmall, pSmall->FontSize, ImVec2(PillMin.x + S(7.0f), PillMin.y + S(3.0f)), Col(Light > 0.6f ? ImVec4(0.05f, 0.06f, 0.08f, Alpha) : ImVec4(1.0f, 1.0f, 1.0f, Alpha)), aLabel);
	}
	// the friends who play the map: a pill with the name over their tee
	for(const CXcCoop::SMember &Member : m_Coop.Members())
	{
		if(Member.m_Id == m_Coop.SelfId() || !Member.m_HasTee || Member.m_TeeMapId != MapId)
			continue;
		const vec2 World = Member.m_TeeDrawPos - vec2(0.0f, 46.0f);
		const ImVec2 Pos((World.x - Rect.m_TopLeft.x) / Rect.Width() * Display.x, (World.y - Rect.m_TopLeft.y) / Rect.Height() * Display.y);
		const ImVec4 Color = MemberColor(Member.m_Id);
		char aLabel[128];
		str_format(aLabel, sizeof(aLabel), "%s · %s", Member.m_Name.c_str(), Tr("playing"));
		const ImVec2 TextSize = pSmall->CalcTextSizeA(pSmall->FontSize, FLT_MAX, 0.0f, aLabel);
		const ImVec2 PillMin(Pos.x - TextSize.x * 0.5f - S(7.0f), Pos.y - TextSize.y - S(6.0f));
		const ImVec2 PillMax(Pos.x + TextSize.x * 0.5f + S(7.0f), Pos.y);
		pDraw->AddRectFilled(PillMin, PillMax, Col(Color), S(6.0f));
		const float Light = Color.x * 0.3f + Color.y * 0.59f + Color.z * 0.11f;
		pDraw->AddText(pSmall, pSmall->FontSize, ImVec2(PillMin.x + S(7.0f), PillMin.y + S(3.0f)), Col(Light > 0.6f ? ImVec4(0.05f, 0.06f, 0.08f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f)), aLabel);
	}
	pDraw->PopClipRect();
}

// the friends who play the map, drawn with their skin where their tee is (under the panels of the editor)
void CXcEditorUi::RenderCoopTees()
{
	CEditor &E = *m_pEditor;
	const uint32_t MapId = m_Coop.MapId(E.Map());
	if(m_Coop.State() != CXcCoop::STATE_ONLINE || MapId == 0)
		return;
	CLayerGroup *pGame = nullptr;
	for(const auto &pGroup : E.Map()->m_vpGroups)
		if(pGroup->m_GameGroup)
			pGame = pGroup.get();
	if(!pGame)
		return;
	CGameClient *pGameClient = (CGameClient *)E.Kernel()->RequestInterface<IGameClient>();
	bool Mapped = false;
	for(const CXcCoop::SMember &Member : m_Coop.Members())
	{
		if(Member.m_Id == m_Coop.SelfId() || !Member.m_HasTee || Member.m_TeeMapId != MapId)
			continue;
		if(!Mapped)
		{
			pGame->MapScreen();
			Mapped = true;
		}
		CTeeRenderInfo Info;
		Info.Apply(pGameClient->m_Skins.Find(Member.m_Skin.c_str()));
		Info.ApplyColors(Member.m_CustomColor, Member.m_ColorBody, Member.m_ColorFeet);
		Info.m_Size = 64.0f;
		pGameClient->RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, EMOTE_NORMAL, direction(Member.m_TeeAngle), Member.m_TeeDrawPos);
	}
	if(Mapped)
		E.Ui()->MapScreen();
}

// in the game, playing the map together: the cursors of the friends who edit it, in the world
void CXcEditorUi::RenderInGame()
{
	if(m_Coop.State() != CXcCoop::STATE_ONLINE || !m_Coop.PlayingNow())
		return;
	CEditor &E = *m_pEditor;
	CGameClient *pGameClient = (CGameClient *)E.Kernel()->RequestInterface<IGameClient>();
	IGraphics *pGraphics = E.Graphics();
	ITextRender *pTextRender = E.TextRender();
	const float Time = E.Client()->GlobalTime();
	bool Mapped = false;
	for(const CXcCoop::SMember &Member : m_Coop.Members())
	{
		// only the ones in the editor with the played map open, who moved lately
		if(Member.m_Id == m_Coop.SelfId() || !Member.m_HasCursor || Member.m_HasTee || Member.m_MapId != m_Coop.PlayingMapId() || Time - Member.m_LastSeen > 30.0f)
			continue;
		if(!Mapped)
		{
			pGraphics->MapScreenToInterface(pGameClient->m_Camera.m_Center.x, pGameClient->m_Camera.m_Center.y, pGameClient->m_Camera.m_Zoom);
			Mapped = true;
		}
		const unsigned Rgb = CXcCoop::MemberColor(Member.m_Id);
		const ColorRGBA Color(((Rgb >> 16) & 0xff) / 255.0f, ((Rgb >> 8) & 0xff) / 255.0f, (Rgb & 0xff) / 255.0f, 1.0f);
		const vec2 p = Member.m_Cursor;
		const float s = 1.6f; // world units per pixel of the arrow
		// an arrow like the system cursor: a dark rim, then the color
		pGraphics->TextureClear();
		pGraphics->TrianglesBegin();
		for(int Pass = 0; Pass < 2; Pass++)
		{
			const float Grow = Pass == 0 ? 1.8f : 0.0f;
			pGraphics->SetColor(Pass == 0 ? ColorRGBA(0.04f, 0.04f, 0.06f, 0.9f) : Color);
			const IGraphics::CFreeformItem aItems[] = {
				IGraphics::CFreeformItem(p.x - Grow, p.y - Grow * 2.0f, p.x - Grow, p.y + (17.0f + Grow) * s, p.x + (13.0f + Grow * 1.5f) * s, p.y + (12.0f + Grow * 0.5f) * s, p.x + 4.5f * s, p.y + 13.0f * s),
				IGraphics::CFreeformItem(p.x + (4.5f - Grow * 0.5f) * s, p.y + 11.0f * s, p.x + (7.5f + Grow * 0.5f) * s, p.y + (11.0f - Grow * 0.3f) * s, p.x + (8.0f - Grow * 0.4f) * s, p.y + (20.0f + Grow * 0.5f) * s, p.x + (11.0f + Grow * 0.5f) * s, p.y + (18.5f + Grow * 0.3f) * s),
			};
			pGraphics->QuadsDrawFreeform(aItems, (int)std::size(aItems));
		}
		pGraphics->TrianglesEnd();
		// the name and what he does
		char aLabel[128];
		str_format(aLabel, sizeof(aLabel), "%s · %s", Member.m_Name.c_str(), Tr("editing"));
		const float FontSize = 14.0f;
		const float Width = pTextRender->TextWidth(FontSize, aLabel);
		const vec2 LabelPos = p + vec2(14.0f * s, 20.0f * s);
		pGraphics->TextureClear();
		pGraphics->DrawRect(LabelPos.x, LabelPos.y, Width + 12.0f, FontSize + 8.0f, Color.WithAlpha(0.9f), IGraphics::CORNER_ALL, 6.0f);
		const float Light = Color.r * 0.3f + Color.g * 0.59f + Color.b * 0.11f;
		pTextRender->TextColor(Light > 0.6f ? ColorRGBA(0.05f, 0.06f, 0.08f, 1.0f) : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
		pTextRender->Text(LabelPos.x + 6.0f, LabelPos.y + 4.0f, FontSize, aLabel);
		pTextRender->TextColor(pTextRender->DefaultTextColor());
	}
}


// game time as on the race timer: 1:23.45
static void FormatRaceTime(char *pBuf, int Size, int Ticks)
{
	const int Centis = Ticks * 2; // 50 ticks a second
	str_format(pBuf, Size, "%d:%02d.%02d", Centis / 6000, (Centis / 100) % 60, Centis % 100);
}

void CXcEditorUi::StartSolve(int TowerDepth, const char *pLevel, bool Forgiving, bool Ideal)
{
	CEditor &E = *m_pEditor;
	const std::shared_ptr<CLayerGame> &pGame = E.Map()->m_pGameLayer;
	if(!pGame)
		return;
	const std::shared_ptr<CLayerFront> &pFront = E.Map()->m_pFrontLayer;
	m_vSolvePath.clear();
	const std::shared_ptr<CLayerTele> &pTele = E.Map()->m_pTeleLayer;
	m_pSolveJob = XcSolver::Start(pGame->m_pTiles, pFront ? pFront->m_pTiles : nullptr, pTele ? pTele->m_pTeleTile : nullptr, pGame->m_Width, pGame->m_Height, 250000, m_vSolveHints, TowerDepth, Forgiving, Ideal);
	m_SolveIdeal = Ideal;
	m_SolveTowerDepth = TowerDepth;
	m_pSolveTowerLevel = pLevel;
	if(!m_pSolveJob->m_HasStart || !m_pSolveJob->m_HasGoal)
	{
		XcUi::Toast(Tr("The bot needs a spawn (or a start line) and a finish line in the game layer"), XcUi::TOAST_ERROR);
		m_pSolveJob.reset();
		return;
	}
	XcUi::Toast(Tr("The bot is looking for a way..."), XcUi::TOAST_INFO);
}

void CXcEditorUi::UpdateSolve()
{
	if(!m_pSolveJob || !m_pSolveJob->m_Done)
		return;
	CEditor &E = *m_pEditor;
	m_SolveResult = std::move(m_pSolveJob->m_Result);
	const XcSolver::SResult &Result = m_SolveResult;
	// the report for the mapper to send: xclient/bot_log.txt in the save folder
	{
		E.Storage()->CreateFolder("xclient", IStorage::TYPE_SAVE);
		IOHANDLE File = E.Storage()->OpenFile("xclient/bot_log.txt", IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(File)
		{
			io_write(File, Result.m_Log.data(), Result.m_Log.size());
			io_close(File);
		}
	}
	m_vSolvePath = Result.m_vPath;
	m_SolveFound = Result.m_Found;
	if(Result.m_Found && !m_SolveIdeal)
		m_SolveHumanTicks = Result.m_Ticks;
	char aMsg[256];
	char aTime[32];
	FormatRaceTime(aTime, sizeof(aTime), Result.m_Ticks);
	if(Result.m_Found && m_SolveIdeal)
	{
		if(m_SolveHumanTicks > 0)
		{
			char aHuman[32];
			FormatRaceTime(aHuman, sizeof(aHuman), m_SolveHumanTicks);
			str_format(aMsg, sizeof(aMsg), Tr("Ideal run: %s (the human-like bot: %s)"), aTime, aHuman);
		}
		else
			str_format(aMsg, sizeof(aMsg), Tr("Ideal run: %s"), aTime);
	}
	else if(Result.m_Found)
		str_format(aMsg, sizeof(aMsg), Tr("The bot finished in %s: %d hooks, %d jumps"), aTime, Result.m_Hooks, Result.m_Jumps);
	else
		str_format(aMsg, sizeof(aMsg), Tr("The bot found no way (%d tries). It got to the red mark in %s"), Result.m_Explored, aTime);
	XcUi::Toast(aMsg, Result.m_Found ? XcUi::TOAST_SUCCESS : XcUi::TOAST_ERROR);
	// the toast goes away: the time stays written over the map
	if(Result.m_Found)
		str_format(m_aSolveSummary, sizeof(m_aSolveSummary), m_SolveIdeal ? Tr("Ideal run: %s") : Tr("The bot finished in %s"), aTime);
	else
		str_format(m_aSolveSummary, sizeof(m_aSolveSummary), Tr("The bot got to the red mark in %s"), aTime);
	// the length of the way in cells
	float Length = 0.0f;
	for(size_t i = 1; i < Result.m_vPath.size(); i++)
		Length += distance(Result.m_vPath[i - 1], Result.m_vPath[i]);
	str_format(aMsg, sizeof(aMsg), Tr("Length of the way: %d cells. Map > Bot: heat map shows the hard places"), (int)(Length / 32.0f));
	XcUi::Toast(aMsg, XcUi::TOAST_INFO);
	if(m_SolveTowerDepth > 0)
	{
		if(Result.m_vTowerSpots.empty())
			str_format(aMsg, sizeof(aMsg), Tr("Tower skips (%s): none found on this way"), m_pSolveTowerLevel);
		else
			str_format(aMsg, sizeof(aMsg), Tr("Tower skips (%s): %d places, marked with T"), m_pSolveTowerLevel, (int)Result.m_vTowerSpots.size());
		XcUi::Toast(aMsg, Result.m_vTowerSpots.empty() ? XcUi::TOAST_SUCCESS : XcUi::TOAST_ERROR);
		m_ShowHeat = false;
	}
	m_pSolveJob.reset();
}

void CXcEditorUi::RenderSolveHints()
{
	if(m_vSolveHints.empty())
		return;
	CEditor &E = *m_pEditor;
	CLayerGroup *pGroup = nullptr;
	for(const auto &pG : E.Map()->m_vpGroups)
		if(pG->m_GameGroup)
			pGroup = pG.get();
	if(!pGroup)
		return;
	pGroup->MapScreen();
	IGraphics *pGraphics = E.Graphics();
	pGraphics->TextureClear();
	pGraphics->LinesBegin();
	pGraphics->SetColor(0.45f, 0.75f, 1.0f, 0.6f);
	for(size_t i = 0; i + 1 < m_vSolveHints.size(); i++)
	{
		const IGraphics::CLineItem Line(m_vSolveHints[i].x, m_vSolveHints[i].y, m_vSolveHints[i + 1].x, m_vSolveHints[i + 1].y);
		pGraphics->LinesDraw(&Line, 1);
	}
	pGraphics->LinesEnd();
	pGraphics->QuadsBegin();
	pGraphics->SetColor(0.45f, 0.75f, 1.0f, 0.9f);
	for(const vec2 &Pos : m_vSolveHints)
	{
		const IGraphics::CQuadItem Quad(Pos.x - 10.0f, Pos.y - 10.0f, 20.0f, 20.0f);
		pGraphics->QuadsDrawTL(&Quad, 1);
	}
	pGraphics->QuadsEnd();
	for(size_t i = 0; i < m_vSolveHints.size(); i++)
	{
		char aNumber[8];
		str_format(aNumber, sizeof(aNumber), "%d", (int)i + 1);
		E.TextRender()->Text(m_vSolveHints[i].x - 6.0f, m_vSolveHints[i].y - 9.0f, 16.0f, aNumber);
	}
	E.Ui()->MapScreen();
}

// the heat map: the cells the bot tried in, green (few tries) to red (many), and the way colored by its room
void CXcEditorUi::RenderSolveHeat()
{
	CEditor &E = *m_pEditor;
	const XcSolver::SResult &Result = m_SolveResult;
	CLayerGroup *pGroup = nullptr;
	for(const auto &pG : E.Map()->m_vpGroups)
		if(pG->m_GameGroup)
			pGroup = pG.get();
	if(!pGroup)
		return;
	pGroup->MapScreen();
	IGraphics *pGraphics = E.Graphics();
	uint16_t Max = 1;
	for(uint16_t Heat : Result.m_vHeat)
		Max = std::max(Max, Heat);
	const float LogMax = std::log2(1.0f + Max);
	pGraphics->TextureClear();
	pGraphics->QuadsBegin();
	for(int y = 0; y < Result.m_Height; y++)
		for(int x = 0; x < Result.m_Width; x++)
		{
			const uint16_t Heat = Result.m_vHeat[y * Result.m_Width + x];
			if(!Heat)
				continue;
			const float t = std::log2(1.0f + Heat) / LogMax;
			pGraphics->SetColor(std::min(1.0f, t * 2.0f), std::min(1.0f, 2.0f - t * 2.0f), 0.15f, 0.35f);
			const IGraphics::CQuadItem Quad(x * 32.0f, y * 32.0f, 32.0f, 32.0f);
			pGraphics->QuadsDrawTL(&Quad, 1);
		}
	// the way: red where it is one cell wide or freeze is right next to it, yellow when tight, green with room
	for(size_t i = 0; i + 1 < m_vSolvePath.size() && i < Result.m_vRoom.size(); i++)
	{
		const int Room = Result.m_vRoom[i];
		pGraphics->SetColor(Room <= 1 ? ColorRGBA(1.0f, 0.25f, 0.25f, 1.0f) : Room == 2 ? ColorRGBA(1.0f, 0.85f, 0.2f, 1.0f) : ColorRGBA(0.3f, 1.0f, 0.5f, 1.0f));
		const vec2 a = m_vSolvePath[i], b = m_vSolvePath[i + 1];
		const vec2 Dir = b - a;
		const float Len = length(Dir);
		if(Len < 0.01f)
			continue;
		const vec2 Side = vec2(-Dir.y, Dir.x) / Len * 3.0f;
		const IGraphics::CFreeformItem Item(a + Side, b + Side, a - Side, b - Side);
		pGraphics->QuadsDrawFreeform(&Item, 1);
	}
	pGraphics->QuadsEnd();
	E.Ui()->MapScreen();
}

void CXcEditorUi::RenderSolvePath()
{
	CEditor &E = *m_pEditor;
	RenderSolveHints();
	if(m_pSolveJob && !m_pSolveJob->m_Done)
	{
		// while it searches: the tried places as faint dots, the best way so far in yellow, and the count
		std::vector<vec2> vPath, vTried;
		{
			std::lock_guard<std::mutex> Lock(m_pSolveJob->m_LiveMutex);
			vPath = m_pSolveJob->m_vLivePath;
			vTried = m_pSolveJob->m_vLiveTried;
		}
		CLayerGroup *pGroup = nullptr;
		for(const auto &pG : E.Map()->m_vpGroups)
			if(pG->m_GameGroup)
				pGroup = pG.get();
		if(pGroup)
		{
			pGroup->MapScreen();
			IGraphics *pGraphics = E.Graphics();
			pGraphics->TextureClear();
			pGraphics->QuadsBegin();
			pGraphics->SetColor(1.0f, 0.85f, 0.3f, 0.25f);
			for(const vec2 &Pos : vTried)
			{
				const IGraphics::CQuadItem Dot(Pos.x - 2.0f, Pos.y - 2.0f, 4.0f, 4.0f);
				pGraphics->QuadsDrawTL(&Dot, 1);
			}
			pGraphics->QuadsEnd();
			pGraphics->LinesBegin();
			pGraphics->SetColor(1.0f, 0.9f, 0.2f, 1.0f);
			for(size_t i = 0; i + 1 < vPath.size(); i++)
			{
				const IGraphics::CLineItem Line(vPath[i].x, vPath[i].y, vPath[i + 1].x, vPath[i + 1].y);
				pGraphics->LinesDraw(&Line, 1);
			}
			pGraphics->LinesEnd();
			E.Ui()->MapScreen();
		}
		char aMsg[160], aTime[32];
		FormatRaceTime(aTime, sizeof(aTime), m_pSolveJob->m_LiveTicks.load());
		str_format(aMsg, sizeof(aMsg), Tr("The bot is searching: %d tries, the best way so far %s"), m_pSolveJob->m_Progress.load(), aTime);
		const CUIRect Screen = *E.Ui()->Screen();
		const float FontSize = 12.0f;
		const float Width = E.TextRender()->TextWidth(FontSize, aMsg);
		E.TextRender()->TextColor(1.0f, 0.9f, 0.3f, 1.0f);
		E.TextRender()->Text(Screen.w * 0.5f - Width * 0.5f, 34.0f, FontSize, aMsg);
		E.TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		return;
	}
	if(m_vSolvePath.size() < 2)
		return;
	if(m_aSolveSummary[0])
	{
		E.Ui()->MapScreen();
		const CUIRect Screen = *E.Ui()->Screen();
		const float FontSize = 12.0f;
		const float Width = E.TextRender()->TextWidth(FontSize, m_aSolveSummary);
		if(m_SolveFound)
			E.TextRender()->TextColor(0.45f, 1.0f, 0.65f, 1.0f);
		else
			E.TextRender()->TextColor(1.0f, 0.65f, 0.3f, 1.0f);
		E.TextRender()->Text(Screen.w * 0.5f - Width * 0.5f, 34.0f, FontSize, m_aSolveSummary);
		E.TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	if(m_ShowHeat && !m_SolveResult.m_vHeat.empty())
	{
		RenderSolveHeat();
		return;
	}
	CLayerGroup *pGame = nullptr;
	for(const auto &pGroup : E.Map()->m_vpGroups)
		if(pGroup->m_GameGroup)
			pGame = pGroup.get();
	if(!pGame)
		return;
	pGame->MapScreen();
	IGraphics *pGraphics = E.Graphics();
	pGraphics->TextureClear();
	pGraphics->LinesBegin();
	pGraphics->SetColor(m_SolveFound ? ColorRGBA(0.35f, 1.0f, 0.6f, 0.95f) : ColorRGBA(1.0f, 0.65f, 0.25f, 0.95f));
	for(size_t i = 0; i + 1 < m_vSolvePath.size(); i++)
	{
		const IGraphics::CLineItem Line(m_vSolvePath[i].x, m_vSolvePath[i].y, m_vSolvePath[i + 1].x, m_vSolvePath[i + 1].y);
		pGraphics->LinesDraw(&Line, 1);
	}
	pGraphics->LinesEnd();
	if(!m_SolveFound)
	{
		// the farthest point it reached
		const vec2 End = m_vSolvePath.back();
		pGraphics->QuadsBegin();
		pGraphics->SetColor(1.0f, 0.25f, 0.25f, 0.9f);
		const IGraphics::CQuadItem Quad(End.x - 8.0f, End.y - 8.0f, 16.0f, 16.0f);
		pGraphics->QuadsDrawTL(&Quad, 1);
		pGraphics->QuadsEnd();
	}
	// the hints the bot set itself along the route
	if(!m_SolveResult.m_vAutoHints.empty())
	{
		pGraphics->TextureClear();
		pGraphics->QuadsBegin();
		pGraphics->SetColor(0.45f, 0.75f, 1.0f, 0.45f);
		for(const vec2 &Hint : m_SolveResult.m_vAutoHints)
		{
			const IGraphics::CQuadItem Quad(Hint.x - 4.0f, Hint.y - 4.0f, 8.0f, 8.0f);
			pGraphics->QuadsDrawTL(&Quad, 1);
		}
		pGraphics->QuadsEnd();
	}
	// tower skips: the frozen tees the way stood on or hooked
	if(!m_SolveResult.m_vTowerSpots.empty())
	{
		pGraphics->TextureClear();
		pGraphics->QuadsBegin();
		pGraphics->SetColor(1.0f, 0.2f, 0.25f, 0.7f);
		for(const vec2 &Spot : m_SolveResult.m_vTowerSpots)
		{
			const IGraphics::CQuadItem Quad(Spot.x - 16.0f, Spot.y - 16.0f, 32.0f, 32.0f);
			pGraphics->QuadsDrawTL(&Quad, 1);
		}
		pGraphics->QuadsEnd();
		for(const vec2 &Spot : m_SolveResult.m_vTowerSpots)
			E.TextRender()->Text(Spot.x - 7.0f, Spot.y - 11.0f, 20.0f, "T");
	}
	E.Ui()->MapScreen();
}
