// Visuals page: every effect is a card with its style, color, sizes and "mine / dummy / others".
#include "feedback.h"
#include "icons.h"
#include "lang.h"
#include "menu.h"
#include "theme.h"
#include "widgets.h"

#include <base/fs.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/external/imgui/imgui.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/client/components/xclient/ambience.h>
#include <game/client/components/xclient/xc_particles.h>
#include <game/client/gameclient.h>

using namespace XcUi;

// Cards for effects that have several styles and "off" as 0: the header toggle switches between off
// and the last used style, the styles are a segmented control in the body.
bool CXcMenu::StyleCard(const char *pId, const char *pTitle, const char *pDescription, const char *pConfig, int *pValue, const char *const *ppStyles, int NumStyles)
{
	int &LastStyle = m_aLastFxStyle[ImGui::GetID(pId) % std::size(m_aLastFxStyle)];
	if(*pValue > 0)
		LastStyle = *pValue;
	if(LastStyle <= 0 || LastStyle > NumStyles)
		LastStyle = 1;
	int Enabled = *pValue > 0;
	const int WasEnabled = Enabled;
	if(!BeginCard(pId, pTitle, pDescription, &Enabled))
		return false;
	if(Enabled != WasEnabled)
		SetConfigInt(pConfig, Enabled ? LastStyle : 0);
	int Style = std::clamp((Enabled ? *pValue : LastStyle) - 1, 0, NumStyles - 1);
	// many styles do not fit one segmented row, they wrap as chips
	if(NumStyles > 4 ? ChoiceChips("##style", &Style, ppStyles, NumStyles) : Segmented("##style", &Style, ppStyles, NumStyles))
	{
		LastStyle = Style + 1;
		SetConfigInt(pConfig, LastStyle);
	}
	return true;
}

// "Fine particle settings" folded under the effect: amount, life time, speed and size of its particles
void CXcMenu::ParticleTuning(int Group)
{
	static const char *const s_apTitles[FxTune::NUM_GROUPS] = {"Particles: jumps", "Particles: run dust", "Particles", "Particles", "Particles", "Particles", "Particles", "Particles", "Particles", "Particles", "Particles", "Particles: sparkles", "Particles"};
	ImGui::PushID("##particle_tuning");
	ImGui::PushID(Group);
	ImGuiStorage *pStorage = ImGui::GetStateStorage();
	const ImGuiID OpenId = ImGui::GetID("##open");
	bool Open = pStorage->GetBool(OpenId, false);
	// marked when something is not at 100 %
	bool Changed = false;
	for(int i = 0; i < 4; i++)
		Changed |= *FxTune::Setting(Group, i) != 100;
	const STheme &t = Theme();
	const ImVec2 Pos = ImGui::GetCursorScreenPos();
	const float Width = AvailWidth();
	ImFont *pFont = Fonts().m_pLabel;
	const float Height = S(26.0f);
	if(ImGui::InvisibleButton("##toggle", ImVec2(Width, Height)))
	{
		Open = !Open;
		pStorage->SetBool(OpenId, Open);
		PlayUiSound(SOUND_CLICK);
	}
	const bool Hovered = ImGui::IsItemHovered();
	ImDrawList *pDraw = ImGui::GetWindowDrawList();
	pDraw->AddRectFilled(Pos, ImVec2(Pos.x + Width, Pos.y + Height), Col(t.m_Track, Hovered ? 1.0f : 0.6f), S(6.0f));
	// a small arrow: right when folded, down when open
	const ImVec2 Arrow(Pos.x + S(12.0f), Pos.y + Height * 0.5f);
	const float a = S(4.0f);
	if(Open)
		pDraw->AddTriangleFilled(ImVec2(Arrow.x - a, Arrow.y - a * 0.5f), ImVec2(Arrow.x + a, Arrow.y - a * 0.5f), ImVec2(Arrow.x, Arrow.y + a * 0.7f), Col(t.m_TextSecondary));
	else
		pDraw->AddTriangleFilled(ImVec2(Arrow.x - a * 0.5f, Arrow.y - a), ImVec2(Arrow.x - a * 0.5f, Arrow.y + a), ImVec2(Arrow.x + a * 0.7f, Arrow.y), Col(t.m_TextSecondary));
	char aTitle[96];
	str_format(aTitle, sizeof(aTitle), "%s%s", Tr(s_apTitles[Group]), Changed ? Tr(" (changed)") : "");
	pDraw->AddText(pFont, pFont->FontSize, ImVec2(Pos.x + S(24.0f), Pos.y + (Height - pFont->FontSize) * 0.5f), Col(Changed ? t.m_Accent : t.m_TextSecondary), aTitle);
	if(Open)
	{
		const char *pName = FxTune::g_apNames[Group];
		char aConfig[64];
		static const char *const s_apSuffix[4] = {"amount", "life", "speed", "size"};
		static const char *const s_apLabels[4] = {"Amount", "How long they live", "Speed", "Size"};
		static const int s_aMin[4] = {0, 25, 0, 25};
		static const int s_aMax[4] = {500, 400, 400, 400};
		for(int i = 0; i < 4; i++)
		{
			str_format(aConfig, sizeof(aConfig), "xc_fxtune_%s_%s", pName, s_apSuffix[i]);
			ConfigSlider(Tr(s_apLabels[i]), aConfig, FxTune::Setting(Group, i), s_aMin[i], s_aMax[i], "%d%%");
		}
		if(Changed && Button(Tr("Reset"), false, -1.0f))
			for(int i = 0; i < 4; i++)
			{
				str_format(aConfig, sizeof(aConfig), "xc_fxtune_%s_%s", pName, s_apSuffix[i]);
				SetConfigInt(aConfig, 100);
			}
	}
	ImGui::PopID();
	ImGui::PopID();
}

// Weather: many kinds plus own pictures and gifs, all with color, wind and spin
void CXcMenu::RenderWeatherCard()
{
	CXcBackgrounds &Images = GameClient()->m_Weather.Images();
	if(!m_WeatherPendingAdd.empty())
	{
		HandleWeatherFile(m_WeatherPendingAdd.c_str());
		m_WeatherPendingAdd.clear();
	}
	if(!m_WeatherPendingRemove.empty())
	{
		if(Images.Remove(m_WeatherPendingRemove.c_str()))
		{
			if(str_comp(g_Config.m_XcWeatherFile, m_WeatherPendingRemove.c_str()) == 0)
				SetConfigString("xc_weather_file", "");
			Toast(Tr("Deleted"));
		}
		else
			Toast(Tr("Could not delete the file"), TOAST_ERROR);
		m_WeatherPendingRemove.clear();
	}

	// the header switch remembers the last kind
	static int s_LastKind = 1;
	if(g_Config.m_XcWeather > 0)
		s_LastKind = g_Config.m_XcWeather;
	int Enabled = g_Config.m_XcWeather > 0;
	const int WasEnabled = Enabled;
	if(!BeginCard("##fx_weather", Tr("Weather"), Tr("Falling or rising particles over the map and the menu, also your own pictures and gifs"), &Enabled))
		return;
	if(Enabled != WasEnabled)
		SetConfigInt("xc_weather", Enabled ? s_LastKind : 0);
	const char *apKinds[] = {Tr("Snow"), Tr("Leaves"), Tr("Sakura"), Tr("Ash"), Tr("Hearts"), Tr("Stars"), Tr("Crystals"), Tr("Fireflies"), Tr("Money"), Tr("Halloween"), Tr("Embers"), Tr("Rain"), Tr("Bubbles"), Tr("Confetti"), Tr("My picture")};
	int Kind = std::clamp((Enabled ? g_Config.m_XcWeather : s_LastKind) - 1, 0, (int)std::size(apKinds) - 1);
	if(ChoiceChips("##weather_kind", &Kind, apKinds, (int)std::size(apKinds)))
	{
		s_LastKind = Kind + 1;
		SetConfigInt("xc_weather", s_LastKind);
	}

	if(Kind + 1 == CWeather::WEATHER_CUSTOM)
	{
		if(!m_WeatherScanned)
		{
			Images.Refresh();
			m_WeatherScanned = true;
		}
		Images.UpdateThumbnails();
		ParamLabel(Tr("My weather pictures"));
		std::vector<CXcBackgrounds::SEntry> &vEntries = Images.Entries();
		CTileGrid Grid(AvailWidth(), S(90.0f), S(10.0f));
		for(const CXcBackgrounds::SEntry &Entry : vEntries)
		{
			Grid.Next();
			const std::string Key = "w:" + Entry.m_Name;
			const bool Selected = str_comp(g_Config.m_XcWeatherFile, Entry.m_Name.c_str()) == 0;
			char aName[128];
			fs_split_file_extension(Entry.m_Name.c_str(), aName, sizeof(aName));
			bool Delete;
			const char *pBadge = Entry.m_Broken ? Tr("broken") : (Entry.m_Animated ? "GIF" : nullptr);
			if(BackgroundTile(Key.c_str(), Entry.m_Thumbnail, Entry.m_Aspect, aName, pBadge, Selected, !Entry.m_Builtin, &Delete, Grid.Width(), 0.0f, 1.0f, 1.0f, true))
				SetConfigString("xc_weather_file", Entry.m_Name.c_str());
			if(Delete)
			{
				if(m_BgDeleteArmed == Key && ImGui::GetTime() < m_BgDeleteArmedUntil)
				{
					m_BgDeleteArmed.clear();
					m_WeatherPendingRemove = Entry.m_Name;
				}
				else
				{
					m_BgDeleteArmed = Key;
					m_BgDeleteArmedUntil = (float)ImGui::GetTime() + 3.0f;
				}
			}
		}
		if(vEntries.empty())
			Description(Tr("No pictures yet: drag a png or gif onto the game window, paste its path or open the folder"));
		ImGui::Dummy(ImVec2(0.0f, S(6.0f)));
		const float Gap = S(8.0f);
		const float ButtonWidth = S(110.0f);
		InputText("##weather_path", m_aWeatherPath, sizeof(m_aWeatherPath), Tr("Path to a picture or gif"), std::max(AvailWidth() - ButtonWidth * 2.0f - Gap * 2.0f, S(80.0f)));
		const bool Enter = ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
		ImGui::SameLine(0.0f, Gap);
		if((Button(Tr("Add"), true, ButtonWidth) || Enter) && m_aWeatherPath[0])
			m_WeatherPendingAdd = m_aWeatherPath;
		ImGui::SameLine(0.0f, Gap);
		if(Button(Tr("Open folder"), false, ButtonWidth))
		{
			char aFolder[IO_MAX_PATH_LENGTH];
			Storage()->CreateFolder(CWeather::FOLDER, IStorage::TYPE_SAVE);
			Storage()->GetCompletePath(IStorage::TYPE_SAVE, Images.Folder(), aFolder, sizeof(aFolder));
			Client()->ViewFile(aFolder);
			m_WeatherScanned = false;
		}
		ConfigSlider(Tr("Gif speed"), "xc_weather_gif_speed", &g_Config.m_XcWeatherGifSpeed, 10, 400, "%d%%");
	}

	ConfigSlider(Tr("Amount"), "xc_weather_density", &g_Config.m_XcWeatherDensity, 1, 100, "%d%%");
	ConfigSlider(Tr("Size"), "xc_weather_size", &g_Config.m_XcWeatherSize, 25, 300, "%d%%");
	ConfigSlider(Tr("Speed"), "xc_weather_speed", &g_Config.m_XcWeatherSpeed, 10, 300, "%d%%");
	ConfigSlider(Tr("Wind"), "xc_weather_wind", &g_Config.m_XcWeatherWind, 0, 400, "%d%%");
	ConfigSlider(Tr("Spin"), "xc_weather_spin", &g_Config.m_XcWeatherSpin, 0, 400, "%d%%");
	ConfigSlider(Tr("Opacity"), "xc_weather_alpha", &g_Config.m_XcWeatherAlpha, 5, 100, "%d%%");
	ToggleRow(Tr("Own color"), "xc_weather_tint", &g_Config.m_XcWeatherTint);
	if(g_Config.m_XcWeatherTint)
		EffectColorPicker("##weather_color", &g_Config.m_XcWeatherColorMode, &g_Config.m_XcWeatherColor);
	if(Kind + 1 == CWeather::WEATHER_STARS || Kind + 1 == CWeather::WEATHER_CRYSTALS || Kind + 1 == CWeather::WEATHER_FIREFLIES || Kind + 1 == CWeather::WEATHER_EMBERS)
		ToggleRow(Tr("Glow"), "xc_weather_glow", &g_Config.m_XcWeatherGlow);
	ToggleRow(Tr("In game"), "xc_weather_ingame", &g_Config.m_XcWeatherInGame);
	ToggleRow(Tr("In the main menu"), "xc_weather_menu", &g_Config.m_XcWeatherInMenu);
	EndCard();
}

