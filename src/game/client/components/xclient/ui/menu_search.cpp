// Search over all pages and settings cards (top bar button or Ctrl+F).
#include "feedback.h"
#include "icons.h"
#include "lang.h"
#include "menu.h"
#include "theme.h"
#include "widgets.h"

#include <base/str.h>

#include <engine/external/imgui/imgui.h>
#include <engine/external/imgui/imgui_internal.h>

#include <algorithm>
#include <iterator>
#include <vector>

using namespace XcUi;

namespace
{
	struct SSearchEntry
	{
		int m_Page;
		int m_Section; // settings section, -1 for a page
		const char *m_pCard; // card id to highlight, nullptr for a page or section
		const char *m_pTitle; // English, translated when shown
		const char *m_pDescription;
		const char *m_pKeywords; // extra words in both languages
	};

	// clang-format off
	const SSearchEntry gs_aSearchEntries[] = {
		{CXcMenu::PAGE_HOME, -1, nullptr, "Home", "Profile, friends online and DDNet news", "главная профиль друзья новости play играть"},
		{CXcMenu::PAGE_SERVERS, -1, nullptr, "Servers", "Server cards, communities, filters, friends", "серверы сервер браузер join войти избранное favorites lan ddrace сообщества communities фильтры filters друзья friends сортировка sort kog страны"},
		{CXcMenu::PAGE_VISUALS, -1, nullptr, "Visuals", "Visual effects", "визуалы эффекты effects"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_TEE, "##fx_glow", "Neon glow", "Soft light and a crisp outline around the tee", "свечение неон обводка контур подсветка glow neon outline"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_trail", "Trail", "A glowing ribbon behind the tee", "шлейф хвост след trail ribbon"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_NAMES, "##fx_name_gradient", "Gradient name", "The name above the tee in a smooth gradient that can flow", "градиент ник имя радуга gradient name nickname rainbow"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_NAMES, "##fx_flying_names", "Flying names", "Names follow the tees on a soft spring, like kites", "летающие ники пружина змей flying names nameplates kite"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_TEE, "##fx_jelly", "Jelly tee", "Squashes when landing, stretches when jumping and wobbles back", "желе желейный сплющивание пружина jelly squash stretch"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_motion_blur", "Motion blur", "Fast tees leave a soft smear of copies behind them", "размытие движения моушен блюр смаз motion blur"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_speed_lines", "Speed lines", "Thin streaks behind the tee at high speed, like in anime", "линии скорости штрихи аниме speed lines"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_COSMETICS, "##fx_accessory", "Accessory", "Something on the head that sways with the tee", "аксессуар шапка нимб корона конус china hat halo crown"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_jump", "Jumps", "Effect under the feet on every jump", "прыжки прыжок кольца искры звёзды двойной jump circles rings double air"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_land", "Landing", "A ring on the ground after a high fall", "приземление падение кольцо land landing fall"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_run", "Running", "Dust under the feet of a fast tee", "бег пыль занос дым run dust skid"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_hook", "Hook", "Colored chain, clean line or a glowing beam", "хук крюк цепь hook chain"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##fx_hook_hit", "Hook grabs", "What shows where the hook catches a block or a player, or bounces off an unhookable block", "хук захват зацеп блок игрок отскок нехук hook grab hit unhookable bounce"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_COSMETICS, "##fx_cape", "Cape", "Cloth that hangs from the shoulders, flaps on jumps and streams away from the hook", "плащ накидка мантия королевский мех узор cape cloak royal fur pattern"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_COSMETICS, "##fx_scarf", "Scarf", "A knitted scarf around the neck, its ends fly behind the tee", "шарф вязаный полосы бахрома scarf knitted stripes fringe"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_GENERAL, "##fx_glow_tune", "Glow", "How bright and how wide every effect glows", "свечение блум ореол яркость glow bloom brightness neon неон"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WORLD, "##fx_items", "Pickups", "Hearts, armor, weapons and ninja softly glow", "предметы сердца броня оружие пикапы pickups items hearts armor"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##fx_shots", "Shots", "A flash at the gun, light around bullets and grenades, comet tails", "пули гранаты выстрел вспышка ствол снаряды хвост дым bullets grenade muzzle projectiles trail smoke shots"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##fx_projectiles", "Lasers", "Glow along rifle and shotgun lasers, or our own beam", "лазер дробовик луч laser rifle shotgun beam"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WORLD, "##fx_floaters", "Floating lights", "Glowing cubes and orbs drift around, your tee pushes them away", "кубики огоньки частицы 3d летающие cubes orbs particles floating lights"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WORLD, "##fx_weather", "Weather", "Falling or rising particles over the map and the menu, also your own pictures and gifs", "погода снег листья сакура пепел weather snow сердечки звёзды кристаллы светлячки деньги хэллоуин угольки дождь пузыри конфетти своя картинка гифка hearts stars rain bubbles confetti fireflies gif picture"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_TEE, "##fx_freeze", "Freeze", "Everything that happens when a tee freezes, stays frozen and thaws", "фриз заморозка лёд осколки снежинки звёзды freeze ice shards snowflakes stars"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_TEE, "##fx_ninja", "Ninja and invincible", "The standard shine of the ninja sword and the sparkles of invincible tees", "ниндзя блеск неуязвимость искорки ninja shine invincible sparkle"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##fx_hammer", "Hammer hit", "A flash where the hammer lands", "молоток удар хаммер hammer hit"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##fx_explosion", "Grenade explosion", "A shockwave, neon rings or hot sparks where a grenade explodes", "взрыв граната волна неон explosion grenade shockwave"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##fx_impact", "Hits on blocks", "Sparks where a bullet or a laser hits a block", "попадания пули искры стена блок bullet hit impact sparks wall"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##fx_hit_player", "Hits on players", "A flash where a bullet or a laser hits a tee", "попадание урон игрок звёзды звёздочки damage hit player stars"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_GENERAL, "##fx_over_tiles", "Over the blocks", "Effects and hats can be drawn over the map blocks, they stay visible behind walls", "поверх блоков стен карты слой over tiles blocks walls layer"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_GENERAL, "##fx_juice", "Juice", "Screen flashes and camera shake. Which events use them is chosen in the card of each event", "сочность вспышка тряска камера shake flash juice"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_GENERAL, "##fx_stock", "Stock effects", "The standard game effects are shown or hidden in the card of each event. Here all at once", "стандартные эффекты замена скрыть кровь дым лазер replace stock default"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_CROSSHAIR, "##fx_crosshair", "Crosshair", "Your own aim instead of the weapon cursor", "прицел курсор точка крест crosshair cursor aim"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_CROSSHAIR, "##fx_crosshair_motion", "Crosshair motion", "Blur and a trail when you move the mouse, a kick on every shot. Works with any crosshair", "прицел движение размытие моушен блюр след отдача crosshair motion blur trail kick"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WORLD, "##fx_tint", "World color", "A soft color filter over the map", "цвет мира фильтр тёплый холодный ночь tint filter color grading"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WORLD, "##fx_vignette", "Vignette", "Darker screen edges to focus on the center", "виньетка затемнение края vignette"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_COSMETICS, "##hat_editor", "Own hat", "Any picture or gif on your tee. Drag it on the tee, pull the corner to resize, the circle to turn it", "шапка шляпа косметика редактор кепка уши очки hat cosmetics editor gif png"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_COSMETICS, "##hat_gallery", "My hats", "Pictures and gifs with a transparent background work best. Drag a file onto the game window, paste its path or open the folder", "мои шапки галерея добавить hats gallery add"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_COSMETICS, "##cosmetic_models", "3D cosmetics", "Wings, backpacks, hats and pets made of cubes. In 2D they are drawn in our style, in 3D they turn and have depth", "3д косметика крылья рюкзак питомец пет шапка нимб майнкрафт 3d cosmetics wings backpack pet minecraft"},
		{CXcMenu::PAGE_HOME, -1, "##music", "Music", "What plays in the browser or a music app", "музыка плеер трек песня ютуб саундклауд яндекс спотифай music player youtube soundcloud yandex spotify"},
		{CXcMenu::PAGE_HOME, -1, "##mapinfo", "Current map", "Mapper, difficulty, points and the best players of the map", "карта инфо маппер автор сложность топ рекорды map info mapper top mapinfo"},
		{CXcMenu::PAGE_HOME, -1, "##tools", "Tools", "Make your own maps and play them on a server right on your computer", "редактор карт локальный сервер инструменты editor map local server tools"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_EVENTS, "##fx_finish", "Finish", "Confetti or fireworks when you finish the map", "финиш конфетти салют рекорд finish confetti fireworks record"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_EVENTS, "##fx_death", "Death", "A tee bursts into pieces", "смерть килл осколки пепел кровь death kill blood"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_EVENTS, "##fx_spawn", "Spawn", "Rings close in where a tee appears", "появление респавн спавн spawn respawn"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_EVENTS, "##fx_teleport", "Teleports", "A flash where your tee or dummy leaves and arrives", "телепорт тп вспышка teleport tele"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_SOUND, "##fx_event_sounds", "Event sounds", "Short sounds when you freeze, finish, set a record or teleport", "звуки событий фриз финиш рекорд телепорт sounds events"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_EVENTS, "##fx_checkpoint", "Checkpoints", "A ring and rising sparks when your tee passes a time checkpoint", "чекпоинт контрольная точка вспышка checkpoint cp flash"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_EVENTS, "##fx_dummy_arrow", "Arrows to players", "Glowing arrows at the screen edge show where the dummy, team mates and friends are", "стрелка дамми напарник друзья команда за экраном arrow dummy partner team friends"},
		{CXcMenu::PAGE_CONFIGS, -1, nullptr, "Configs", "Save, load and share your settings", "конфиги конфиг config import export импорт экспорт код code"},
		{CXcMenu::PAGE_DEMOS, -1, nullptr, "Demos", "Watch, rename and delete recorded demos", "демки демо demo replay запись повтор"},
		{CXcMenu::PAGE_VOTES, -1, nullptr, "Votes", "Vote yes or no, start server votes, kick or move players to the spectators", "голосование голос голосовать кик зрители vote votes kick spectate"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##theme", "Theme", "Look of the menu: dark or light, and the accent color", "тема тёмная светлая dark light акцент accent цвет color"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##presets", "Theme presets", "Save theme, accent, font and background together", "пресеты темы сохранённые saved presets стиль style"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##font", "Font", "Main font of the menu", "шрифт font manrope onest unbounded"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##scale", "Scale", "Size of the menu for your screen", "масштаб размер scale size 1080p 2k 4k"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##animations", "Animations", "How smooth the menu moves", "анимации плавность animations smooth"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##language", "Game language", "Language of the menu and the game", "язык language русский english"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_SOUND, "##ui_sounds", "Interface sounds", "Clicks, switches and notifications", "звуки звук клик sounds click"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##topbar", "Top bar", "Turn tabs and buttons on or off and move them. The ones you turn off are in the ... menu of the bar", "верхний бар вкладки кнопки порядок панель скрыть top bar tabs buttons order"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##classic", "Classic menu", "The old DDNet menu, for settings that are not here yet", "классическое старое меню classic old"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_BACKGROUND, "##bg_kind", "Menu background", "What is behind the menu", "фон меню background блюр blur анимация animated"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_BACKGROUND, "##bg_look", "Look", "Darkening, blur and gif speed", "затемнение блюр гифка скорость dim blur gif speed"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_BACKGROUND, "##bg_own", "My backgrounds", "Pictures and gifs", "свой фон картинка гифка фото обои picture image gif wallpaper"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_BACKGROUND, "##bg_maps", "Maps", "Animated DDNet maps behind the main menu", "карты карта темы map maps themes"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_PLAYER, "##profile", "Profile", "Name and clan of you and your dummy", "ник имя клан дамми name nick clan dummy"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_PLAYER, "##skin", "Skin", "Skin name and custom colors", "скин цвет skin color colour"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CONTROLS, "##mouse", "Mouse", "Sensitivity in game and in menus", "мышь чувствительность сенса mouse sensitivity sens"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CONTROLS, "##camera", "Camera", "Dynamic camera follows the mouse", "камера динамическая dyncam camera"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CONTROLS, "##binds", "Key binds", "Click a key and press a new one, Esc cancels", "бинды клавиши кнопки управление binds keys jump hook прыжок хук"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_SOUND, "##volume", "Sound", "Sound and volume", "звук громкость музыка sound volume music"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CHAT, "##chat_look", "Chat", "How the chat looks", "чат сообщения chat messages"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_GRAPHICS, "##latency", "Input delay", "Lowest delay of the mouse and keys: VSync, fullscreen, frame queue, synchronous frames", "задержка ввода инпут лаг мышь клавиатура вертикальная синхронизация полноэкранный raw input сырой ввод input lag delay latency mouse keyboard vsync fullscreen sync"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_GRAPHICS, "##performance", "Performance", "Low, medium or high in one click, then each effect by hand", "производительность пресет низкое среднее высокое fps лаги performance preset low medium high"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_GRAPHICS, "##screen", "Screen", "Window mode and frame rate", "экран окно fps vsync полноэкранный fullscreen window"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_GRAPHICS, "##performance", "Effects", "Quality of the render module (OpenGL 3.3)", "эффекты качество bloom свечение quality render"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##hud_main", "Effects (freeze, jetpack, endless hook...)", "Active states of the tee with timers, like potion effects", "эффекты зелья зелье фриз заморозка джетпак бесконечный хук таймер effects potions freeze jetpack endless status"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_CROSSHAIR, "##fx_block_outline", "Block under the crosshair", "A soft frame around the block you aim at, with its name: freeze, unfreeze, teleport...", "блок выделение рамка подсветка прицел контур block outline highlight frame tile"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WORLD, "##fx_ambience", "Atmosphere", "The light of the time of day, sun or moon rays, fog and drifting mist over the map", "атмосфера время суток закат рассвет ночь день лучи солнце туман влажность туманность ambience atmosphere sunset dawn night day rays fog mist"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_GENERAL, "##fx_particles_all", "Particles of all effects", "One look for the sparks of every effect, and physics: they fall and bounce off the blocks", "частицы физика отскок гравитация форма сердечки звёзды particles physics bounce gravity shape hearts stars"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_WEAPONS, "##weapon_view", "Weapon in the hand", "Where the weapon sits, its size and how the hammer swings. Only the picture", "оружие рука молоток взмах удар анимация размер положение viewmodel swing hammer weapon hand animation"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CHAT, "##streamer", "Streamer mode", "Hide names on name plates, in the scoreboard, the chat and the HUD. Only on your screen", "стример стрим скрыть ник имена анонимно streamer stream hide names nickname anonymous nameprotect"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_TEE, "##fx_tee_shader", "Tee shader", "A moving pattern painted over the skin: gradient, rainbow, galaxy with stars, waves or chrome", "шейдер ти скин заливка радуга градиент галактика хром волны shader tee skin rainbow gradient galaxy chrome waves hands"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##hud_main", "Hide the standard HUD in the corner (weapons, jumps, states)", "The new HUD shows all weapons, jumps and effects instead", "стандартный худ скрыть убрать оружие прыжки иконки угол classic hud hide weapons jumps"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CHAT, "##chat_new", "New chat", "Five looks, avatars, time, groups and an own input line. Needs the new HUD", "чат новый минимализм стекло пузыри панель плашки аватарки время chat minimal glass bubbles panel plates avatars time"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_INTERFACE, "##cursor", "Cursor", "The mouse cursor of the menus, the HUD editor and the wheels", "курсор мышь стрелка указатель cursor mouse pointer arrow"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##hud_classic", "Standard DDNet HUD", "Turn off the whole standard HUD or only some of its parts", "стандартный худ дднет убрать скрыть части здоровье патроны таймер очки фпс время голосование зрители фриз standard ddnet hud hide parts health ammo timer score fps vote spectators"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##hud_main", "New HUD", "Watermark, timer, speed and weapon in the style of the menu", "худ hud редактор editor ватермарка watermark таймер timer скорость speed оружие weapon"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##music_player", "Music player", "What plays in the browser or a music app: YouTube, SoundCloud, Yandex Music, Spotify...", "музыка плеер трек music player spotify youtube"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_TEXTURES, "##tex_sets", "Textures", "Weapons, emotes, particles, HUD icons and entities. Click a set to use it", "текстуры ассеты оружие эмоции частицы худ энтитис entities assets textures game emoticons particles hud extras gameskin"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_WARLIST, "##war_list", "War list", "Players and clans in colored groups", "вар лист варлист война враги команда клан группы цвет ники war list warlist enemy team clan groups colors"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_DDNET, nullptr, "DDNet", "All DDNet settings", "ddnet дднет все настройки предсказание prediction антипинг antiping ники nameplates линия хука hook призрак ghost демо demo скриншот screenshot камера camera зум zoom лазер laser entities"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_TCLIENT, nullptr, "TClient", "All TClient settings", "tclient тклиент тиклиент обводка outline индикатор indicator радуга rainbow вар war питомец pet антипинг antiping призраки ghosts перевод translate"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##hud_scoreboard", "Own scoreboard", "Tab shows players by teams with medals, times and pings in the style of the menu", "таблица игроков таб скорборд топ мини-топ scoreboard tab top minitop"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CHAT, "##chat_style", "Chat in the HUD style", "Plates in the theme colors, marks for mentions, team chat and whispers, a framed input", "чат стиль плашки упоминание шёпот команда chat style mention whisper"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_NAMES, "##hud_plates", "Name plates", "Names above the tees on dark rounded plates", "ники ники-плашки плашки nameplates names"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_NAMES, "##names_ddnet", "Names above the tees", "What the name plates show and how big they are", "ники ник плашки клан размер номера id nameplates names clan"},
		{CXcMenu::PAGE_VISUALS, CXcMenu::VISUALS_MOVEMENT, "##hook_line", "Hook aim line", "The line that shows where your hook would catch", "линия хука прицел хука коллизия hook line collision aim"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_CONTROLS, "##zoom", "Camera zoom", "How far the camera sees and how smooth it moves", "зум приближение камера плавность zoom camera"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_GRAPHICS, "##prediction", "Prediction and ping", "How the client shows the game ahead of the server, so it feels without lag", "предсказание антипинг пинг лаг prediction antiping ping lag"},
		{CXcMenu::PAGE_SETTINGS, CXcMenu::SETTINGS_HUD, "##hud_wheels", "Wheels", "Emote wheel and spectator menu in the colors of the menu", "колесо эмоции эмоций спектатор наблюдение wheel emote spectator"},
	};
	// clang-format on

	bool Contains(const char *pHaystack, const char *pNeedle)
	{
		return pHaystack && str_utf8_find_nocase(pHaystack, pNeedle) != nullptr;
	}

	// every word of the query must be found somewhere, titles count most
	int Score(const SSearchEntry &Entry, const char *pQuery)
	{
		int Total = 0;
		char aWord[64];
		const char *pRest = pQuery;
		while((pRest = str_next_token(pRest, " ", aWord, sizeof(aWord))))
		{
			if(!aWord[0])
				continue;
			int Best = 0;
			if(Contains(Tr(Entry.m_pTitle), aWord) || Contains(Entry.m_pTitle, aWord))
				Best = 4;
			else if(Contains(Entry.m_pKeywords, aWord))
				Best = 2;
			else if(Contains(Tr(Entry.m_pDescription), aWord) || Contains(Entry.m_pDescription, aWord))
				Best = 1;
			if(Best == 0)
				return 0;
			Total += Best;
		}
		return Total;
	}
} // namespace

void CXcMenu::OpenSearch()
{
	m_SearchOpen = true;
	m_SearchFocus = true;
	m_aSearch[0] = '\0';
	m_SearchSelected = 0;
}

void CXcMenu::RenderSearch()
{
	const float Open = Animate(ImGui::GetID("##search_open"), m_SearchOpen ? 1.0f : 0.0f, 18.0f);
	if(Open <= 0.001f)
		return;
	const STheme &t = Theme();
	const ImGuiIO &Io = ImGui::GetIO();

	// results: everything matching, best first; an empty query shows all pages and sections
	std::vector<std::pair<int, const SSearchEntry *>> vResults;
	for(const SSearchEntry &Entry : gs_aSearchEntries)
	{
		const int s = m_aSearch[0] ? Score(Entry, m_aSearch) : (Entry.m_pCard ? 0 : 1);
		if(s > 0)
			vResults.emplace_back(s, &Entry);
	}
	std::stable_sort(vResults.begin(), vResults.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	const int MaxShown = 8;
	const int NumShown = std::min((int)vResults.size(), MaxShown);
	m_SearchSelected = std::clamp(m_SearchSelected, 0, std::max(NumShown - 1, 0));

	// dim everything behind
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(Io.DisplaySize);
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * Open);
	ImGui::Begin("##search_overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
	ImGui::PopStyleVar(2);
	ImDrawList *pDraw = ImGui::GetWindowDrawList();
	pDraw->AddRectFilled(ImVec2(0, 0), Io.DisplaySize, Col(ImVec4(0, 0, 0, 1), 0.45f * Open));
	if(!m_SearchOpen)
	{
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}

	const float Width = std::min(S(640.0f), Io.DisplaySize.x - S(40.0f));
	const float RowHeight = S(52.0f);
	const float InputHeight = S(52.0f);
	const float Height = InputHeight + S(8.0f) + std::max(NumShown, 1) * RowHeight + S(12.0f);
	const ImVec2 Min((Io.DisplaySize.x - Width) * 0.5f, S(110.0f) - (1.0f - Open) * S(12.0f));
	const ImVec2 Max(Min.x + Width, Min.y + Height);
	// a click outside the box closes the search
	if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseHoveringRect(Min, Max))
		m_SearchOpen = false;

	for(int s = 4; s >= 1; s--)
		pDraw->AddRectFilled(ImVec2(Min.x - s * 3.0f, Min.y - s * 2.0f + S(6.0f)), ImVec2(Max.x + s * 3.0f, Max.y + s * 3.0f + S(6.0f)), Col(ImVec4(0, 0, 0, 1), 0.07f), S(16.0f) + s * 3.0f);
	PanelRect(pDraw, Min, Max, S(16.0f), ImVec4(t.m_Card.x, t.m_Card.y, t.m_Card.z, 1.0f));

	// input with a search icon
	ImFont *pIcons = Fonts().m_pLabel;
	const ImVec2 IconSize = pIcons->CalcTextSizeA(pIcons->FontSize, FLT_MAX, 0.0f, XC_ICON_SEARCH);
	pDraw->AddText(pIcons, pIcons->FontSize, ImVec2(Min.x + S(20.0f), Min.y + (InputHeight - IconSize.y) * 0.5f), Col(t.m_TextMuted), XC_ICON_SEARCH);
	ImGui::SetCursorScreenPos(ImVec2(Min.x + S(44.0f), Min.y + (InputHeight - S(34.0f)) * 0.5f));
	if(m_SearchFocus)
	{
		ImGui::SetKeyboardFocusHere();
		m_SearchFocus = false;
	}
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
	ImGui::PushFont(Fonts().m_pTitle);
	if(ImGui::InputTextWithHint("##search", Tr("Search settings and pages"), m_aSearch, sizeof(m_aSearch)))
		m_SearchSelected = 0;
	ImGui::PopFont();
	ImGui::PopStyleColor(2);
	pDraw->AddLine(ImVec2(Min.x, Min.y + InputHeight), ImVec2(Max.x, Min.y + InputHeight), Col(t.m_Border));

	// keyboard: arrows move, Enter opens
	if(ImGui::IsKeyPressed(ImGuiKey_DownArrow))
		m_SearchSelected = std::min(m_SearchSelected + 1, NumShown - 1);
	if(ImGui::IsKeyPressed(ImGuiKey_UpArrow))
		m_SearchSelected = std::max(m_SearchSelected - 1, 0);
	int Chosen = -1;
	if(NumShown > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
		Chosen = m_SearchSelected;

	static const char *const s_apPageIcons[] = {XC_ICON_HOME, XC_ICON_SERVER, XC_ICON_WAND, XC_ICON_FOLDER, XC_ICON_FILM, XC_ICON_CHECK, XC_ICON_GEAR};
	static const char *const s_apPageNames[] = {"Home", "Servers", "Visuals", "Configs", "Demos", "Votes", "Settings"};
	static_assert(std::size(s_apPageIcons) == NUM_PAGES && std::size(s_apPageNames) == NUM_PAGES);
	// one name per section: a missing one would be a null string and crash the search
	static const char *const s_apSectionNames[] = {"Interface", "Background", "Player", "Controls", "Sound", "Chat", "Graphics", "HUD", "Textures", "War list", "TClient", "DDNet"};
	static_assert(std::size(s_apSectionNames) == NUM_SETTINGS, "every settings section needs a name");
	float y = Min.y + InputHeight + S(8.0f);
	if(NumShown == 0)
	{
		ImGui::SetCursorScreenPos(ImVec2(Min.x + S(20.0f), y + S(14.0f)));
		Description(Tr("Nothing found"));
	}
	for(int i = 0; i < NumShown; i++)
	{
		const SSearchEntry &Entry = *vResults[i].second;
		const ImVec2 RowMin(Min.x + S(8.0f), y);
		const ImVec2 RowMax(Max.x - S(8.0f), y + RowHeight);
		const bool Hovered = ImGui::IsMouseHoveringRect(RowMin, RowMax);
		if(Hovered && (Io.MouseDelta.x != 0.0f || Io.MouseDelta.y != 0.0f))
			m_SearchSelected = i;
		if(Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			Chosen = i;
		const float Select = Animate(ImGui::GetID(Entry.m_pTitle) + i, m_SearchSelected == i ? 1.0f : 0.0f, 22.0f);
		if(Select > 0.0f)
			pDraw->AddRectFilled(RowMin, RowMax, Col(t.m_Accent, 0.14f * Select), S(10.0f));

		// icon, title, breadcrumb
		const char *pIcon = s_apPageIcons[Entry.m_Page];
		const ImVec2 IconCenter(RowMin.x + S(26.0f), y + RowHeight * 0.5f);
		pDraw->AddCircleFilled(IconCenter, S(15.0f), Col(t.m_Track), 24);
		const ImVec2 Size = pIcons->CalcTextSizeA(pIcons->FontSize, FLT_MAX, 0.0f, pIcon);
		pDraw->AddText(pIcons, pIcons->FontSize, ImVec2(IconCenter.x - Size.x * 0.5f, IconCenter.y - Size.y * 0.5f), Col(Mix(t.m_TextSecondary, t.m_Accent, Select)), pIcon);
		ImFont *pTitle = Fonts().m_pLabel;
		ImFont *pSmall = Fonts().m_pSmall;
		pDraw->AddText(pTitle, pTitle->FontSize, ImVec2(RowMin.x + S(54.0f), y + S(8.0f)), Col(t.m_Text), Tr(Entry.m_pTitle));
		char aPath[128];
		static const char *const s_apVisualNames[] = {"General", "Tee", "Movement", "Cosmetics", "Weapons", "Crosshair", "World", "Events", "Names"};
		static_assert(std::size(s_apVisualNames) == NUM_VISUALS, "every visuals section needs a name");
		if(Entry.m_Section >= 0)
			str_format(aPath, sizeof(aPath), "%s  >  %s", Tr(s_apPageNames[Entry.m_Page]), Tr(Entry.m_Page == PAGE_VISUALS ? s_apVisualNames[Entry.m_Section] : s_apSectionNames[Entry.m_Section]));
		else
			str_copy(aPath, Tr(Entry.m_pDescription));
		pDraw->AddText(pSmall, pSmall->FontSize, ImVec2(RowMin.x + S(54.0f), y + S(28.0f)), Col(t.m_TextMuted), aPath);
		if(m_SearchSelected == i)
		{
			const ImVec2 ArrowSize = pIcons->CalcTextSizeA(pIcons->FontSize, FLT_MAX, 0.0f, XC_ICON_ARROW_RIGHT);
			pDraw->AddText(pIcons, pIcons->FontSize, ImVec2(RowMax.x - S(16.0f) - ArrowSize.x, y + (RowHeight - ArrowSize.y) * 0.5f), Col(t.m_Accent, Select), XC_ICON_ARROW_RIGHT);
		}
		y += RowHeight;
	}

	if(Chosen >= 0)
	{
		const SSearchEntry &Entry = *vResults[Chosen].second;
		PlayUiSound(SOUND_TAB);
		m_Page = Entry.m_Page;
		if(Entry.m_Section >= 0)
		{
			if(Entry.m_Page == PAGE_VISUALS)
				m_VisualsSection = Entry.m_Section;
			else
				m_SettingsSection = Entry.m_Section;
		}
		m_ScrollTarget = 0.0f;
		if(Entry.m_pCard)
		{
			HighlightCard(Entry.m_pCard);
			m_ScrollToHighlight = true;
		}
		m_SearchOpen = false;
	}

	ImGui::PopStyleVar();
	ImGui::End();
}
