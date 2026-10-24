#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <array>
#include <cmath>

using namespace geode::prelude;

namespace {
    constexpr int kJumpButton = 1;
    constexpr float kFallbackDelta = 1.f / 240.f;
    constexpr int kCubeHorizon = 48;
    constexpr int kWaveHorizon = 38;
    constexpr int kMiniWaveHorizon = 28;

    struct PendingInput {
        bool active = false;
        int ticksRemaining = 0;
        bool state = false;
        bool oppositeQueued = false;
        bool isPlayer1 = true;
        int button = kJumpButton;
    };

    struct SimulationResult {
        int survived = 0;
        bool diedBeforeInput = false;
        bool supported = true;
    };

    std::array<PendingInput, 2> g_pending{};
    PlayerObject* g_ghost = nullptr;
    bool g_simulating = false;
    bool g_ghostDied = false;
    bool g_injecting = false;
    float g_frameDelta = kFallbackDelta;

    bool isEnabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    int getMaxDelay() {
        return std::clamp(Mod::get()->getSettingValue<int>("max-delay-frames"), 1, 8);
    }

    bool isSupportedPlayer(PlayerObject* player, PlayLayer* layer, bool isPlayer1) {
        return player
            && layer
            && isPlayer1
            && !layer->m_gameState.m_isDualMode
            && !player->m_isDead
            && !player->m_isShip
            && !player->m_isPlatformer
            && !player->m_isDashing;
    }

    bool isFlying(PlayerObject* player) {
        return player && (player->m_isDart || player->m_isBird || player->m_isSwing);
    }

    bool isCollisionRelevant(GameObject* object) {
        if (!object || object->m_isDisabled || object->m_isDisabled2) return false;
        auto type = static_cast<unsigned>(object->m_objectType);
        return type == static_cast<unsigned>(GameObjectType::Solid)
            || type == static_cast<unsigned>(GameObjectType::Hazard)
            || type == static_cast<unsigned>(GameObjectType::Slope)
            || type == static_cast<unsigned>(GameObjectType::CollisionObject)
            || type == static_cast<unsigned>(GameObjectType::AnimatedHazard);
    }

    PendingInput* pendingFor(bool isPlayer1) {
        return &g_pending[isPlayer1 ? 0u : 1u];
    }

    void resetPending() {
        for (auto& input : g_pending) input = PendingInput{};
    }

    void setGhostButton(PlayerObject* ghost, bool down) {
        if (down) ghost->pushButton(PlayerButton::Jump);
        else ghost->releaseButton(PlayerButton::Jump);
    }