void CXcMenu::HandleWeatherFile(const char *pPath)
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_copy(aPath, str_skip_whitespaces_const(pPath));
	str_utf8_trim_right(aPath);
	int Length = str_length(aPath);
	// "Copy as path" in the Windows explorer puts quotes around the path
	if(Length >= 2 && aPath[0] == '"' && aPath[Length - 1] == '"')
	{
		aPath[Length - 1] = '\0';
		mem_move(aPath, aPath + 1, Length - 1);
	}
	if(aPath[0] == '\0')
		return;
	Storage()->CreateFolder(CWeather::FOLDER, IStorage::TYPE_SAVE);
	char aAdded[128];
	switch(GameClient()->m_Weather.Images().AddFile(aPath, aAdded, sizeof(aAdded)))
	{
	case CXcBackgrounds::ADD_IMAGE:
		SetConfigString("xc_weather_file", aAdded);
		SetConfigInt("xc_weather", CWeather::WEATHER_CUSTOM);
		Toast(Tr("Picture added, it falls now"));
		m_aWeatherPath[0] = '\0';
		m_WeatherScanned = false;
		break;
	case CXcBackgrounds::ADD_UNSUPPORTED:
		Toast(Tr("Only png, jpg, gif and bmp files"), TOAST_ERROR);
		break;
	default:
		Toast(Tr("Could not read this file"), TOAST_ERROR);
		break;
	}
}

