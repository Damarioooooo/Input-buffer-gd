#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace pib {
	using Clock = std::chrono::steady_clock;

	struct Pending {
		int64_t fireAt;
		bool down;
	};

	enum class Outcome { Survives, Dies, Special, Timeout };

	inline const char* name(Outcome o) {
		switch (o) {
			case Outcome::Survives: return "survives";
			case Outcome::Dies: return "dies";
			case Outcome::Special: return "special";
			case Outcome::Timeout: return "timeout";
		}
		return "?";
	}

	// ---- predictive state ----
	inline std::deque<Pending> g_queue;
	inline PlayerObject* g_clone = nullptr;
	inline GJBaseGameLayer* g_cloneLayer = nullptr;
	inline int64_t g_updates = 0;
	inline int64_t g_shift = 0;
	inline float g_dt = 1.f / 240.f;
	inline bool g_bypass = false;
	inline bool g_simulating = false;
	inline bool g_cloneDied = false;
	inline bool g_cloneTouchedSpecial = false;
	inline bool g_tickSeen = false;
	inline std::string g_lastReason;

	// ---- classic state ----
	inline bool g_held = false;
	inline bool g_classicPending = false;
	inline int g_classicLeft = 0;
	inline int g_classicReleaseIn = 0;

	// ---- predictive "hold and wait" state ----
	inline bool g_pendingPress = false;
	inline int64_t g_pendingSince = 0;
	inline bool g_pendingReleased = false;
	inline int64_t g_pendingHold = 0;

	// ---- real input tracking (wave + orb assist) ----
	inline bool g_userDown = false;
	inline bool g_actualDown = false;
	inline int64_t g_userPressTick = -1000000;
	inline bool g_orbArmed = false;
	inline int g_releaseIn = 0;
	inline bool g_wavePrev = false;

	// ---- diagnostics ----
	inline CCPoint g_simStart = CCPoint{0.f, 0.f};
	inline CCPoint g_simEnd = CCPoint{0.f, 0.f};
	inline int64_t g_simTicks = 0;
	inline bool g_recordTrace = false;
	inline std::vector<CCPoint> g_trace;
	inline bool g_traceActive = false;
	inline int64_t g_traceStart = 0;
	inline int g_traceNext = 0;

	inline bool enabled() {
		return Mod::get()->getSettingValue<bool>("enabled");
	}
	inline bool classicMode() {
		return Mod::get()->getSettingValue<std::string>("mode") == "Classic";
	}
	inline int64_t classicTicks() {
		return Mod::get()->getSettingValue<int64_t>("classic-ticks");
	}
	inline int64_t maxDelayTicks() {
		return Mod::get()->getSettingValue<int64_t>("max-delay-frames") * 4;
	}
	inline int64_t lookahead() {
		return Mod::get()->getSettingValue<int64_t>("lookahead-ticks");
	}
	inline int64_t holdTicks() {
		return std::max<int64_t>(1, Mod::get()->getSettingValue<int64_t>("hold-ticks"));
	}
	inline int64_t budgetMs() {
		return Mod::get()->getSettingValue<int64_t>("time-budget-ms");
	}
	inline bool noclip() {
		return Mod::get()->getSettingValue<bool>("noclip");
	}
	inline bool waveEnabled() {
		return Mod::get()->getSettingValue<bool>("wave-enabled");
	}
	inline int64_t waveLookahead() {
		return std::max<int64_t>(8, Mod::get()->getSettingValue<int64_t>("wave-lookahead-ticks"));
	}
	inline int64_t waveReact() {
		return std::max<int64_t>(2, Mod::get()->getSettingValue<int64_t>("wave-react-ticks"));
	}
	inline bool orbAssistOn() {
		return Mod::get()->getSettingValue<bool>("orb-assist");
	}
	inline int64_t orbWindow() {
		return std::max<int64_t>(2, Mod::get()->getSettingValue<int64_t>("orb-window-ticks"));
	}
	inline bool practiceOnly() {
		return Mod::get()->getSettingValue<bool>("practice-only");
	}
	inline bool debugOn() {
		return Mod::get()->getSettingValue<bool>("debug");
	}

	inline void say(std::string const& msg, float secs = 0.9f) {
		log::info("{}", msg);
		Notification::create(msg, NotificationIcon::None, secs)->show();
	}

	inline bool isClone(PlayerObject* p) {
		return p != nullptr && p == g_clone;
	}

	inline bool groundMode(PlayerObject* p) {
		return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
	}

	inline void resetRuntime() {
		g_queue.clear();
		g_updates = 0;
		g_shift = 0;
		g_bypass = false;
		g_simulating = false;
		g_cloneDied = false;
		g_cloneTouchedSpecial = false;
		g_held = false;
		g_classicPending = false;
		g_classicLeft = 0;
		g_classicReleaseIn = 0;
		g_pendingPress = false;
		g_pendingReleased = false;
		g_pendingHold = 0;
		g_userDown = false;
		g_actualDown = false;
		g_userPressTick = -1000000;
		g_orbArmed = false;
		g_releaseIn = 0;
		g_wavePrev = false;
		g_recordTrace = false;
		g_traceActive = false;
		g_traceNext = 0;
		g_trace.clear();
	}

	inline void releaseClone() {
		if (g_clone) {
			g_clone->release();
			g_clone = nullptr;
		}
		g_cloneLayer = nullptr;
	}

	inline bool ensureClone(GJBaseGameLayer* layer) {
		if (g_clone && g_cloneLayer == layer) return true;
		releaseClone();
		g_clone = PlayerObject::create(1, 1, layer, layer, false);
		if (!g_clone) return false;
		g_clone->retain();
		g_clone->setVisible(false);
		g_cloneLayer = layer;
		return true;
	}

	inline const char* inactiveReason(PlayLayer* pl) {
		if (!enabled()) return "disabled in settings";
		if (!pl || !pl->m_player1) return "no player";
		if (maxDelayTicks() <= 0) return "max delay is 0";
		if (practiceOnly() && !pl->m_isPracticeMode) return "practice-only is on";
		if (pl->m_player1->m_isPlatformer) return "platformer level";
		if (pl->m_gameState.m_isDualMode) return "dual mode";
		return nullptr;
	}

	// Send a button state to the game, bypassing our own hook.
	inline void deliver(PlayLayer* pl, bool down) {
		g_bypass = true;
		pl->handleButton(down, 1, true);
		g_bypass = false;
		g_actualDown = down;
	}

	inline void flush(PlayLayer* pl) {
		while (!g_queue.empty()) {
			auto e = g_queue.front();
			g_queue.pop_front();
			deliver(pl, e.down);
		}
	}

	// ---------------- classic buffer ----------------

	inline bool classicActive(PlayLayer* pl) {
		if (!enabled() || !pl || !pl->m_player1) return false;
		if (practiceOnly() && !pl->m_isPracticeMode) return false;
		return true;
	}

	inline void classicTick(PlayLayer* pl, PlayerObject* p) {
		if (!classicMode()) {
			g_classicPending = false;
			g_classicReleaseIn = 0;
			return;
		}

		if (g_classicReleaseIn > 0 && --g_classicReleaseIn == 0 && !g_held) {
			deliver(pl, false);
		}

		if (!g_classicPending) return;

		if (!classicActive(pl)) {
			g_classicPending = false;
			return;
		}

		if (p->m_isOnGround && groundMode(p)) {
			g_classicPending = false;
			if (g_held) deliver(pl, false);
			deliver(pl, true);
			if (!g_held) g_classicReleaseIn = 3;
			if (debugOn()) say("PIB classic: click delivered on landing");
			return;
		}

		if (--g_classicLeft <= 0) g_classicPending = false;
	}

	// ---------------- predictive buffer ----------------

	inline void syncClone(PlayerObject* real) {
		auto* c = g_clone;
		c->copyAttributes(real);
		c->setPosition(real->getPosition());
		c->m_position = real->m_position;
		c->setScaleX(real->getScaleX());
		c->setScaleY(real->getScaleY());
		c->setRotation(real->getRotation());
		c->m_yVelocity = real->m_yVelocity;
		c->m_isOnGround = real->m_isOnGround;
		c->m_isUpsideDown = real->m_isUpsideDown;
		c->m_isSideways = real->m_isSideways;
		c->m_gravityMod = real->m_gravityMod;
		c->m_playerSpeed = real->m_playerSpeed;
		c->m_vehicleSize = real->m_vehicleSize;
		c->m_isShip = real->m_isShip;
		c->m_isBird = real->m_isBird;
		c->m_isBall = real->m_isBall;
		c->m_isDart = real->m_isDart;
		c->m_isRobot = real->m_isRobot;
		c->m_isSpider = real->m_isSpider;
		c->m_isSwing = real->m_isSwing;
		c->m_isDead = false;
		c->releaseAllButtons();
	}

	inline Outcome simulate(
		GJBaseGameLayer* layer, PlayerObject* real, int64_t delay,
		bool pressEvent, Clock::time_point deadline
	) {
		syncClone(real);
		g_cloneDied = false;
		g_cloneTouchedSpecial = false;
		g_simulating = true;

		g_simStart = g_clone->m_position;
		g_simTicks = 0;
		if (g_recordTrace) g_trace.clear();

		if (!pressEvent) g_clone->pushButton(PlayerButton::Jump);

		int64_t hold = holdTicks();
		int64_t total = delay + lookahead();
		Outcome result = Outcome::Survives;

		for (int64_t t = 0; t < total; ++t) {
			if (pressEvent) {
				if (t == delay) g_clone->pushButton(PlayerButton::Jump);
				else if (t == delay + hold) g_clone->releaseButton(PlayerButton::Jump);
			} else if (t == delay) {
				g_clone->releaseButton(PlayerButton::Jump);
			}

			g_clone->update(g_dt);
			layer->checkCollisions(g_clone, g_dt, false);

			g_simTicks = t + 1;
			if (g_recordTrace) g_trace.push_back(g_clone->m_position);

			if (g_cloneDied) { result = Outcome::Dies; break; }
			if (g_cloneTouchedSpecial) { result = Outcome::Special; break; }
			if ((t & 7) == 7 && Clock::now() > deadline) { result = Outcome::Timeout; break; }
		}

		g_simEnd = g_clone->m_position;
		g_simulating = false;
		g_clone->releaseAllButtons();
		return result;
	}

	inline bool shouldHold(GJBaseGameLayer* layer, PlayerObject* real) {
		if (!ensureClone(layer)) {
			if (debugOn()) say("PIB: could not create clone");
			return false;
		}
		auto start = Clock::now();
		g_recordTrace = debugOn();
		auto r0 = simulate(layer, real, 0, true, start + std::chrono::milliseconds(budgetMs()));
		g_recordTrace = false;
		double took = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		if (r0 == Outcome::Dies) {
			if (debugOn()) say(fmt::format("PIB press: would die -> holding click ({:.1f}ms)", took));
			return true;
		}
		if (debugOn()) {
			say(fmt::format("PIB press: now {} | sim moved dx={:.0f} dy={:.0f} in {} ticks ({:.1f}ms)",
				name(r0), g_simEnd.x - g_simStart.x, g_simEnd.y - g_simStart.y,
				g_simTicks, took), 2.5f);
			if (r0 == Outcome::Survives) {
				g_traceActive = true;
				g_traceStart = g_updates;
				g_traceNext = 0;
			}
		}
		return false;
	}

	// Compares the real player with the simulated path after a pass-through click.
	inline void traceCheck(PlayerObject* real) {
		if (!g_traceActive) return;
		if (real->m_isDead) { g_traceActive = false; return; }

		static const int64_t marks[] = {8, 24, 60};
		int64_t k = g_updates - g_traceStart;
		if (g_traceNext >= 3) { g_traceActive = false; return; }
		if (k != marks[g_traceNext]) return;

		g_traceNext++;
		if (k <= static_cast<int64_t>(g_trace.size())) {
			auto sim = g_trace[k - 1];
			auto rp = real->m_position;
			say(fmt::format("PIB trace k={}: real ({:.0f},{:.0f}) sim ({:.0f},{:.0f})",
				k, rp.x, rp.y, sim.x, sim.y), 3.0f);
		} else {
			say(fmt::format("PIB trace k={}: sim stopped early", k), 3.0f);
		}
		if (g_traceNext >= 3) g_traceActive = false;
	}

	inline void tickPending(PlayLayer* pl, PlayerObject* p) {
		int64_t waited = g_updates - g_pendingSince;
		bool fire = false;
		std::string why;

		if (waited >= maxDelayTicks()) {
			fire = true;
			why = "wait limit reached";
		} else {
			auto* layer = static_cast<GJBaseGameLayer*>(pl);
			if (!ensureClone(layer)) {
				fire = true;
				why = "no clone";
			} else {
				auto start = Clock::now();
				auto r = simulate(layer, p, 0, true, start + std::chrono::milliseconds(budgetMs()));
				if (r == Outcome::Survives || r == Outcome::Special) {
					fire = true;
					why = fmt::format("safe ({})", name(r));
				}
			}
		}

		if (!fire) return;

		g_pendingPress = false;
		deliver(pl, true);

		if (g_pendingReleased) {
			int64_t hold = std::max<int64_t>(1, g_pendingHold);
			g_queue.push_back({g_updates + hold, false});
		}
		g_pendingReleased = false;

		if (debugOn()) say(fmt::format("PIB: click delivered after {} ticks - {}", waited, why));
	}

	// ---------------- wave controller ----------------

	inline Outcome simulateWave(
		GJBaseGameLayer* layer, PlayerObject* real, bool want, int64_t flip,
		int64_t total, Clock::time_point deadline, int64_t& diedAt
	) {
		syncClone(real);
		g_cloneDied = false;
		g_cloneTouchedSpecial = false;
		g_simulating = true;
		diedAt = -1;

		bool held = false;
		Outcome result = Outcome::Survives;

		for (int64_t t = 0; t < total; ++t) {
			bool state = (t < flip) ? !want : want;
			if (state != held) {
				held = state;
				if (held) g_clone->pushButton(PlayerButton::Jump);
				else g_clone->releaseButton(PlayerButton::Jump);
			}

			g_clone->update(g_dt);
			layer->checkCollisions(g_clone, g_dt, false);

			if (g_cloneDied) { result = Outcome::Dies; diedAt = t; break; }
			if (g_cloneTouchedSpecial) { result = Outcome::Special; break; }
			if ((t & 7) == 7 && Clock::now() > deadline) { result = Outcome::Timeout; break; }
		}

		g_simulating = false;
		g_clone->releaseAllButtons();
		return result;
	}

	inline void waveTick(PlayLayer* pl, PlayerObject* p) {
		bool want = g_userDown;
		bool decide = want;
		auto* layer = static_cast<GJBaseGameLayer*>(pl);

		if (ensureClone(layer)) {
			auto deadline = Clock::now() + std::chrono::milliseconds(budgetMs());
			int64_t total = waveLookahead();
			int64_t died = -1;

			auto r = simulateWave(layer, p, want, 0, total, deadline, died);
			if (r == Outcome::Dies && died <= waveReact()) {
				static const int64_t flips[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 48};
				for (auto f : flips) {
					if (f > total) break;
					if (Clock::now() > deadline) break;
					int64_t d2 = -1;
					auto r2 = simulateWave(layer, p, want, f, total, deadline, d2);
					if (r2 == Outcome::Survives || r2 == Outcome::Special) {
						decide = !want;
						if (debugOn()) say(fmt::format("PIB wave: flip {} ticks", f), 0.4f);
						break;
					}
				}
			}
		}

		if (g_actualDown != decide) deliver(pl, decide);
	}

	// ---------------- orb assist ----------------

	inline void orbAssist(PlayerObject* player) {
		auto* pl = PlayLayer::get();
		if (!pl || player != pl->m_player1) return;
		if (!g_orbArmed || !enabled() || !orbAssistOn()) return;
		if (practiceOnly() && !pl->m_isPracticeMode) return;
		if (player->m_isDart) return;

		int64_t age = g_updates - g_userPressTick;
		if (age < 2 || age > orbWindow()) return;

		g_orbArmed = false;
		g_bypass = true;
		pl->handleButton(false, 1, true);
		pl->handleButton(true, 1, true);
		g_bypass = false;
		g_actualDown = true;
		if (!g_userDown) g_releaseIn = 3;
		if (debugOn()) say(fmt::format("PIB orb: click re-fired ({} ticks early)", age), 0.5f);
	}
}

