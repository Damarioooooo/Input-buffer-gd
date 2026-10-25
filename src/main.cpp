#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

using namespace geode::prelude;

// ---- settings --------------------------------------------------------
static constexpr char const* STAGE_NAMES[] = {"2 ticks", "4 ticks", "8 ticks", "16 ticks", "Off"};
static constexpr int STAGES[] = {2, 4, 8, 16, 0};
static constexpr int STAGE_COUNT = 5;
static int g_stage = 1;

static constexpr int WAVE_MAX_TICKS = 8;

// After a spawn, wave Grid / Min Gap are skipped for this many ticks.
static constexpr int SPAWN_GRACE_TICKS = 16;

// Index: 0 cube, 1 ball, 2 robot, 3 spider, 4 ship, 5 ufo, 6 swing, 7 wave.
// Value: -1 = same as main window, 0 = off, >0 = custom ticks.
static constexpr int MODE_COUNT = 8;
static constexpr int MODE_WAVE = 7;
static constexpr char const* MODE_KEYS[] = {
    "cube-ticks", "ball-ticks", "robot-ticks", "spider-ticks",
    "ship-ticks", "ufo-ticks", "swing-ticks", "wave-ticks"
};
static int g_modeTicks[MODE_COUNT] = {-1, -1, -1, -1, -1, -1, -1, -1};

static constexpr char const* WAVE_NAMES[] = {"Min Hold", "Min Gap", "Grid"};
enum WaveMode { WAVE_HOLD = 0, WAVE_GAP = 1, WAVE_GRID = 2 };
static constexpr int WAVE_MODE_COUNT = 3;
static int g_waveMode = WAVE_GRID;

// Experimental: the same three timing styles for ship, UFO and swing.
// Index: 0 ship, 1 ufo, 2 swing. Default is Min Hold (the old behavior).
static constexpr char const* FLY_STYLE_KEYS[] = {"ship-style", "ufo-style", "swing-style"};
static int g_flyStyle[3] = {WAVE_HOLD, WAVE_HOLD, WAVE_HOLD};

// The game speed is typed in as a number. These common speeds are only used
// by the TPS Up / TPS Down hotkeys.
static constexpr int TPS_PRESETS[] = {60, 120, 240, 360, 480, 720, 960, 1000};
static constexpr int TPS_COUNT = 8;
static constexpr int TPS_MIN = 1;
static constexpr int TPS_MAX = 10000;
static int g_tps = 240;

static bool g_debug = false;

// Experimental: buffering during dual-portal sections of one-player levels.
// Off by default. Real 2-player levels are not affected by this.
static bool g_dualEnabled = false;

// Extras (v1.2): master switch, injected-click hold, ring re-press, coyote
// time, legit mode, on-screen buttons and click feedback.
static bool g_enabled = true;
static bool g_injectMirror = false;   // injected click mirrors the real hold length
static bool g_rering = false;         // re-press when a held click reaches a ring
static int g_coyote = 0;              // late-click window in ticks (0 = off)
static constexpr char const* LEGIT_NAMES[] = {"Off", "Rated levels", "All levels"};
static int g_legit = 0;
static bool g_hud = false;
static constexpr char const* HUD_POS_NAMES[] = {"Top Left", "Top Right", "Bottom Left", "Bottom Right"};
static int g_hudPos = 1;
static bool g_feedback = true;
static constexpr int INJECT_CAP_TICKS = 30;
static void refreshHud();

// Smart Jump (cube): learns a real jump, then delays an early ground click to
// a start tick that clears the hazards ahead.
static bool g_smart = false;
static int g_smartWindow = 12;     // ticks to look ahead
static int g_smartMargin = 0;      // extra safety around hazards, in units
static bool g_smartEarliest = false;
static int g_sjLat = 0;            // steps between our click and the icon lifting off
static float g_vx = 0.f;           // measured horizontal units per step
static int g_robotMinHold = 0;     // robot: shortest hold in ticks (0 = off)
static bool g_smartWave = false;   // wave: retime a turn that would hit a hazard
static int g_smartWaveWindow = 4;  // wave: ticks a turn may be delayed
static float g_wvy = 0.f;          // measured wave vertical units per step
static constexpr char const* INJECT_NAMES[] = {"Short", "Mirror my hold"};
static constexpr char const* SMART_PICK_NAMES[] = {"Middle", "Earliest"};

// Hotkeys are keybind settings. Order: 0 stage, 1 tps up, 2 tps down, 3 debug, 4 wave mode, 5 add section.
static constexpr int KEYBIND_COUNT = 6;
static constexpr char const* KEYBIND_SETTINGS[] = {"kb-stage", "kb-tps-up", "kb-tps-down", "kb-debug", "kb-wave", "kb-section"};

// Raised whenever the level resets (death, restart, checkpoint).
static int g_resetEpoch = 0;
// ----------------------------------------------------------------------

#define DBG(...) do { if (g_debug) log::info(__VA_ARGS__); } while (0)

static int indexOf(char const* const* names, int n, std::string const& v, int def) {
    for (int i = 0; i < n; i++) if (v == names[i]) return i;
    return def;
}

static void loadSettings() {
    auto m = Mod::get();
    g_stage = indexOf(STAGE_NAMES, STAGE_COUNT, m->getSettingValue<std::string>("stage"), 1);
    g_tps = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("tps-value")), TPS_MIN, TPS_MAX);
    g_waveMode = indexOf(WAVE_NAMES, WAVE_MODE_COUNT, m->getSettingValue<std::string>("wave-mode"), WAVE_GRID);
    for (int i = 0; i < MODE_COUNT; i++) {
        g_modeTicks[i] = static_cast<int>(m->getSettingValue<int64_t>(MODE_KEYS[i]));
    }
    for (int i = 0; i < 3; i++) {
        g_flyStyle[i] = indexOf(WAVE_NAMES, WAVE_MODE_COUNT, m->getSettingValue<std::string>(FLY_STYLE_KEYS[i]), WAVE_HOLD);
    }
    g_debug = m->getSettingValue<bool>("debug");
    g_dualEnabled = m->getSettingValue<bool>("dual-mode");
    g_enabled = m->getSettingValue<bool>("enabled");
    g_injectMirror = m->getSettingValue<std::string>("inject-hold") == "Mirror my hold";
    g_rering = m->getSettingValue<bool>("rering");
    g_coyote = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("coyote-ticks")), 0, 16);
    g_legit = indexOf(LEGIT_NAMES, 3, m->getSettingValue<std::string>("legit-mode"), 0);
    g_hud = m->getSettingValue<bool>("hud");
    g_hudPos = indexOf(HUD_POS_NAMES, 4, m->getSettingValue<std::string>("hud-pos"), 1);
    g_feedback = m->getSettingValue<bool>("feedback");
    g_smart = m->getSettingValue<bool>("smart-jump");
    g_smartWindow = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("smart-window")), 1, 40);
    g_smartMargin = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("smart-margin")), 0, 6);
    g_smartEarliest = m->getSettingValue<std::string>("smart-pick") == "Earliest";
    g_robotMinHold = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("robot-minhold")), 0, 32);
    g_smartWave = m->getSettingValue<bool>("smart-wave");
    g_smartWaveWindow = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("smart-wave-window")), 1, 16);
    refreshHud();
}

static char const* waveModeName() { return WAVE_NAMES[g_waveMode]; }

static float stepsPerTick() {
    return g_tps / 240.0f;
}

static int mainLimitSteps() {
    return static_cast<int>(std::lround(STAGES[g_stage] * stepsPerTick()));
}

static bool isFlyMode(PlayerObject* p) {
    return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
}

static bool isGroundMode(PlayerObject* p) {
    return !isFlyMode(p);
}

static int modeIndex(PlayerObject* p) {
    if (p->m_isShip) return 4;
    if (p->m_isBird) return 5;
    if (p->m_isDart) return 7;
    if (p->m_isSwing) return 6;
    if (p->m_isBall) return 1;
    if (p->m_isRobot) return 2;
    if (p->m_isSpider) return 3;
    return 0;
}

// Timing style of the icon's current mode (Min Hold, Min Gap or Grid).
static int styleFor(PlayerObject* p) {
    int idx = modeIndex(p);
    if (idx == MODE_WAVE) return g_waveMode;
    if (idx >= 4 && idx <= 6) return g_flyStyle[idx - 4];
    return WAVE_HOLD;
}

static int ticksForMode(int idx) {
    int c = g_modeTicks[idx];
    if (c < 0) {
        int t = STAGES[g_stage];
        if (idx == MODE_WAVE && t > 0) t = std::min(t, WAVE_MAX_TICKS);
        return t;
    }
    return c;
}

static int modeLimitSteps(PlayerObject* p) {
    int ticks = ticksForMode(modeIndex(p));
    if (ticks <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(ticks * stepsPerTick())));
}

static int waveGridSteps() {
    int ticks = ticksForMode(MODE_WAVE);
    if (ticks <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(ticks * stepsPerTick())));
}

static int spawnGraceSteps() {
    return std::max(1, static_cast<int>(std::lround(SPAWN_GRACE_TICKS * stepsPerTick())));
}

static int ringsTouching(PlayerObject* p) {
    return p->m_touchingRings ? static_cast<int>(p->m_touchingRings->count()) : 0;
}

static char const* modeName(PlayerObject* p) {
    if (p->m_isShip) return "ship";
    if (p->m_isBird) return "ufo";
    if (p->m_isDart) return "wave";
    if (p->m_isSwing) return "swing";
    if (p->m_isBall) return "ball";
    if (p->m_isRobot) return "robot";
    if (p->m_isSpider) return "spider";
    return "cube";
}

// Next common speed above / below the current one. A typed value between two
// presets moves to the neighbor in the direction pressed; at the ends it stays.
static int stepTps(int cur, bool up) {
    if (up) {
        for (int i = 0; i < TPS_COUNT; i++) {
            if (TPS_PRESETS[i] > cur) return TPS_PRESETS[i];
        }
        return cur;
    }
    for (int i = TPS_COUNT - 1; i >= 0; i--) {
        if (TPS_PRESETS[i] < cur) return TPS_PRESETS[i];
    }
    return cur;
}

static void addSectionHere();

static void doHotkey(int action) {
    auto m = Mod::get();
    switch (action) {
        case 0:
            g_stage = (g_stage + 1) % STAGE_COUNT;
            m->setSettingValue<std::string>("stage", STAGE_NAMES[g_stage]);
            log::info("[InputBuffer] Main window -> {} ({} steps, wave {} steps @ {} TPS)",
                      STAGE_NAMES[g_stage], mainLimitSteps(), waveGridSteps(), g_tps);
            break;
        case 1:
            g_tps = stepTps(g_tps, true);
            m->setSettingValue<int64_t>("tps-value", static_cast<int64_t>(g_tps));
            log::info("[InputBuffer] TPS -> {} ({:.2f} steps/tick)", g_tps, stepsPerTick());
            break;
        case 2:
            g_tps = stepTps(g_tps, false);
            m->setSettingValue<int64_t>("tps-value", static_cast<int64_t>(g_tps));
            log::info("[InputBuffer] TPS -> {} ({:.2f} steps/tick)", g_tps, stepsPerTick());
            break;
        case 3:
            g_debug = !g_debug;
            m->setSettingValue<bool>("debug", g_debug);
            log::info("[InputBuffer] Debug logging -> {}", g_debug ? "ON" : "OFF");
            break;
        case 4:
            g_waveMode = (g_waveMode + 1) % WAVE_MODE_COUNT;
            m->setSettingValue<std::string>("wave-mode", WAVE_NAMES[g_waveMode]);
            log::info("[InputBuffer] Wave mode -> {} ({} steps)", waveModeName(), waveGridSteps());
            break;
        case 5:
            addSectionHere();
            break;
    }
    refreshHud();
}

static void hotkeyPressed(int action, bool down, bool repeat) {
    if (!down || repeat) return;
    if (!PlayLayer::get()) return; // hotkeys only act inside a level
    doHotkey(action);
}

// ---- extras: legit mode, stats, feedback, on-screen buttons -------------
// The assist is skipped when it is switched off, or when legit mode says this
// kind of run should be played raw. Practice mode and test mode always keep it.
static bool assistAllowed() {
    if (!g_enabled) return false;
    if (g_legit == 0) return true;
    auto pl = PlayLayer::get();
    if (!pl) return true;
    if (pl->m_isPracticeMode || pl->m_isTestMode) return true;
    if (g_legit == 2) return false;
    return !(pl->m_level && pl->m_level->m_stars.value() > 0);
}

// How early (in ticks) each buffered click was, for cube / ball / robot / spider.
static std::vector<float> g_samples[4];
static int g_expired[4] = {0, 0, 0, 0};

static void clearStats() {
    for (auto& v : g_samples) v.clear();
    for (auto& e : g_expired) e = 0;
}

static void recordSample(int idx, float ticks) {
    if (idx < 0 || idx > 3) return;
    auto& v = g_samples[idx];
    if (v.size() >= 500) v.erase(v.begin());
    v.push_back(ticks);
}

static void recordExpired(int idx) {
    if (idx >= 0 && idx <= 3) g_expired[idx]++;
}

// Median, 90th percentile and a suggested window for one mode.
static bool statsFor(int i, float& med, float& p90, int& suggest) {
    if (i < 0 || i > 3 || g_samples[i].empty()) return false;
    auto v = g_samples[i];
    std::sort(v.begin(), v.end());
    med = v[v.size() / 2];
    size_t k = std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.9));
    p90 = v[k];
    suggest = std::clamp(static_cast<int>(std::ceil(p90)) + 1, 1, 64);
    return true;
}

// A small "+3.2t" that fades out at the top of the screen when a buffered
// click fires.
static void showText(char const* text) {
    if (!g_feedback) return;
    auto pl = PlayLayer::get();
    if (!pl) return;
    auto win = CCDirector::get()->getWinSize();
    auto label = CCLabelBMFont::create(text, "bigFont.fnt");
    label->setScale(0.4f);
    label->setColor({90, 255, 120});
    label->setPosition({win.width / 2.f, win.height - 30.f});
    pl->addChild(label, 1001);
    label->runAction(CCSequence::create(
        CCDelayTime::create(0.25f),
        CCFadeOut::create(0.35f),
        CCCallFunc::create(label, callfunc_selector(CCNode::removeFromParent)),
        nullptr));
}

static void showFeedback(float ticks) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "+%.1ft", ticks);
    showText(buf);
}

static ButtonSprite* g_hudToggle = nullptr;
static ButtonSprite* g_hudWin = nullptr;
static ButtonSprite* g_hudWave = nullptr;

class HudHandler : public CCObject {
public:
    void onToggle(CCObject*) {
        Mod::get()->setSettingValue<bool>("enabled", !g_enabled);
        loadSettings();
    }
    void onWindow(CCObject*) { doHotkey(0); }
    void onWave(CCObject*) { doHotkey(4); }
};