void CXcMenu::RenderVisualsGeneral()
{
	BeginCardGrid("##vis_general", 2);
	if(BeginCard("##fx_glow_tune", Tr("Glow"), Tr("How bright and how wide every effect glows")))
	{
		ParamLabel(Tr("Effect look"));
		const char *apLooks[] = {Tr("Classic"), Tr("Soft light")};
		ConfigSegmented("##fx_style", "xc_fx_style", &g_Config.m_XcFxStyle, apLooks, 2);
		ConfigSlider(Tr("Bloom halo"), "xc_bloom", &g_Config.m_XcBloom, 0, 200, "%d%%");
		ConfigSlider(Tr("Bloom threshold"), "xc_bloom_threshold", &g_Config.m_XcBloomThreshold, 0, 100, "%d%%");
		ConfigSlider(Tr("All effects: strength"), "xc_fxglow_all", &g_Config.m_XcFxglowAll, 0, 300, "%d%%");
		ConfigSlider(Tr("All effects: size"), "xc_fxglow_all_size", &g_Config.m_XcFxglowAllSize, 25, 300, "%d%%");

		// one effect at a time: its chip, then its two sliders; a dot marks effects that differ from 100 %
		struct SGroup
		{
			const char *m_pName;
			const char *m_pStrength;
			int *m_pStrengthValue;
			const char *m_pSize;
			int *m_pSizeValue;
		};
		const SGroup aGroups[] = {
			{Tr("Trail"), "xc_fxglow_trail", &g_Config.m_XcFxglowTrail, "xc_fxglow_trail_size", &g_Config.m_XcFxglowTrailSize},
			{Tr("Hook"), "xc_fxglow_hook", &g_Config.m_XcFxglowHook, "xc_fxglow_hook_size", &g_Config.m_XcFxglowHookSize},
			{Tr("Particles"), "xc_fxglow_particles", &g_Config.m_XcFxglowParticles, "xc_fxglow_particles_size", &g_Config.m_XcFxglowParticlesSize},
			{Tr("Pickups and shots"), "xc_fxglow_items", &g_Config.m_XcFxglowItems, "xc_fxglow_items_size", &g_Config.m_XcFxglowItemsSize},
			{Tr("Lasers"), "xc_fxglow_lasers", &g_Config.m_XcFxglowLasers, "xc_fxglow_lasers_size", &g_Config.m_XcFxglowLasersSize},
			{Tr("Accessory"), "xc_fxglow_accessory", &g_Config.m_XcFxglowAccessory, "xc_fxglow_accessory_size", &g_Config.m_XcFxglowAccessorySize},
			{Tr("Freeze"), "xc_fxglow_freeze", &g_Config.m_XcFxglowFreeze, "xc_fxglow_freeze_size", &g_Config.m_XcFxglowFreezeSize},
			{Tr("Crosshair"), "xc_fxglow_crosshair", &g_Config.m_XcFxglowCrosshair, "xc_fxglow_crosshair_size", &g_Config.m_XcFxglowCrosshairSize},
			{Tr("Weather"), "xc_fxglow_weather", &g_Config.m_XcFxglowWeather, "xc_fxglow_weather_size", &g_Config.m_XcFxglowWeatherSize},
			{Tr("Floating lights"), "xc_fxglow_floaters", &g_Config.m_XcFxglowFloaters, "xc_fxglow_floaters_size", &g_Config.m_XcFxglowFloatersSize},
			{Tr("Dummy arrow"), "xc_fxglow_arrow", &g_Config.m_XcFxglowArrow, "xc_fxglow_arrow_size", &g_Config.m_XcFxglowArrowSize},
			{Tr("Cape and scarf"), "xc_fxglow_cape", &g_Config.m_XcFxglowCape, "xc_fxglow_cape_size", &g_Config.m_XcFxglowCapeSize},
		};
		constexpr int NUM_GROUPS = (int)std::size(aGroups);
		const char *apNames[NUM_GROUPS];
		bool aChanged[NUM_GROUPS];
		bool AnyChanged = g_Config.m_XcFxglowAll != 100 || g_Config.m_XcFxglowAllSize != 100;
		for(int i = 0; i < NUM_GROUPS; i++)
		{
			apNames[i] = aGroups[i].m_pName;
			aChanged[i] = *aGroups[i].m_pStrengthValue != 100 || *aGroups[i].m_pSizeValue != 100;
			AnyChanged |= aChanged[i];
		}
		static int s_Group = 0;
		ParamLabel(Tr("Effect"));
		ChoiceChips("##glow_groups", &s_Group, apNames, NUM_GROUPS, aChanged);
		const SGroup &Group = aGroups[std::clamp(s_Group, 0, NUM_GROUPS - 1)];
		ConfigSlider(Tr("Strength"), Group.m_pStrength, Group.m_pStrengthValue, 0, 300, "%d%%");
		ConfigSlider(Tr("Size"), Group.m_pSize, Group.m_pSizeValue, 25, 300, "%d%%");
		if(AnyChanged && Button(Tr("Reset all glow")))
		{
			SetConfigInt("xc_fxglow_all", 100);
			SetConfigInt("xc_fxglow_all_size", 100);
			for(const SGroup &Reset : aGroups)
			{
				SetConfigInt(Reset.m_pStrength, 100);
				SetConfigInt(Reset.m_pSize, 100);
			}
			Toast(Tr("Glow reset"), TOAST_INFO);
		}
		EndCard();
	}
	if(BeginCard("##fx_particles_all", Tr("Particles of all effects"), Tr("One look for the sparks of every effect, and physics: they fall and bounce off the blocks")))
	{
		ParamLabel(Tr("Shape"));
		const char *apShapes[] = {Tr("As the effect"), Tr("Dots"), Tr("Stars"), Tr("Hearts"), Tr("Rhombi"), Tr("Snowflakes"), Tr("Fireflies")};
		ChoiceChips("##particles_shape", &g_Config.m_XcParticlesShape, apShapes, 7);
		ToggleRow(Tr("Physics: fall and bounce off the blocks"), "xc_particles_physics", &g_Config.m_XcParticlesPhysics);
		if(g_Config.m_XcParticlesPhysics)
		{
			ConfigSlider(Tr("Gravity"), "xc_particles_gravity", &g_Config.m_XcParticlesGravity, 0, 200, "%d%%");
			ConfigSlider(Tr("Bounce"), "xc_particles_bounce", &g_Config.m_XcParticlesBounce, 0, 100, "%d%%");
		}
		Description(Tr("Amount, life, speed and size are set in the card of each effect"));
		EndCard();
	}
	if(BeginCard("##fx_over_tiles", Tr("Over the blocks"), Tr("Effects and hats can be drawn over the map blocks, they stay visible behind walls")))
	{
		ToggleRow(Tr("Effects over the blocks"), "xc_fx_over_tiles", &g_Config.m_XcFxOverTiles);
		ToggleRow(Tr("Hats over the blocks"), "xc_hats_over_tiles", &g_Config.m_XcHatsOverTiles);
		Description(Tr("Jumps, rings, freeze, death, finish and accessories. Hats placed behind the tee stay behind"));
		EndCard();
	}
	if(BeginCard("##fx_juice", Tr("Juice"), Tr("Screen flashes and camera shake. Which events use them is chosen in the card of each event")))
	{
		ToggleRow(Tr("Screen flash"), "xc_screen_flash", &g_Config.m_XcScreenFlash);
		ConfigSlider(Tr("Flash strength"), "xc_screen_flash_strength", &g_Config.m_XcScreenFlashStrength, 5, 100, "%d%%");
		ToggleRow(Tr("Camera shake"), "xc_camera_shake", &g_Config.m_XcCameraShake);
		ConfigSlider(Tr("Shake strength"), "xc_camera_shake_strength", &g_Config.m_XcCameraShakeStrength, 10, 300, "%d%%");
		Description(Tr("Only the picture moves, aim and movement stay the same"));
		EndCard();
	}
	if(BeginCard("##fx_stock", Tr("Stock effects"), Tr("The standard game effects are shown or hidden in the card of each event. Here all at once")))
	{
		int *apStock[] = {&g_Config.m_XcReplaceHammer, &g_Config.m_XcReplaceExplosion, &g_Config.m_XcReplaceLaser, &g_Config.m_XcReplaceTrails, &g_Config.m_XcReplaceDamage, &g_Config.m_XcReplaceMuzzle, &g_Config.m_XcReplaceBullets, &g_Config.m_XcReplaceAirJump, &g_Config.m_XcReplaceSkid, &g_Config.m_XcReplaceSpawn, &g_Config.m_XcReplaceDeath, &g_Config.m_XcReplaceFreeze, &g_Config.m_XcReplaceConfetti, &g_Config.m_XcReplaceNinjaShine, &g_Config.m_XcReplaceSparkle};
		int Hidden = 0;
		for(int *pValue : apStock)
			Hidden += *pValue ? 1 : 0;
		char aCount[128];
		str_format(aCount, sizeof(aCount), Tr("Hidden: %d of %d"), Hidden, (int)std::size(apStock));
		Description(aCount);
		const float Half = (AvailWidth() - S(8.0f)) * 0.5f;
		if(Button(Tr("Hide all"), false, Half))
			for(int *pValue : apStock)
				*pValue = 1;
		ImGui::SameLine(0.0f, S(8.0f));
		if(Button(Tr("Show all"), false, Half))
			for(int *pValue : apStock)
				*pValue = 0;
		Description(Tr("Sounds stay. Hide a standard effect and turn ours on, or the event shows nothing"));
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsTee()
{
	BeginCardGrid("##vis_tee", 2);
	if(BeginCard("##fx_glow", Tr("Neon glow"), Tr("Soft light and a crisp outline around the tee"), &g_Config.m_XcGlow))
	{
		if(!Graphics()->XcRenderAvailable())
			Description(Tr("Needs OpenGL 3.3, see Settings > Graphics"));
		EffectColorPicker("##glow_color", &g_Config.m_XcGlowColorMode, &g_Config.m_XcGlowColor);
		ConfigSlider(Tr("Size"), "xc_glow_size", &g_Config.m_XcGlowSize, 25, 300, "%d%%");
		ConfigSlider(Tr("Glow strength"), "xc_glow_strength", &g_Config.m_XcGlowStrength, 10, 300, "%d%%");
		ConfigSlider(Tr("Outline"), "xc_glow_outline", &g_Config.m_XcGlowOutline, 0, 300, "%d%%");
		ToggleRow(Tr("Pulse"), "xc_glow_pulse", &g_Config.m_XcGlowPulse);
		TargetChips("##glow_targets", &g_Config.m_XcGlowSelf, &g_Config.m_XcGlowDummy, &g_Config.m_XcGlowOthers);
		EndCard();
	}
	const char *apShaders[] = {Tr("Gradient"), Tr("Rainbow"), Tr("Galaxy"), Tr("Waves"), Tr("Chrome")};
	if(StyleCard("##fx_tee_shader", Tr("Tee shader"), Tr("A moving pattern painted over the skin: gradient, rainbow, galaxy with stars, waves or chrome"), "xc_tee_shader", &g_Config.m_XcTeeShader, apShaders, 5))
	{
		if(!Graphics()->XcRenderAvailable())
			Description(Tr("Needs OpenGL 3.3, see Settings > Graphics"));
		if(g_Config.m_XcTeeShader != 2)
		{
			EffectColorPicker("##tee_shader_color", &g_Config.m_XcTeeShaderColorMode, &g_Config.m_XcTeeShaderColor);
			if(g_Config.m_XcTeeShader != 5)
				ConfigColorRow(Tr("Second color"), &g_Config.m_XcTeeShaderColor2, false);
		}
		ConfigSlider(Tr("Strength"), "xc_tee_shader_alpha", &g_Config.m_XcTeeShaderAlpha, 0, 100, "%d%%");
		ConfigSlider(Tr("Pattern size"), "xc_tee_shader_scale", &g_Config.m_XcTeeShaderScale, 25, 400, "%d%%");
		ConfigSlider(Tr("Speed"), "xc_tee_shader_speed", &g_Config.m_XcTeeShaderSpeed, 0, 400, "%d%%");
		ConfigSlider(Tr("Keep the skin shading"), "xc_tee_shader_shade", &g_Config.m_XcTeeShaderShade, 0, 100, "%d%%");
		TargetChips("##tee_shader_targets", &g_Config.m_XcTeeShaderSelf, &g_Config.m_XcTeeShaderDummy, &g_Config.m_XcTeeShaderOthers);
		EndCard();
	}
	if(BeginCard("##fx_jelly", Tr("Jelly tee"), Tr("Squashes when landing, stretches when jumping and wobbles back"), &g_Config.m_XcJelly))
	{
		ConfigSlider(Tr("Softness"), "xc_jelly_strength", &g_Config.m_XcJellyStrength, 10, 300, "%d%%");
		TargetChips("##jelly_targets", &g_Config.m_XcJellySelf, &g_Config.m_XcJellyDummy, &g_Config.m_XcJellyOthers);
		EndCard();
	}
	if(BeginCard("##fx_freeze", Tr("Freeze"), Tr("Everything that happens when a tee freezes, stays frozen and thaws")))
	{
		ToggleRow(Tr("Icy aura while frozen"), "xc_freeze_fx", &g_Config.m_XcFreezeFx);
		ToggleRow(Tr("Shards on freeze and unfreeze"), "xc_freeze_burst", &g_Config.m_XcFreezeBurst);
		EffectColorPicker("##freeze_color", &g_Config.m_XcFreezeColorMode, &g_Config.m_XcFreezeColor);
		ToggleRow(Tr("Hide the standard snowflakes"), "xc_replace_freeze", &g_Config.m_XcReplaceFreeze);
		ToggleRow(Tr("Countdown stars over the tee"), "cl_freeze_stars", &g_Config.m_ClFreezeStars);
		ToggleRow(Tr("Screen flash when you freeze"), "xc_flash_freeze", &g_Config.m_XcFlashFreeze);
		ToggleRow(Tr("Camera shake when you freeze"), "xc_shake_freeze", &g_Config.m_XcShakeFreeze);
		ToggleRow(Tr("Sound on freeze and unfreeze"), "xc_event_sound_freeze", &g_Config.m_XcEventSoundFreeze);
		TargetChips("##freeze_targets", &g_Config.m_XcFreezeSelf, &g_Config.m_XcFreezeDummy, &g_Config.m_XcFreezeOthers);
		ParticleTuning(FxTune::FREEZE);
		EndCard();
	}
	if(BeginCard("##fx_ninja", Tr("Ninja and invincible"), Tr("The standard shine of the ninja sword and the sparkles of invincible tees")))
	{
		ToggleRow(Tr("Hide the shine of the ninja sword"), "xc_replace_ninja_shine", &g_Config.m_XcReplaceNinjaShine);
		ToggleRow(Tr("Hide the sparkles of invincible tees"), "xc_replace_sparkle", &g_Config.m_XcReplaceSparkle);
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsMovement()
{
	BeginCardGrid("##vis_movement", 2);
	const char *apTrails[] = {Tr("Ribbon"), Tr("Sparks"), Tr("Stars"), Tr("Comet"), Tr("Dashes"), Tr("Ghosts")};
	if(StyleCard("##fx_trail", Tr("Trail"), Tr("A glowing ribbon behind the tee"), "xc_trail", &g_Config.m_XcTrail, apTrails, 6))
	{
		if(g_Config.m_XcTrail >= 2 && g_Config.m_XcTrail != 4)
			ConfigSlider(Tr("Density"), "xc_trail_density", &g_Config.m_XcTrailDensity, 25, 300, "%d%%");
		if(g_Config.m_XcTrail == 5)
		{
			const char *apDashes[] = {Tr("Stretch"), Tr("Pop"), Tr("Spin"), Tr("Blink"), Tr("Spark")};
			ConfigSegmented("##trail_dash", "xc_trail_dash", &g_Config.m_XcTrailDash, apDashes, 5);
			if(!g_Config.m_XcFxStyle)
				Description(Tr("Dashes need the new effect look (Visuals > General > Glow)"));
		}
		EffectColorPicker("##trail_color", &g_Config.m_XcTrailColorMode, &g_Config.m_XcTrailColor);
		ConfigSlider(Tr("Length"), "xc_trail_length", &g_Config.m_XcTrailLength, 100, 2000, "%d ms");
		ConfigSlider(Tr("Width"), "xc_trail_width", &g_Config.m_XcTrailWidth, 25, 300, "%d%%");
		ConfigSlider(Tr("Glow"), "xc_trail_glow", &g_Config.m_XcTrailGlow, 0, 200, "%d%%");
		if(g_Config.m_XcTrail == 1)
			ToggleRow(Tr("In pieces with gaps"), "xc_trail_segments", &g_Config.m_XcTrailSegments);
		TargetChips("##trail_targets", &g_Config.m_XcTrailSelf, &g_Config.m_XcTrailDummy, &g_Config.m_XcTrailOthers);
		if(g_Config.m_XcTrailOthers)
			ConfigSlider(Tr("Others: up to blocks away"), "xc_trail_max_distance", &g_Config.m_XcTrailMaxDistance, 0, 200, g_Config.m_XcTrailMaxDistance ? "%d" : Tr("No limit"));
		ParticleTuning(FxTune::TRAIL);
		EndCard();
	}
	const char *apJumps[] = {Tr("Rings"), Tr("Sparks"), Tr("Stars")};
	if(StyleCard("##fx_jump", Tr("Jumps"), Tr("Effect under the feet on every jump"), "xc_jump_fx", &g_Config.m_XcJumpFx, apJumps, 3))
	{
		EffectColorPicker("##jump_color", &g_Config.m_XcJumpFxColorMode, &g_Config.m_XcJumpFxColor);
		ConfigSlider(Tr("Size"), "xc_jump_fx_size", &g_Config.m_XcJumpFxSize, 25, 300, "%d%%");
		if(g_Config.m_XcJumpFx == 1 && g_Config.m_XcFxStyle)
			ConfigSlider(Tr("Duration"), "xc_jump_fx_duration", &g_Config.m_XcJumpFxDuration, 200, 3000, "%d ms");
		ToggleRow(Tr("Also on double jumps"), "xc_jump_fx_air", &g_Config.m_XcJumpFxAir);
		ToggleRow(Tr("Hide the standard double jump puffs"), "xc_replace_air_jump", &g_Config.m_XcReplaceAirJump);
		TargetChips("##jump_targets", &g_Config.m_XcJumpFxSelf, &g_Config.m_XcJumpFxDummy, &g_Config.m_XcJumpFxOthers);
		ParticleTuning(FxTune::JUMP);
		EndCard();
	}
	if(BeginCard("##fx_land", Tr("Landing"), Tr("A ring on the ground after a high fall"), &g_Config.m_XcLandFx))
	{
		Description(Tr("Color, size and players as for the jumps"));
		EndCard();
	}
	if(BeginCard("##fx_run", Tr("Running"), Tr("Dust under the feet of a fast tee"), &g_Config.m_XcRunDust))
	{
		ToggleRow(Tr("Hide the standard skid smoke"), "xc_replace_skid", &g_Config.m_XcReplaceSkid);
		Description(Tr("Color and players as for the jumps"));
		ParticleTuning(FxTune::DUST);
		EndCard();
	}
	const char *apHooks[] = {Tr("Chain"), Tr("Line"), Tr("Glow")};
	if(StyleCard("##fx_hook", Tr("Hook"), Tr("Colored chain, clean line or a glowing beam"), "xc_hook_style", &g_Config.m_XcHookStyle, apHooks, 3))
	{
		EffectColorPicker("##hook_color", &g_Config.m_XcHookColorMode, &g_Config.m_XcHookColor);
		ConfigSlider(Tr("Width"), "xc_hook_width", &g_Config.m_XcHookWidth, 25, 300, "%d%%");
		TargetChips("##hook_targets", &g_Config.m_XcHookSelf, &g_Config.m_XcHookDummy, &g_Config.m_XcHookOthers);
		EndCard();
	}
	if(BeginCard("##fx_hook_hit", Tr("Hook grabs"), Tr("What shows where the hook catches a block or a player, or bounces off an unhookable block")))
	{
		const char *apHits[] = {Tr("Nothing"), Tr("Ring"), Tr("Sparks"), Tr("Stars")};
		ParamLabel(Tr("Grabs a block"));
		ConfigSegmented("##hook_hit", "xc_hook_hit_fx", &g_Config.m_XcHookHitFx, apHits, 4);
		ParamLabel(Tr("Grabs a player"));
		ConfigSegmented("##hook_hit_player", "xc_hook_hit_player_fx", &g_Config.m_XcHookHitPlayerFx, apHits, 4);
		ParamLabel(Tr("Bounces off an unhookable block"));
		ConfigSegmented("##hook_nohook", "xc_hook_nohook_fx", &g_Config.m_XcHookNohookFx, apHits, 4);
		Description(Tr("In the color of the hook, the unhookable one in red. Players as for the hook"));
		ParticleTuning(FxTune::HOOK);
		EndCard();
	}
	if(BeginCard("##hook_line", Tr("Hook aim line"), Tr("The line that shows where your hook would catch")))
	{
		const char *apModes[] = {Tr("Off"), Tr("When aiming"), Tr("Always")};
		ParamLabel(Tr("My line"));
		ConfigSegmented("##hook_coll_own", "cl_show_hook_coll_own", &g_Config.m_ClShowHookCollOwn, apModes, 3);
		ParamLabel(Tr("Other players"));
		ConfigSegmented("##hook_coll_other", "cl_show_hook_coll_other", &g_Config.m_ClShowHookCollOther, apModes, 3);
		ConfigSlider(Tr("My line width"), "cl_hook_coll_size", &g_Config.m_ClHookCollSize, 0, 20);
		ConfigSlider(Tr("Others' line width"), "cl_hook_coll_size_other", &g_Config.m_ClHookCollSizeOther, 0, 20);
		ConfigSlider(Tr("Visibility"), "cl_hook_coll_alpha", &g_Config.m_ClHookCollAlpha, 0, 100, "%d%%");
		ConfigColorRow(Tr("Catches nothing"), &g_Config.m_ClHookCollColorNoColl, false);
		ConfigColorRow(Tr("Catches a wall"), &g_Config.m_ClHookCollColorHookableColl, false);
		ConfigColorRow(Tr("Catches a player"), &g_Config.m_ClHookCollColorTeeColl, false);
		EndCard();
	}
	if(BeginCard("##fx_motion_blur", Tr("Motion blur"), Tr("Fast tees leave a soft smear of copies behind them"), &g_Config.m_XcMotionBlur))
	{
		ConfigSlider(Tr("Blur length"), "xc_motion_blur_strength", &g_Config.m_XcMotionBlurStrength, 10, 100, "%d%%");
		TargetChips("##motion_blur_targets", &g_Config.m_XcMotionBlurSelf, &g_Config.m_XcMotionBlurDummy, &g_Config.m_XcMotionBlurOthers);
		EndCard();
	}
	if(BeginCard("##fx_speed_lines", Tr("Speed lines"), Tr("Thin streaks behind the tee at high speed, like in anime"), &g_Config.m_XcSpeedLines))
	{
		ConfigSlider(Tr("Brightness"), "xc_speed_lines_alpha", &g_Config.m_XcSpeedLinesAlpha, 10, 100, "%d%%");
		Description(Tr("Shown for the same tees as the motion blur"));
		EndCard();
	}
	EndCardGrid();
}

// a stage with your tee wearing the cape and the scarf; the eyes follow the mouse, so the cloth swings over
void CXcMenu::CapePreview(const char *pId)
{
	const STheme &t = Theme();
	ImDrawList *pDraw = ImGui::GetWindowDrawList();
	const float Height = S(170.0f);
	const ImVec2 Min = ImGui::GetCursorScreenPos();
	const ImVec2 Max(Min.x + AvailWidth(), Min.y + Height);
	ImGui::InvisibleButton(pId, ImVec2(AvailWidth(), Height));
	pDraw->AddRectFilled(Min, Max, Col(t.m_Track), S(12.0f));
	const ImVec2 Mid((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
	pDraw->AddCircleFilled(Mid, Height * 0.36f, Col(t.m_Accent, 0.08f), 64);
	const float Rect = Height * 0.85f;
	AddTeePreview(pDraw, ImVec2(Mid.x - Rect * 0.5f, Mid.y - Rect * 0.5f), ImVec2(Mid.x + Rect * 0.5f, Mid.y + Rect * 0.5f));
	if(!g_Config.m_XcCapeSelf && !g_Config.m_XcScarfSelf)
		Description(Tr("Turn on \"Mine\" below to see it here"));
}

void CXcMenu::RenderVisualsCosmeticsExtra()
{
	BeginCardGrid("##vis_cosmetics_extra", 2);
	const char *apAccessories[] = {Tr("Cone hat"), Tr("Halo"), Tr("Crown")};
	if(StyleCard("##fx_accessory", Tr("Accessory"), Tr("Something on the head that sways with the tee"), "xc_accessory", &g_Config.m_XcAccessory, apAccessories, 3))
	{
		EffectColorPicker("##accessory_color", &g_Config.m_XcAccessoryColorMode, &g_Config.m_XcAccessoryColor);
		ConfigSlider(Tr("Size"), "xc_accessory_size", &g_Config.m_XcAccessorySize, 50, 200, "%d%%");
		ConfigSlider(Tr("Opacity"), "xc_accessory_alpha", &g_Config.m_XcAccessoryAlpha, 10, 100, "%d%%");
		ConfigSlider(Tr("Sway"), "xc_accessory_physics", &g_Config.m_XcAccessoryPhysics, 0, 200, "%d%%");
		if(g_Config.m_XcAccessory == 1)
			ConfigSlider(Tr("Height"), "xc_accessory_height", &g_Config.m_XcAccessoryHeight, 30, 250, "%d%%");
		// the same values as the accessory group in Visuals > World > Glow
		ConfigSlider(Tr("Glow strength"), "xc_fxglow_accessory", &g_Config.m_XcFxglowAccessory, 0, 300, "%d%%");
		ConfigSlider(Tr("Glow size"), "xc_fxglow_accessory_size", &g_Config.m_XcFxglowAccessorySize, 25, 300, "%d%%");
		ToggleRow(Tr("Also on friends"), "xc_accessory_friends", &g_Config.m_XcAccessoryFriends);
		TargetChips("##accessory_targets", &g_Config.m_XcAccessorySelf, &g_Config.m_XcAccessoryDummy, &g_Config.m_XcAccessoryOthers);
		EndCard();
	}
	if(BeginCard("##fx_cape", Tr("Cape"), Tr("Cloth that hangs from the shoulders, flaps on jumps and streams away from the hook"), &g_Config.m_XcCape))
	{
		CapePreview("##cape_preview");
		const char *apLooks[] = {Tr("Energy"), Tr("Neon"), Tr("Cloth")};
		ParamLabel(Tr("Look"));
		ConfigSegmented("##cape_look", "xc_cape_look", &g_Config.m_XcCapeLook, apLooks, 3);
		ParamLabel(Tr("Shape"));
		const char *apShapes[] = {Tr("Classic"), Tr("Pointed"), Tr("Ragged"), Tr("Royal")};
		ConfigSegmented("##cape_shape", "xc_cape_shape", &g_Config.m_XcCapeShape, apShapes, 4);
		ParamLabel(Tr("Outer side"));
		EffectColorPicker("##cape_color", &g_Config.m_XcCapeColorMode, &g_Config.m_XcCapeColor);
		ParamLabel(Tr("Inner side"));
		const char *apInner[] = {Tr("Darker"), Tr("Own")};
		ConfigSegmented("##cape_inner", "xc_cape_inner_mode", &g_Config.m_XcCapeInnerMode, apInner, 2);
		if(g_Config.m_XcCapeInnerMode)
			ConfigColorRow(Tr("Inner color"), &g_Config.m_XcCapeInnerColor, false);
		ParamLabel(Tr("Pattern"));
		const char *apPatterns[] = {Tr("Plain"), Tr("Stripes"), Tr("Gradient"), Tr("Picture")};
		ConfigSegmented("##cape_pattern", "xc_cape_pattern", &g_Config.m_XcCapePattern, apPatterns, 4);
		if(g_Config.m_XcCapePattern == CXcCape::PATTERN_PICTURE)
		{
			CXcBackgrounds &Pictures = GameClient()->m_XcCape.Pictures();
			if(!m_CapesScanned)
			{
				Pictures.Refresh();
				m_CapesScanned = true;
			}
			std::vector<const char *> vpNames;
			int Selected = -1;
			for(const CXcBackgrounds::SEntry &Entry : Pictures.Entries())
			{
				if(str_comp(Entry.m_Name.c_str(), g_Config.m_XcCapeFile) == 0)
					Selected = (int)vpNames.size();
				vpNames.push_back(Entry.m_Name.c_str());
			}
			if(vpNames.empty())
				Description(Tr("Put a png or gif into the folder, the picture is stretched over the cape"));
			else
			{
				int Choice = Selected;
				ChoiceChips("##cape_files", &Choice, vpNames.data(), (int)vpNames.size());
				if(Choice != Selected && Choice >= 0)
					SetConfigString("xc_cape_file", vpNames[Choice]);
			}
			const float Half = (AvailWidth() - S(8.0f)) * 0.5f;
			if(Button(Tr("Open folder"), false, Half))
			{
				Storage()->CreateFolder("xclient", IStorage::TYPE_SAVE);
				Storage()->CreateFolder(Pictures.Folder(), IStorage::TYPE_SAVE);
				char aFolder[IO_MAX_PATH_LENGTH];
				Storage()->GetCompletePath(IStorage::TYPE_SAVE, Pictures.Folder(), aFolder, sizeof(aFolder));
				Client()->ViewFile(aFolder);
			}
			ImGui::SameLine(0.0f, S(8.0f));
			if(Button(Tr("Refresh"), false, Half))
				m_CapesScanned = false;
		}
		ParamLabel(Tr("Trim"));
		const char *apTrims[] = {Tr("None"), Tr("Edge"), Tr("Fur")};
		ConfigSegmented("##cape_trim", "xc_cape_trim", &g_Config.m_XcCapeTrim, apTrims, 3);
		if(g_Config.m_XcCapeTrim == 1)
			ConfigColorRow(Tr("Edge color"), &g_Config.m_XcCapeTrimColor, false);
		ConfigSlider(Tr("Length"), "xc_cape_length", &g_Config.m_XcCapeLength, 50, 200, "%d%%");
		ConfigSlider(Tr("Width"), "xc_cape_width", &g_Config.m_XcCapeWidth, 50, 200, "%d%%");
		ConfigSlider(Tr("Wind"), "xc_cape_wind", &g_Config.m_XcCapeWind, 0, 200, "%d%%");
		ConfigSlider(Tr("Reaction to jumps and the hook"), "xc_cape_react", &g_Config.m_XcCapeReact, 0, 200, "%d%%");
		ToggleRow(Tr("Wave of light"), "xc_cape_wave", &g_Config.m_XcCapeWave);
		ToggleRow(Tr("Sparks at speed"), "xc_cape_sparks", &g_Config.m_XcCapeSparks);
		ConfigSlider(Tr("Glow strength"), "xc_fxglow_cape", &g_Config.m_XcFxglowCape, 0, 300, "%d%%");
		ConfigSlider(Tr("Glow size"), "xc_fxglow_cape_size", &g_Config.m_XcFxglowCapeSize, 25, 300, "%d%%");
		ConfigSlider(Tr("Opacity"), "xc_cape_alpha", &g_Config.m_XcCapeAlpha, 10, 100, "%d%%");
		TargetChips("##cape_targets", &g_Config.m_XcCapeSelf, &g_Config.m_XcCapeDummy, &g_Config.m_XcCapeOthers);
		EndCard();
	}
	if(BeginCard("##fx_scarf", Tr("Scarf"), Tr("A knitted scarf around the neck, its ends fly behind the tee"), &g_Config.m_XcScarf))
	{
		CapePreview("##scarf_preview");
		const char *apScarfLooks[] = {Tr("Energy"), Tr("Neon"), Tr("Knitted")};
		ParamLabel(Tr("Look"));
		ConfigSegmented("##scarf_look", "xc_scarf_look", &g_Config.m_XcScarfLook, apScarfLooks, 3);
		EffectColorPicker("##scarf_color", &g_Config.m_XcScarfColorMode, &g_Config.m_XcScarfColor);
		ToggleRow(Tr("Knitted stripes"), "xc_scarf_stripes", &g_Config.m_XcScarfStripes);
		if(g_Config.m_XcScarfStripes)
			ConfigColorRow(Tr("Stripe color"), &g_Config.m_XcScarfStripeColor, false);
		ParamLabel(Tr("Ends"));
		const char *apTails[] = {Tr("One"), Tr("Two")};
		const int aTailValues[] = {1, 2};
		ConfigSegmented("##scarf_tails", "xc_scarf_tails", &g_Config.m_XcScarfTails, apTails, 2, aTailValues);
		ToggleRow(Tr("Fringe at the ends"), "xc_scarf_fringe", &g_Config.m_XcScarfFringe);
		ConfigSlider(Tr("Length"), "xc_scarf_length", &g_Config.m_XcScarfLength, 50, 200, "%d%%");
		ConfigSlider(Tr("Wind"), "xc_scarf_wind", &g_Config.m_XcScarfWind, 0, 200, "%d%%");
		Description(Tr("Reaction to jumps, the wave of light, sparks and glow as for the cape"));
		TargetChips("##scarf_targets", &g_Config.m_XcScarfSelf, &g_Config.m_XcScarfDummy, &g_Config.m_XcScarfOthers);
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsWeapons()
{
	BeginCardGrid("##vis_weapons", 2);
	if(BeginCard("##weapon_view", Tr("Weapon in the hand"), Tr("Where the weapon sits, its size and how the hammer swings. Only the picture"), &g_Config.m_XcWeaponView))
	{
		ParamLabel(Tr("Hammer swing"));
		const char *apSwings[] = {Tr("Standard"), Tr("Wide"), Tr("Spin"), Tr("Smooth"), Tr("Stab"), Tr("Bounce")};
		ChoiceChips("##swing_style", &g_Config.m_XcSwingStyle, apSwings, 6);
		ConfigSlider(Tr("Swing width"), "xc_swing_strength", &g_Config.m_XcSwingStrength, 20, 200, "%d%%");
		ConfigSlider(Tr("Swing speed"), "xc_swing_speed", &g_Config.m_XcSwingSpeed, 50, 200, "%d%%");
		ConfigSlider(Tr("Hammer lean"), "xc_swing_tilt", &g_Config.m_XcSwingTilt, -45, 45, "%d°");
		ConfigSlider(Tr("Forward / back"), "xc_weapon_x", &g_Config.m_XcWeaponX, -20, 20);
		ConfigSlider(Tr("Up / down"), "xc_weapon_y", &g_Config.m_XcWeaponY, -20, 20);
		ConfigSlider(Tr("Weapon size"), "xc_weapon_size", &g_Config.m_XcWeaponSize, 50, 160, "%d%%");
		ToggleRow(Tr("Also for other players"), "xc_weapon_view_others", &g_Config.m_XcWeaponViewOthers);
		EndCard();
	}
	if(BeginCard("##fx_shots", Tr("Shots"), Tr("A flash at the gun, light around bullets and grenades, comet tails")))
	{
		ToggleRow(Tr("Flash at the gun"), "xc_impact_muzzle", &g_Config.m_XcImpactMuzzle);
		ToggleRow(Tr("Glow around bullets and grenades"), "xc_projectile_glow", &g_Config.m_XcProjectileGlow);
		ToggleRow(Tr("Comet tail"), "xc_projectile_trail", &g_Config.m_XcProjectileTrail);
		ToggleRow(Tr("Hide the standard muzzle flash"), "xc_replace_muzzle", &g_Config.m_XcReplaceMuzzle);
		ToggleRow(Tr("Hide the standard bullets"), "xc_replace_bullets", &g_Config.m_XcReplaceBullets);
		ToggleRow(Tr("Hide the standard smoke trails"), "xc_replace_trails", &g_Config.m_XcReplaceTrails);
		ParamLabel(Tr("Color of bullets and lasers"));
		const char *apModes[] = {Tr("By type"), Tr("Accent"), Tr("Own"), Tr("Rainbow")};
		ConfigSegmented("##shots_color_mode", "xc_items_color_mode", &g_Config.m_XcItemsColorMode, apModes, 4);
		if(g_Config.m_XcItemsColorMode == 2)
		{
			int Mode = 1; // the picker's own color part
			EffectColorPicker("##shots_color", &Mode, &g_Config.m_XcItemsColor);
		}
		Description(Tr("The same colors as the pickups. The flash at the gun is shown for the players of the bullet hits"));
		EndCard();
	}
	if(BeginCard("##fx_projectiles", Tr("Lasers"), Tr("Glow along rifle and shotgun lasers, or our own beam")))
	{
		ToggleRow(Tr("Laser glow"), "xc_laser_glow", &g_Config.m_XcLaserGlow);
		ConfigSlider(Tr("Laser glow strength"), "xc_laser_glow_strength", &g_Config.m_XcLaserGlowStrength, 10, 250, "%d%%");
		ToggleRow(Tr("Own beam instead of the standard one"), "xc_replace_laser", &g_Config.m_XcReplaceLaser);
		EndCard();
	}
	const char *apHammer[] = {Tr("Ring"), Tr("Sparks"), Tr("Stars")};
	if(StyleCard("##fx_hammer", Tr("Hammer hit"), Tr("A flash where the hammer lands"), "xc_hammer_fx", &g_Config.m_XcHammerFx, apHammer, 3))
	{
		EffectColorPicker("##hammer_color", &g_Config.m_XcHammerColorMode, &g_Config.m_XcHammerColor);
		ToggleRow(Tr("Hide the standard hit"), "xc_replace_hammer", &g_Config.m_XcReplaceHammer);
		ParticleTuning(FxTune::HAMMER);
		EndCard();
	}
	const char *apExplosions[] = {Tr("Shockwave"), Tr("Neon"), Tr("Sparks")};
	if(StyleCard("##fx_explosion", Tr("Grenade explosion"), Tr("A shockwave, neon rings or hot sparks where a grenade explodes"), "xc_explosion_fx", &g_Config.m_XcExplosionFx, apExplosions, 3))
	{
		EffectColorPicker("##explosion_color", &g_Config.m_XcExplosionColorMode, &g_Config.m_XcExplosionColor);
		ConfigSlider(Tr("Size"), "xc_explosion_size", &g_Config.m_XcExplosionSize, 25, 250, "%d%%");
		ToggleRow(Tr("Hide the standard explosion and smoke"), "xc_replace_explosion", &g_Config.m_XcReplaceExplosion);
		ToggleRow(Tr("Camera shake near explosions"), "xc_shake_explosion", &g_Config.m_XcShakeExplosion);
		ParticleTuning(FxTune::EXPLOSION);
		EndCard();
	}
	const char *apImpacts[] = {Tr("Sparks"), Tr("Sparks and dust"), Tr("Stars")};
	if(StyleCard("##fx_impact", Tr("Hits on blocks"), Tr("Sparks where a bullet or a laser hits a block"), "xc_impact_fx", &g_Config.m_XcImpactFx, apImpacts, 3))
	{
		ToggleRow(Tr("Color of the weapon"), "xc_impact_weapon_color", &g_Config.m_XcImpactWeaponColor);
		if(!g_Config.m_XcImpactWeaponColor)
			EffectColorPicker("##impact_color", &g_Config.m_XcImpactColorMode, &g_Config.m_XcImpactColor);
		TargetChips("##impact_targets", &g_Config.m_XcImpactSelf, &g_Config.m_XcImpactDummy, &g_Config.m_XcImpactOthers);
		ParticleTuning(FxTune::IMPACT);
		EndCard();
	}
	const char *apPlayerHits[] = {Tr("Ring"), Tr("Sparks"), Tr("Stars")};
	if(StyleCard("##fx_hit_player", Tr("Hits on players"), Tr("A flash where a bullet or a laser hits a tee"), "xc_hit_player_fx", &g_Config.m_XcHitPlayerFx, apPlayerHits, 3))
	{
		EffectColorPicker("##hit_player_color", &g_Config.m_XcHitPlayerColorMode, &g_Config.m_XcHitPlayerColor);
		ConfigSlider(Tr("Size"), "xc_hit_player_size", &g_Config.m_XcHitPlayerSize, 25, 300, "%d%%");
		ToggleRow(Tr("Hide the standard orange stars"), "xc_replace_damage", &g_Config.m_XcReplaceDamage);
		Description(Tr("Bullets and lasers that hit a tee. Players and particles as for the hits on blocks"));
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsCrosshair()
{
	BeginCardGrid("##vis_crosshair", 2);
	const char *apCrosshairs[] = {Tr("Dot"), Tr("Cross"), Tr("Circle"), Tr("Circle + dot")};
	if(StyleCard("##fx_crosshair", Tr("Crosshair"), Tr("Your own aim instead of the weapon cursor"), "xc_crosshair", &g_Config.m_XcCrosshair, apCrosshairs, 4))
	{
		EffectColorPicker("##crosshair_color", &g_Config.m_XcCrosshairColorMode, &g_Config.m_XcCrosshairColor);
		ConfigSlider(Tr("Size"), "xc_crosshair_size", &g_Config.m_XcCrosshairSize, 30, 300, "%d%%");
		ConfigSlider(Tr("Gap"), "xc_crosshair_gap", &g_Config.m_XcCrosshairGap, 0, 300, "%d%%");
		ToggleRow(Tr("Glow"), "xc_crosshair_glow", &g_Config.m_XcCrosshairGlow);
		ToggleRow(Tr("Dark outline"), "xc_crosshair_outline", &g_Config.m_XcCrosshairOutline);
		EndCard();
	}
	if(BeginCard("##fx_crosshair_motion", Tr("Crosshair motion"), Tr("Blur and a trail when you move the mouse, a kick on every shot. Works with any crosshair"), &g_Config.m_XcCrosshairMotion))
	{
		ToggleRow(Tr("Motion blur"), "xc_crosshair_blur", &g_Config.m_XcCrosshairBlur);
		if(g_Config.m_XcCrosshairBlur)
			ConfigSlider(Tr("Blur length"), "xc_crosshair_blur_strength", &g_Config.m_XcCrosshairBlurStrength, 10, 100, "%d%%");
		ToggleRow(Tr("Glowing trail"), "xc_crosshair_trail", &g_Config.m_XcCrosshairTrail);
		if(g_Config.m_XcCrosshairTrail)
			ConfigSlider(Tr("Trail length"), "xc_crosshair_trail_length", &g_Config.m_XcCrosshairTrailLength, 10, 100, "%d%%");
		ToggleRow(Tr("Kick on a shot"), "xc_crosshair_kick", &g_Config.m_XcCrosshairKick);
		if(g_Config.m_XcCrosshairKick)
			ConfigSlider(Tr("Kick strength"), "xc_crosshair_kick_strength", &g_Config.m_XcCrosshairKickStrength, 10, 100, "%d%%");
		EndCard();
	}
	if(BeginCard("##fx_block_outline", Tr("Block under the crosshair"), Tr("A soft frame around the block you aim at, with its name: freeze, unfreeze, teleport..."), &g_Config.m_XcBlockOutline))
	{
		ParamLabel(Tr("Look"));
		const char *apModes[] = {Tr("Frame"), Tr("Corners"), Tr("Fill")};
		ConfigSegmented("##block_outline_mode", "xc_block_outline_mode", &g_Config.m_XcBlockOutlineMode, apModes, 3);
		ToggleRow(Tr("Color of the kind of block"), "xc_block_outline_tile_color", &g_Config.m_XcBlockOutlineTileColor);
		if(!g_Config.m_XcBlockOutlineTileColor)
			EffectColorPicker("##block_outline_color", &g_Config.m_XcBlockOutlineColorMode, &g_Config.m_XcBlockOutlineColor);
		ConfigSlider(Tr("Line width"), "xc_block_outline_width", &g_Config.m_XcBlockOutlineWidth, 1, 8);
		ConfigSlider(Tr("Fill"), "xc_block_outline_fill", &g_Config.m_XcBlockOutlineFill, 0, 100, "%d%%");
		ToggleRow(Tr("Glow"), "xc_block_outline_glow", &g_Config.m_XcBlockOutlineGlow);
		ToggleRow(Tr("Name of the block"), "xc_block_outline_label", &g_Config.m_XcBlockOutlineLabel);
		ToggleRow(Tr("Glide from block to block"), "xc_block_outline_smooth", &g_Config.m_XcBlockOutlineSmooth);
		if(g_Config.m_XcBlockOutlineSmooth)
			ConfigSlider(Tr("Speed"), "xc_block_outline_speed", &g_Config.m_XcBlockOutlineSpeed, 1, 100, "%d%%");
		ToggleRow(Tr("Also over empty air"), "xc_block_outline_air", &g_Config.m_XcBlockOutlineAir);
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsWorld()
{
	BeginCardGrid("##vis_world", 2);
	if(BeginCard("##fx_3d", Tr("3D view"), Tr("The camera looks at the map from an angle and the blocks get depth. Only the picture: aiming and physics stay the same"), &g_Config.m_Xc3d))
	{
		if(!Graphics()->XcRenderAvailable())
			Description(Tr("Not available: needs OpenGL 3.3"));
		ParamLabel(Tr("Camera"));
		const char *apModes[] = {Tr("From the side"), Tr("First person")};
		ConfigSegmented("##3d_mode", "xc_3d_mode", &g_Config.m_Xc3dMode, apModes, 2);
		if(g_Config.m_Xc3dMode == 1)
		{
			Description(Tr("The camera is in the head of your tee and looks where you aim. Players, weapons and pickups stand in the corridor of blocks; hooks, lasers, names and particles are not shown"));
			ConfigSlider(Tr("Field of view"), "xc_3d_fp_fov", &g_Config.m_Xc3dFpFov, 50, 120, "%d°");
			ConfigSlider(Tr("Corridor width"), "xc_3d_depth", &g_Config.m_Xc3dDepth, 0, 1200, "%d%%");
		}
		else
		{
			ConfigSlider(Tr("Camera from above"), "xc_3d_pitch", &g_Config.m_Xc3dPitch, -45, 60, "%d°");
			ConfigSlider(Tr("Camera from the side"), "xc_3d_yaw", &g_Config.m_Xc3dYaw, 0, 50, "%d°");
			ToggleRow(Tr("Look from behind the way you run"), "xc_3d_follow", &g_Config.m_Xc3dFollow);
			ConfigSlider(Tr("Field of view"), "xc_3d_fov", &g_Config.m_Xc3dFov, 20, 100, "%d°");
			ConfigSlider(Tr("Block depth"), "xc_3d_depth", &g_Config.m_Xc3dDepth, 0, 1200, "%d%%");
		}
		ConfigSlider(Tr("Darkness in the depth"), "xc_3d_fog", &g_Config.m_Xc3dFog, 0, 100, "%d%%");
		ToggleRow(Tr("Colors of the map"), "xc_3d_map_colors", &g_Config.m_Xc3dMapColors);
		if(!g_Config.m_Xc3dMapColors)
		{
			ConfigColorRow(Tr("Blocks"), &g_Config.m_Xc3dColor, false);
			ConfigColorRow(Tr("Unhookable blocks"), &g_Config.m_Xc3dColor2, false);
		}
		ToggleRow(Tr("Background layers in depth"), "xc_3d_layer_depth", &g_Config.m_Xc3dLayerDepth);
		ToggleRow(Tr("Freeze as ice"), "xc_3d_freeze", &g_Config.m_Xc3dFreeze);
		if(g_Config.m_Xc3dFreeze)
		{
			ConfigColorRow(Tr("Freeze"), &g_Config.m_Xc3dFreezeColor, false);
			ConfigSlider(Tr("Ice opacity"), "xc_3d_freeze_alpha", &g_Config.m_Xc3dFreezeAlpha, 10, 100, "%d%%");
		}
		EndCard();
	}
	if(BeginCard("##fx_items", Tr("Pickups"), Tr("Hearts, armor, weapons and ninja softly glow"), &g_Config.m_XcItemsGlow))
	{
		ParamLabel(Tr("Color"));
		const char *apModes[] = {Tr("By type"), Tr("Accent"), Tr("Own"), Tr("Rainbow")};
		ConfigSegmented("##items_color_mode", "xc_items_color_mode", &g_Config.m_XcItemsColorMode, apModes, 4);
		if(g_Config.m_XcItemsColorMode == 2)
		{
			int Mode = 1; // the picker's own color part
			EffectColorPicker("##items_color", &Mode, &g_Config.m_XcItemsColor);
		}
		ConfigSlider(Tr("Size"), "xc_items_glow_size", &g_Config.m_XcItemsGlowSize, 25, 250, "%d%%");
		ConfigSlider(Tr("Brightness"), "xc_items_glow_strength", &g_Config.m_XcItemsGlowStrength, 10, 250, "%d%%");
		ToggleRow(Tr("Sparkles"), "xc_items_sparkles", &g_Config.m_XcItemsSparkles);
		Description(Tr("Bullets, grenades and lasers use the same colors (Weapons > Shots)"));
		ParticleTuning(FxTune::ITEMS);
		EndCard();
	}
	RenderWeatherCard();
	if(BeginCard("##fx_ambience", Tr("Atmosphere"), Tr("The light of the time of day, sun or moon rays, fog and drifting mist over the map"), &g_Config.m_XcAmbience))
	{
		ParamLabel(Tr("Time of day"));
		const char *apTimes[] = {Tr("Clock"), Tr("Dawn"), Tr("Day"), Tr("Sunset"), Tr("Night"), Tr("Own")};
		ConfigSegmented("##ambience_time", "xc_ambience_time", &g_Config.m_XcAmbienceTime, apTimes, 6);
		if(g_Config.m_XcAmbienceTime == CXcAmbience::TIME_HOUR)
			ConfigSlider(Tr("Hour"), "xc_ambience_hour", &g_Config.m_XcAmbienceHour, 0, 23, "%d:00");
		ConfigSlider(Tr("Light color"), "xc_ambience_strength", &g_Config.m_XcAmbienceStrength, 0, 100, "%d%%");
		ConfigSlider(Tr("Rays"), "xc_ambience_rays", &g_Config.m_XcAmbienceRays, 0, 100, "%d%%");
		ConfigSlider(Tr("Fog"), "xc_ambience_fog", &g_Config.m_XcAmbienceFog, 0, 100, "%d%%");
		if(g_Config.m_XcAmbienceFog > 0)
			ConfigSlider(Tr("Fog height"), "xc_ambience_fog_height", &g_Config.m_XcAmbienceFogHeight, 5, 100, "%d%%");
		ConfigSlider(Tr("Humidity"), "xc_ambience_mist", &g_Config.m_XcAmbienceMist, 0, 100, "%d%%");
		ToggleRow(Tr("Own fog color"), "xc_ambience_fog_own", &g_Config.m_XcAmbienceFogOwn);
		if(g_Config.m_XcAmbienceFogOwn)
			EffectColorPicker("##ambience_fog_color", &g_Config.m_XcAmbienceFogColorMode, &g_Config.m_XcAmbienceFogColor);
		EndCard();
	}
	const char *apFloaters[] = {Tr("Cubes"), Tr("Orbs"), Tr("Both")};
	if(StyleCard("##fx_floaters", Tr("Floating lights"), Tr("Glowing cubes and orbs drift around, your tee pushes them away"), "xc_floaters", &g_Config.m_XcFloaters, apFloaters, 3))
	{
		EffectColorPicker("##floaters_color", &g_Config.m_XcFloatersColorMode, &g_Config.m_XcFloatersColor);
		ConfigSlider(Tr("Amount"), "xc_floaters_count", &g_Config.m_XcFloatersCount, 5, 150);
		ConfigSlider(Tr("Size"), "xc_floaters_size", &g_Config.m_XcFloatersSize, 30, 300, "%d%%");
		ConfigSlider(Tr("Speed"), "xc_floaters_speed", &g_Config.m_XcFloatersSpeed, 0, 300, "%d%%");
		ConfigSlider(Tr("Opacity"), "xc_floaters_alpha", &g_Config.m_XcFloatersAlpha, 10, 100, "%d%%");
		ToggleRow(Tr("Glow"), "xc_floaters_glow", &g_Config.m_XcFloatersGlow);
		ToggleRow(Tr("Your tee pushes them"), "xc_floaters_push", &g_Config.m_XcFloatersPush);
		EndCard();
	}
	const char *apTints[] = {Tr("Warm"), Tr("Cool"), Tr("Accent"), Tr("Night")};
	if(StyleCard("##fx_tint", Tr("World color"), Tr("A soft color filter over the map"), "xc_world_tint", &g_Config.m_XcWorldTint, apTints, 4))
	{
		ConfigSlider(Tr("Strength"), "xc_world_tint_strength", &g_Config.m_XcWorldTintStrength, 5, 100, "%d%%");
		EndCard();
	}
	if(BeginCard("##fx_vignette", Tr("Vignette"), Tr("Darker screen edges to focus on the center")))
	{
		ConfigSlider(Tr("Strength"), "xc_vignette", &g_Config.m_XcVignette, 0, 100, "%d%%");
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsEvents()
{
	BeginCardGrid("##vis_events", 2);
	const char *apFinish[] = {Tr("Confetti"), Tr("Fireworks")};
	if(StyleCard("##fx_finish", Tr("Finish"), Tr("Confetti or fireworks when you finish the map"), "xc_finish_fx", &g_Config.m_XcFinishFx, apFinish, 2))
	{
		EffectColorPicker("##finish_color", &g_Config.m_XcFinishColorMode, &g_Config.m_XcFinishColor);
		ConfigSlider(Tr("Amount"), "xc_finish_amount", &g_Config.m_XcFinishAmount, 25, 300, "%d%%");
		ToggleRow(Tr("Banner with the time and records"), "xc_finish_banner", &g_Config.m_XcFinishBanner);
		ToggleRow(Tr("Hide the standard confetti"), "xc_replace_confetti", &g_Config.m_XcReplaceConfetti);
		ToggleRow(Tr("Screen flash"), "xc_flash_finish", &g_Config.m_XcFlashFinish);
		ToggleRow(Tr("Sound at the finish and for records"), "xc_event_sound_finish", &g_Config.m_XcEventSoundFinish);
		TargetChips("##finish_targets", &g_Config.m_XcFinishSelf, &g_Config.m_XcFinishDummy, &g_Config.m_XcFinishOthers);
		const int Local = GameClient()->m_Snap.m_LocalClientId;
		if(IsOnline() && Local >= 0 && GameClient()->m_Snap.m_aCharacters[Local].m_Active)
		{
			if(Button(Tr("Show"), true, -1.0f))
			{
				// closes the menu so the effect is seen right on the tee
				SetActive(false);
				GameClient()->m_XcEventFx.Finish(GameClient()->m_aClients[Local].m_RenderPos, Local);
				if(g_Config.m_XcFinishBanner)
					GameClient()->m_XcHud.ShowBanner(Tr("New personal record!"), "00:42.137  (-0.52)", 1);
			}
		}
		else
			Description(Tr("Join a server to try it"));
		ParticleTuning(FxTune::FINISH);
		EndCard();
	}
	const char *apDeath[] = {Tr("Shards"), Tr("Sparks"), Tr("Ash"), Tr("Lightning"), Tr("Soul"), Tr("Hearts"), Tr("Pixels")};
	if(StyleCard("##fx_death", Tr("Death"), Tr("A tee bursts into pieces"), "xc_death_fx", &g_Config.m_XcDeathFx, apDeath, 7))
	{
		EffectColorPicker("##death_color", &g_Config.m_XcDeathColorMode, &g_Config.m_XcDeathColor);
		ToggleRow(Tr("Mark where you died last"), "xc_death_marker", &g_Config.m_XcDeathMarker);
		if(g_Config.m_XcDeathMarker)
			ConfigSlider(Tr("Mark stays"), "xc_death_marker_time", &g_Config.m_XcDeathMarkerTime, 2, 60, "%d s");
		ToggleRow(Tr("Hide the standard blood"), "xc_replace_death", &g_Config.m_XcReplaceDeath);
		ToggleRow(Tr("Screen flash when you die"), "xc_flash_death", &g_Config.m_XcFlashDeath);
		ToggleRow(Tr("Camera shake when you die"), "xc_shake_death", &g_Config.m_XcShakeDeath);
		ToggleRow(Tr("Sound when you die"), "xc_event_sound_death", &g_Config.m_XcEventSoundDeath);
		TargetChips("##death_targets", &g_Config.m_XcDeathSelf, &g_Config.m_XcDeathDummy, &g_Config.m_XcDeathOthers);
		ParticleTuning(FxTune::DEATH);
		EndCard();
	}
	if(BeginCard("##fx_spawn", Tr("Spawn"), Tr("Rings close in where a tee appears"), &g_Config.m_XcSpawnFx))
	{
		EffectColorPicker("##spawn_color", &g_Config.m_XcSpawnColorMode, &g_Config.m_XcSpawnColor);
		ToggleRow(Tr("Hide the standard purple smoke"), "xc_replace_spawn", &g_Config.m_XcReplaceSpawn);
		TargetChips("##spawn_targets", &g_Config.m_XcSpawnSelf, &g_Config.m_XcSpawnDummy, &g_Config.m_XcSpawnOthers);
		EndCard();
	}
	if(BeginCard("##fx_teleport", Tr("Teleports"), Tr("A flash where your tee or dummy leaves and arrives"), &g_Config.m_XcTeleportFx))
	{
		EffectColorPicker("##teleport_color", &g_Config.m_XcTeleportColorMode, &g_Config.m_XcTeleportColor);
		ToggleRow(Tr("Sound when you are teleported"), "xc_event_sound_teleport", &g_Config.m_XcEventSoundTeleport);
		ParticleTuning(FxTune::TELEPORT);
		EndCard();
	}
	if(BeginCard("##fx_checkpoint", Tr("Checkpoints"), Tr("A ring and rising sparks when your tee passes a time checkpoint"), &g_Config.m_XcCheckpointFx))
	{
		EffectColorPicker("##checkpoint_color", &g_Config.m_XcCheckpointColorMode, &g_Config.m_XcCheckpointColor);
		ToggleRow(Tr("Screen flash"), "xc_flash_checkpoint", &g_Config.m_XcFlashCheckpoint);
		ParticleTuning(FxTune::CHECKPOINT);
		EndCard();
	}
	if(BeginCard("##fx_dummy_arrow", Tr("Arrows to players"), Tr("Glowing arrows at the screen edge show where the dummy, team mates and friends are")))
	{
		ToggleRow(Tr("Dummy"), "xc_dummy_arrow", &g_Config.m_XcDummyArrow);
		ToggleRow(Tr("Team mates"), "xc_team_arrows", &g_Config.m_XcTeamArrows);
		ToggleRow(Tr("Friends"), "xc_friend_arrows", &g_Config.m_XcFriendArrows);
		ParamLabel(Tr("Dummy arrow color"));
		EffectColorPicker("##arrow_color", &g_Config.m_XcDummyArrowColorMode, &g_Config.m_XcDummyArrowColor);
		ConfigSlider(Tr("Size"), "xc_dummy_arrow_size", &g_Config.m_XcDummyArrowSize, 50, 250, "%d%%");
		EndCard();
	}
	EndCardGrid();
}

void CXcMenu::RenderVisualsNames()
{
	BeginCardGrid("##vis_names", 2);
	if(BeginCard("##names_ddnet", Tr("Names above the tees"), Tr("What the name plates show and how big they are"), &g_Config.m_ClNamePlates))
	{
		ToggleRow(Tr("Always, at any distance"), "cl_nameplates_always", &g_Config.m_ClNamePlatesAlways);
		ToggleRow(Tr("My own name too"), "cl_nameplates_own", &g_Config.m_ClNamePlatesOwn);
		ToggleRow(Tr("Team colors"), "cl_nameplates_teamcolors", &g_Config.m_ClNamePlatesTeamcolors);
		ToggleRow(Tr("Heart for friends"), "cl_nameplates_friendmark", &g_Config.m_ClNamePlatesFriendMark);
		ConfigSlider(Tr("Name size"), "cl_nameplates_size", &g_Config.m_ClNamePlatesSize, -50, 100);
		ConfigSlider(Tr("Letter spacing"), "xc_name_spacing", &g_Config.m_XcNameSpacing, -15, 80, "%d%%");
		ConfigSlider(Tr("Gap above the tee"), "cl_nameplates_offset", &g_Config.m_ClNamePlatesOffset, 10, 50);
		ToggleRow(Tr("Clan"), "cl_nameplates_clan", &g_Config.m_ClNamePlatesClan);
		if(g_Config.m_ClNamePlatesClan)
			ConfigSlider(Tr("Clan size"), "cl_nameplates_clan_size", &g_Config.m_ClNamePlatesClanSize, -50, 100);
		ToggleRow(Tr("Player numbers"), "cl_nameplates_ids", &g_Config.m_ClNamePlatesIds);
		EndCard();
	}
	if(BeginCard("##hud_plates", Tr("Name plates"), Tr("Names above the tees on dark rounded plates"), &g_Config.m_XcHudPlates))
	{
		Description(Tr("The text stays readable on bright maps"));
		EndCard();
	}
	const char *apGradients[] = {Tr("Accent"), Tr("Rainbow"), Tr("Skin"), Tr("Two colors")};
	if(StyleCard("##fx_name_gradient", Tr("Gradient name"), Tr("The name above the tee in a smooth gradient that can flow"), "xc_name_gradient", &g_Config.m_XcNameGradient, apGradients, 4))
	{
		if(g_Config.m_XcNameGradient == 4)
		{
			// the own part of the picker for each of the two colors
			int OwnMode = 1;
			ParamLabel(Tr("First color"));
			EffectColorPicker("##gradient_color1", &OwnMode, &g_Config.m_XcNameGradientColor1);
			ParamLabel(Tr("Second color"));
			EffectColorPicker("##gradient_color2", &OwnMode, &g_Config.m_XcNameGradientColor2);
		}
		ToggleRow(Tr("Flowing"), "xc_name_gradient_flow", &g_Config.m_XcNameGradientFlow);
		TargetChips("##gradient_targets", &g_Config.m_XcNameGradientSelf, &g_Config.m_XcNameGradientDummy, &g_Config.m_XcNameGradientOthers);
		EndCard();
	}
	if(BeginCard("##fx_flying_names", Tr("Flying names"), Tr("Names follow the tees on a soft spring, like kites"), &g_Config.m_XcFlyingNames))
	{
		ConfigSlider(Tr("Lag"), "xc_flying_names_lag", &g_Config.m_XcFlyingNamesLag, 30, 300, "%d%%");
		EndCard();
	}
	EndCardGrid();
}

// lives in Settings > Sound, next to the other sounds
void CXcMenu::RenderEventSoundsCard()
{
	if(BeginCard("##fx_event_sounds", Tr("Event sounds"), Tr("Short sounds when you freeze, finish, set a record or teleport"), &g_Config.m_XcEventSounds))
	{
		ConfigSlider(Tr("Volume"), "xc_event_sound_volume", &g_Config.m_XcEventSoundVolume, 0, 100, "%d%%");
		ToggleRow(Tr("Freeze and unfreeze"), "xc_event_sound_freeze", &g_Config.m_XcEventSoundFreeze);
		ToggleRow(Tr("Finish and records"), "xc_event_sound_finish", &g_Config.m_XcEventSoundFinish);
		ToggleRow(Tr("Teleports"), "xc_event_sound_teleport", &g_Config.m_XcEventSoundTeleport);
		ToggleRow(Tr("Death"), "xc_event_sound_death", &g_Config.m_XcEventSoundDeath);
		// listen without playing
		const char *apNames[] = {Tr("Finish"), Tr("Record"), Tr("Freeze"), Tr("Unfreeze"), Tr("Death"), Tr("Teleport")};
		ParamLabel(Tr("Listen"));
		const float Gap = S(6.0f);
		const float Width = (AvailWidth() - Gap * 2.0f) / 3.0f;
		for(int i = 0; i < CXcEventFx::NUM_EVENT_SOUNDS; i++)
		{
			if(i % 3)
				ImGui::SameLine(0.0f, Gap);
			ImGui::PushID(i);
			if(Button(apNames[i], false, Width))
			{
				// the preview plays even when this sound is switched off
				const int Old = g_Config.m_XcEventSounds;
				int *apEnabled[] = {&g_Config.m_XcEventSoundFinish, &g_Config.m_XcEventSoundFreeze, &g_Config.m_XcEventSoundDeath, &g_Config.m_XcEventSoundTeleport};
				int aOld[4];
				for(int k = 0; k < 4; k++)
				{
					aOld[k] = *apEnabled[k];
					*apEnabled[k] = 1;
				}
				g_Config.m_XcEventSounds = 1;
				GameClient()->m_XcEventFx.PlayEventSound(i);
				g_Config.m_XcEventSounds = Old;
				for(int k = 0; k < 4; k++)
					*apEnabled[k] = aOld[k];
			}
			ImGui::PopID();
		}
		EndCard();
	}
}

void CXcMenu::RenderVisuals()
{
	const char *apIcons[NUM_VISUALS] = {XC_ICON_SLIDERS, XC_ICON_USER, XC_ICON_BOLT, XC_ICON_HAT, XC_ICON_HAMMER, XC_ICON_CROSSHAIR, XC_ICON_GLOBE, XC_ICON_STAR, XC_ICON_USERS};
	const char *apLabels[NUM_VISUALS] = {Tr("General"), Tr("Tee"), Tr("Movement"), Tr("Cosmetics"), Tr("Weapons"), Tr("Crosshair"), Tr("World"), Tr("Events"), Tr("Names")};
	const float NavWidth = S(210.0f);
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	if(SideNav("##visual_sections", &m_VisualsSection, apIcons, apLabels, NUM_VISUALS, NavWidth))
		m_ScrollTarget = 0.0f;
	ImGui::SetCursorScreenPos(ImVec2(Start.x + NavWidth + S(20.0f), Start.y));
	const ImVec2 Avail = ImGui::GetContentRegionAvail();
	BeginSmoothScroll("##visuals_scroll", ImVec2(Avail.x, Avail.y));
	Header(apLabels[m_VisualsSection]);
	ImGui::Dummy(ImVec2(0.0f, S(4.0f)));
	switch(m_VisualsSection)
	{
	case VISUALS_TEE: RenderVisualsTee(); break;
	case VISUALS_MOVEMENT: RenderVisualsMovement(); break;
	case VISUALS_COSMETICS:
		RenderVisualsCosmetics();
		RenderCosmetics3d();
		RenderVisualsCosmeticsExtra();
		break;
	case VISUALS_WEAPONS: RenderVisualsWeapons(); break;
	case VISUALS_CROSSHAIR: RenderVisualsCrosshair(); break;
	case VISUALS_WORLD: RenderVisualsWorld(); break;
	case VISUALS_EVENTS: RenderVisualsEvents(); break;
	case VISUALS_NAMES: RenderVisualsNames(); break;
	default: RenderVisualsGeneral(); break;
	}
	if(m_VisualsSection != VISUALS_COSMETICS)
		m_WheelCaptured = false;
	if(m_ScrollToHighlight)
	{
		const float CardY = HighlightedCardY();
		if(CardY >= 0.0f)
		{
			m_ScrollTarget = std::max(CardY - S(12.0f), 0.0f);
			m_ScrollToHighlight = false;
		}
	}
	ImGui::Dummy(ImVec2(0.0f, S(12.0f)));
	EndSmoothScroll();
}
