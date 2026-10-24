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

using namespace geode::prelude;

namespace pib {
	using Clock = std::chrono::steady_clock;

	struct Pending {
		int64_t fireAt;
		bool down;
	};

	// Survives: ran the whole lookahead alive.
	// Dies:     clone crashed.
	// Special:  clone reached a portal/orb/pad/trigger alive (can't simulate past it).
	// Timeout:  ran out of time budget, result unknown.
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
	inline int64_t g_updates = 0;   // physics updates of the real player so far
	inline int64_t g_shift = 0;     // delay applied to the latest click (keeps rhythm)
	inline float g_dt = 1.f / 240.f;
	inline bool g_bypass = false;   // delivering a queued click
	inline bool g_simulating = false;
	inline bool g_cloneDied = false;
	inline bool g_cloneTouchedSpecial = false;
	inline bool g_tickSeen = false; // probe: has the update hook run in this level?
	inline std::string g_lastReason;

	// ---- classic state ----
	inline bool g_held = false;          // is the player physically holding the button
	inline bool g_classicPending = false;// a click is waiting for the ground
	inline int g_classicLeft = 0;        // ticks left in the buffer window
	inline int g_classicReleaseIn = 0;   // ticks until we let go of a click we fired

	// ---- predictive "hold and wait" state ----
	inline bool g_pendingPress = false;     // a click is being held back, waiting for a safe moment
	inline int64_t g_pendingSince = 0;      // g_updates when the click was made
	inline bool g_pendingReleased = false;  // player let go while it was waiting
	inline int64_t g_pendingHold = 0;       // how long the player held the click (ticks)

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
	inline bool waveRelease() {
		return Mod::get()->getSettingValue<bool>("wave-release");
	}
	inline bool practiceOnly() {
		return Mod::get()->getSettingValue<bool>("practice-only");
	}
	inline bool debugOn() {
		return Mod::get()->getSettingValue<bool>("debug");
	}

	// Log + short on-screen message (only call outside of simulation).
	inline void say(std::string const& msg, float secs = 0.9f) {
		log::info("{}", msg);
		Notification::create(msg, NotificationIcon::None, secs)->show();
	}

	inline bool isClone(PlayerObject* p) {
		return p != nullptr && p == g_clone;
	}

	inline bool groundMode(PlayerObject* p) {
		// Ship, UFO, wave and swing don't need to land to click.
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
	}

	inline void releaseClone() {
		if (g_clone) {
			g_clone->release();
			g_clone = nullptr;
		}
		g_cloneLayer = nullptr;
	}

	// The clone is tied to the layer it was created for; rebuild it if the layer changed.
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

	// nullptr = predictions are active; otherwise why they are skipped.
	inline const char* inactiveReason(PlayLayer* pl) {
		if (!enabled()) return "disabled in settings";
		if (!pl || !pl->m_player1) return "no player";
		if (maxDelayTicks() <= 0) return "max delay is 0";
		if (practiceOnly() && !pl->m_isPracticeMode) return "practice-only is on";
		if (pl->m_player1->m_isPlatformer) return "platformer level";
		if (pl->m_gameState.m_isDualMode) return "dual mode";
		return nullptr;
	}

	inline void flush(PlayLayer* pl) {
		while (!g_queue.empty()) {
			auto e = g_queue.front();
			g_queue.pop_front();
			g_bypass = true;
			pl->handleButton(e.down, 1, true);
			g_bypass = false;
		}
	}

	// ---------------- classic buffer ----------------

	inline bool classicActive(PlayLayer* pl) {
		if (!enabled() || !pl || !pl->m_player1) return false;
		if (practiceOnly() && !pl->m_isPracticeMode) return false;
		return true;
	}