static HudHandler* hudHandler() {
    static HudHandler* h = new HudHandler();
    return h;
}

static void refreshHud() {
    if (!PlayLayer::get()) {
        g_hudToggle = g_hudWin = g_hudWave = nullptr;
        return;
    }
    if (g_hudToggle) g_hudToggle->setString(g_enabled ? "ON" : "OFF");
    if (g_hudWin) g_hudWin->setString(STAGE_NAMES[g_stage]);
    if (g_hudWave) g_hudWave->setString(WAVE_NAMES[g_waveMode]);
}

// Three small buttons in a corner of the level: on/off, buffer window, wave
// mode. Off by default (a button in the way can eat a tap).
static void buildHud(PlayLayer* pl) {
    g_hudToggle = g_hudWin = g_hudWave = nullptr;
    if (!pl) return;
    if (auto old = pl->getChildByID("pib-hud")) old->removeFromParent();
    if (!g_hud) return;

    auto win = CCDirector::get()->getWinSize();
    bool top = g_hudPos == 0 || g_hudPos == 1;
    bool left = g_hudPos == 0 || g_hudPos == 2;
    float dir = top ? -1.f : 1.f;

    auto menu = CCMenu::create();
    menu->setID("pib-hud");
    menu->setPosition({left ? 38.f : win.width - 38.f, top ? win.height - 24.f : 24.f});

    auto add = [&](char const* label, SEL_MenuHandler cb, int row) -> ButtonSprite* {
        auto spr = ButtonSprite::create(label);
        spr->setScale(0.4f);
        auto btn = CCMenuItemSpriteExtra::create(spr, hudHandler(), cb);
        btn->setPosition({0.f, dir * 22.f * row});
        menu->addChild(btn);
        return spr;
    };
    g_hudToggle = add("ON", menu_selector(HudHandler::onToggle), 0);
    g_hudWin = add("4 ticks", menu_selector(HudHandler::onWindow), 1);
    g_hudWave = add("Grid", menu_selector(HudHandler::onWave), 2);

    pl->addChild(menu, 1000);
    refreshHud();
}

// ---- profiles --------------------------------------------------------
// A profile is a named copy of the timing settings (not hotkeys or debug).
// It is stored as one line of text: key=value|key=value|...
struct ProfileKey { char const* key; char type; }; // s = string, i = int, b = bool
static constexpr ProfileKey PROFILE_KEYS[] = {
    {"stage", 's'}, {"wave-mode", 's'}, {"tps-value", 'i'},
    {"cube-ticks", 'i'}, {"ball-ticks", 'i'}, {"robot-ticks", 'i'}, {"spider-ticks", 'i'},
    {"ship-ticks", 'i'}, {"ufo-ticks", 'i'}, {"swing-ticks", 'i'}, {"wave-ticks", 'i'},
    {"dual-mode", 'b'},
    {"ship-style", 's'}, {"ufo-style", 's'}, {"swing-style", 's'},
    {"inject-hold", 's'}, {"rering", 'b'}, {"coyote-ticks", 'i'},
    {"smart-jump", 'b'}, {"smart-window", 'i'}, {"smart-margin", 'i'}, {"smart-pick", 's'},
    {"robot-minhold", 'i'},
    {"smart-wave", 'b'}, {"smart-wave-window", 'i'},
};

static std::string serializeProfile() {
    auto m = Mod::get();
    std::string out;
    for (auto const& k : PROFILE_KEYS) {
        if (!out.empty()) out += '|';
        out += k.key;
        out += '=';
        if (k.type == 's') out += m->getSettingValue<std::string>(k.key);
        else if (k.type == 'i') out += std::to_string(m->getSettingValue<int64_t>(k.key));
        else out += m->getSettingValue<bool>(k.key) ? "1" : "0";
    }
    return out;
}

static void applyProfileData(std::string const& data) {
    auto m = Mod::get();
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t end = data.find('|', pos);
        if (end == std::string::npos) end = data.size();
        std::string item = data.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string key = item.substr(0, eq);
        std::string val = item.substr(eq + 1);
        for (auto const& k : PROFILE_KEYS) {
            if (key != k.key) continue;
            if (k.type == 's') m->setSettingValue<std::string>(k.key, val);
            else if (k.type == 'i') m->setSettingValue<int64_t>(k.key, static_cast<int64_t>(std::strtoll(val.c_str(), nullptr, 10)));
            else m->setSettingValue<bool>(k.key, val == "1");
            break;
        }
    }
    loadSettings();
}

static std::string profileDataKey(std::string const& name) { return "profile-data-" + name; }

static std::vector<std::string> loadProfileNames() {
    std::vector<std::string> out;
    std::string raw = Mod::get()->getSavedValue<std::string>("profile-names", "");
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find('\n', pos);
        if (end == std::string::npos) end = raw.size();
        if (end > pos) out.push_back(raw.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

static void saveProfileNames(std::vector<std::string> const& names) {
    std::string raw;
    for (auto const& n : names) {
        raw += n;
        raw += '\n';
    }
    Mod::get()->setSavedValue<std::string>("profile-names", raw);
}

static bool applyProfileByName(std::string const& name) {
    std::string data = Mod::get()->getSavedValue<std::string>(profileDataKey(name), "");
    if (data.empty()) return false;
    applyProfileData(data);
    return true;
}

// When a level applies its own profile, the player's settings from before are
// saved first and put back when the level is left. The copy is kept on disk,
// so it is also restored the next time the game starts if the game crashed.
static void restoreAfterLevel() {
    std::string snap = Mod::get()->getSavedValue<std::string>("restore-snapshot", "");
    if (snap.empty()) return;
    Mod::get()->setSavedValue<std::string>("restore-snapshot", "");
    applyProfileData(snap);
    log::info("[InputBuffer] Restored your settings from before the level profile");
}

// Which profile a level should use (level id -> profile name).
static std::vector<std::pair<int, std::string>> loadLevelProfiles() {
    std::vector<std::pair<int, std::string>> out;
    std::string raw = Mod::get()->getSavedValue<std::string>("level-profiles", "");
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find('\n', pos);
        if (end == std::string::npos) end = raw.size();
        std::string line = raw.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out.emplace_back(std::atoi(line.substr(0, eq).c_str()), line.substr(eq + 1));
    }
    return out;
}

static void saveLevelProfiles(std::vector<std::pair<int, std::string>> const& v) {
    std::string raw;
    for (auto const& e : v) {
        raw += std::to_string(e.first);
        raw += '=';
        raw += e.second;
        raw += '\n';
    }
    Mod::get()->setSavedValue<std::string>("level-profiles", raw);
}

static std::string levelProfileFor(int id) {
    for (auto const& e : loadLevelProfiles()) {
        if (e.first == id) return e.second;
    }
    return "";
}

static void setLevelProfile(int id, std::string const& name) {
    auto v = loadLevelProfiles();
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto const& e) { return e.first == id; }), v.end());
    if (!name.empty()) v.emplace_back(id, name);
    saveLevelProfiles(v);
}

static void forgetProfileEverywhere(std::string const& name) {
    auto v = loadLevelProfiles();
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto const& e) { return e.second == name; }), v.end());
    saveLevelProfiles(v);
}

// A key that identifies a level. Uploaded levels use their real ID. Levels
// that were never uploaded (such as copies you made to edit) have ID 0, so
// they get a negative key made from their name instead.
static int levelKeyFor(GJGameLevel* level) {
    if (!level) return 0;
    int id = level->m_levelID.value();
    if (id > 0) return id;
    std::string name = level->m_levelName;
    if (name.empty()) return 0;
    int h = static_cast<int>(std::hash<std::string>{}(name) & 0x3fffffff);
    return -(h + 1);
}

static int currentLevelID() {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) return 0;
    return levelKeyFor(pl->m_level);
}


// ---- sections (experimental) -------------------------------------------
// A level can be split into sections by percent. Each section has its own
// start and end (two decimals) and its own copy of the settings. Parts of the
// level that no section covers follow the "empty sections" choice. If sections
// overlap, the one that starts last wins.
struct Section { float start; float end; std::string data; };
struct LevelSections {
    int emptyMode = 0; // 0 level's blanket profile, 1 normal settings, 2 buffer off
    std::vector<Section> secs;
};
static constexpr char const* EMPTY_MODE_NAMES[] = {"Blanket profile", "My normal settings", "Buffer off"};
static constexpr int EMPTY_MODE_COUNT = 3;
static constexpr char const* OFF_PROFILE =
    "stage=Off|cube-ticks=0|ball-ticks=0|robot-ticks=0|spider-ticks=0|ship-ticks=0|ufo-ticks=0|swing-ticks=0|wave-ticks=0";

static std::vector<std::string> splitStr(std::string const& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(sep, pos);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

static std::string fmtPct(float p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", p);
    return buf;
}

static float roundPct(float p) {
    return std::round(std::clamp(p, 0.f, 100.f) * 100.f) / 100.f;
}

static void sortSections(LevelSections& ls) {
    std::stable_sort(ls.secs.begin(), ls.secs.end(), [](Section const& a, Section const& b) { return a.start < b.start; });
}

// Read / change one key inside a profile string (key=value|key=value|...).
static std::string profGet(std::string const& data, std::string const& key, std::string const& def = "") {
    for (auto const& item : splitStr(data, '|')) {
        size_t eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == key) return item.substr(eq + 1);
    }
    return def;
}

static std::string profSet(std::string const& data, std::string const& key, std::string const& val) {
    std::string out;
    bool done = false;
    for (auto const& item : splitStr(data, '|')) {
        if (item.empty()) continue;
        size_t eq = item.find('=');
        std::string k = eq == std::string::npos ? item : item.substr(0, eq);
        if (!out.empty()) out += '|';
        if (k == key) { out += key + "=" + val; done = true; }
        else out += item;
    }
    if (!done) {
        if (!out.empty()) out += '|';
        out += key + "=" + val;
    }
    return out;
}

// Saved as one line per level: id <tab> emptyMode <tab> start,end,data;start,end,data
static LevelSections loadLevelSections(int id) {
    LevelSections ls;
    std::string raw = Mod::get()->getSavedValue<std::string>("level-sections", "");
    for (auto const& line : splitStr(raw, '\n')) {
        auto parts = splitStr(line, '\t');
        if (parts.size() < 3 || std::atoi(parts[0].c_str()) != id) continue;
        ls.emptyMode = std::clamp(std::atoi(parts[1].c_str()), 0, EMPTY_MODE_COUNT - 1);
        for (auto const& item : splitStr(parts[2], ';')) {
            size_t c1 = item.find(',');
            size_t c2 = c1 == std::string::npos ? c1 : item.find(',', c1 + 1);
            if (c2 == std::string::npos) continue;
            float a = roundPct(static_cast<float>(std::atof(item.substr(0, c1).c_str())));
            float b = roundPct(static_cast<float>(std::atof(item.substr(c1 + 1, c2 - c1 - 1).c_str())));
            ls.secs.push_back({a, std::max(a, b), item.substr(c2 + 1)});
        }
        break;
    }
    return ls;
}

static void saveLevelSections(int id, LevelSections const& ls) {
    std::string raw = Mod::get()->getSavedValue<std::string>("level-sections", "");
    std::string out;
    for (auto const& line : splitStr(raw, '\n')) {
        if (line.empty()) continue;
        auto parts = splitStr(line, '\t');
        if (!parts.empty() && std::atoi(parts[0].c_str()) == id) continue;
        out += line;
        out += '\n';
    }
    if (!ls.secs.empty() || ls.emptyMode != 0) {
        out += std::to_string(id) + '\t' + std::to_string(ls.emptyMode) + '\t';
        for (size_t i = 0; i < ls.secs.size(); i++) {
            if (i) out += ';';
            out += fmtPct(ls.secs[i].start) + ',' + fmtPct(ls.secs[i].end) + ',' + ls.secs[i].data;
        }
        out += '\n';
    }
    Mod::get()->setSavedValue<std::string>("level-sections", out);
}

// Runtime state for the level being played.
static bool g_secActive = false;
static LevelSections g_sec;      // sorted copy
static std::string g_secToken;   // what is currently applied
static float g_lastDeathPct = -1.f;

static std::string snapshotData() {
    return Mod::get()->getSavedValue<std::string>("restore-snapshot", "");
}

static void ensureSnapshot() {
    if (snapshotData().empty()) {
        Mod::get()->setSavedValue<std::string>("restore-snapshot", serializeProfile());
    }
}

static std::string emptyToken() {
    return g_sec.emptyMode == 1 ? "N" : (g_sec.emptyMode == 2 ? "O" : "B");
}

// Which section covers this percent. The one that starts last wins.
static std::string sectionToken(float pct) {
    int idx = -1;
    for (size_t i = 0; i < g_sec.secs.size(); i++) {
        auto const& s = g_sec.secs[i];
        if (s.data.empty()) continue;
        bool inside = pct + 0.0001f >= s.start && (pct < s.end - 0.0001f || s.end >= 99.995f);
        if (inside) idx = static_cast<int>(i);
    }
    return idx >= 0 ? "S:" + std::to_string(idx) : emptyToken();
}

static void applyToken(std::string const& tok, int levelID) {
    if (tok == "O") { applyProfileData(OFF_PROFILE); return; }
    if (tok.rfind("S:", 0) == 0) {
        size_t i = static_cast<size_t>(std::atoi(tok.c_str() + 2));
        if (i < g_sec.secs.size()) applyProfileData(g_sec.secs[i].data);
        return;
    }
    if (tok == "B") {
        std::string name = levelProfileFor(levelID);
        if (!name.empty() && applyProfileByName(name)) return;
    }
    std::string snap = snapshotData();
    if (!snap.empty()) applyProfileData(snap);
}

// Call after the sections of the current level were edited.
static void reloadSectionsRuntime() {
    int id = currentLevelID();
    if (id == 0) return;
    bool wasActive = g_secActive;
    LevelSections ls = loadLevelSections(id);
    sortSections(ls);
    bool on = Mod::get()->getSettingValue<bool>("sections") && !ls.secs.empty();
    g_sec = ls;
    g_secActive = on;
    g_secToken.clear();
    if (on) ensureSnapshot();
    else if (wasActive) applyToken("B", id);
}

// A new section starts at `start`, ends where the next one starts (or at the
// end of the level), and begins as a copy of the current settings.
static Section newSection(LevelSections const& ls, float start) {
    float end = 100.f;
    for (auto const& s : ls.secs) {
        if (s.start > start + 0.005f) end = std::min(end, s.start);
    }
    return {start, end, serializeProfile()};
}

static void addSectionHere() {
    auto pl = PlayLayer::get();
    int id = currentLevelID();
    if (!pl || id == 0) return;
    float pct = roundPct(pl->getCurrentPercent());
    LevelSections ls = loadLevelSections(id);
    for (auto const& s : ls.secs) {
        if (std::fabs(s.start - pct) < 0.005f) return;
    }
    ls.secs.push_back(newSection(ls, pct));
    sortSections(ls);
    saveLevelSections(id, ls);
    reloadSectionsRuntime();
    std::string msg = "Section added at " + fmtPct(pct) + "%";
    Notification::create(msg.c_str(), NotificationIcon::Success, 1.5f)->show();
}