class $modify(PIBMenu, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;
		auto size = CCDirector::sharedDirector()->getWinSize();
		auto* label = CCLabelBMFont::create("PIB v0.7.2 loaded", "bigFont.fnt");
		label->setScale(0.4f);
		label->setPosition({ size.width / 2.f, 12.f });
		label->setZOrder(100);
		this->addChild(label);
		return true;
	}
};

class $modify(PIBGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		auto* self = static_cast<GJBaseGameLayer*>(this);
		auto* pl = PlayLayer::get();
		bool inPlay = pl && static_cast<GJBaseGameLayer*>(pl) == self;

		if (pib::g_bypass || pib::g_simulating || !inPlay || button != 1 || !isPlayer1) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		pib::g_userDown = down;
		if (down) {
			pib::g_userPressTick = pib::g_updates;
			pib::g_orbArmed = true;
		}

		// ---- classic mode ----
		if (pib::classicMode()) {
			pib::flush(pl);
			pib::g_pendingPress = false;
			auto* p = pl->m_player1;
			bool arm = down && pib::classicActive(pl) && pib::groundMode(p) && !p->m_isOnGround;
			pib::g_actualDown = down;
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			pib::g_held = down;
			if (arm) {
				pib::g_classicPending = true;
				pib::g_classicLeft = static_cast<int>(pib::classicTicks());
			}
			return;
		}

		// ---- predictive mode ----
		if (auto* why = pib::inactiveReason(pl)) {
			if (pib::debugOn() && pib::g_lastReason != why) {
				pib::g_lastReason = why;
				pib::say(fmt::format("PIB inactive: {}", why));
			}
			pib::flush(pl);
			pib::g_pendingPress = false;
			pib::g_actualDown = down;
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}
		pib::g_lastReason.clear();

		auto* player = pl->m_player1;
		int64_t delay = 0;

		if (player->m_isDart) {
			pib::flush(pl);
			pib::g_pendingPress = false;
			pib::g_shift = 0;
			if (!pib::waveEnabled()) {
				pib::g_actualDown = down;
				GJBaseGameLayer::handleButton(down, button, isPlayer1);
				return;
			}
			pib::g_wavePrev = true;
			return;
		}

		if (pib::g_pendingPress) {
			if (down) {
				pib::g_pendingReleased = false;
			} else {
				pib::g_pendingReleased = true;
				pib::g_pendingHold = std::max<int64_t>(1, pib::g_updates - pib::g_pendingSince);
			}
			return;
		}

		if (!pib::g_queue.empty()) {
			delay = pib::g_shift;
		} else if (down) {
			pib::g_shift = 0;
			if (pib::shouldHold(self, player)) {
				pib::g_pendingPress = true;
				pib::g_pendingSince = pib::g_updates;
				pib::g_pendingReleased = false;
				pib::g_pendingHold = 0;
				return;
			}
			delay = 0;
		} else {
			delay = pib::g_shift;
		}

		if (delay <= 0 && pib::g_queue.empty()) {
			pib::g_actualDown = down;
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int64_t fire = pib::g_updates + delay;
		if (!pib::g_queue.empty()) fire = std::max(fire, pib::g_queue.back().fireAt + 1);
		pib::g_queue.push_back({fire, down});
	}

	bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return false; }
		return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
	}
	void playerTouchedRing(PlayerObject* player, RingObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return; }
		if (!pib::g_simulating) pib::orbAssist(player);
		GJBaseGameLayer::playerTouchedRing(player, object);
	}
	void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return; }
		GJBaseGameLayer::playerTouchedTrigger(player, object);
	}
	void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return; }
		GJBaseGameLayer::bumpPlayer(player, object);
	}
	void gravBumpPlayer(PlayerObject* player, EffectGameObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return; }
		GJBaseGameLayer::gravBumpPlayer(player, object);
	}
	void destroyObject(GameObject* object) {
		if (pib::g_simulating) { pib::g_cloneTouchedSpecial = true; return; }
		GJBaseGameLayer::destroyObject(object);
	}
	void pickupItem(EffectGameObject* object) {
		if (pib::g_simulating) { pib::g_cloneTouchedSpecial = true; return; }
		GJBaseGameLayer::pickupItem(object);
	}
	void collectedObject(EffectGameObject* object) {
		if (pib::g_simulating) { pib::g_cloneTouchedSpecial = true; return; }
		GJBaseGameLayer::collectedObject(object);
	}
	void gameEventTriggered(GJGameEvent event, int material, int playerID) {
		if (pib::g_simulating) return;
		GJBaseGameLayer::gameEventTriggered(event, material, playerID);
	}
};

