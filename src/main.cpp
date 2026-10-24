#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

// Simple input buffer.
// The click is ALWAYS passed to the game untouched (so orbs, pads, portals etc.
// behave normally). If the click happened while a ground-based player was in the
// air, we also remember it for a few ticks. The moment the player touches the
// ground inside that window, we fire the click for them.
class $modify(BufferedPlayer, PlayerObject) {
	struct Fields {
		bool pending = false;   // a click is waiting for the ground
		bool held = false;      // jump button is currently held down
		bool firing = false;    // true while WE are sending a click
		int ticksLeft = 0;      // remaining buffer window
	};

	bool groundMode() {
		// Ship, UFO, wave and swing don't need landing to click.
		return !(m_isShip || m_isBird || m_isDart || m_isSwing);
	}

	bool bufferActive() {
		if (!Mod::get()->getSettingValue<bool>("enabled")) return false;
		auto pl = PlayLayer::get();
		if (!pl) return false;
		return this == pl->m_player1 || this == pl->m_player2;
	}

	bool pushButton(PlayerButton button) {
		bool ret = PlayerObject::pushButton(button);
		if (button != PlayerButton::Jump || m_fields->firing) return ret;

		m_fields->held = true;

		if (bufferActive() && groundMode() && !m_isOnGround) {
			m_fields->pending = true;
			m_fields->ticksLeft = static_cast<int>(
				Mod::get()->getSettingValue<int64_t>("buffer-ticks")
			);
		}
		return ret;
	}

	bool releaseButton(PlayerButton button) {
		if (button == PlayerButton::Jump && !m_fields->firing) {
			m_fields->held = false;
		}
		return PlayerObject::releaseButton(button);
	}

	void update(float dt) {
		PlayerObject::update(dt);

		if (!m_fields->pending) return;

		if (!bufferActive()) {
			m_fields->pending = false;
			return;
		}

		if (m_isOnGround && groundMode()) {
			m_fields->pending = false;
			m_fields->firing = true;

			// Re-click cleanly: release first if still held, then press.
			if (m_fields->held) PlayerObject::releaseButton(PlayerButton::Jump);
			PlayerObject::pushButton(PlayerButton::Jump);
			// If the player already let go, finish the tap.
			if (!m_fields->held) PlayerObject::releaseButton(PlayerButton::Jump);

			m_fields->firing = false;
			return;
		}

		if (--m_fields->ticksLeft <= 0) {
			m_fields->pending = false;
		}
	}
};