// Small window that asks where a new section should start.
class AddSectionPopup : public geode::Popup {
protected:
    std::function<void(float)> m_pick;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;

    bool init(std::function<void(float)> pick) {
        if (!Popup::init(300.f, 190.f)) return false;
        m_pick = std::move(pick);
        this->setTitle("Add a section");

        auto label = CCLabelBMFont::create("Where should it start?", "bigFont.fnt");
        label->setScale(0.4f);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {0.f, 50.f});

        m_input = TextInput::create(100.f, "Type %");
        m_input->setFilter("0123456789.");
        m_input->setMaxCharCount(6);
        m_input->setScale(0.8f);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {-50.f, 24.f});
        button("Add", 70.f, 24.f, menu_selector(AddSectionPopup::onTyped));

        std::string death = g_lastDeathPct >= 0.f
            ? "Last death: " + fmtPct(roundPct(g_lastDeathPct)) + "%"
            : "Last death: none yet";
        button(death.c_str(), 0.f, -10.f, menu_selector(AddSectionPopup::onDeath));

        auto pl = PlayLayer::get();
        std::string now = pl
            ? "Current position: " + fmtPct(roundPct(pl->getCurrentPercent())) + "%"
            : "Current position: unavailable";
        button(now.c_str(), 0.f, -42.f, menu_selector(AddSectionPopup::onNow));

        m_status = CCLabelBMFont::create("", "bigFont.fnt");
        m_status->setScale(0.3f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0.f, -70.f});
        return true;
    }

    void button(char const* s, float x, float y, SEL_MenuHandler cb) {
        auto spr = ButtonSprite::create(s);
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
    }

    void choose(float pct) {
        auto pick = m_pick;
        this->onClose(nullptr);
        if (pick) pick(roundPct(pct));
    }

public:
    static AddSectionPopup* create(std::function<void(float)> pick) {
        auto ret = new AddSectionPopup();
        if (ret->init(std::move(pick))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onTyped(CCObject*) {
        std::string s = m_input->getString();
        if (s.empty()) { m_status->setString("Type a percentage first."); return; }
        float v = static_cast<float>(std::atof(s.c_str()));
        if (v < 0.f || v > 100.f) { m_status->setString("Use a number from 0 to 100."); return; }
        choose(v);
    }

    void onDeath(CCObject*) {
        if (g_lastDeathPct < 0.f) { m_status->setString("You haven't died in this level yet."); return; }
        choose(g_lastDeathPct);
    }

    void onNow(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) { m_status->setString("Open this from inside a level."); return; }
        choose(pl->getCurrentPercent());
    }
};

// The window where sections are edited. Every section keeps its own copy of
// the settings, which can be changed here without leaving the window.
class SectionsPopup : public geode::Popup {
protected:
    int m_levelID = 0;
    LevelSections m_ls;
    int m_idx = 0;
    int m_page = 0; // 0 window, 1 modes, 2 styles, 3 more
    bool m_updating = false;
    std::vector<std::string> m_names;
    int m_copySel = 0;
    std::vector<CCNode*> m_pageNodes;
    TextInput* m_startInput = nullptr;
    TextInput* m_endInput = nullptr;
    TextInput* m_copyInput = nullptr;
    CCLabelBMFont* m_idxLabel = nullptr;
    CCLabelBMFont* m_nameLabel = nullptr;
    CCLabelBMFont* m_emptyLabel = nullptr;
    CCLabelBMFont* m_status = nullptr;

    CCLabelBMFont* text(char const* s, float x, float y, float scale, bool leftAlign = false, bool onPage = false) {
        auto label = CCLabelBMFont::create(s, "bigFont.fnt");
        label->setScale(scale);
        if (leftAlign) label->setAnchorPoint({0.f, 0.5f});
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {x, y});
        if (onPage) m_pageNodes.push_back(label);
        return label;
    }

    CCMenuItemSpriteExtra* button(char const* s, float x, float y, SEL_MenuHandler cb, float scale = 0.6f) {
        auto spr = ButtonSprite::create(s);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
        return btn;
    }

    CCMenuItemSpriteExtra* arrow(bool flip, float x, float y, SEL_MenuHandler cb, int tag = 0, bool onPage = false) {
        auto spr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        spr->setScale(0.5f);
        spr->setFlipX(flip);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        btn->setTag(tag);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
        if (onPage) m_pageNodes.push_back(btn);
        return btn;
    }

    TextInput* input(float w, char const* hint, char const* filter, int maxChars, float x, float y, bool onPage) {
        auto in = TextInput::create(w, hint);
        in->setFilter(filter);
        in->setMaxCharCount(maxChars);
        m_mainLayer->addChildAtPosition(in, Anchor::Center, {x, y});
        if (onPage) m_pageNodes.push_back(in);
        return in;
    }

    bool init(int levelID) {
        if (!Popup::init(400.f, 290.f)) return false;
        m_levelID = levelID;
        bool sectionsOn = Mod::get()->getSettingValue<bool>("sections");
        this->setTitle(sectionsOn ? "Sections (Experimental)" : "Sections are OFF");
        m_names = loadProfileNames();
        m_ls = loadLevelSections(levelID);
        sortSections(m_ls);

        m_idxLabel = text("", 0.f, 106.f, 0.45f);
        arrow(false, -95.f, 106.f, menu_selector(SectionsPopup::onPrev));
        arrow(true, 95.f, 106.f, menu_selector(SectionsPopup::onNext));

        text("Starts at %", -190.f, 80.f, 0.35f, true);
        m_startInput = input(70.f, "0.00", "0123456789.", 6, -95.f, 80.f, false);
        m_startInput->setCallback([this](std::string const& s) {
            if (m_updating || m_ls.secs.empty()) return;
            auto& sec = m_ls.secs[m_idx];
            sec.start = roundPct(static_cast<float>(std::atof(s.c_str())));
            if (sec.end < sec.start) sec.end = sec.start;
            this->commit();
        });
        m_startInput->setScale(0.8f);
        text("Ends at %", 0.f, 80.f, 0.35f, true);
        m_endInput = input(70.f, "100.00", "0123456789.", 6, 95.f, 80.f, false);
        m_endInput->setCallback([this](std::string const& s) {
            if (m_updating || m_ls.secs.empty()) return;
            auto& sec = m_ls.secs[m_idx];
            sec.end = std::max(sec.start, roundPct(static_cast<float>(std::atof(s.c_str()))));
            this->commit();
        });
        m_endInput->setScale(0.8f);

        button("Window", -135.f, 54.f, menu_selector(SectionsPopup::onPageWindow), 0.5f);
        button("Modes", -45.f, 54.f, menu_selector(SectionsPopup::onPageModes), 0.5f);
        button("Styles", 40.f, 54.f, menu_selector(SectionsPopup::onPageStyles), 0.5f);
        button("More", 125.f, 54.f, menu_selector(SectionsPopup::onPageMore), 0.5f);

        button("Add", -165.f, 106.f, menu_selector(SectionsPopup::onAdd), 0.5f);
        button("Delete", 165.f, 106.f, menu_selector(SectionsPopup::onDelete), 0.5f);
        m_status = text("", 0.f, 32.f, 0.27f);


        this->refresh();
        this->defaultStatus();
        this->buildPage();
        return true;
    }

    // Saves the edits and updates the running level.
    void commit() {
        saveLevelSections(m_levelID, m_ls);
        reloadSectionsRuntime();
    }

    Section* current() {
        if (m_ls.secs.empty() || m_idx < 0 || m_idx >= static_cast<int>(m_ls.secs.size())) return nullptr;
        return &m_ls.secs[m_idx];
    }

    // Name and short label for the cycling rows (tag = row * 2 + direction).
    void cycleRow(char const* label, int row, std::string const& value, float y) {
        text(label, -190.f, y, 0.4f, true, true);
        arrow(false, -10.f, y, menu_selector(SectionsPopup::onCycle), row * 2, true);
        text(value.c_str(), 70.f, y, 0.4f, false, true);
        arrow(true, 150.f, y, menu_selector(SectionsPopup::onCycle), row * 2 + 1, true);
    }

    void tickInput(char const* label, char const* key, float lx, float ix, float y, std::string const& val, bool isTps) {
        text(label, lx, y, 0.4f, true, true);
        auto in = input(80.f, "-1", isTps ? "0123456789" : "-0123456789", isTps ? 5 : 4, ix, y, true);
        in->setScale(0.7f);
        in->setString(val);
        std::string k = key;
        in->setCallback([this, k, isTps](std::string const& s) {
            if (m_updating) return;
            auto sec = current();
            if (!sec || s.empty() || s == "-") return;
            int v = std::atoi(s.c_str());
            v = isTps ? std::clamp(v, TPS_MIN, TPS_MAX) : std::clamp(v, -1, 1000);
            sec->data = profSet(sec->data, k, std::to_string(v));
            this->commit();
        });
    }

    // Rebuilds the settings area for the current section and page.
    void buildPage() {
        m_updating = true;
        for (auto n : m_pageNodes) n->removeFromParent();
        m_pageNodes.clear();
        m_nameLabel = nullptr;
        m_emptyLabel = nullptr;
        m_copyInput = nullptr;
        if (m_page == 3) {
            buildMore();
            m_updating = false;
            return;
        }
        auto sec = current();
        if (m_levelID == 0 || !sec) {
            text("Add a section to edit its settings.", 0.f, 0.f, 0.4f, false, true);
            m_updating = false;
            return;
        }
        std::string const& d = sec->data;
        if (m_page == 0) {
            cycleRow("Main window", 0, profGet(d, "stage", "4 ticks"), 8.f);
            tickInput("Game speed (TPS)", "tps-value", -190.f, 70.f, -24.f, profGet(d, "tps-value", "240"), true);
            cycleRow("Dual mode", 5, profGet(d, "dual-mode", "0") == "1" ? "On" : "Off", -56.f);
        } else if (m_page == 1) {
            static constexpr char const* NAMES[] = {"Cube", "Ball", "Robot", "Spider", "Ship", "UFO", "Swing", "Wave"};
            for (int i = 0; i < 8; i++) {
                int col = i / 4, row = i % 4;
                float y = 8.f - 32.f * row;
                tickInput(NAMES[i], MODE_KEYS[i], col ? 10.f : -190.f, col ? 100.f : -100.f, y, profGet(d, MODE_KEYS[i], "-1"), false);
            }
        } else {
            cycleRow("Wave", 1, profGet(d, "wave-mode", "Grid"), 8.f);
            cycleRow("Ship", 2, profGet(d, "ship-style", "Min Hold"), -24.f);
            cycleRow("UFO", 3, profGet(d, "ufo-style", "Min Hold"), -56.f);
            cycleRow("Swing", 4, profGet(d, "swing-style", "Min Hold"), -88.f);
        }
        m_updating = false;
    }

    void updateMoreLabels() {
        if (m_nameLabel) {
            std::string nm = m_names.empty() ? "(none saved)" : m_names[std::clamp(m_copySel, 0, static_cast<int>(m_names.size()) - 1)];
            m_nameLabel->setString(nm.c_str());
        }
        if (m_emptyLabel) {
            std::string e = std::string("Empty parts use: ") + EMPTY_MODE_NAMES[m_ls.emptyMode];
            m_emptyLabel->setString(e.c_str());
        }
    }

    // The "More" page: copy a saved profile in, choose what empty parts use,
    // and copy the sections to another level.
    void buildMore() {
        text("Copy settings from a saved profile", -190.f, 12.f, 0.33f, true, true);
        arrow(false, -60.f, -12.f, menu_selector(SectionsPopup::onNamePrev), 0, true);
        m_nameLabel = text("", 20.f, -12.f, 0.4f, false, true);
        arrow(true, 100.f, -12.f, menu_selector(SectionsPopup::onNameNext), 0, true);
        m_pageNodes.push_back(button("Load", 160.f, -12.f, menu_selector(SectionsPopup::onLoadProfile), 0.5f));

        m_emptyLabel = text("", -190.f, -46.f, 0.36f, true, true);
        m_pageNodes.push_back(button("Change", 150.f, -46.f, menu_selector(SectionsPopup::onEmptyMode), 0.5f));

        text("Copy these sections to another level", -190.f, -74.f, 0.33f, true, true);
        m_copyInput = input(100.f, "Level ID", "0123456789", 10, -120.f, -98.f, true);
        m_copyInput->setScale(0.75f);
        m_pageNodes.push_back(button("Copy sections", 0.f, -98.f, menu_selector(SectionsPopup::onCopy), 0.5f));
        text("Recommended: build on a copy of the level, then copy to the main level.", 0.f, -124.f, 0.25f, false, true);
        updateMoreLabels();
    }

    void defaultStatus() {
        if (!Mod::get()->getSettingValue<bool>("sections")) {
            m_status->setString("OFF: turn on in Buffer > Advanced");
            m_status->setColor({255, 120, 120});
            return;
        }
        m_status->setColor({255, 255, 255});
        m_status->setString("Add: type a %, last death, or current position");
    }

    void refresh() {
        m_updating = true;
        int n = static_cast<int>(m_ls.secs.size());
        if (m_idx >= n) m_idx = n - 1;
        if (m_idx < 0) m_idx = 0;
        if (m_levelID == 0) {
            m_idxLabel->setString("Open from inside a level");
            m_startInput->setString("");
            m_endInput->setString("");
        } else if (n == 0) {
            m_idxLabel->setString("No sections yet");
            m_startInput->setString("");
            m_endInput->setString("");
        } else {
            std::string s = "Section " + std::to_string(m_idx + 1) + " / " + std::to_string(n);
            m_idxLabel->setString(s.c_str());
            m_startInput->setString(fmtPct(m_ls.secs[m_idx].start));
            m_endInput->setString(fmtPct(m_ls.secs[m_idx].end));
        }
        updateMoreLabels();
        m_updating = false;
    }