	// Called every physics tick of the real player (classic mode only).
	inline void classicTick(PlayLayer* pl, PlayerObject* p) {
		if (!classicMode()) {
			g_classicPending = false;
			g_classicReleaseIn = 0;
			return;
		}

		// let go of a click we fired for a player who had already released
		if (g_classicReleaseIn > 0 && --g_classicReleaseIn == 0 && !g_held) {
			g_bypass = true;
			pl->handleButton(false, 1, true);
			g_bypass = false;
		}

		if (!g_classicPending) return;

		if (!classicActive(pl)) {
			g_classicPending = false;
			return;
		}

		if (p->m_isOnGround && groundMode(p)) {
			g_classicPending = false;
			g_bypass = true;
			if (g_held) pl->handleButton(false, 1, true); // re-click cleanly
			pl->handleButton(true, 1, true);
			g_bypass = false;
			if (!g_held) g_classicReleaseIn = 3;           // short tap
			if (debugOn()) say("PIB classic: click delivered on landing");
			return;
		}

		if (--g_classicLeft <= 0) g_classicPending = false;
	}

	// ---------------- predictive buffer ----------------

	// Copy the real player's physics state onto the hidden clone.
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

	// Run the clone forward. Press event: click at tick `delay`, hold briefly, release.
	// Release event: button starts held, released at tick `delay`.
	inline Outcome simulate(
		GJBaseGameLayer* layer, PlayerObject* real, int64_t delay,
		bool pressEvent, Clock::time_point deadline
	) {
		syncClone(real);
		g_cloneDied = false;
		g_cloneTouchedSpecial = false;
		g_simulating = true;

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

			if (g_cloneDied) { result = Outcome::Dies; break; }
			if (g_cloneTouchedSpecial) { result = Outcome::Special; break; }
			if ((t & 7) == 7 && Clock::now() > deadline) { result = Outcome::Timeout; break; }
		}

		g_simulating = false;
		g_clone->releaseAllButtons();
		return result;
	}

	// 0 = leave the input alone; otherwise the earliest delay (in ticks) that survives.
	inline int64_t chooseDelay(GJBaseGameLayer* layer, PlayerObject* real, bool pressEvent) {
		if (!ensureClone(layer)) {
			if (debugOn()) say("PIB: could not create clone");
			return 0;
		}
		auto start = Clock::now();
		auto deadline = start + std::chrono::milliseconds(budgetMs());
		auto elapsed = [&]() {
			return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		};
		const char* kind = pressEvent ? "press" : "release";

		// Only intervene if the unmodified input is known to kill.
		auto r0 = simulate(layer, real, 0, pressEvent, deadline);
		if (r0 != Outcome::Dies) {
			if (debugOn()) {
				say(fmt::format("PIB {}: now {} -> pass through ({:.1f}ms of {}ms)",
					kind, name(r0), elapsed(), budgetMs()));
			}
			return 0;
		}

		int64_t maxTicks = maxDelayTicks();
		Outcome last = Outcome::Dies;
		for (int64_t d = 1; d <= maxTicks; ++d) {
			if (Clock::now() > deadline) {
				if (debugOn()) {
					say(fmt::format("PIB {}: budget ran out at d={} ({:.1f}ms) - raise time budget",
						kind, d, elapsed()));
				}
				return 0;
			}
			auto r = simulate(layer, real, d, pressEvent, deadline);
			last = r;
			if (r == Outcome::Timeout) {
				if (debugOn()) {
					say(fmt::format("PIB {}: sim timeout at d={} ({:.1f}ms) - raise time budget",
						kind, d, elapsed()));
				}
				return 0;
			}
			// Reaching a special object alive still beats a delay-0 death.
			if (r == Outcome::Survives || r == Outcome::Special) {
				if (debugOn()) {
					say(fmt::format("PIB {}: dies now, d={} {} -> delaying ({:.1f}ms)",
						kind, d, name(r), elapsed()));
				}
				return d;
			}
		}
		if (debugOn()) {
			say(fmt::format("PIB {}: dies now, no fix within {} ticks (last: {})",
				kind, maxTicks, name(last)));
		}
		return 0;
	}
	// Click just happened: would it kill us right now? If so, the click is held back.
	inline bool shouldHold(GJBaseGameLayer* layer, PlayerObject* real) {
		if (!ensureClone(layer)) {
			if (debugOn()) say("PIB: could not create clone");
			return false;
		}
		auto start = Clock::now();
		auto r0 = simulate(layer, real, 0, true, start + std::chrono::milliseconds(budgetMs()));
		double took = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		if (r0 == Outcome::Dies) {
			if (debugOn()) say(fmt::format("PIB press: would die -> holding click ({:.1f}ms)", took));
			return true;
		}
		if (debugOn()) {
			say(fmt::format("PIB press: now {} -> pass through ({:.1f}ms of {}ms)",
				name(r0), took, budgetMs()));
		}
		return false;
	}

	// Runs once per physics tick while a click is being held back.
	// Fires the click at the first tick where it no longer kills us.
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
				// Dies or Timeout: keep waiting
			}
		}

		if (!fire) return;

		g_pendingPress = false;
		g_bypass = true;
		pl->handleButton(true, 1, true);
		g_bypass = false;

		// If the player already let go, release after the same hold length.
		if (g_pendingReleased) {
			int64_t hold = std::max<int64_t>(1, g_pendingHold);
			g_queue.push_back({g_updates + hold, false});
		}
		g_pendingReleased = false;

		if (debugOn()) say(fmt::format("PIB: click delivered after {} ticks - {}", waited, why));
	}
}

