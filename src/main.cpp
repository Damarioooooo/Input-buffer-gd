#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <deque>

using namespace geode::prelude;

namespace ib {
	struct PendingInput {
		int64_t fireTick;
		bool down;
		int button;
		bool isPlayer1;
	};

	inline std::deque<PendingInput> g_queue;
	inline int64_t g_tick = 0;
	inline bool g_replaying = false;

	inline void reset() {
		g_queue.clear();
		g_tick = 0;
		g_replaying = false;
	}

	inline bool enabled() {
		return Mod::get()->getSettingValue<bool>("enabled");
	}

	inline int64_t delayTicks() {
		return Mod::get()->getSettingValue<int64_t>("delay-ticks");
	}

	inline bool affectMovement() {
		return Mod::get()->getSettingValue<bool>("affect-movement");
	}

	// Button ids: 1 = jump, 2 = left, 3 = right (platformer)
	inline bool shouldDelayButton(int button) {
		if (button == 1) return true;
		return affectMovement();
	}
}

class $modify(IBBaseGameLayer, GJBaseGameLayer) {
	bool inPlayLayer() {
		auto* pl = PlayLayer::get();
		return pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (ib::g_replaying
			|| !ib::enabled()
			|| ib::delayTicks() <= 0
			|| !ib::shouldDelayButton(button)
			|| !this->inPlayLayer()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		ib::g_queue.push_back({
			ib::g_tick + ib::delayTicks(),
			down,
			button,
			isPlayer1
		});
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (this->inPlayLayer()) {
			while (!ib::g_queue.empty() && ib::g_queue.front().fireTick <= ib::g_tick) {
				auto input = ib::g_queue.front();
				ib::g_queue.pop_front();

				ib::g_replaying = true;
				GJBaseGameLayer::handleButton(input.down, input.button, input.isPlayer1);
				ib::g_replaying = false;
			}
		}

		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

		if (!isHalfTick) {
			ib::g_tick++;
		}
	}
};

class $modify(IBPlayLayer, PlayLayer) {
	void resetLevel() {
		ib::reset();
		PlayLayer::resetLevel();
	}

	void onQuit() {
		ib::reset();
		PlayLayer::onQuit();
	}
};

$on_mod(Loaded) {
	log::info("Input Buffer loaded: {} ticks", ib::delayTicks());
}
