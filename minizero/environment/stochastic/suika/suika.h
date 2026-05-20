#pragma once

#include "stochastic_env.h"
#include <chipmunk/chipmunk.h>
#include <string>
#include <vector>

namespace minizero::env::suika {

const std::string kSuikaName = "suika";
const int kSuikaNumPlayer = 1;
const int kSuikaActionSize = 32;
const int kSuikaChanceEventSize = 4;
const int kSuikaNumFruitLevels = 11;
const int kSuikaDiscreteValueSize = 601;
const int kSuikaFeatureWidth = 24;
const int kSuikaFeatureHeight = 24;
const int kSuikaInputChannels = kSuikaNumFruitLevels + kSuikaChanceEventSize;

class SuikaAction : public BaseAction {
public:
    SuikaAction() : BaseAction() {}
    SuikaAction(int action_id, Player player) : BaseAction(action_id, player) {}
    SuikaAction(const std::vector<std::string>& action_string_args);

    Player nextPlayer() const override { return player_ == Player::kPlayer1 ? Player::kPlayerNone : Player::kPlayer1; }
    std::string toConsoleString() const override;
};

class SuikaChanceEvent : public SuikaAction {
public:
    SuikaChanceEvent() : SuikaAction(), level_(-1) {}
    explicit SuikaChanceEvent(int level);
    explicit SuikaChanceEvent(const BaseAction& action);

    inline int getLevel() const { return level_; }

private:
    int level_;
};

class SuikaEnv : public StochasticEnv<SuikaAction> {
public:
    SuikaEnv();
    SuikaEnv(const SuikaEnv& env);
    SuikaEnv& operator=(const SuikaEnv& env);
    ~SuikaEnv() override;

    void reset() override { reset(utils::Random::randInt()); }
    void reset(int seed) override;
    bool act(const SuikaAction& action, bool with_chance = true) override;
    bool act(const std::vector<std::string>& action_string_args, bool with_chance = true) override { return act(SuikaAction(action_string_args), with_chance); }
    bool actChanceEvent(const SuikaAction& action) override;
    bool actChanceEvent();

    std::vector<SuikaAction> getLegalActions() const override;
    std::vector<SuikaAction> getLegalChanceEvents() const override;
    bool isLegalAction(const SuikaAction& action) const override;
    bool isLegalChanceEvent(const SuikaAction& action) const override;
    bool isTerminal() const override { return game_over_; }
    float getChanceEventProbability(const SuikaAction& action) const override;

    int getRotatePosition(int position, utils::Rotation rotation) const override;
    int getRotateAction(int action_id, utils::Rotation rotation) const override;
    int getRotateChanceEvent(int event_id, utils::Rotation rotation) const override { return event_id; }

    std::vector<float> getFeatures(utils::Rotation rotation = utils::Rotation::kRotationNone) const override;
    std::vector<float> getActionFeatures(const SuikaAction& action, utils::Rotation rotation = utils::Rotation::kRotationNone) const override;
    std::vector<float> getChanceEventFeatures(const SuikaAction& event, utils::Rotation rotation = utils::Rotation::kRotationNone) const override;

    inline int getNumInputChannels() const override { return kSuikaInputChannels; }
    inline int getNumActionFeatureChannels() const override { return kSuikaActionSize; }
    inline int getNumChanceEventFeatureChannels() const override { return kSuikaChanceEventSize; }
    inline int getInputChannelHeight() const override { return kSuikaFeatureHeight; }
    inline int getInputChannelWidth() const override { return kSuikaFeatureWidth; }
    inline int getHiddenChannelHeight() const override { return kSuikaFeatureHeight; }
    inline int getHiddenChannelWidth() const override { return kSuikaFeatureWidth; }
    inline int getPolicySize() const override { return kSuikaActionSize; }
    inline int getChanceEventSize() const override { return kSuikaChanceEventSize; }
    inline int getDiscreteValueSize() const override { return kSuikaDiscreteValueSize; }

    std::string toString() const override;
    std::string name() const override { return kSuikaName; }
    int getNumPlayer() const override { return kSuikaNumPlayer; }
    float getReward() const override { return reward_; }
    float getEvalScore(bool is_resign = false) const override { return total_reward_; }

    static void setUpEnv()
    {
        config::env_board_size = kSuikaFeatureWidth;
        config::actor_use_random_rotation_features = true;
        config::learner_n_step_return = 10;
        config::zero_actor_intermediate_sequence_length = 200;
    }

private:
    struct Fruit {
        int id = -1;
        int level = 0;
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float radius = 0.0f;
        bool alive = true;
        cpBody* body = nullptr;
        cpShape* shape = nullptr;
    };

private:
    void copyFrom(const SuikaEnv& env);
    void clearPhysics();
    void initializePhysics();
    void setupWalls();
    void rebuildPhysicsFromState();
    void syncFruitsFromPhysics();
    void removeFruitPhysics(Fruit& fruit);
    int spawnFruit(int level, float x, float y);
    void simulateUntilStable();
    void simulateOneStep(float dt);
    void applyMerges();
    void updateDropState();
    void updateDeathState(float dt);
    bool hasDangerFruit() const;
    bool isFruitTouchingSomething(const Fruit& fruit) const;
    float getActionX(int action_id, int fruit_level) const;
    float getMinX(int fruit_level) const;
    float getMaxX(int fruit_level) const;
    int sampleChanceLevel();
    bool areAllBodiesSlow() const;
    bool pointInsideFruit(float x, float y, const Fruit& fruit) const;
    utils::Rotation normalizeRotation(utils::Rotation rotation) const;
    bool isHorizontalFlip(utils::Rotation rotation) const;

private:
    cpSpace* space_ = nullptr;
    std::vector<cpShape*> walls_;
    std::vector<Fruit> fruits_;
    int next_fruit_level_ = -1;
    int reward_ = 0;
    int total_reward_ = 0;
    bool game_over_ = false;
    float death_timer_ = 0.0f;
    int active_drop_id_ = -1;
    int next_fruit_id_ = 0;
};

class SuikaEnvLoader : public StochasticEnvLoader<SuikaAction, SuikaEnv> {
public:
    std::vector<float> getActionFeatures(const int pos, utils::Rotation rotation = utils::Rotation::kRotationNone) const override;
    std::vector<float> getValue(const int pos) const override;
    std::vector<float> getReward(const int pos) const override;
    float getPriority(const int pos) const override;

    std::string name() const override { return kSuikaName; }
    int getPolicySize() const override { return kSuikaActionSize; }
    int getRotatePosition(int position, utils::Rotation rotation) const override { return SuikaEnv().getRotatePosition(position, rotation); }
    int getRotateAction(int action_id, utils::Rotation rotation) const override { return SuikaEnv().getRotateAction(action_id, rotation); }

private:
    float calculateNStepValue(const int pos) const;
    std::vector<float> toDiscreteValue(float value) const;
};

} // namespace minizero::env::suika