// Probe 1: proves this exact build is installed and loaded (label on the main menu).
class $modify(PIBMenu, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;
		auto size = CCDirector::sharedDirector()->getWinSize();
		auto* label = CCLabelBMFont::create("PIB v0.6.1 loaded", "bigFont.fnt");
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

		// ---- classic mode: click always goes through, and is also remembered ----
		if (pib::classicMode()) {
			pib::flush(pl); // in case the mode was switched mid-level
			pib::g_pendingPress = false;
			auto* p = pl->m_player1;
			// Decide BEFORE the click is applied, otherwise a normal ground jump
			// would look like an air click.
			bool arm = down && pib::classicActive(pl) && pib::groundMode(p) && !p->m_isOnGround;
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
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}
		pib::g_lastReason.clear();

		auto* player = pl->m_player1;
		int64_t delay = 0;

		// A click is already being held back: just remember what the player does with it.
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
			delay = pib::g_shift;                       // something is waiting: keep the rhythm
		} else if (down) {
			pib::g_shift = 0;
			if (pib::shouldHold(self, player)) {
				// This click would kill us: swallow it and wait for a safe moment.
				pib::g_pendingPress = true;
				pib::g_pendingSince = pib::g_updates;
				pib::g_pendingReleased = false;
				pib::g_pendingHold = 0;
				return;
			}
			delay = 0;
		} else if (player->m_isDart && pib::waveRelease()) {
			delay = pib::chooseDelay(self, player, false);
			pib::g_shift = delay;
		} else {
			delay = pib::g_shift;                       // keep the hold length of a delayed press
		}

		if (delay <= 0 && pib::g_queue.empty()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int64_t fire = pib::g_updates + delay;
		if (!pib::g_queue.empty()) fire = std::max(fire, pib::g_queue.back().fireAt + 1);
		pib::g_queue.push_back({fire, down});
	}

	// Things the hidden clone must never do to the real level.
	bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return false; }
		return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
	}
	void playerTouchedRing(PlayerObject* player, RingObject* object) {
		if (pib::isClone(player)) { pib::g_cloneTouchedSpecial = true; return; }
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
			// Probe 2: proves the PlayerObject::update hook runs inside a level.
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

			if (dt > 0.f) pib::g_dt = dt;

			// a held-back click: fire it at the first safe tick
			if (pib::g_pendingPress) pib::tickPending(pl, self);

			// deliver delayed clicks at the start of their tick (predictive mode)
			while (!pib::g_queue.empty() && pib::g_queue.front().fireAt <= pib::g_updates) {
				auto e = pib::g_queue.front();
				pib::g_queue.pop_front();
				pib::g_bypass = true;
				pl->handleButton(e.down, 1, true);
				pib::g_bypass = false;
			}

			PlayerObject::update(dt);
			pib::g_updates++;
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
			pib::g_cloneDied = true;   // the clone crashed: record it, never run the real death
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}
};

$on_mod(Loaded) {
	log::info("Input Buffer loaded");
}