    void copyPlayerState(PlayerObject* ghost, PlayerObject* real) {
        ghost->copyAttributes(real);
        ghost->setPosition(real->getPosition());
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDead = false;
        ghost->setVisible(false);
        ghost->m_playEffects = false;
        ghost->m_hasGroundParticles = false;
        ghost->m_hasShipParticles = false;
        ghost->removeAllParticles();
        ghost->releaseAllButtons();
        ghost->resetCollisionLog(true);
    }

    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->resetCollisionLog(true);
        layer->checkCollisions(ghost, dt, false);
    }

    // Returns how many simulation updates the player survives after the
    // requested transition. For delay > 0, the opposite button state is held
    // until the exact candidate frame, then the requested state remains held.
    SimulationResult simulate(PlayLayer* layer, PlayerObject* real, bool down, int delay, int horizon) {
        SimulationResult result;
        if (!layer || !real || !g_ghost || g_ghost == real) {
            result.supported = false;
            return result;
        }

        float dt = g_frameDelta;
        if (!std::isfinite(dt) || dt <= 0.f || dt > 0.05f) dt = kFallbackDelta;

        copyPlayerState(g_ghost, real);
        g_ghostDied = false;
        g_simulating = true;

        if (delay > 0) {
            setGhostButton(g_ghost, !down);
        }
        else {
            // The zero-delay baseline must include the requested input. Without
            // this, it only simulates the current trajectory and treats most
            // dangerous inputs as safe, so the candidate search never runs.
            setGhostButton(g_ghost, down);
        }

        const int total = delay + horizon;
        int airTicks = 0;
        int groundTicks = 0;
        bool flying = isFlying(real);

        for (int tick = 0; tick < total; ++tick) {
            if (tick == delay) setGhostButton(g_ghost, down);

            advanceGhost(g_ghost, dt);
            if (g_ghostDied || g_ghost->m_isDead) {
                result.diedBeforeInput = tick < delay;
                break;
            }

            checkGhost(layer, g_ghost, dt);
            if (g_ghostDied || g_ghost->m_isDead) {
                result.diedBeforeInput = tick < delay;
                break;
            }

            if (tick < delay) continue;
            ++result.survived;

            if (flying) continue;
            if (!g_ghost->m_isOnGround) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (airTicks < 6) continue;
            if (groundTicks > 2) {
                result.survived = horizon;
                break;
            }
            ++groundTicks;
        }

        g_simulating = false;
        return result;
    }

    // Search only the user's configured 1–8 update window. A delay is selected
    // only when the no-delay path dies sooner and the delayed path survives a
    // meaningful additional section; otherwise the original input passes now.
    int findSafeDelay(PlayLayer* layer, PlayerObject* player, bool down) {
        bool wave = player->m_isDart;
        bool miniWave = wave && player->m_vehicleSize < 1.f;
        const int horizon = miniWave ? kMiniWaveHorizon : (wave ? kWaveHorizon : kCubeHorizon);

        auto immediate = simulate(layer, player, down, 0, horizon);
        if (!immediate.supported || immediate.survived >= horizon) return 0;

        const int maxDelay = getMaxDelay();
        if (!wave) {
            const int target = std::min(horizon, immediate.survived + 12);
            for (int delay = 1; delay <= maxDelay; ++delay) {
                auto candidate = simulate(layer, player, down, delay, horizon);
                if (!candidate.supported || candidate.diedBeforeInput) break;
                if (candidate.survived >= target) return delay;
            }
        }
        else {
            int target = immediate.survived + 10;
            int gain = 6;
            for (int delay = 1; delay <= maxDelay; ++delay) {
                auto candidate = simulate(layer, player, down, delay, horizon);
                if (!candidate.supported || candidate.diedBeforeInput) break;
                bool safeImprovement = candidate.survived < horizon
                    ? candidate.survived >= target
                    : (candidate.survived - immediate.survived >= gain || candidate.survived >= target);
                if (safeImprovement) return delay;
                target += 2;
                gain += 2;
            }
        }
        return 0;
    }

    void sendButton(GJBaseGameLayer* layer, bool down, bool isPlayer1, int button) {
        if (!layer) return;
        g_injecting = true;
        layer->handleButton(down, button, isPlayer1);
        g_injecting = false;
    }

    void flushOne(GJBaseGameLayer* layer, PendingInput& input) {
        if (!input.active) return;
        bool sendOppositeNext = input.oppositeQueued;
        bool nextState = !input.state;
        input.active = false;
        input.oppositeQueued = false;
        sendButton(layer, input.state, input.isPlayer1, input.button);
        if (sendOppositeNext) {
            input.active = true;
            input.ticksRemaining = 1;
            input.state = nextState;
        }
    }

    void flushAll(GJBaseGameLayer* layer) {
        for (auto& input : g_pending) flushOne(layer, input);
    }

    bool interceptInput(GJBaseGameLayer* self, bool down, int button, bool isPlayer1) {
        if (!isEnabled() || button != kJumpButton || !isPlayer1) return false;

        auto* pending = pendingFor(isPlayer1);
        if (pending->active) {
            pending->oppositeQueued = pending->state != down;
            return true;
        }

        auto* layer = PlayLayer::get();
        auto* player = self->m_player1;
        if (!layer || static_cast<GJBaseGameLayer*>(layer) != self
            || !isSupportedPlayer(player, layer, isPlayer1)) {
            return false;
        }

        // Cube releases do not create a new jump; robot and wave releases do
        // change the trajectory and are therefore tested like presses.
        if (!down && !player->m_isRobot && !player->m_isDart) return false;

        const int delay = findSafeDelay(layer, player, down);
        if (delay <= 0) return false;

        pending->active = true;
        pending->ticksRemaining = delay;
        pending->state = down;
        pending->oppositeQueued = false;
        pending->isPlayer1 = isPlayer1;
        pending->button = button;
        return true;
    }
}