class $modify(PIBPlayer, PlayerObject) {
	void update(float dt) {
		auto* pl = PlayLayer::get();
		PlayerObject* self = this;
		if (pl && self == pl->m_player1 && !pib::isClone(self)) {
			if (!pib::g_tickSeen) {
				pib::g_tickSeen = true;
				if (pib::debugOn()) {
					if (pib::classicMode()) {
						pib::say("PIB: update hook OK (classic mode)", 2.5f);
					} else {
						auto* why = pib::inactiveReason(pl);
						pib::say(fmt::format("PIB: update hook OK ({})", why ? why : "predictive active"), 2.5f);
					}
				}
			}

			if (dt >= 0.001f && dt <= 1.f / 120.f) pib::g_dt = dt;

			bool predictive = !pib::classicMode() && pib::inactiveReason(pl) == nullptr;
			bool waveManaged = predictive && self->m_isDart && pib::waveEnabled();

			if (waveManaged) {
				pib::g_pendingPress = false;
			} else {
				if (pib::g_wavePrev) {
					pib::g_wavePrev = false;
					if (pib::g_actualDown != pib::g_userDown) pib::deliver(pl, pib::g_userDown);
				}
				if (pib::g_pendingPress) pib::tickPending(pl, self);
			}

			while (!pib::g_queue.empty() && pib::g_queue.front().fireAt <= pib::g_updates) {
				auto e = pib::g_queue.front();
				pib::g_queue.pop_front();
				pib::deliver(pl, e.down);
			}

			if (pib::g_releaseIn > 0 && --pib::g_releaseIn == 0 && !pib::g_userDown) {
				pib::deliver(pl, false);
			}

			if (waveManaged) {
				pib::g_wavePrev = true;
				pib::waveTick(pl, self);
			}

			PlayerObject::update(dt);
			pib::g_updates++;
			pib::traceCheck(self);
			pib::classicTick(pl, self);
			return;
		}
		PlayerObject::update(dt);
	}

