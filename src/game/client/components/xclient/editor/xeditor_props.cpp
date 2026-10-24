// The properties panel of the new editor interface: the classic property popups (group, layer, quad, quad
// point, sound source) run every frame in capture mode, their properties and buttons are drawn here as
// cards, and a change made here goes back into the popup code the next frame. So every rule of the
// classic editor (undo, trackers, limits, warnings) keeps working.
#include "xeditor.h"

#include <base/math.h>
#include <base/str.h>

#include <engine/font_icons.h>
#include <engine/shared/config.h>

#include <game/client/components/xclient/hud/xc_hud.h>
#include <game/client/components/xclient/ui/lang.h>
#include <game/client/components/xclient/ui/theme.h>
#include <game/client/components/xclient/ui/widgets.h>
#include <game/editor/editor.h>
#include <game/editor/enums.h>
#include <game/editor/explanations.h>
#include <game/editor/mapitems/image.h>
#include <game/editor/mapitems/layer_group.h>
#include <game/editor/mapitems/layer_speedup.h>
#include <game/editor/mapitems/layer_switch.h>
#include <game/editor/mapitems/layer_tele.h>
#include <game/editor/mapitems/layer_tiles.h>
#include <game/editor/mapitems/layer_tune.h>
#include <game/editor/mapitems/sound.h>

#include <engine/external/imgui/imgui.h>
#include <engine/external/imgui/imgui_internal.h>

#include <algorithm>
#include <cfloat>

using namespace XcUi;

void CXcEditorUi::SetPending(int Kind, int Call, int Prop, int Value, EEditState State)
{
	SCaptureState &Capture = m_aCaptures[Kind];
	// a click that both starts and ends in one frame is one step
	if(Capture.m_PendingCall == Call && Capture.m_PendingProp == Prop && Capture.m_PendingState == EEditState::START && State == EEditState::END)
		State = EEditState::ONE_GO;
	Capture.m_PendingCall = Call;
	Capture.m_PendingProp = Prop;
	Capture.m_PendingValue = Value;
	Capture.m_PendingState = State;
}

CUi::EPopupMenuFunctionResult CXcEditorUi::RunCapture(int Kind, CUi::FPopupMenuFunction pfnPopup, void *pContext, bool Active)
{
	CEditor &E = *m_pEditor;
	SCaptureState &Capture = m_aCaptures[Kind];
	E.m_XcCapture = SXcCapture();
	E.m_XcCapture.m_Active = true;
	E.m_XcCapture.m_PendingCall = Capture.m_PendingCall;
	E.m_XcCapture.m_PendingProp = Capture.m_PendingProp;
	E.m_XcCapture.m_PendingValue = Capture.m_PendingValue;
	E.m_XcCapture.m_PendingState = Capture.m_PendingState;
	E.m_XcCapture.m_pPendingButton = Capture.m_pPendingButton;
	Capture.m_PendingCall = -1;
	Capture.m_pPendingButton = nullptr;
	// out of sight: the popup draws its labels and edit boxes there, nothing can be clicked
	const CUIRect Hidden = {-10000.0f, -10000.0f, 200.0f, 1000.0f};
	const CUi::EPopupMenuFunctionResult Result = pfnPopup(pContext, Hidden, Active);
	E.m_XcCapture.m_Active = false;
	Capture.m_Result = E.m_XcCapture;
	return Result;
}