class $modify(PredictiveInputBufferPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (g_ghost && g_ghost->getParent()) g_ghost->removeFromParentAndCleanup(true);
        g_ghost = nullptr;
        if (m_objectLayer) {
            g_ghost = PlayerObject::create(1, 1, this, this, true);
            if (g_ghost) {
                g_ghost->setVisible(false);
                g_ghost->m_playEffects = false;
                g_ghost->m_hasGroundParticles = false;
                g_ghost->m_hasShipParticles = false;
                m_objectLayer->addChild(g_ghost);
            }
        }
        resetPending();
    }

    void resetLevel() {
        resetPending();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        auto* ghost = g_ghost;
        g_ghost = nullptr;
        resetPending();
        if (ghost && ghost->getParent()) ghost->removeFromParentAndCleanup(true);
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_simulating && player == g_ghost) {
            g_ghostDied = true;
            player->m_isDead = true;
            return;
        }
        if (player == m_player1) resetPending();
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (!g_simulating) PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(PredictiveInputBufferGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_simulating || g_injecting) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (!isEnabled()) {
            flushAll(this);
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (interceptInput(this, down, button, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!g_simulating || player != g_ghost) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        if (!objects) return;
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        int limit = std::clamp(objectCount, 0, static_cast<int>(objects->size()));
        if (static_cast<int>(relevant.capacity()) < limit) relevant.reserve(static_cast<size_t>(limit));
        for (int index = 0; index < limit; ++index) {
            auto* object = (*objects)[static_cast<size_t>(index)];
            if (isCollisionRelevant(object)) relevant.push_back(object);
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_simulating) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (g_simulating) return;
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (g_simulating) return;
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (!g_simulating) GJBaseGameLayer::activateSFXTrigger(object);
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (!g_simulating) GJBaseGameLayer::activateSongEditTrigger(object);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (!g_simulating) GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }
};

class $modify(PredictiveInputBufferEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int id, gd::vector<int> const* groups) {
        if (!g_simulating) EffectGameObject::triggerObject(layer, id, groups);
    }
};

class $modify(PredictiveInputBufferGameObject, GameObject) {
    void playShineEffect() {
        if (!g_simulating) GameObject::playShineEffect();
    }
};

class $modify(PredictiveInputBufferHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (!g_simulating) HardStreak::addPoint(point);
    }
};

class $modify(PredictiveInputBufferPlayerObject, PlayerObject) {
    void update(float dt) {
        if (!g_simulating) {
            auto* layer = PlayLayer::get();
            if (layer && this == layer->m_player1 && std::isfinite(dt) && dt > 0.f) {
                float warp = layer->m_gameState.m_timeWarp;
                g_frameDelta = warp > 0.f ? dt / warp : dt;
            }
            if (layer && this == layer->m_player1) {
                if (!isEnabled()) {
                    flushAll(layer);
                }
                else {
                    for (auto& input : g_pending) {
                        if (!input.active) continue;
                        if (input.ticksRemaining > 0) --input.ticksRemaining;
                        if (input.ticksRemaining <= 0) flushOne(layer, input);
                    }
                }
            }
        }
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        if (!g_simulating) PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (!g_simulating) PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (!g_simulating) PlayerObject::ringJump(ring, skipCheck);
    }
};

$on_mod(Loaded) {
    log::info(
        "[PredictiveInputBuffer] Loaded. Enabled: {}; maximum candidate delay: {} updates.",
        isEnabled(), getMaxDelay()
    );
}