public:
    static SectionsPopup* create(int levelID) {
        auto ret = new SectionsPopup();
        if (ret->init(levelID)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onPrev(CCObject*) {
        int n = static_cast<int>(m_ls.secs.size());
        if (n == 0) return;
        m_idx = (m_idx - 1 + n) % n;
        refresh();
        defaultStatus();
        buildPage();
    }

    void onNext(CCObject*) {
        int n = static_cast<int>(m_ls.secs.size());
        if (n == 0) return;
        m_idx = (m_idx + 1) % n;
        refresh();
        defaultStatus();
        buildPage();
    }

    void onPageWindow(CCObject*) { m_page = 0; defaultStatus(); buildPage(); }
    void onPageModes(CCObject*) {
        m_page = 1;
        m_status->setString("-1 = main window, 0 = off");
        buildPage();
    }
    void onPageMore(CCObject*) { m_page = 3; defaultStatus(); buildPage(); }
    void onPageStyles(CCObject*) { m_page = 2; defaultStatus(); buildPage(); }

    // Arrow on a cycling row. Tag = row * 2 + (1 for next, 0 for previous).
    void onCycle(CCObject* sender) {
        auto sec = current();
        if (!sec) return;
        int tag = static_cast<CCNode*>(sender)->getTag();
        int row = tag / 2;
        int dir = (tag % 2) ? 1 : -1;
        std::string& d = sec->data;
        if (row == 0) {
            int cur = indexOf(STAGE_NAMES, STAGE_COUNT, profGet(d, "stage", "4 ticks"), 1);
            d = profSet(d, "stage", STAGE_NAMES[(cur + dir + STAGE_COUNT) % STAGE_COUNT]);
        } else if (row == 5) {
            d = profSet(d, "dual-mode", profGet(d, "dual-mode", "0") == "1" ? "0" : "1");
        } else {
            std::string key = row == 1 ? "wave-mode" : FLY_STYLE_KEYS[row - 2];
            int cur = indexOf(WAVE_NAMES, WAVE_MODE_COUNT, profGet(d, key, row == 1 ? "Grid" : "Min Hold"), 0);
            d = profSet(d, key, WAVE_NAMES[(cur + dir + WAVE_MODE_COUNT) % WAVE_MODE_COUNT]);
        }
        commit();
        buildPage();
    }

    void onNamePrev(CCObject*) {
        if (m_names.empty()) return;
        int n = static_cast<int>(m_names.size());
        m_copySel = (m_copySel - 1 + n) % n;
        refresh();
    }

    void onNameNext(CCObject*) {
        if (m_names.empty()) return;
        int n = static_cast<int>(m_names.size());
        m_copySel = (m_copySel + 1) % n;
        refresh();
    }

    // Copies a saved profile's settings into the current section.
    void onLoadProfile(CCObject*) {
        auto sec = current();
        if (!sec) { m_status->setString("Add a section first."); return; }
        if (m_names.empty()) { m_status->setString("No saved profiles yet."); return; }
        std::string name = m_names[std::clamp(m_copySel, 0, static_cast<int>(m_names.size()) - 1)];
        std::string data = Mod::get()->getSavedValue<std::string>(profileDataKey(name), "");
        if (data.empty()) { m_status->setString("Could not load that profile."); return; }
        for (auto const& item : splitStr(data, '|')) {
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            sec->data = profSet(sec->data, item.substr(0, eq), item.substr(eq + 1));
        }
        commit();
        buildPage();
        m_status->setString(("Loaded '" + name + "'.").c_str());
    }

    // Add asks where the new section should start.
    void onAdd(CCObject*) {
        if (m_levelID == 0) { m_status->setString("Open this from inside a level."); return; }
        auto popup = AddSectionPopup::create([this](float pct) { this->addAt(pct); });
        if (popup) popup->show();
    }

    void addAt(float start) {
        start = roundPct(start);
        for (auto const& s : m_ls.secs) {
            if (std::fabs(s.start - start) < 0.005f) { m_status->setString("A section already starts there."); return; }
        }
        m_ls.secs.push_back(newSection(m_ls, start));
        sortSections(m_ls);
        for (size_t i = 0; i < m_ls.secs.size(); i++) {
            if (std::fabs(m_ls.secs[i].start - start) < 0.005f) m_idx = static_cast<int>(i);
        }
        commit();
        refresh();
        m_status->setString("Section added.");
        buildPage();
    }

    void onDelete(CCObject*) {
        if (!current()) return;
        m_ls.secs.erase(m_ls.secs.begin() + m_idx);
        commit();
        refresh();
        m_status->setString("Section deleted.");
        buildPage();
    }

    void onEmptyMode(CCObject*) {
        m_ls.emptyMode = (m_ls.emptyMode + 1) % EMPTY_MODE_COUNT;
        commit();
        refresh();
    }

    // Copies these sections (and the level's linked profile) to another level.
    void onCopy(CCObject*) {
        int target = std::atoi(m_copyInput->getString().c_str());
        if (target <= 0) { m_status->setString("Type the level ID to copy to."); return; }
        if (target == m_levelID) { m_status->setString("That is this level."); return; }
        if (m_ls.secs.empty()) { m_status->setString("There are no sections to copy."); return; }
        saveLevelSections(target, m_ls);
        std::string linked = levelProfileFor(m_levelID);
        if (!linked.empty()) setLevelProfile(target, linked);
        std::string msg = "Copied " + std::to_string(m_ls.secs.size()) + " sections to level " + std::to_string(target) + ".";
        m_status->setString(msg.c_str());
    }

    void onClose(CCObject* sender) {
        // Sections only take their final order once the window is closed.
        sortSections(m_ls);
        commit();
        Popup::onClose(sender);
    }
};

// Opens the Sections window. If sections are switched off, a message says so.
static void openSectionsWindow(int levelID) {
    if (auto p = SectionsPopup::create(levelID)) p->show();
    if (!Mod::get()->getSettingValue<bool>("sections")) {
        FLAlertLayer::create(
            "Sections are off",
            "Level sections are turned off, so nothing you set here will be used.\n\nTurn on <cy>Level sections</c> in <cy>Buffer > Advanced</c> to use them.",
            "OK"
        )->show();
    }
}

class ProfilesPopup : public geode::Popup {
protected:
    TextInput* m_nameInput = nullptr;
    CCLabelBMFont* m_selLabel = nullptr;
    CCLabelBMFont* m_levelLabel = nullptr;
    CCLabelBMFont* m_status = nullptr;
    std::vector<std::string> m_names;
    int m_sel = -1;
    int m_levelID = 0;

    bool init(int levelID) {
        if (!Popup::init(360.f, 260.f)) return false;
        m_levelID = levelID;
        this->setTitle("Profiles");

        m_names = loadProfileNames();
        if (!m_names.empty()) {
            m_sel = 0;
            std::string linked = levelProfileFor(levelID);
            for (size_t i = 0; i < m_names.size(); i++) {
                if (m_names[i] == linked) m_sel = static_cast<int>(i);
            }
        }

        auto text = [&](char const* s, float x, float y, float scale) {
            auto label = CCLabelBMFont::create(s, "bigFont.fnt");
            label->setScale(scale);
            m_mainLayer->addChildAtPosition(label, Anchor::Center, {x, y});
            return label;
        };
        auto button = [&](char const* s, float x, float y, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(s);
            spr->setScale(0.6f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
        };

        button("Sections", 125.f, 100.f, menu_selector(ProfilesPopup::onSections));
        text("Saved profile", 0.f, 88.f, 0.45f);
        m_selLabel = text("", 0.f, 62.f, 0.6f);

        auto left = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        left->setScale(0.6f);
        auto leftBtn = CCMenuItemSpriteExtra::create(left, this, menu_selector(ProfilesPopup::onPrev));
        m_buttonMenu->addChildAtPosition(leftBtn, Anchor::Center, {-125.f, 62.f});

        auto right = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        right->setScale(0.6f);
        right->setFlipX(true);
        auto rightBtn = CCMenuItemSpriteExtra::create(right, this, menu_selector(ProfilesPopup::onNext));
        m_buttonMenu->addChildAtPosition(rightBtn, Anchor::Center, {125.f, 62.f});

        button("Load", -55.f, 30.f, menu_selector(ProfilesPopup::onLoad));
        button("Delete", 55.f, 30.f, menu_selector(ProfilesPopup::onDelete));

        text("Save current settings as:", 0.f, 2.f, 0.4f);
        m_nameInput = TextInput::create(190.f, "Profile name");
        m_nameInput->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");
        m_nameInput->setMaxCharCount(20);
        m_mainLayer->addChildAtPosition(m_nameInput, Anchor::Center, {-45.f, -22.f});
        button("Save", 120.f, -22.f, menu_selector(ProfilesPopup::onSave));

        m_levelLabel = text("", 0.f, -58.f, 0.4f);
        button("Use for this level", -70.f, -86.f, menu_selector(ProfilesPopup::onLink));
        button("Clear", 85.f, -86.f, menu_selector(ProfilesPopup::onUnlink));

        m_status = text("", 0.f, -115.f, 0.4f);

        this->refresh();
        return true;
    }

    void refresh() {
        if (m_sel >= 0 && m_sel < static_cast<int>(m_names.size())) {
            m_selLabel->setString(m_names[m_sel].c_str());
        } else {
            m_selLabel->setString("(none saved)");
        }
        if (m_levelID == 0) {
            m_levelLabel->setString("Open this from inside a level to link a profile");
        } else {
            std::string n = levelProfileFor(m_levelID);
            std::string s = "This level uses: " + (n.empty() ? std::string("your current settings") : n);
            m_levelLabel->setString(s.c_str());
        }
    }

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    std::string selectedName() const {
        if (m_sel < 0 || m_sel >= static_cast<int>(m_names.size())) return "";
        return m_names[m_sel];
    }

public:
    static ProfilesPopup* create(int levelID) {
        auto ret = new ProfilesPopup();
        if (ret->init(levelID)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onPrev(CCObject*) {
        if (m_names.empty()) return;
        int n = static_cast<int>(m_names.size());
        m_sel = (m_sel - 1 + n) % n;
        refresh();
    }

    void onNext(CCObject*) {
        if (m_names.empty()) return;
        int n = static_cast<int>(m_names.size());
        m_sel = (m_sel + 1) % n;
        refresh();
    }

    void onLoad(CCObject*) {
        std::string name = selectedName();
        if (name.empty()) { setStatus("No profile selected."); return; }
        if (applyProfileByName(name)) setStatus("Loaded '" + name + "'.");
        else setStatus("Could not load that profile.");
    }

    void onDelete(CCObject*) {
        std::string name = selectedName();
        if (name.empty()) { setStatus("No profile selected."); return; }
        Mod::get()->setSavedValue<std::string>(profileDataKey(name), "");
        m_names.erase(m_names.begin() + m_sel);
        saveProfileNames(m_names);
        forgetProfileEverywhere(name);
        if (m_sel >= static_cast<int>(m_names.size())) m_sel = static_cast<int>(m_names.size()) - 1;
        setStatus("Deleted '" + name + "'.");
        refresh();
    }

    void onSave(CCObject*) {
        std::string name = m_nameInput->getString();
        // trim spaces on both ends
        size_t a = name.find_first_not_of(' ');
        size_t b = name.find_last_not_of(' ');
        name = a == std::string::npos ? std::string() : name.substr(a, b - a + 1);
        if (name.empty()) { setStatus("Type a name first."); return; }

        bool exists = std::find(m_names.begin(), m_names.end(), name) != m_names.end();
        if (!exists) {
            m_names.push_back(name);
            saveProfileNames(m_names);
        }
        Mod::get()->setSavedValue<std::string>(profileDataKey(name), serializeProfile());
        for (size_t i = 0; i < m_names.size(); i++) {
            if (m_names[i] == name) m_sel = static_cast<int>(i);
        }
        m_nameInput->setString("");
        setStatus((exists ? "Updated '" : "Saved '") + name + "'.");
        refresh();
    }

    void onLink(CCObject*) {
        if (m_levelID == 0) { setStatus("Open this from inside a level."); return; }
        std::string name = selectedName();
        if (name.empty()) { setStatus("Pick a profile first."); return; }
        setLevelProfile(m_levelID, name);
        setStatus("This level will use '" + name + "'.");
        refresh();
    }

    void onSections(CCObject*) {
        openSectionsWindow(m_levelID);
    }

    void onUnlink(CCObject*) {
        if (m_levelID == 0) { setStatus("Open this from inside a level."); return; }
        setLevelProfile(m_levelID, "");
        setStatus("Level profile cleared.");
        refresh();
    }
};


// ---- main popup: Basic and Advanced tabs --------------------------------
static constexpr int PRESET_COUNT = 5;
static constexpr char const* PRESET_NAMES[PRESET_COUNT] = {"Off", "Strict", "Standard", "Relaxed", "Very relaxed"};
static constexpr char const* PRESET_STAGE[PRESET_COUNT] = {"Off", "2 ticks", "4 ticks", "8 ticks", "16 ticks"};
static constexpr char const* PRESET_INFO[PRESET_COUNT] = {
    "No buffering or timing help.",
    "Remembers a click about half a frame early.",
    "Remembers a click about 1 frame early. A good start.",
    "Remembers a click about 2 frames early.",
    "Remembers a click about 4 frames early. Very forgiving."
};
static constexpr char const* DEFAULT_PROFILE =
    "stage=4 ticks|wave-mode=Grid|tps-value=240|cube-ticks=-1|ball-ticks=-1|robot-ticks=-1|spider-ticks=-1|"
    "ship-ticks=-1|ufo-ticks=-1|swing-ticks=-1|wave-ticks=-1|dual-mode=0|"
    "ship-style=Min Hold|ufo-style=Min Hold|swing-style=Min Hold";

// Which quick setup matches the current settings, or -1 for custom.
static int currentPreset() {
    auto m = Mod::get();
    for (auto key : MODE_KEYS) {
        if (m->getSettingValue<int64_t>(key) != -1) return -1;
    }
    std::string stage = m->getSettingValue<std::string>("stage");
    for (int i = 0; i < PRESET_COUNT; i++) {
        if (stage == PRESET_STAGE[i]) return i;
    }
    return -1;
}

class BufferPopup : public geode::Popup {
protected:
    bool m_advanced = false;
    int m_levelID = 0;
    std::vector<CCNode*> m_tabNodes;
    CCLabelBMFont* m_presetLabel = nullptr;
    CCLabelBMFont* m_presetInfo = nullptr;
    CCLabelBMFont* m_waveLabel = nullptr;
    CCLabelBMFont* m_dualLabel = nullptr;
    CCLabelBMFont* m_secLabel = nullptr;
    CCLabelBMFont* m_note = nullptr;

    bool init(int levelID) {
        if (!Popup::init(360.f, 260.f)) return false;
        m_levelID = levelID;
        this->setTitle("Input Buffer");
        tabButton("Basic", -60.f, menu_selector(BufferPopup::onTabBasic));
        tabButton("Advanced", 60.f, menu_selector(BufferPopup::onTabAdvanced));
        m_note = CCLabelBMFont::create("", "bigFont.fnt");
        m_note->setScale(0.35f);
        m_mainLayer->addChildAtPosition(m_note, Anchor::Center, {0.f, -122.f});
        this->build();
        return true;
    }

    void tabButton(char const* s, float x, SEL_MenuHandler cb) {
        auto spr = ButtonSprite::create(s);
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, 76.f});
    }

    CCLabelBMFont* text(char const* s, float x, float y, float scale) {
        auto label = CCLabelBMFont::create(s, "bigFont.fnt");
        label->setScale(scale);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {x, y});
        m_tabNodes.push_back(label);
        return label;
    }

    void button(char const* s, float x, float y, SEL_MenuHandler cb) {
        auto spr = ButtonSprite::create(s);
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
        m_tabNodes.push_back(btn);
    }

    void arrow(bool flip, float x, float y, SEL_MenuHandler cb) {
        auto spr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        spr->setScale(0.6f);
        spr->setFlipX(flip);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
        m_tabNodes.push_back(btn);
    }

    // Rebuilds the content of the current tab.
    void build() {
        for (auto n : m_tabNodes) n->removeFromParent();
        m_tabNodes.clear();
        m_presetLabel = m_presetInfo = m_waveLabel = m_dualLabel = m_secLabel = nullptr;
        m_note->setString("");

        if (!m_advanced) {
            text("Quick setup", 0.f, 48.f, 0.45f);
            m_presetLabel = text("", 0.f, 26.f, 0.6f);
            arrow(false, -125.f, 26.f, menu_selector(BufferPopup::onPresetPrev));
            arrow(true, 125.f, 26.f, menu_selector(BufferPopup::onPresetNext));
            m_presetInfo = text("", 0.f, 4.f, 0.35f);

            text("Game speed (TPS)", 0.f, -24.f, 0.45f);
            auto input = TextInput::create(100.f, "240");
            input->setFilter("0123456789");
            input->setMaxCharCount(5);
            input->setString(std::to_string(g_tps));
            input->setCallback([](std::string const& s) {
                if (s.empty()) return;
                int v = std::clamp(std::atoi(s.c_str()), TPS_MIN, TPS_MAX);
                Mod::get()->setSettingValue<int64_t>("tps-value", static_cast<int64_t>(v));
            });
            m_mainLayer->addChildAtPosition(input, Anchor::Center, {0.f, -48.f});
            m_tabNodes.push_back(input);

            text("Not sure? Leave it on Standard and set your TPS.", 0.f, -80.f, 0.33f);
        } else {
            m_waveLabel = text("", -50.f, 46.f, 0.38f);
            button("Change", 125.f, 46.f, menu_selector(BufferPopup::onWaveStyle));
            m_dualLabel = text("", -50.f, 20.f, 0.38f);
            button("Change", 125.f, 20.f, menu_selector(BufferPopup::onToggleDual));
            m_secLabel = text("", -50.f, -6.f, 0.38f);
            button("Change", 125.f, -6.f, menu_selector(BufferPopup::onToggleSections));

            button("All settings", -75.f, -38.f, menu_selector(BufferPopup::onAllSettings));
            button("Profiles", 75.f, -38.f, menu_selector(BufferPopup::onProfiles));
            button("Sections", -75.f, -68.f, menu_selector(BufferPopup::onSections));
            button("Reset all", 75.f, -68.f, menu_selector(BufferPopup::onResetAll));
            text("Per-mode windows are in All settings.", 0.f, -96.f, 0.33f);
        }
        this->refresh();
    }

    void refresh() {
        auto m = Mod::get();
        if (m_presetLabel) {
            int p = currentPreset();
            m_presetLabel->setString(p >= 0 ? PRESET_NAMES[p] : "Custom");
            m_presetInfo->setString(p >= 0 ? PRESET_INFO[p] : "You changed the settings by hand.");
        }
        if (m_waveLabel) {
            std::string a = std::string("Wave style: ") + waveModeName();
            m_waveLabel->setString(a.c_str());
            std::string b = std::string("Dual mode (experimental): ") + (g_dualEnabled ? "On" : "Off");
            m_dualLabel->setString(b.c_str());
            std::string c = std::string("Level sections (experimental): ") + (m->getSettingValue<bool>("sections") ? "On" : "Off");
            m_secLabel->setString(c.c_str());
        }
    }

public:
    static BufferPopup* create(int levelID) {
        auto ret = new BufferPopup();
        if (ret->init(levelID)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onTabBasic(CCObject*) { m_advanced = false; build(); }
    void onTabAdvanced(CCObject*) { m_advanced = true; build(); }

    // Picking a quick setup resets the per-mode windows to follow it.
    void setPreset(int p) {
        auto m = Mod::get();
        m->setSettingValue<std::string>("stage", PRESET_STAGE[p]);
        for (auto key : MODE_KEYS) m->setSettingValue<int64_t>(key, -1);
        refresh();
    }

    void onPresetPrev(CCObject*) {
        int p = currentPreset();
        setPreset(p < 0 ? 2 : (p + PRESET_COUNT - 1) % PRESET_COUNT);
    }

    void onPresetNext(CCObject*) {
        int p = currentPreset();
        setPreset(p < 0 ? 2 : (p + 1) % PRESET_COUNT);
    }

    void onWaveStyle(CCObject*) {
        int next = (g_waveMode + 1) % WAVE_MODE_COUNT;
        Mod::get()->setSettingValue<std::string>("wave-mode", WAVE_NAMES[next]);
        refresh();
    }

    void onToggleDual(CCObject*) {
        Mod::get()->setSettingValue<bool>("dual-mode", !g_dualEnabled);
        refresh();
    }

    void onToggleSections(CCObject*) {
        auto m = Mod::get();
        m->setSettingValue<bool>("sections", !m->getSettingValue<bool>("sections"));
        refresh();
    }

    void onAllSettings(CCObject*) { openSettingsPopup(Mod::get(), false); }
    void onProfiles(CCObject*) { if (auto p = ProfilesPopup::create(m_levelID)) p->show(); }
    void onSections(CCObject*) { openSectionsWindow(m_levelID); }

    void onResetAll(CCObject*) {
        applyProfileData(DEFAULT_PROFILE);
        m_note->setString("Settings reset to defaults.");
        this->build();
        m_note->setString("Settings reset to defaults.");
    }
};

$on_mod(Loaded) {
    loadSettings();
    restoreAfterLevel(); // undo a level profile left over from a crash
    listenForSettingChanges<std::string>("stage", [](std::string) { loadSettings(); });
    listenForSettingChanges<int64_t>("tps-value", [](int64_t) { loadSettings(); });
    listenForSettingChanges<std::string>("wave-mode", [](std::string) { loadSettings(); });
    for (auto key : MODE_KEYS) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { loadSettings(); });
    }
    listenForSettingChanges<bool>("debug", [](bool) { loadSettings(); });
    listenForSettingChanges<bool>("dual-mode", [](bool) { loadSettings(); });
    listenForSettingChanges<bool>("enabled", [](bool) { loadSettings(); });
    listenForSettingChanges<std::string>("inject-hold", [](std::string) { loadSettings(); });
    listenForSettingChanges<bool>("rering", [](bool) { loadSettings(); });
    listenForSettingChanges<int64_t>("coyote-ticks", [](int64_t) { loadSettings(); });
    listenForSettingChanges<std::string>("legit-mode", [](std::string) { loadSettings(); });
    listenForSettingChanges<bool>("feedback", [](bool) { loadSettings(); });
    listenForSettingChanges<bool>("smart-jump", [](bool) { loadSettings(); });
    listenForSettingChanges<int64_t>("robot-minhold", [](int64_t) { loadSettings(); });
    listenForSettingChanges<bool>("smart-wave", [](bool) { loadSettings(); });
    listenForSettingChanges<int64_t>("smart-wave-window", [](int64_t) { loadSettings(); });
    listenForSettingChanges<int64_t>("smart-window", [](int64_t) { loadSettings(); });
    listenForSettingChanges<int64_t>("smart-margin", [](int64_t) { loadSettings(); });
    listenForSettingChanges<std::string>("smart-pick", [](std::string) { loadSettings(); });
    listenForSettingChanges<bool>("hud", [](bool) {
        loadSettings();
        if (auto pl = PlayLayer::get()) buildHud(pl);
    });
    listenForSettingChanges<std::string>("hud-pos", [](std::string) {
        loadSettings();
        if (auto pl = PlayLayer::get()) buildHud(pl);
    });
    for (auto key : FLY_STYLE_KEYS) {
        listenForSettingChanges<std::string>(key, [](std::string) { loadSettings(); });
    }

    log::info("[InputBuffer] BufferMod loaded (v27: smart wave). Main: {}, TPS: {}, wave: {}, dual: {}",
              STAGE_NAMES[g_stage], g_tps, waveModeName(), g_dualEnabled ? "ON" : "OFF");
}

$on_game(Loaded) {
    for (int i = 0; i < KEYBIND_COUNT; i++) {
        listenForKeybindSettingPresses(KEYBIND_SETTINGS[i], [i](Keybind const&, bool down, bool repeat, double) {
            hotkeyPressed(i, down, repeat);
        });
    }
}

// ---- Smart Jump data -----------------------------------------------------
struct Hz { float minx, maxx, miny, maxy; bool circle; float cx, cy, r; };
static std::vector<Hz> g_haz;      // hazards of the current level, sorted by minx
static bool g_hazBuilt = false;
static std::map<std::string, std::vector<float>> g_tmpl; // jump shapes by icon size / gravity / TPS
static std::map<std::string, int> g_tmplHold;            // robot: how long the learned jump was held (steps)

static std::string smartKey(PlayerObject* p) {
    std::string k = std::string(modeIndex(p) == 2 ? "sjr-" : "sj-") + std::to_string(static_cast<int>(std::lround(p->getScale() * 100.f)));
    k += p->m_isUpsideDown ? "-u-" : "-d-";
    k += std::to_string(g_tps);
    return k;
}

static std::vector<float> const* smartTemplate(std::string const& key) {
    auto it = g_tmpl.find(key);
    if (it != g_tmpl.end()) return &it->second;
    std::string raw = Mod::get()->getSavedValue<std::string>("tmpl-" + key, "");
    if (raw.empty()) return nullptr;
    std::vector<float> v;
    for (auto const& part : splitStr(raw, ',')) {
        if (!part.empty()) v.push_back(static_cast<float>(std::atof(part.c_str())));
    }
    if (v.size() < 8) return nullptr;
    g_tmpl[key] = std::move(v);
    return &g_tmpl[key];
}

static void smartStore(std::string const& key, std::vector<float> v, int hold = 0) {
    std::string raw;
    char buf[32];
    for (float f : v) {
        std::snprintf(buf, sizeof buf, "%.4f,", f);
        raw += buf;
    }
    Mod::get()->setSavedValue<std::string>("tmpl-" + key, raw);
    g_tmpl[key] = std::move(v);
    if (hold > 0) {
        g_tmplHold[key] = hold;
        Mod::get()->setSavedValue<std::string>("hold-" + key, std::to_string(hold));
    }
}

// How long the learned robot jump was held, in steps (0 if unknown).
static int smartHold(std::string const& key) {
    auto it = g_tmplHold.find(key);
    if (it != g_tmplHold.end()) return it->second;
    int h = std::atoi(Mod::get()->getSavedValue<std::string>("hold-" + key, "").c_str());
    if (h > 0) g_tmplHold[key] = h;
    return h;
}

// Tells the player why a jump could not be learned (only until it is calibrated).
static void smartNote(PlayerObject* p, char const* msg) {
    if (smartTemplate(smartKey(p))) return;
    Notification::create(msg, NotificationIcon::Warning, 2.0f)->show();
}

// ---- tabbed settings panel -----------------------------------------------
// kinds: c = choice, i = number (-1 shows "Main", 0 shows "Off"), n = plain number,
//        b = on/off, t = TPS presets, p = quick preset
struct PRow { char const* label; char const* key; char kind; int lo; int hi; char const* const* names; int n; };

static constexpr PRow ROWS_BUFFER[] = {
    {"Quick preset", nullptr, 'p', 0, 0, nullptr, 0},
    {"Main window", "stage", 'c', 0, 0, STAGE_NAMES, STAGE_COUNT},
    {"Game speed (TPS)", "tps-value", 't', 0, 0, nullptr, 0},
    {"Cube window", "cube-ticks", 'i', -1, 64, nullptr, 0},
    {"Ball window", "ball-ticks", 'i', -1, 64, nullptr, 0},
    {"Robot window", "robot-ticks", 'i', -1, 64, nullptr, 0},
    {"Spider window", "spider-ticks", 'i', -1, 64, nullptr, 0},
    {"Robot min hold", "robot-minhold", 'i', 0, 32, nullptr, 0},
};
static constexpr PRow ROWS_FLY[] = {
    {"Ship window", "ship-ticks", 'i', -1, 64, nullptr, 0},
    {"UFO window", "ufo-ticks", 'i', -1, 64, nullptr, 0},
    {"Swing window", "swing-ticks", 'i', -1, 64, nullptr, 0},
    {"Wave window", "wave-ticks", 'i', -1, 64, nullptr, 0},
    {"Wave mode", "wave-mode", 'c', 0, 0, WAVE_NAMES, WAVE_MODE_COUNT},
    {"Ship style", "ship-style", 'c', 0, 0, WAVE_NAMES, WAVE_MODE_COUNT},
    {"UFO style", "ufo-style", 'c', 0, 0, WAVE_NAMES, WAVE_MODE_COUNT},
    {"Swing style", "swing-style", 'c', 0, 0, WAVE_NAMES, WAVE_MODE_COUNT},
};
static constexpr PRow ROWS_SMART[] = {
    {"Smart Jump", "smart-jump", 'b', 0, 0, nullptr, 0},
    {"Look-ahead (ticks)", "smart-window", 'n', 1, 40, nullptr, 0},
    {"Safety margin", "smart-margin", 'n', 0, 6, nullptr, 0},
    {"Pick timing", "smart-pick", 'c', 0, 0, SMART_PICK_NAMES, 2},
    {"Smart Wave", "smart-wave", 'b', 0, 0, nullptr, 0},
    {"Wave look-ahead", "smart-wave-window", 'n', 1, 16, nullptr, 0},
};
static constexpr PRow ROWS_EXTRAS[] = {
    {"Buffer on/off", "enabled", 'b', 0, 0, nullptr, 0},
    {"Injected hold", "inject-hold", 'c', 0, 0, INJECT_NAMES, 2},
    {"Ring re-press", "rering", 'b', 0, 0, nullptr, 0},
    {"Late click (ticks)", "coyote-ticks", 'i', 0, 16, nullptr, 0},
    {"Legit mode", "legit-mode", 'c', 0, 0, LEGIT_NAMES, 3},
    {"Click feedback", "feedback", 'b', 0, 0, nullptr, 0},
    {"On-screen buttons", "hud", 'b', 0, 0, nullptr, 0},
    {"Buttons corner", "hud-pos", 'c', 0, 0, HUD_POS_NAMES, 4},
};
static constexpr PRow ROWS_TOOLS[] = {
    {"Dual mode (exp.)", "dual-mode", 'b', 0, 0, nullptr, 0},
    {"Level sections (exp.)", "sections", 'b', 0, 0, nullptr, 0},
    {"Debug logging", "debug", 'b', 0, 0, nullptr, 0},
};
static constexpr char const* EXTRA_DEFAULTS =
    "inject-hold=Short|rering=0|coyote-ticks=0|smart-jump=0|smart-window=12|smart-margin=0|smart-pick=Middle|robot-minhold=0|smart-wave=0|smart-wave-window=4";

// Shows how early your buffered clicks were and suggests a window for each mode.
class StatsPopup : public geode::Popup {
protected:
    std::vector<CCNode*> m_nodes;

    bool init() {
        if (!Popup::init(360.f, 240.f)) return false;
        this->setTitle("Click stats");
        auto apply = ButtonSprite::create("Apply");
        apply->setScale(0.6f);
        m_buttonMenu->addChildAtPosition(
            CCMenuItemSpriteExtra::create(apply, this, menu_selector(StatsPopup::onApply)),
            Anchor::Center, {-60.f, -80.f});
        auto clear = ButtonSprite::create("Clear");
        clear->setScale(0.6f);
        m_buttonMenu->addChildAtPosition(
            CCMenuItemSpriteExtra::create(clear, this, menu_selector(StatsPopup::onClear)),
            Anchor::Center, {60.f, -80.f});
        this->build();
        return true;
    }

    void line(std::string const& s, float y, float scale) {
        auto label = CCLabelBMFont::create(s.c_str(), "bigFont.fnt");
        label->setScale(scale);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {0.f, y});
        m_nodes.push_back(label);
    }

    void build() {
        for (auto n : m_nodes) n->removeFromParent();
        m_nodes.clear();
        static constexpr char const* names[4] = {"Cube", "Ball", "Robot", "Spider"};
        line("Play with clicks a bit early, then press Apply.", 78.f, 0.3f);
        float y = 48.f;
        for (int i = 0; i < 4; i++) {
            float med = 0.f, p90 = 0.f;
            int sug = 0;
            char buf[160];
            if (statsFor(i, med, p90, sug)) {
                std::snprintf(buf, sizeof buf, "%s  n=%d  med %.1f  p90 %.1f  miss %d  -> %dt",
                              names[i], static_cast<int>(g_samples[i].size()), med, p90, g_expired[i], sug);
            } else {
                std::snprintf(buf, sizeof buf, "%s  no buffered clicks yet  miss %d", names[i], g_expired[i]);
            }
            line(buf, y, 0.32f);
            y -= 26.f;
        }
        line("Needs 5+ clicks per mode to apply. Times are in ticks.", -56.f, 0.28f);
    }

public:
    static StatsPopup* create() {
        auto ret = new StatsPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onApply(CCObject*) {
        int done = 0;
        for (int i = 0; i < 4; i++) {
            float med = 0.f, p90 = 0.f;
            int sug = 0;
            if (g_samples[i].size() >= 5 && statsFor(i, med, p90, sug)) {
                Mod::get()->setSettingValue<int64_t>(MODE_KEYS[i], static_cast<int64_t>(sug));
                done++;
            }
        }
        std::string msg = done ? "Applied windows for " + std::to_string(done) + " mode(s)" : "Not enough clicks yet";
        Notification::create(msg.c_str(), done ? NotificationIcon::Success : NotificationIcon::Warning, 1.5f)->show();
    }

    void onClear(CCObject*) {
        clearStats();
        this->build();
    }
};

// One window for every setting, split into tabs.
class PanelPopup : public geode::Popup {
protected:
    static constexpr char const* TAB_NAMES[5] = {"Buffer", "Fly", "Smart", "Extras", "Tools"};
    int m_tab = 0;
    int m_levelID = 0;
    std::vector<CCNode*> m_nodes;
    std::vector<CCLabelBMFont*> m_values;
    PRow const* m_rows = nullptr;
    int m_rowCount = 0;
    CCLabelBMFont* m_note = nullptr;

    bool init(int levelID) {
        if (!Popup::init(380.f, 290.f)) return false;
        m_levelID = levelID;
        this->setTitle("Input Buffer");
        for (int i = 0; i < 5; i++) {
            auto spr = ButtonSprite::create(TAB_NAMES[i]);
            spr->setScale(0.5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PanelPopup::onTab));
            btn->setTag(i);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {-150.f + i * 75.f, 100.f});
        }
        m_note = CCLabelBMFont::create("", "bigFont.fnt");
        m_note->setScale(0.3f);
        m_mainLayer->addChildAtPosition(m_note, Anchor::Center, {0.f, -122.f});
        this->build();
        return true;
    }

    void addButton(char const* s, float x, float y, SEL_MenuHandler cb) {
        auto spr = ButtonSprite::create(s);
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {x, y});
        m_nodes.push_back(btn);
    }

    void build() {
        for (auto n : m_nodes) n->removeFromParent();
        m_nodes.clear();
        m_values.clear();
        m_note->setString("");
        m_rows = nullptr;
        m_rowCount = 0;
        switch (m_tab) {
            case 0: m_rows = ROWS_BUFFER; m_rowCount = static_cast<int>(std::size(ROWS_BUFFER)); break;
            case 1: m_rows = ROWS_FLY; m_rowCount = static_cast<int>(std::size(ROWS_FLY)); break;
            case 2: m_rows = ROWS_SMART; m_rowCount = static_cast<int>(std::size(ROWS_SMART)); break;
            case 3: m_rows = ROWS_EXTRAS; m_rowCount = static_cast<int>(std::size(ROWS_EXTRAS)); break;
            default: m_rows = ROWS_TOOLS; m_rowCount = static_cast<int>(std::size(ROWS_TOOLS)); break;
        }

        float y = 66.f;
        for (int i = 0; i < m_rowCount; i++, y -= 22.f) {
            auto label = CCLabelBMFont::create(m_rows[i].label, "bigFont.fnt");
            label->setScale(0.36f);
            label->setAnchorPoint({0.f, 0.5f});
            m_mainLayer->addChildAtPosition(label, Anchor::Center, {-172.f, y});
            m_nodes.push_back(label);

            auto value = CCLabelBMFont::create("", "bigFont.fnt");
            value->setScale(0.4f);
            m_mainLayer->addChildAtPosition(value, Anchor::Center, {95.f, y});
            m_nodes.push_back(value);
            m_values.push_back(value);

            for (int side = 0; side < 2; side++) {
                auto arrow = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
                arrow->setScale(0.45f);
                arrow->setFlipX(side == 1);
                auto btn = CCMenuItemSpriteExtra::create(arrow, this, menu_selector(PanelPopup::onStep));
                btn->setTag(i * 2 + side);
                m_buttonMenu->addChildAtPosition(btn, Anchor::Center, {side == 0 ? 35.f : 155.f, y});
                m_nodes.push_back(btn);
            }
        }

        if (m_tab == 2) {
            std::string st = "Learns jumps from your own jumps (cube, robot). Wave needs no setup.";
            if (auto pl = PlayLayer::get(); pl && pl->m_player1) {
                st = smartTemplate(smartKey(pl->m_player1))
                    ? "Calibrated for this icon size and gravity."
                    : "Not calibrated yet: jump once on flat ground.";
            }
            m_note->setString(st.c_str());
        } else if (m_tab == 4) {
            addButton("Profiles", -75.f, -10.f, menu_selector(PanelPopup::onProfiles));
            addButton("Sections", 75.f, -10.f, menu_selector(PanelPopup::onSections));
            addButton("Stats", -75.f, -38.f, menu_selector(PanelPopup::onStats));
            addButton("Reset all", 75.f, -38.f, menu_selector(PanelPopup::onReset));
            addButton("Geode settings", 0.f, -66.f, menu_selector(PanelPopup::onGeode));
        }
        this->refreshValues();
    }

    std::string valueText(PRow const& r) {
        auto m = Mod::get();
        switch (r.kind) {
            case 'c': return m->getSettingValue<std::string>(r.key);
            case 'b': return m->getSettingValue<bool>(r.key) ? "On" : "Off";
            case 't': return std::to_string(g_tps);
            case 'p': {
                int p = currentPreset();
                return p >= 0 ? PRESET_NAMES[p] : "Custom";
            }
            case 'n': return std::to_string(m->getSettingValue<int64_t>(r.key));
            default: {
                int64_t v = m->getSettingValue<int64_t>(r.key);
                if (v == -1) return "Main";
                if (v == 0) return "Off";
                return std::to_string(v);
            }
        }
    }

    void refreshValues() {
        for (int i = 0; i < m_rowCount && i < static_cast<int>(m_values.size()); i++) {
            m_values[i]->setString(valueText(m_rows[i]).c_str());
        }
    }