void CXcEditorUi::RenderProp(int Kind, int Call, int Index, const SXcCapturedProp &Prop, float LabelWidth)
{
	CEditor &E = *m_pEditor;
	CEditorMap *pMap = E.Map();
	const STheme &t = Theme();
	SCaptureState &Capture = m_aCaptures[Kind];
	ImGui::PushID(Call * 100 + Index);

	// label on the left like Figma, the control on the right
	ImFont *pSmall = Fonts().m_pSmall;
	const float RowHeight = S(28.0f);
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddText(pSmall, pSmall->FontSize, ImVec2(Start.x, Start.y + (RowHeight - pSmall->FontSize) * 0.5f), Col(t.m_TextSecondary), Tr(Prop.m_Name.c_str()));
	const float ControlX = Start.x + LabelWidth;
	const float ControlWidth = m_CardInnerWidth - LabelWidth;
	ImGui::SetCursorScreenPos(ImVec2(ControlX, Start.y));

	const bool Editing = Capture.m_EditingCall == Call && Capture.m_EditingProp == Index;
	// drags start, go on and end like the classic value selectors, so undo gets one step per drag
	const auto DragResult = [&](bool Changed, int Value) {
		if(Changed)
		{
			SetPending(Kind, Call, Index, Value, Editing ? EEditState::EDITING : EEditState::START);
			Capture.m_EditingCall = Call;
			Capture.m_EditingProp = Index;
			Capture.m_EditingValue = Value;
		}
		else if(Editing && !ImGui::IsItemActive())
		{
			SetPending(Kind, Call, Index, Capture.m_EditingValue, EEditState::END);
			Capture.m_EditingCall = -1;
			Capture.m_EditingProp = -1;
		}
	};
	const float Small = S(26.0f);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8.0f), (RowHeight - ImGui::GetFontSize()) * 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4.0f), S(4.0f)));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, t.m_Track);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Mix(t.m_Track, t.m_Text, 0.08f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Mix(t.m_Track, t.m_Accent, 0.25f));
	ImGui::PushStyleColor(ImGuiCol_Button, t.m_Track);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(t.m_Track, t.m_Text, 0.12f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(t.m_Track, t.m_Accent, 0.35f));
	switch(Prop.m_Type)
	{
	case PROPTYPE_INT:
	case PROPTYPE_ANGLE_SCROLL:
	{
		const bool Angle = Prop.m_Type == PROPTYPE_ANGLE_SCROLL;
		const int Step = Angle ? (E.Input()->ShiftIsPressed() ? 1 : 45) : 1;
		int Value = Prop.m_Value;
		if(ImGui::Button(FontIcon::MINUS, ImVec2(Small, RowHeight)))
		{
			int NewValue = Angle ? ((int)std::ceil(Value / (float)Step) - 1) * Step : Value - 1;
			if(Angle && NewValue < 0)
				NewValue += 360;
			SetPending(Kind, Call, Index, Angle ? NewValue : std::clamp(NewValue, Prop.m_Min, Prop.m_Max), EEditState::ONE_GO);
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ControlWidth - (Small + S(4.0f)) * 2.0f);
		const int Min = Angle ? 0 : Prop.m_Min;
		const int Max = Angle ? 359 : Prop.m_Max;
		const bool Changed = ImGui::DragInt("##v", &Value, Angle ? 1.0f : 0.25f, Min, Max, "%d", ImGuiSliderFlags_AlwaysClamp);
		HoldMouseWhileDragging();
		if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("%s", Tr("Drag to change, double click to type"));
		DragResult(Changed, Value);
		ImGui::SameLine();
		if(ImGui::Button(FontIcon::PLUS, ImVec2(Small, RowHeight)))
		{
			const int NewValue = Angle ? (Value + Step) / Step * Step % 360 : Value + 1;
			SetPending(Kind, Call, Index, Angle ? NewValue : std::clamp(NewValue, Prop.m_Min, Prop.m_Max), EEditState::ONE_GO);
		}
		break;
	}
	case PROPTYPE_BOOL:
	{
		bool Value = Prop.m_Value != 0;
		ImGui::SetCursorScreenPos(ImVec2(ControlX + ControlWidth - S(44.0f), Start.y + (RowHeight - S(22.0f)) * 0.5f));
		if(Toggle("##toggle", &Value))
			SetPending(Kind, Call, Index, Value ? 1 : 0, EEditState::ONE_GO);
		break;
	}
	case PROPTYPE_COLOR:
	{
		ColorRGBA Color = ColorRGBA::UnpackAlphaLast<ColorRGBA>(Prop.m_Value);
		float aColor[4] = {Color.r, Color.g, Color.b, Color.a};
		if(ImGui::ColorButton("##swatch", ImVec4(Color.r, Color.g, Color.b, Color.a), ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(ControlWidth, RowHeight)))
			ImGui::OpenPopup("##picker");
		if(ImGui::BeginPopup("##picker"))
		{
			const bool Changed = ImGui::ColorPicker4("##color", aColor, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex);
			const int NewValue = ColorRGBA(aColor[0], aColor[1], aColor[2], aColor[3]).PackAlphaLast();
			if(Changed)
			{
				SetPending(Kind, Call, Index, NewValue, Editing ? EEditState::EDITING : EEditState::START);
				Capture.m_EditingCall = Call;
				Capture.m_EditingProp = Index;
				Capture.m_EditingValue = NewValue;
			}
			else if(Editing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				SetPending(Kind, Call, Index, Capture.m_EditingValue, EEditState::END);
				Capture.m_EditingCall = -1;
				Capture.m_EditingProp = -1;
			}
			ImGui::EndPopup();
		}
		break;
	}
	case PROPTYPE_IMAGE:
	case PROPTYPE_SOUND:
	{
		const bool Image = Prop.m_Type == PROPTYPE_IMAGE;
		const int Num = Image ? (int)pMap->m_vpImages.size() : (int)pMap->m_vpSounds.size();
		const auto Name = [&](int i) -> const char * {
			if(i < 0 || i >= Num)
				return Tr("None");
			return Image ? pMap->m_vpImages[i]->m_aName : pMap->m_vpSounds[i]->m_aName;
		};
		ImGui::SetNextItemWidth(ControlWidth);
		if(ImGui::BeginCombo("##combo", Name(Prop.m_Value), ImGuiComboFlags_HeightLarge))
		{
			if(ImGui::Selectable(Tr("None"), Prop.m_Value < 0))
				SetPending(Kind, Call, Index, -1, EEditState::ONE_GO);
			for(int i = 0; i < Num; i++)
			{
				ImGui::PushID(i);
				if(Image)
				{
					// a small picture of the tileset next to its name
					const float Size = S(28.0f);
					const ImVec2 Pos = ImGui::GetCursorScreenPos();
					if(ImGui::Selectable("##img", Prop.m_Value == i, 0, ImVec2(0.0f, Size)))
						SetPending(Kind, Call, Index, i, EEditState::ONE_GO);
					const auto &pImage = pMap->m_vpImages[i];
					if(pImage->m_Texture.IsValid())
						ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)m_Gui.TextureId(pImage->m_Texture), Pos, ImVec2(Pos.x + Size, Pos.y + Size));
					ImGui::GetWindowDrawList()->AddText(ImVec2(Pos.x + Size + S(8.0f), Pos.y + (Size - ImGui::GetFontSize()) * 0.5f), Col(t.m_Text), pImage->m_aName);
				}
				else if(ImGui::Selectable(Name(i), Prop.m_Value == i))
					SetPending(Kind, Call, Index, i, EEditState::ONE_GO);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		break;
	}
	case PROPTYPE_ENVELOPE:
	{
		// value 0 is no envelope, the envelopes start at 1
		const auto Name = [&](int Value, char *pBuf, int BufSize) {
			if(Value <= 0 || Value > (int)pMap->m_vpEnvelopes.size())
			{
				str_copy(pBuf, Tr("None"), BufSize);
				return;
			}
			const auto &pEnv = pMap->m_vpEnvelopes[Value - 1];
			const int Channels = pEnv->GetChannels();
			const char *pKind = Channels == 4 ? Tr("color") : (Channels == 3 ? Tr("position") : Tr("sound"));
			if(pEnv->m_aName[0])
				str_format(pBuf, BufSize, "%d. %s (%s)", Value, pEnv->m_aName, pKind);
			else
				str_format(pBuf, BufSize, "%d (%s)", Value, pKind);
		};
		char aPreview[96];
		Name(Prop.m_Value, aPreview, sizeof(aPreview));
		ImGui::SetNextItemWidth(ControlWidth);
		if(ImGui::BeginCombo("##env", aPreview, ImGuiComboFlags_HeightLarge))
		{
			for(int i = 0; i <= (int)pMap->m_vpEnvelopes.size(); i++)
			{
				char aItem[96];
				Name(i, aItem, sizeof(aItem));
				ImGui::PushID(i);
				if(ImGui::Selectable(aItem, Prop.m_Value == i))
					SetPending(Kind, Call, Index, i, EEditState::ONE_GO);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		break;
	}
	case PROPTYPE_SHIFT:
	{
		// move all tiles of the layer by "Shift by" tiles
		static const char *const s_apArrows[] = {FontIcon::CHEVRON_LEFT, FontIcon::CHEVRON_RIGHT, FontIcon::CHEVRON_UP, FontIcon::CHEVRON_DOWN};
		const char *apHints[] = {Tr("Shift left"), Tr("Shift right"), Tr("Shift up"), Tr("Shift down")};
		const float Width = (ControlWidth - S(4.0f) * 3.0f) / 4.0f;
		for(int Dir = 0; Dir < 4; Dir++)
		{
			if(Dir > 0)
				ImGui::SameLine();
			ImGui::PushID(Dir);
			if(ImGui::Button(s_apArrows[Dir], ImVec2(Width, RowHeight)))
				SetPending(Kind, Call, Index, Dir, EEditState::ONE_GO);
			if(ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", apHints[Dir]);
			ImGui::PopID();
		}
		break;
	}
	case PROPTYPE_AUTOMAPPER:
	{
		const int ImageIndex = Prop.m_Min;
		const bool HasImage = ImageIndex >= 0 && ImageIndex < (int)pMap->m_vpImages.size();
		const int Num = HasImage ? pMap->m_vpImages[ImageIndex]->m_Automapper.ConfigNamesNum() : 0;
		const char *pPreview = (Prop.m_Value >= 0 && Prop.m_Value < Num) ? pMap->m_vpImages[ImageIndex]->m_Automapper.GetConfigName(Prop.m_Value) : Tr("None");
		// XClient builder next to the list: make or change rules for this picture
		const float BuildW = HasImage ? ImGui::GetFrameHeight() : 0.0f;
		ImGui::SetNextItemWidth(ControlWidth - (HasImage ? BuildW + S(6.0f) : 0.0f));
		ImGui::BeginDisabled(Num == 0);
		if(ImGui::BeginCombo("##rule", pPreview, ImGuiComboFlags_HeightLarge))
		{
			if(ImGui::Selectable(Tr("None"), Prop.m_Value < 0))
				SetPending(Kind, Call, Index, -1, EEditState::ONE_GO);
			for(int i = 0; i < Num; i++)
			{
				ImGui::PushID(i);
				if(ImGui::Selectable(pMap->m_vpImages[ImageIndex]->m_Automapper.GetConfigName(i), Prop.m_Value == i))
					SetPending(Kind, Call, Index, i, EEditState::ONE_GO);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		if(Num == 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", Tr("This image has no automapper rules"));
		if(HasImage)
		{
			ImGui::SameLine(0.0f, S(6.0f));
			if(ImGui::Button("+##am_build", ImVec2(BuildW, BuildW)))
				OpenAutomapBuilder(ImageIndex);
			if(ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Tr("Automapper builder: make rules for this tileset"));
		}
		break;
	}
	case PROPTYPE_AUTOMAPPER_REFERENCE:
	{
		const int Num = (int)std::size(AUTOMAP_REFERENCE_NAMES);
		const char *pPreview = (Prop.m_Value >= 0 && Prop.m_Value < Num) ? Tr(AUTOMAP_REFERENCE_NAMES[Prop.m_Value]) : Tr("None");
		ImGui::SetNextItemWidth(ControlWidth);
		if(ImGui::BeginCombo("##ref", pPreview))
		{
			if(ImGui::Selectable(Tr("None"), Prop.m_Value < 0))
				SetPending(Kind, Call, Index, -1, EEditState::ONE_GO);
			for(int i = 0; i < Num; i++)
			{
				ImGui::PushID(i);
				if(ImGui::Selectable(Tr(AUTOMAP_REFERENCE_NAMES[i]), Prop.m_Value == i))
					SetPending(Kind, Call, Index, i, EEditState::ONE_GO);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		break;
	}
	default:
		break;
	}
	ImGui::PopStyleColor(6);
	ImGui::PopStyleVar(3);
	ImGui::SetCursorScreenPos(ImVec2(Start.x, Start.y + RowHeight + S(6.0f)));
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
	ImGui::PopID();
}

void CXcEditorUi::RenderCaptureCard(int Kind, const char *pId, const char *pTitle, const char *pSubtitle, int SkipButtons)
{
	CEditor &E = *m_pEditor;
	const STheme &t = Theme();
	SCaptureState &Capture = m_aCaptures[Kind];
	const SXcCapture &Result = Capture.m_Result;
	(void)SkipButtons;

	ImGui::PushID(pId);
	ImDrawList *pDraw = ImGui::GetWindowDrawList();
	// a card with a header like the cards of the menu
	pDraw->ChannelsSplit(2);
	pDraw->ChannelsSetCurrent(1);
	const ImVec2 CardMin = ImGui::GetCursorScreenPos();
	const float Width = ImGui::GetContentRegionAvail().x;
	const float Pad = S(12.0f);
	ImGui::SetCursorScreenPos(ImVec2(CardMin.x + Pad, CardMin.y + Pad));
	ImGui::BeginGroup();
	ImGui::PushItemWidth(Width - Pad * 2.0f);
	ImFont *pTitleFont = Fonts().m_pTitle;
	ImGui::PushFont(pTitleFont);
	ImGui::TextColored(t.m_Text, "%s", pTitle);
	ImGui::PopFont();
	if(pSubtitle && pSubtitle[0])
	{
		ImGui::PushFont(Fonts().m_pSmall);
		ImGui::TextColored(t.m_TextMuted, "%s", pSubtitle);
		ImGui::PopFont();
	}
	ImGui::Dummy(ImVec2(Width - Pad * 2.0f, S(4.0f)));

	m_CardInnerWidth = Width - Pad * 2.0f;
	const float LabelWidth = m_CardInnerWidth * 0.38f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4.0f), 0.0f));
	for(int Call = 0; Call < (int)Result.m_vvProps.size(); Call++)
	{
		if(Call > 0)
		{
			// the next block of the popup, e.g. the type properties after the common ones
			const ImVec2 P = ImGui::GetCursorScreenPos();
			pDraw->AddLine(ImVec2(P.x, P.y + S(4.0f)), ImVec2(P.x + Width - Pad * 2.0f, P.y + S(4.0f)), Col(t.m_Border));
			ImGui::Dummy(ImVec2(0.0f, S(12.0f)));
		}
		for(int i = 0; i < (int)Result.m_vvProps[Call].size(); i++)
		{
			const SXcCapturedProp &Prop = Result.m_vvProps[Call][i];
			if(Prop.m_Type == PROPTYPE_NULL)
				continue;
			RenderProp(Kind, Call, i, Prop, LabelWidth);
		}
	}
	ImGui::PopStyleVar();

	// the buttons of the popup; it adds them from the bottom up, so they come here in reverse
	std::vector<const SXcCapturedButton *> vButtons;
	for(auto It = Result.m_vButtons.rbegin(); It != Result.m_vButtons.rend(); ++It)
	{
		// the layers are added from the tree and the layer menu, the group card keeps its own actions
		if(Kind == CAPTURE_GROUP && (It->m_pId == &E.m_QuickActionAddTileLayer || It->m_pId == &E.m_QuickActionAddQuadsLayer || It->m_pId == &E.m_QuickActionAddSoundLayer ||
						    It->m_pId == &E.m_QuickActionAddFrontLayer || It->m_pId == &E.m_QuickActionAddTeleLayer || It->m_pId == &E.m_QuickActionAddSpeedupLayer ||
						    It->m_pId == &E.m_QuickActionAddSwitchLayer || It->m_pId == &E.m_QuickActionAddTuneLayer))
			continue;
		vButtons.push_back(&*It);
	}
	if(!vButtons.empty())
		ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
	for(size_t i = 0; i < vButtons.size(); i++)
	{
		const SXcCapturedButton &ButtonData = *vButtons[i];
		const bool Danger = str_find_nocase(ButtonData.m_Label.c_str(), "delete") != nullptr || str_find_nocase(ButtonData.m_Label.c_str(), "clean") != nullptr;
		ImGui::PushID((int)i);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(7.0f));
		ImGui::PushStyleColor(ImGuiCol_Button, Danger ? ImVec4(0.85f, 0.25f, 0.25f, 0.18f) : (ButtonData.m_Checked % 2 ? ImVec4(t.m_Accent.x, t.m_Accent.y, t.m_Accent.z, 0.3f) : t.m_Track));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Danger ? ImVec4(0.85f, 0.25f, 0.25f, 0.35f) : Mix(t.m_Track, t.m_Text, 0.12f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, Danger ? ImVec4(0.85f, 0.25f, 0.25f, 0.5f) : Mix(t.m_Track, t.m_Accent, 0.35f));
		ImGui::PushStyleColor(ImGuiCol_Text, Danger ? ImVec4(1.0f, 0.55f, 0.55f, 1.0f) : t.m_Text);
		ImGui::PushFont(Fonts().m_pLabel);
		const bool Letter = ButtonData.m_Label.size() <= 1 && !ButtonData.m_Tooltip.empty();
		if(ImGui::Button(Tr(Letter ? ButtonData.m_Tooltip.c_str() : ButtonData.m_Label.c_str()), ImVec2(Width - Pad * 2.0f, S(30.0f))))
			Capture.m_pPendingButton = ButtonData.m_pId;
		ImGui::PopFont();
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();
		if(!ButtonData.m_Tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("%s", Tr(ButtonData.m_Tooltip.c_str()));
		ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
		ImGui::PopID();
	}
	ImGui::PopItemWidth();
	ImGui::EndGroup();
	const ImVec2 CardMax(CardMin.x + Width, ImGui::GetItemRectMax().y + Pad);
	pDraw->ChannelsSetCurrent(0);
	pDraw->AddRectFilled(CardMin, CardMax, Col(ImVec4(t.m_Card.x, t.m_Card.y, t.m_Card.z, 1.0f)), S(12.0f));
	pDraw->AddRect(CardMin, CardMax, Col(t.m_Border), S(12.0f));
	pDraw->ChannelsMerge();
	ImGui::SetCursorScreenPos(ImVec2(CardMin.x, CardMax.y + S(10.0f)));
	ImGui::Dummy(ImVec2(Width, 0.0f));
	ImGui::PopID();
}

void CXcEditorUi::RenderRightPanel()
{
	CEditor &E = *m_pEditor;
	CEditorMap *pMap = E.Map();
	const STheme &t = Theme();
	const ImVec2 Display = ImGui::GetIO().DisplaySize;
	const float Height = Display.y - m_TopHeight - m_StatusHeight;
	const float x = Display.x - m_RightWidth;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12.0f), S(12.0f)));
	ImGui::SetNextWindowPos(ImVec2(x, m_TopHeight));
	ImGui::SetNextWindowSize(ImVec2(m_RightWidth, Height));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(t.m_Panel.x, t.m_Panel.y, t.m_Panel.z, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	const bool Open = ImGui::Begin("##xe_right", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor();
	if(Open)
	{
		ImGui::GetWindowDrawList()->AddLine(ImVec2(x, m_TopHeight), ImVec2(x, m_TopHeight + Height), Col(t.m_Border));
		ImGui::PushFont(Fonts().m_pHeader);
		ImGui::TextColored(t.m_Text, "%s", Tr("Properties"));
		ImGui::PopFont();
		ImGui::Dummy(ImVec2(0.0f, S(4.0f)));

		std::shared_ptr<CLayerGroup> pGroup = pMap->SelectedGroup();
		std::shared_ptr<CLayer> pLayer = pMap->SelectedLayer(0);
		char aSubtitle[128];

		if(E.m_Mode == MODE_LAYERS && pLayer && pMap->m_vSelectedLayers.size() == 1)
		{
			// a selected quad, its points and a sound source come first, like the selection in Figma
			if(pLayer->m_Type == LAYERTYPE_QUADS && !pMap->m_vSelectedQuads.empty())
			{
				std::vector<CQuad *> vpQuads = pMap->SelectedQuads();
				if(!vpQuads.empty())
				{
					if(m_pLastQuads != vpQuads[0])
					{
						m_pLastQuads = vpQuads[0];
						E.m_QuadPopupContext.m_Color = PackColor(vpQuads[0]->m_aColors[0]);
					}
					E.m_QuadPopupContext.m_pEditor = &E;
					E.m_QuadPopupContext.m_SelectedQuadIndex = 0;
					RunCapture(CAPTURE_QUAD, CEditor::PopupQuad, &E.m_QuadPopupContext);
					str_format(aSubtitle, sizeof(aSubtitle), Tr("%d selected"), (int)vpQuads.size());
					RenderCaptureCard(CAPTURE_QUAD, "##quad", Tr("Quad"), aSubtitle);
					// the selected corner points
					int Point = -1;
					for(int v = 0; v < 4; v++)
						if(pMap->m_SelectedQuadPoints & (1 << v))
						{
							Point = v;
							break;
						}
					if(Point >= 0)
					{
						E.m_PointPopupContext.m_pEditor = &E;
						E.m_PointPopupContext.m_SelectedQuadIndex = 0;
						E.m_PointPopupContext.m_SelectedQuadPoint = Point;
						RunCapture(CAPTURE_POINT, CEditor::PopupPoint, &E.m_PointPopupContext);
						RenderCaptureCard(CAPTURE_POINT, "##point", Tr("Corner points"), Tr("Position, color and texture of the selected corners"));
					}
				}
			}
			else
				m_pLastQuads = nullptr;
			if(pLayer->m_Type == LAYERTYPE_SOUNDS && pMap->SelectedSoundSource() != nullptr)
			{
				RunCapture(CAPTURE_SOURCE, CEditor::PopupSource, &E);
				RenderCaptureCard(CAPTURE_SOURCE, "##source", Tr("Sound source"), nullptr);
			}
		}

		// the numbers the brush paints on the special layers of the game group
		if(E.m_Mode == MODE_LAYERS && pLayer && pMap->m_vSelectedLayers.size() == 1)
		{
			CUi::FPopupMenuFunction pfnBrush = nullptr;
			const char *pBrushTitle = nullptr;
			if(pLayer == pMap->m_pTeleLayer)
			{
				pfnBrush = CEditor::PopupTele;
				pBrushTitle = Tr("Teleporter brush");
			}
			else if(pLayer == pMap->m_pSwitchLayer)
			{
				pfnBrush = CEditor::PopupSwitch;
				pBrushTitle = Tr("Switch brush");
			}
			else if(pLayer == pMap->m_pSpeedupLayer)
			{
				pfnBrush = CEditor::PopupSpeedup;
				pBrushTitle = Tr("Speedup brush");
			}
			else if(pLayer == pMap->m_pTuneLayer)
			{
				pfnBrush = CEditor::PopupTune;
				pBrushTitle = Tr("Tune brush");
			}
			if(pfnBrush)
			{
				// these popups also react to keys (N, Ctrl+F) like when they are open, so they only run while the
				// mouse is over this panel; the card keeps the last values meanwhile
				// not through a menu or another popup lying over the panel: a click on "Bot: stop" in the Map menu
				// went through to the brush card (its number wrapped to 255) and opened the tune zones
				const ImGuiWindow *pHoveredWindow = GImGui->HoveredWindow;
				const bool OverPopup = pHoveredWindow && (pHoveredWindow->Flags & ImGuiWindowFlags_Popup) && pHoveredWindow->RootWindow != ImGui::GetCurrentWindow()->RootWindow;
				const bool PanelHovered = !OverPopup && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup);
				if(PanelHovered || m_pLastBrushLayer != pLayer.get() || m_aCaptures[CAPTURE_BRUSH].m_PendingCall >= 0 || m_aCaptures[CAPTURE_BRUSH].m_pPendingButton)
					RunCapture(CAPTURE_BRUSH, pfnBrush, &E);
				m_pLastBrushLayer = pLayer.get();
				RenderCaptureCard(CAPTURE_BRUSH, "##brush", pBrushTitle, Tr("What the brush paints on this layer"));
				if(pLayer == pMap->m_pTuneLayer)
					RenderTuneSummary();
			}
		}

		if(E.m_Mode == MODE_LAYERS && pLayer)
		{
			CEditor::SLayerPopupContext &Context = E.m_RenderLayersState.m_LayerPopupContext;
			Context.m_pEditor = &E;
			Context.m_vpLayers.clear();
			Context.m_vLayerIndices.clear();
			// several tile layers share one card with their common properties
			if(pMap->m_vSelectedLayers.size() > 1 && pGroup)
			{
				bool AllTiles = true;
				for(int LayerIndex : pMap->m_vSelectedLayers)
				{
					if(LayerIndex < 0 || LayerIndex >= (int)pGroup->m_vpLayers.size() || pGroup->m_vpLayers[LayerIndex]->m_Type != LAYERTYPE_TILES)
					{
						AllTiles = false;
						break;
					}
					Context.m_vpLayers.push_back(std::static_pointer_cast<CLayerTiles>(pGroup->m_vpLayers[LayerIndex]));
					Context.m_vLayerIndices.push_back(LayerIndex);
				}
				if(!AllTiles)
				{
					Context.m_vpLayers.clear();
					Context.m_vLayerIndices.clear();
				}
			}
			RunCapture(CAPTURE_LAYER, CEditor::PopupLayer, &Context);
			if(Context.m_vpLayers.size() > 1)
				str_format(aSubtitle, sizeof(aSubtitle), Tr("%d tile layers"), (int)Context.m_vpLayers.size());
			else
				str_format(aSubtitle, sizeof(aSubtitle), "%s · %s", pLayer->m_aName[0] ? pLayer->m_aName : "-", Tr(pLayer->TypeName()));
			RenderCaptureCard(CAPTURE_LAYER, "##layer", Tr("Layer"), aSubtitle);
		}

		if(E.m_Mode == MODE_LAYERS && pGroup)
		{
			RunCapture(CAPTURE_GROUP, CEditor::PopupGroup, &E);
			str_copy(aSubtitle, pGroup->m_GameGroup ? Tr("Game") : (pGroup->m_aName[0] ? pGroup->m_aName : "-"));
			RenderCaptureCard(CAPTURE_GROUP, "##group", Tr("Group"), aSubtitle);
		}

		if(E.m_Mode == MODE_LAYERS && pLayer && pLayer->m_Type == LAYERTYPE_TILES)
			RenderInspector(std::static_pointer_cast<CLayerTiles>(pLayer));

		if(E.m_Mode == MODE_IMAGES)
			RenderImageCard();
		else if(E.m_Mode == MODE_SOUNDS)
		{
			Description(Tr("Right click a sound on the left to replace, export or delete it."));
		}
		else if(!pLayer && !pGroup)
			Description(Tr("Select a group or a layer on the left"));
	}
	ImGui::End();
}

// the tile under the mouse: what it is, how it is turned and the numbers of the special layers
void CXcEditorUi::RenderInspector(const std::shared_ptr<CLayerTiles> &pLayer)
{
	CEditor &E = *m_pEditor;
	CEditorMap *pMap = E.Map();
	const STheme &t = Theme();
	const vec2 Mouse = E.Ui()->UpdatedMousePos();
	if(E.Ui()->MouseInside(&m_View))
	{
		const vec2 World = E.MapView()->MouseWorldPos();
		m_InspectX = pLayer->ConvertX(World.x);
		m_InspectY = pLayer->ConvertY(World.y);
	}
	(void)Mouse;
	if(m_InspectX < 0 || m_InspectY < 0 || m_InspectX >= pLayer->m_Width || m_InspectY >= pLayer->m_Height)
		return;
	const CTile Tile = pLayer->GetTile(m_InspectX, m_InspectY);

	ImDrawList *pDraw = ImGui::GetWindowDrawList();
	pDraw->ChannelsSplit(2);
	pDraw->ChannelsSetCurrent(1);
	const ImVec2 CardMin = ImGui::GetCursorScreenPos();
	const float Width = ImGui::GetContentRegionAvail().x;
	const float Pad = S(12.0f);
	ImGui::SetCursorScreenPos(ImVec2(CardMin.x + Pad, CardMin.y + Pad));
	ImGui::BeginGroup();
	ImGui::PushFont(Fonts().m_pTitle);
	ImGui::TextColored(t.m_Text, "%s", Tr("Under the mouse"));
	ImGui::PopFont();
	ImGui::PushFont(Fonts().m_pSmall);
	ImGui::TextColored(t.m_TextMuted, "x %d  y %d", m_InspectX, m_InspectY);
	ImGui::Dummy(ImVec2(Width - Pad * 2.0f, S(4.0f)));
	const auto Row = [&](const char *pName, const char *pValue) {
		ImGui::TextColored(t.m_TextSecondary, "%s", pName);
		ImGui::SameLine(Pad + (Width - Pad * 2.0f) * 0.42f);
		ImGui::TextColored(t.m_Text, "%s", pValue);
	};
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "%d", Tile.m_Index);
	Row(Tr("Tile"), Tile.m_Index ? aBuf : Tr("empty"));
	if(Tile.m_Index)
	{
		// rotation and mirrors the same way the editor stores them
		const int Rotation = (Tile.m_Flags & TILEFLAG_ROTATE) ? 90 : 0;
		const bool FlipX = Tile.m_Flags & TILEFLAG_XFLIP;
		const bool FlipY = Tile.m_Flags & TILEFLAG_YFLIP;
		const int Angle = (Rotation + (FlipX && FlipY ? 180 : 0)) % 360;
		str_format(aBuf, sizeof(aBuf), "%d°%s", Angle, FlipX != FlipY ? (FlipX ? Tr(", mirrored") : Tr(", flipped")) : "");
		Row(Tr("Rotation"), aBuf);
	}
	// the numbers of the special layers
	if(pLayer->m_HasTele && pMap->m_pTeleLayer)
	{
		const CTeleTile &Tele = pMap->m_pTeleLayer->m_pTeleTile[m_InspectY * pMap->m_pTeleLayer->m_Width + m_InspectX];
		str_format(aBuf, sizeof(aBuf), "%d", Tele.m_Number);
		Row(Tr("Teleporter number"), aBuf);
	}
	else if(pLayer->m_HasSwitch && pMap->m_pSwitchLayer)
	{
		const CSwitchTile &Switch = pMap->m_pSwitchLayer->m_pSwitchTile[m_InspectY * pMap->m_pSwitchLayer->m_Width + m_InspectX];
		str_format(aBuf, sizeof(aBuf), "%d", Switch.m_Number);
		Row(Tr("Switch number"), aBuf);
		str_format(aBuf, sizeof(aBuf), "%d", Switch.m_Delay);
		Row(Tr("Delay"), aBuf);
	}
	else if(pLayer->m_HasSpeedup && pMap->m_pSpeedupLayer)
	{
		const CSpeedupTile &Speedup = pMap->m_pSpeedupLayer->m_pSpeedupTile[m_InspectY * pMap->m_pSpeedupLayer->m_Width + m_InspectX];
		str_format(aBuf, sizeof(aBuf), "%d", Speedup.m_Force);
		Row(Tr("Force"), aBuf);
		str_format(aBuf, sizeof(aBuf), "%d", Speedup.m_MaxSpeed);
		Row(Tr("Max speed"), aBuf);
		str_format(aBuf, sizeof(aBuf), "%d°", Speedup.m_Angle);
		Row(Tr("Angle"), aBuf);
	}
	else if(pLayer->m_HasTune && pMap->m_pTuneLayer)
	{
		const CTuneTile &Tune = pMap->m_pTuneLayer->m_pTuneTile[m_InspectY * pMap->m_pTuneLayer->m_Width + m_InspectX];
		str_format(aBuf, sizeof(aBuf), "%d", Tune.m_Number);
		Row(Tr("Tune zone"), aBuf);
	}
	// what the tile does in the game
	int Layer = NUM_LAYERS;
	if(pLayer == pMap->m_pGameLayer)
		Layer = LAYER_GAME;
	else if(pLayer == pMap->m_pFrontLayer)
		Layer = LAYER_FRONT;
	else if(pLayer == pMap->m_pSwitchLayer)
		Layer = LAYER_SWITCH;
	else if(pLayer == pMap->m_pTeleLayer)
		Layer = LAYER_TELE;
	else if(pLayer == pMap->m_pSpeedupLayer)
		Layer = LAYER_SPEEDUP;
	else if(pLayer == pMap->m_pTuneLayer)
		Layer = LAYER_TUNE;
	if(Layer != NUM_LAYERS && Tile.m_Index)
	{
		const char *pExplanation = CExplanations::Explain(CExplanations::EGametype::DDNET, Tile.m_Index, Layer);
		if(pExplanation)
		{
			ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Width - Pad * 2.0f);
			ImGui::TextColored(t.m_TextMuted, "%s", pExplanation);
			ImGui::PopTextWrapPos();
		}
	}
	ImGui::PopFont();
	ImGui::EndGroup();
	const ImVec2 CardMax(CardMin.x + Width, ImGui::GetItemRectMax().y + Pad);
	pDraw->ChannelsSetCurrent(0);
	pDraw->AddRectFilled(CardMin, CardMax, Col(t.m_Card), S(12.0f));
	pDraw->AddRect(CardMin, CardMax, Col(t.m_Border), S(12.0f));
	pDraw->ChannelsMerge();
	ImGui::SetCursorScreenPos(ImVec2(CardMin.x, CardMax.y + S(10.0f)));
	ImGui::Dummy(ImVec2(Width, 0.0f));
}