	void tryPlaceCheckpoint() {
		if (pib::isClone(this)) return;
		PlayerObject::tryPlaceCheckpoint();
	}

	void incrementJumps() {
		if (pib::isClone(this)) return;
		PlayerObject::incrementJumps();
	}
};

class $modify(PIBPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();

		auto size = CCDirector::sharedDirector()->getWinSize();
		bool on = pib::noclip();
		auto* label = CCLabelBMFont::create(on ? "NC: ON" : "NC: OFF", "bigFont.fnt");
		label->setScale(0.35f);
		label->setOpacity(150);
		label->setID("pib-nc-label");
		auto* btn = CCMenuItemSpriteExtra::create(
			label, this, menu_selector(PIBPlayLayer::onNoclipBtn)
		);
		auto* menu = CCMenu::create();
		menu->addChild(btn);
		menu->setPosition({ 38.f, size.height - 14.f });
		this->addChild(menu, 100);
	}

	void onNoclipBtn(CCObject*) {
		bool now = !pib::noclip();
		Mod::get()->setSettingValue<bool>("noclip", now);
		if (auto* node = this->getChildByIDRecursive("pib-nc-label")) {
			if (auto* label = typeinfo_cast<CCLabelBMFont*>(node)) {
				label->setString(now ? "NC: ON" : "NC: OFF");
			}
		}
	}

	void resetLevel() {
		pib::resetRuntime();
		PlayLayer::resetLevel();
	}

	void onQuit() {
		pib::resetRuntime();
		pib::releaseClone();
		pib::g_tickSeen = false;
		PlayLayer::onQuit();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (pib::isClone(player)) {
			pib::g_cloneDied = true;
			return;
		}
		if (pib::noclip()) return;
		PlayLayer::destroyPlayer(player, object);
	}
};

$on_mod(Loaded) {
	log::info("Input Buffer loaded");
}