public:
    static PanelPopup* create(int levelID) {
        auto ret = new PanelPopup();
        if (ret->init(levelID)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onTab(CCObject* sender) {
        m_tab = static_cast<CCNode*>(sender)->getTag();
        this->build();
    }

    void onStep(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        int i = tag / 2;
        int dir = (tag % 2) ? 1 : -1;
        if (i < 0 || i >= m_rowCount) return;
        auto const& r = m_rows[i];
        auto m = Mod::get();
        switch (r.kind) {
            case 'c': {
                int cur = indexOf(r.names, r.n, m->getSettingValue<std::string>(r.key), 0);
                cur = (cur + dir + r.n) % r.n;
                m->setSettingValue<std::string>(r.key, r.names[cur]);
                break;
            }
            case 'b':
                m->setSettingValue<bool>(r.key, !m->getSettingValue<bool>(r.key));
                break;
            case 't':
                m->setSettingValue<int64_t>(r.key, static_cast<int64_t>(stepTps(g_tps, dir > 0)));
                break;
            case 'p': {
                int p = currentPreset();
                int next = p < 0 ? 2 : (p + dir + PRESET_COUNT) % PRESET_COUNT;
                m->setSettingValue<std::string>("stage", PRESET_STAGE[next]);
                for (auto k : MODE_KEYS) m->setSettingValue<int64_t>(k, -1);
                break;
            }
            default: {
                int64_t v = m->getSettingValue<int64_t>(r.key) + dir;
                v = std::clamp<int64_t>(v, r.lo, r.hi);
                m->setSettingValue<int64_t>(r.key, v);
                break;
            }
        }
        loadSettings();
        if (r.key && (std::string(r.key) == "hud" || std::string(r.key) == "hud-pos")) {
            if (auto pl = PlayLayer::get()) buildHud(pl);
        }
        this->refreshValues();
    }

    void onProfiles(CCObject*) { if (auto p = ProfilesPopup::create(m_levelID)) p->show(); }
    void onSections(CCObject*) { openSectionsWindow(m_levelID); }
    void onStats(CCObject*) { if (auto p = StatsPopup::create()) p->show(); }
    void onGeode(CCObject*) { openSettingsPopup(Mod::get(), false); }

    void onReset(CCObject*) {
        applyProfileData(DEFAULT_PROFILE);
        applyProfileData(EXTRA_DEFAULTS);
        this->build();
        m_note->setString("Settings reset to defaults.");
    }
};

class $modify(BufferPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();

        auto spr = ButtonSprite::create("Buffer");
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufferPause::onBufferSettings));
        menu->addChild(btn);

        auto spr2 = ButtonSprite::create("Profiles");
        spr2->setScale(0.7f);
        auto btn2 = CCMenuItemSpriteExtra::create(spr2, this, menu_selector(BufferPause::onBufferProfiles));
        btn2->setPosition({0.f, 30.f});
        menu->addChild(btn2);

        auto spr3 = ButtonSprite::create("Stats");
        spr3->setScale(0.7f);
        auto btn3 = CCMenuItemSpriteExtra::create(spr3, this, menu_selector(BufferPause::onBufferStats));
        btn3->setPosition({0.f, 60.f});
        menu->addChild(btn3);

        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 60.f, 30.f});
        this->addChild(menu);
    }

    void onBufferSettings(CCObject*) {
        if (auto popup = PanelPopup::create(currentLevelID())) popup->show();
    }

    void onBufferProfiles(CCObject*) {
        if (auto popup = ProfilesPopup::create(currentLevelID())) popup->show();
    }

    void onBufferStats(CCObject*) {
        if (auto popup = StatsPopup::create()) popup->show();
    }
};

// Raises the reset flag on death, restart and checkpoint loads. The buffer
// layer clears its state on the next step.
class $modify(BufferReset, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        g_resetEpoch++;
    }

    void resetLevelFromStart() {
        PlayLayer::resetLevelFromStart();
        g_resetEpoch++;
    }

    void loadFromCheckpoint(CheckpointObject* object) {
        PlayLayer::loadFromCheckpoint(object);
        g_resetEpoch++;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        g_resetEpoch++;
        g_lastDeathPct = this->getCurrentPercent();
    }

    // Switches settings when the player moves into a different section.
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!g_secActive) return;
        if (m_level && m_level->isPlatformer()) return;
        std::string tok = sectionToken(this->getCurrentPercent());
        if (tok == g_secToken) return;
        g_secToken = tok;
        applyToken(tok, levelKeyFor(m_level));
        g_resetEpoch++; // start the new section with clean input state
        DBG("[InputBuffer] Section -> {}", tok);
    }

    // Leaving the level puts the player's own settings back.
    void onExit() {
        PlayLayer::onExit();
        g_hudToggle = g_hudWin = g_hudWave = nullptr;
        g_secActive = false;
        g_secToken.clear();
        restoreAfterLevel();
    }

    // Applies the profile linked to this level, if there is one.
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        clearStats();
        g_hazBuilt = false;
        buildHud(this);
        if (!m_level) return;
        int id = levelKeyFor(m_level);
        if (id == 0) return;
        std::string name = levelProfileFor(id);
        g_secActive = false;
        g_secToken.clear();
        LevelSections ls = loadLevelSections(id);
        bool useSections = Mod::get()->getSettingValue<bool>("sections") && !ls.secs.empty();
        if (name.empty() && !useSections) return;
        // Remember the player's own settings so they can be restored on exit.
        ensureSnapshot();
        if (!name.empty() && applyProfileByName(name)) {
            log::info("[InputBuffer] Applied profile '{}' for level {}", name, id);
            std::string msg = "Input Buffer profile: " + name;
            Notification::create(msg.c_str(), NotificationIcon::Success, 1.5f)->show();
        }
        if (useSections) {
            sortSections(ls);
            g_sec = ls;
            g_secActive = true;
            log::info("[InputBuffer] {} sections active for level {}", ls.secs.size(), id);
        }
    }
};

struct PState {
    bool held = false;
    bool pending = false;
    int pendingStart = 0;
    int lastRingCount = 0;
    bool releaseNext = false;

    int pressStep = -1;
    bool delayedRelease = false;
    int releaseAt = 0;

    std::vector<std::pair<bool, int>> waveQueue;
    int lastReleaseStep = -1000000;

    // v1.2 extras
    int realPressStep = -1;      // step of the player's last real press
    int lastHoldSteps = 1;       // how long the player's last real hold lasted
    int releaseNextAt = 0;       // when the injected click lets go
    int lastGroundStep = -1000000;
    bool coyoteFire = false;     // a late click is waiting to be injected

    // Smart Jump
    bool haveLast = false;
    int lastStep = -1;
    float lastX = 0.f, lastY = 0.f;
    bool wasGround = false;
    bool jrec = false;           // recording a real jump
    float jy0 = 0.f, jsgn = 1.f, jd2 = 0.f;
    std::vector<float> jT;
    bool smartFire = false;      // a delayed click is on its way
    int smartFireAt = 0;
    int sjFireStep = -1;         // when we last fired, to measure liftoff latency
    int smartHold = 0;           // robot: how long the fired click is held (steps)

    // Smart Wave
    bool wHave = false;
    int wLastStep = -1;
    float wLastX = 0.f, wLastY = 0.f;
    bool swQueued = false;       // the queue holds retimed inputs of ours

    // Robot minimum hold: a quick release is held back so the jump is not cut short.
    bool robotRelease = false;
    int robotReleaseAt = 0;
    bool pressOnGround = false;
};

class $modify(BufferLayer, GJBaseGameLayer) {
    struct Fields {
        int step = 0;
        int epoch = 0;
        int graceUntil = 0;
        PState ps[2];
    };

    // True in a one-player level with a dual portal: a single click is sent
    // as player 1 and the game applies it to both icons.
    bool sharedDualInput() {
        return m_gameState.m_isDualMode && m_levelSettings && !m_levelSettings->m_twoPlayerMode;
    }

    // Press or release for one icon. In a dual section each icon has its own
    // state and decides for itself, so nothing is mirrored here.
    void sendInput(PlayerObject* p, bool down) {
        if (down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    // When experimental dual support is off, nothing may stay held back from
    // an earlier section: deliver what is queued and drop pending rescues.
    void bypassCleanup() {
        auto f = m_fields.operator->();
        PlayerObject* players[2] = {m_player1, m_player2};
        for (int i = 0; i < 2; i++) {
            auto p = players[i];
            auto& s = f->ps[i];
            if (!p) { s = PState(); continue; }
            if (!s.waveQueue.empty()) flushWaveQueue(p, s, i);
            if (s.delayedRelease) {
                s.delayedRelease = false;
                s.held = false;
                sendInput(p, false);
            }
            if (s.releaseNext) {
                s.releaseNext = false;
                if (!s.held) sendInput(p, false);
            }
            if (s.robotRelease) {
                s.robotRelease = false;
                sendInput(p, false);
            }
            s.pending = false;
        }
    }

    // Clears our state after a death / restart / checkpoint load.
    void syncState() {
        auto f = m_fields.operator->();
        if (f->epoch == g_resetEpoch) return;
        f->epoch = g_resetEpoch;

        bool inconsistent = false;
        for (auto& s : f->ps) {
            if (s.delayedRelease || s.releaseNext || s.robotRelease || !s.waveQueue.empty()) {
                inconsistent = true;
            }
            s = PState();
        }

        // Wave Grid / Min Gap are skipped briefly after a spawn so the first
        // clicks go straight through.
        f->graceUntil = f->step + spawnGraceSteps();

        // We may have held back a release the game never saw. Releasing a
        // button that is not held is harmless, so make sure nothing is stuck.
        if (inconsistent) {
            if (m_player1) m_player1->releaseButton(PlayerButton::Jump);
            if (m_player2) m_player2->releaseButton(PlayerButton::Jump);
        }
        DBG("[InputBuffer] state reset (death/restart), spawn grace until step {}{}",
            f->graceUntil, inconsistent ? ", released stuck button" : "");
    }

    void flushWaveQueue(PlayerObject* p, PState& s, int idx) {
        for (auto const& ev : s.waveQueue) {
            sendInput(p, ev.first);
        }
        s.waveQueue.clear();
    }

    // How long an injected click is held: one step, or the length of the
    // player's own last hold (so robot jump height and held orbs match).
    int injectHoldSteps(PState& s) {
        if (!g_injectMirror) return 1;
        int cap = std::max(1, static_cast<int>(std::lround(INJECT_CAP_TICKS * stepsPerTick())));
        return std::clamp(s.lastHoldSteps, 1, cap);
    }

    // Watches the cube to measure its speed and to learn the shape of a real
    // jump (height per step) from the player's own jumps.
    void smartObserve(PlayerObject* p, PState& s, int step) {
        int mi = modeIndex(p);
        if (!g_smart || (mi != 0 && mi != 2) || sharedDualInput()) {
            s.haveLast = false;
            s.jrec = false;
            return;
        }
        bool robot = mi == 2; // robot height depends on the hold, so its jump shape is not a plain arc
        auto pos = p->getPosition();
        float x = pos.x, y = pos.y;
        bool ground = p->m_isOnGround;
        float sgn = p->m_isUpsideDown ? -1.f : 1.f;
        // A click is queued with a timestamp and can be applied a few steps
        // after it arrived, so the jump may show up several steps later.
        int pressWin = std::max(12, static_cast<int>(std::lround(16 * stepsPerTick())));

        if (s.haveLast && step == s.lastStep + 1) {
            float dx = x - s.lastX;
            if (dx > 0.01f) g_vx = dx;
        }

        if (s.jrec) {
            bool bad = sgn != s.jsgn || ringsTouching(p) > 0 || s.jT.size() > 600;
            if (bad) {
                s.jrec = false;
                smartNote(p, "Smart Jump: jump touched a ring or gravity changed. Try again.");
            } else if (ground) {
                if (s.jT.size() >= 8 && std::fabs((y - s.jy0) * sgn) < 1.5f) {
                    bool had = smartTemplate(smartKey(p)) != nullptr;
                    int hold = 0;
                    if (robot) hold = s.held ? std::max(1, step - s.realPressStep) : s.lastHoldSteps;
                    smartStore(smartKey(p), s.jT, hold);
                    log::info("[InputBuffer] smart jump: learned a jump of {} steps ({})", s.jT.size(), smartKey(p));
                    if (!had) Notification::create("Smart Jump: learned your jump", NotificationIcon::Success, 2.0f)->show();
                } else {
                    smartNote(p, "Smart Jump: jump was too short or landed at another height. Try on flat ground.");
                }
                s.jrec = false;
            } else {
                float dy = (y - s.jy0) * sgn;
                size_t n = s.jT.size();
                if (!robot && n >= 3) {
                    float d2 = dy - 2.f * s.jT[n - 1] + s.jT[n - 2];
                    if (n == 3) s.jd2 = d2;
                    else if (std::fabs(d2 - s.jd2) > std::max(0.0005f, 0.5f * std::fabs(s.jd2))) {
                        s.jrec = false;
                        smartNote(p, "Smart Jump: jump shape was not smooth. Try again on flat ground.");
                    }
                }
                if (s.jrec) s.jT.push_back(dy);
            }
        } else if (s.haveLast && s.wasGround && !ground && step == s.lastStep + 1 &&
                   (y - s.lastY) * sgn > (robot ? 0.05f : 0.2f) &&
                   (step - s.realPressStep <= pressWin || s.sjFireStep >= 0)) {
            s.jrec = true;
            s.jy0 = s.lastY;
            s.jsgn = sgn;
            s.jd2 = 0.f;
            s.jT.clear();
            s.jT.push_back((y - s.lastY) * sgn);
            if (s.sjFireStep >= 0) {
                g_sjLat = std::clamp(step - 1 - s.sjFireStep, 0, 3);
                s.sjFireStep = -1;
            }
        } else if (!s.jrec && s.haveLast && s.wasGround && !ground && step == s.lastStep + 1 &&
                   (y - s.lastY) * sgn > 0.2f) {
            smartNote(p, "Smart Jump: that jump did not come from your click (pad or orb?).");
        }

        s.wasGround = ground;
        s.lastX = x;
        s.lastY = y;
        s.lastStep = step;
        s.haveLast = true;
    }

    // Collects the deadly objects of the level once.
    void ensureHazards() {
        if (g_hazBuilt) return;
        g_hazBuilt = true;
        g_haz.clear();
        if (!m_objects) return;
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj || obj->m_objectType != GameObjectType::Hazard || obj->m_isNoTouch) continue;
            Hz h{};
            auto c = obj->getPosition();
            if (obj->m_objectRadius > 0.f) {
                h.circle = true;
                h.cx = c.x;
                h.cy = c.y;
                h.r = obj->m_objectRadius;
                h.minx = c.x - h.r;
                h.maxx = c.x + h.r;
                h.miny = c.y - h.r;
                h.maxy = c.y + h.r;
            } else {
                auto r = obj->getObjectRect();
                if (r.size.width < 0.5f || r.size.height < 0.5f) r = CCRect(c.x - 6.f, c.y - 6.f, 12.f, 12.f);
                h.minx = r.getMinX();
                h.maxx = r.getMaxX();
                h.miny = r.getMinY();
                h.maxy = r.getMaxY();
            }
            g_haz.push_back(h);
        }
        std::sort(g_haz.begin(), g_haz.end(), [](Hz const& a, Hz const& b) { return a.minx < b.minx; });
        log::info("[InputBuffer] smart jump: cached {} hazards", g_haz.size());
    }

    // Returns how many steps to wait before firing the click, or -1 to let the
    // click go through untouched.
    int smartDecide(PlayerObject* p, PState& s, int step, int& holdOut) {
        holdOut = 0;
        int mi = modeIndex(p);
        if (!g_smart || (mi != 0 && mi != 2) || !p->m_isOnGround || sharedDualInput()) return -1;
        if (g_vx <= 0.05f) return -1;
        std::string key = smartKey(p);
        auto tm = smartTemplate(key);
        if (!tm) {
            DBG("[InputBuffer] smart jump: not calibrated for {} (jump once first)", key);
            return -1;
        }
        if (mi == 2) {
            // Robot: the fired click is held as long as the learned jump was held.
            holdOut = smartHold(key);
            if (holdOut <= 0) return -1;
        }
        ensureHazards();
        if (g_haz.empty()) return -1;

        auto pos = p->getPosition();
        float x0 = pos.x, y0 = pos.y;
        CCRect pr = p->getObjectRect();
        float pw = pr.size.width, ph = pr.size.height;
        float offx = pr.getMidX() - pos.x, offy = pr.getMidY() - pos.y;
        if (pw < 2.f || ph < 2.f) {
            pw = ph = 30.f * p->getScale();
            offx = offy = 0.f;
        }
        float sgn = p->m_isUpsideDown ? -1.f : 1.f;
        int N = static_cast<int>(tm->size());
        int W = std::max(1, static_cast<int>(std::lround(g_smartWindow * stepsPerTick())));
        int lat = g_sjLat;
        float margin = static_cast<float>(g_smartMargin);
        float vx = g_vx;
        int total = 1 + W + lat + N + 2;

        float xmax = x0 + vx * total + pw + 40.f;
        std::vector<Hz const*> nearby;
        auto it = std::lower_bound(g_haz.begin(), g_haz.end(), x0 - 300.f,
                                   [](Hz const& h, float v) { return h.minx < v; });
        for (; it != g_haz.end() && it->minx <= xmax; ++it) nearby.push_back(&*it);
        if (nearby.empty()) return -1;

        // Would starting the jump d steps from now survive?
        auto survives = [&](int d) {
            int O = 1 + d + lat;            // step the icon lifts off
            int last = O + N + 1;
            for (int m = 1; m <= last; m++) {
                float cx = x0 + vx * m + offx;
                float dy = 0.f;
                if (m >= O) {
                    int k = m - O + 1;
                    if (k <= N) dy = (*tm)[k - 1];
                }
                float cy = y0 + sgn * dy + offy;
                float l = cx - pw * 0.5f, r = cx + pw * 0.5f;
                float b = cy - ph * 0.5f, t = cy + ph * 0.5f;
                for (auto h : nearby) {
                    if (r < h->minx - margin || l > h->maxx + margin) continue;
                    if (h->circle) {
                        float qx = std::clamp(h->cx, l, r), qy = std::clamp(h->cy, b, t);
                        float ddx = qx - h->cx, ddy = qy - h->cy;
                        float rr = h->r + margin;
                        if (ddx * ddx + ddy * ddy < rr * rr) return false;
                    } else if (b < h->maxy + margin && t > h->miny - margin) {
                        return false;
                    }
                }
            }
            return true;
        };

        if (survives(0)) return -1; // the click as it is already clears everything

        int a = -1;
        for (int d = 1; d <= W; d++) {
            if (survives(d)) { a = d; break; }
        }
        if (a < 0) {
            DBG("[InputBuffer] smart jump: no safe start within {} steps, click passed through", W);
            return -1;
        }
        int b = a;
        while (b + 1 <= W && survives(b + 1)) b++;
        int pick = g_smartEarliest ? a : a + (b - a) / 2;
        DBG("[InputBuffer] smart jump: delaying click {} steps (safe {}..{}, window {})", pick, a, b, W);
        return pick;
    }

    // Measures the wave's real speed (horizontal and vertical units per step).
    void smartObserveWave(PlayerObject* p, PState& s, int step) {
        auto pos = p->getPosition();
        if (s.wHave && step == s.wLastStep + 1) {
            float dx = pos.x - s.wLastX, dy = pos.y - s.wLastY;
            if (dx > 0.01f) g_vx = dx;
            if (std::fabs(dy) > 0.2f) g_wvy = std::fabs(dy);
        }
        s.wLastX = pos.x;
        s.wLastY = pos.y;
        s.wLastStep = step;
        s.wHave = true;
    }

    // Wave: if turning now would fly into a hazard but turning a few steps
    // later would not, returns how many steps to wait; otherwise -1. It assumes
    // the wave keeps its new direction for a short while after the turn.
    int smartWaveDecide(PlayerObject* p, PState& s, int step, bool down) {
        if (!g_smartWave || sharedDualInput() || s.held == down) return -1;
        if (g_vx <= 0.05f || g_wvy <= 0.05f) return -1;
        ensureHazards();
        if (g_haz.empty()) return -1;

        auto pos = p->getPosition();
        float x0 = pos.x, y0 = pos.y;
        CCRect pr = p->getObjectRect();
        float pw = pr.size.width, ph = pr.size.height;
        float offx = pr.getMidX() - pos.x, offy = pr.getMidY() - pos.y;
        if (pw < 1.f || ph < 1.f) {
            pw = ph = 10.f * p->getScale();
            offx = offy = 0.f;
        }
        float sgn = p->m_isUpsideDown ? -1.f : 1.f;
        int W = std::max(1, static_cast<int>(std::lround(g_smartWaveWindow * stepsPerTick())));
        int H = std::max(8, static_cast<int>(std::lround(16 * stepsPerTick())));
        float margin = static_cast<float>(g_smartMargin);
        float vx = g_vx, vy = g_wvy;
        bool wasHeld = s.held;

        float xmax = x0 + vx * (1 + W + H) + pw + 40.f;
        std::vector<Hz const*> nearby;
        auto it = std::lower_bound(g_haz.begin(), g_haz.end(), x0 - 300.f,
                                   [](Hz const& h, float v) { return h.minx < v; });
        for (; it != g_haz.end() && it->minx <= xmax; ++it) nearby.push_back(&*it);
        if (nearby.empty()) return -1;

        auto survives = [&](int d) {
            float y = y0;
            int last = 1 + d + H;
            for (int m = 1; m <= last; m++) {
                bool heldNow = (m <= d) ? wasHeld : down;
                y += (heldNow ? 1.f : -1.f) * sgn * vy;
                float cx = x0 + vx * m + offx, cy = y + offy;
                float l = cx - pw * 0.5f, r = cx + pw * 0.5f;
                float b = cy - ph * 0.5f, t = cy + ph * 0.5f;
                for (auto h : nearby) {
                    if (r < h->minx - margin || l > h->maxx + margin) continue;
                    if (h->circle) {
                        float qx = std::clamp(h->cx, l, r), qy = std::clamp(h->cy, b, t);
                        float ddx = qx - h->cx, ddy = qy - h->cy;
                        float rr = h->r + margin;
                        if (ddx * ddx + ddy * ddy < rr * rr) return false;
                    } else if (b < h->maxy + margin && t > h->miny - margin) {
                        return false;
                    }
                }
            }
            return true;
        };

        if (survives(0)) return -1;
        int a = -1;
        for (int d = 1; d <= W; d++) {
            if (survives(d)) { a = d; break; }
        }
        if (a < 0) return -1;
        int b = a;
        while (b + 1 <= W && survives(b + 1)) b++;
        int pick = g_smartEarliest ? a : a + (b - a) / 2;
        DBG("[InputBuffer] smart wave: delaying {} {} steps (safe {}..{}, window {})",
            down ? "press" : "release", pick, a, b, W);
        return pick;
    }

    void stepPlayer(PlayerObject* p, PState& s, int idx) {
        int step = m_fields->step;
        char const* tag = idx == 0 ? "P1" : "P2";

        if (isGroundMode(p) && p->m_isOnGround) s.lastGroundStep = step;
        if (idx == 0) smartObserve(p, s, step);
        if (idx == 0 && g_smartWave && modeIndex(p) == MODE_WAVE) smartObserveWave(p, s, step);
        else s.wHave = false;

        // Smart Jump: the held-back click goes out at the chosen tick.
        if (s.smartFire && step >= s.smartFireAt) {
            s.smartFire = false;
            if (isGroundMode(p) && p->m_isOnGround) {
                sendInput(p, true);
                s.releaseNext = true;
                s.releaseNextAt = step + (s.smartHold > 0 ? s.smartHold : injectHoldSteps(s));
                s.sjFireStep = step;
                DBG("[InputBuffer] {} SMART JUMP click at step {}", tag, step);
                showText("SMART");
            }
        }

        // Robot minimum hold: the held-back release goes out now.
        if (s.robotRelease && (step >= s.robotReleaseAt || modeIndex(p) != 2)) {
            s.robotRelease = false;
            sendInput(p, false);
            DBG("[InputBuffer] {} ROBOT release applied at step {}", tag, step);
        }

        // A click that came a few ticks after leaving the ground is injected
        // as if the player were still standing on it (experimental).
        if (s.coyoteFire) {
            s.coyoteFire = false;
            if (isGroundMode(p)) {
                p->m_isOnGround = true;
                sendInput(p, true);
                s.releaseNext = true;
                s.releaseNextAt = step + injectHoldSteps(s);
                DBG("[InputBuffer] {} COYOTE press at step {} [{}]", tag, step, modeName(p));
            }
        }

        if (!s.waveQueue.empty()) {
            if (!isFlyMode(p)) {
                flushWaveQueue(p, s, idx);
            } else {
                size_t applied = 0;
                while (applied < s.waveQueue.size() &&
                       step >= s.waveQueue[applied].second) {
                    auto ev = s.waveQueue[applied];
                    sendInput(p, ev.first);
                    DBG("[InputBuffer] {} {} {} {} applied at step {}",
                        tag, modeName(p), WAVE_NAMES[styleFor(p)], ev.first ? "PRESS" : "RELEASE", step);
                    applied++;
                }
                s.waveQueue.erase(s.waveQueue.begin(), s.waveQueue.begin() + applied);
            }
        }

        if (s.delayedRelease) {
            if (!isFlyMode(p) || step >= s.releaseAt) {
                s.delayedRelease = false;
                s.held = false;
                sendInput(p, false);
                DBG("[InputBuffer] {} MIN-HOLD release applied at step {} [{}]",
                    tag, step, modeName(p));
            }
        }

        if (s.releaseNext && step >= s.releaseNextAt) {
            s.releaseNext = false;
            if (!s.held) sendInput(p, false);
        }

        if (!s.pending) return;

        int waited = step - s.pendingStart;
        int limit = modeLimitSteps(p);

        if (limit <= 0 || waited > limit || !isGroundMode(p)) {
            DBG("[InputBuffer] {} pending EXPIRED waited={} limit={} mode={}",
                tag, waited, limit, modeName(p));
            if (limit > 0 && waited > limit && isGroundMode(p)) recordExpired(modeIndex(p));
            s.pending = false;
            return;
        }

        int rings = ringsTouching(p);
        if (rings != s.lastRingCount) {
            DBG("[InputBuffer] {} ring overlap count {} -> {} (waited {})",
                tag, s.lastRingCount, rings, waited);
            s.lastRingCount = rings;
        }

        bool onGround = p->m_isOnGround;
        bool onRing = rings > 0;

        if (onGround || onRing) {
            s.pending = false;
            float ticksSaved = waited / stepsPerTick();
            char const* what = onRing ? "RING" : "GROUND";

            if (s.held) {
                if (g_rering && onRing && !onGround) {
                    // Still holding when the ring was reached: let go and press
                    // again so the ring gets a fresh click.
                    sendInput(p, false);
                    sendInput(p, true);
                    DBG("[InputBuffer] {} RING re-press after {} steps ({})", tag, waited, modeName(p));
                    recordSample(modeIndex(p), ticksSaved);
                    showFeedback(ticksSaved);
                } else {
                    DBG("[InputBuffer] {} reached {} after {} steps but button still held, skipping ({})",
                        tag, what, waited, modeName(p));
                }
            } else {
                sendInput(p, true);
                s.releaseNext = true;
                s.releaseNextAt = step + injectHoldSteps(s);
                recordSample(modeIndex(p), ticksSaved);
                showFeedback(ticksSaved);
                log::info("[InputBuffer] *** SAVED +{:.1f} TICKS ({:.2f} frames @ 60fps) PRESS! [{} {} {}] (Step0: {} -> Delayed: {}) [Limit: {} steps] ***",
                          ticksSaved, ticksSaved / 4.0f, tag, modeName(p), what,
                          s.pendingStart, step, limit);
            }
        }
    }

    bool inputPlayer(PlayerObject* p, PState& s, int idx, bool down) {
        int step = m_fields->step;
        char const* tag = idx == 0 ? "P1" : "P2";

        // Remember how long the player really holds, for Mirror hold.
        if (down) {
            s.realPressStep = step;
            s.pressOnGround = p->m_isOnGround;
            // A release is still being held back: let go first so the new
            // press arrives as a fresh click, in the right order.
            if (s.robotRelease) {
                s.robotRelease = false;
                sendInput(p, false);
            }
        } else if (s.realPressStep >= 0) {
            s.lastHoldSteps = std::max(1, step - s.realPressStep);
            // Robot minimum hold: a very quick tap on the ground is held for at
            // least the set time, so the jump height stays consistent.
            if (g_robotMinHold > 0 && modeIndex(p) == 2 && s.pressOnGround && !s.robotRelease) {
                int need = std::max(1, static_cast<int>(std::lround(g_robotMinHold * stepsPerTick())));
                if (step - s.realPressStep < need) {
                    s.robotRelease = true;
                    s.robotReleaseAt = s.realPressStep + need;
                    s.held = false;
                    DBG("[InputBuffer] {} ROBOT release held back until step {}", tag, s.robotReleaseAt);
                    return true;
                }
            }
        }

        if (!isFlyMode(p) && !s.waveQueue.empty()) {
            flushWaveQueue(p, s, idx);
        }

        // Smart Wave: retime a turn that would hit a hazard. Later inputs queue
        // behind a retimed one so the order is kept.
        if (g_smartWave && idx == 0 && modeIndex(p) == MODE_WAVE && step >= m_fields->graceUntil) {
            if (!s.waveQueue.empty()) {
                if (s.swQueued) {
                    int target = std::max(step + 1, s.waveQueue.back().second + 1);
                    s.held = down;
                    s.waveQueue.emplace_back(down, target);
                    return true;
                }
            } else {
                s.swQueued = false;
                int d = smartWaveDecide(p, s, step, down);
                if (d > 0) {
                    s.held = down;
                    s.swQueued = true;
                    s.waveQueue.emplace_back(down, step + 1 + d);
                    return true;
                }
            }
        }

        int n = modeLimitSteps(p);
        int style = styleFor(p);
        bool inGrace = step < m_fields->graceUntil;
        if (isFlyMode(p) && inGrace && n > 0 && style != WAVE_HOLD) {
            DBG("[InputBuffer] {} {} {} {} passed through (spawn grace) at step {}",
                tag, modeName(p), WAVE_NAMES[style], down ? "PRESS" : "RELEASE", step);
        }

        if (isFlyMode(p) && n > 0 && style != WAVE_HOLD && !inGrace) {
            int target;
            if (style == WAVE_GRID) {
                target = ((step + n - 1) / n) * n;
                if (!s.waveQueue.empty()) {
                    auto const& last = s.waveQueue.back();
                    target = std::max(target, last.second);
                    // A tap must hold for at least one grid cell, or the
                    // press and release would cancel out in the same step.
                    if (!down && last.first) {
                        target = std::max(target, last.second + n);
                    }
                }
            } else { // WAVE_GAP
                target = step;
                if (!s.waveQueue.empty()) {
                    target = std::max(target, s.waveQueue.back().second);
                }
                if (down) {
                    target = std::max(target, s.lastReleaseStep + n);
                } else {
                    s.lastReleaseStep = target;
                }
                if (target == step && s.waveQueue.empty()) {
                    s.held = down;
                    return false;
                }
            }

            s.held = down;
            s.waveQueue.emplace_back(down, target);
            DBG("[InputBuffer] {} {} {} {} queued at step {} -> {} ({} steps)",
                tag, modeName(p), WAVE_NAMES[style], down ? "PRESS" : "RELEASE", step, target, n);
            return true;
        }

        if (down && s.delayedRelease) {
            s.delayedRelease = false;
            s.held = false;
            sendInput(p, false);
        }

        if (!down && isFlyMode(p) && s.pressStep >= 0) {
            int minHold = modeLimitSteps(p);
            int heldFor = step - s.pressStep;
            if (minHold > 0 && heldFor < minHold) {
                s.delayedRelease = true;
                s.releaseAt = s.pressStep + minHold;
                DBG("[InputBuffer] {} MIN-HOLD delaying release: held {} < {} steps [{}]",
                    tag, heldFor, minHold, modeName(p));
                return true;
            }
        }

        // Smart Jump (cube): if starting the jump now would hit a hazard but a
        // slightly later start clears it, hold the click back and fire it then.
        if (down && g_smart && idx == 0) {
            if (s.smartFire) return true; // one is already on its way
            int hold = 0;
            int d = smartDecide(p, s, step, hold);
            if (d > 0) {
                s.smartHold = hold;
                s.held = true;
                s.pending = false;
                s.smartFire = true;
                s.smartFireAt = step + 1 + d;
                return true;
            }
        }

        // Late click (coyote time): a press a few ticks after walking off an
        // edge still counts. Not while rising, and not while on a ring.
        if (down && g_coyote > 0 && isGroundMode(p) && !p->m_isOnGround && ringsTouching(p) == 0) {
            int cs = std::max(1, static_cast<int>(std::lround(g_coyote * stepsPerTick())));
            double vy = p->m_isUpsideDown ? -p->m_yVelocity : p->m_yVelocity;
            if (vy <= 0.0 && step - s.lastGroundStep <= cs) {
                s.held = true;
                s.pending = false;
                s.coyoteFire = true;
                DBG("[InputBuffer] {} late click accepted {} steps after ground [{}]",
                    tag, step - s.lastGroundStep, modeName(p));
                return true;
            }
        }

        s.held = down;

        if (down) {
            s.pressStep = isFlyMode(p) ? step : -1;

            bool early = isGroundMode(p) && !p->m_isOnGround && ringsTouching(p) == 0;
            if (early && modeLimitSteps(p) > 0) {
                s.pending = true;
                s.pendingStart = step;
                s.lastRingCount = 0;
            } else {
                s.pending = false;
            }
        }
        return false;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.operator->();
        f->step++;
        syncState();

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!PlayLayer::get()) {
            for (auto& s : f->ps) s = PState();
            return;
        }

        // Experimental dual support is off: plain game input in dual sections.
        if ((sharedDualInput() && !g_dualEnabled) || !assistAllowed()) {
            bypassCleanup();
            return;
        }

        if (m_player1) stepPlayer(m_player1, f->ps[0], 0);
        if (m_player2) stepPlayer(m_player2, f->ps[1], 1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.operator->();
        syncState();

        // Switched off, or legit mode: plain game input.
        if (PlayLayer::get() && button == 1 && !assistAllowed()) {
            bypassCleanup();
            f->ps[0].held = down;
            f->ps[1].held = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (PlayLayer::get() && button == 1) {
            // Outside two-player mode the game applies every input to player 1,
            // even one that arrives flagged as player 2 (for example a Jump P2
            // keybind). Treat those inputs as player 1 so the right icon is helped.
            bool asPlayer1 = isPlayer1 || !(m_levelSettings && m_levelSettings->m_twoPlayerMode);

            // Dual portal in a one-player level: the game applies a single
            // click to both icons.
            if (asPlayer1 && m_player1 && sharedDualInput()) {
                if (!g_dualEnabled) {
                    // Experimental dual support is off: plain game input.
                    bypassCleanup();
                    f->ps[0].held = down;
                    f->ps[1].held = down;
                    GJBaseGameLayer::handleButton(down, button, isPlayer1);
                    return;
                }

                // Each icon follows its own game mode and its own state.
                bool hold1 = inputPlayer(m_player1, f->ps[0], 0, down);
                bool hold2 = m_player2 ? inputPlayer(m_player2, f->ps[1], 1, down) : false;
                if (hold1 && hold2) return;
                if (!hold1 && !hold2) {
                    GJBaseGameLayer::handleButton(down, button, isPlayer1);
                    return;
                }
                // Only one icon is held back: give the click to the other now.
                auto now = hold1 ? m_player2 : m_player1;
                if (now) sendInput(now, down);
                return;
            }

            int idx = asPlayer1 ? 0 : 1;
            auto p = asPlayer1 ? m_player1 : m_player2;
            if (p) {
                if (inputPlayer(p, f->ps[idx], idx, down)) return;
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
