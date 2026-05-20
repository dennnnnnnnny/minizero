#if SUIKA

#include "suika.h"
#include "random.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace minizero::env::suika {

using namespace minizero::utils;

namespace {

constexpr float kWidth = 575.0f;
constexpr float kHeight = 700.0f;
constexpr float kSideMargin = 60.0f;
constexpr float kBottomMargin = 50.0f;
constexpr float kDeadlineY = 200.0f;
constexpr float kGravity = 1000.0f;
constexpr float kDropSpawnY = kDeadlineY - 70.0f;
constexpr float kFloorY = kHeight - kBottomMargin;
constexpr float kLeftWallX = kSideMargin;
constexpr float kRightWallX = kWidth - kSideMargin;
constexpr float kFeatureTopY = kDeadlineY;
constexpr float kFeatureBottomY = kFloorY;
constexpr float kDt = 1.0f / 60.0f;
constexpr int kMaxSimulationTicks = 480;
constexpr int kStableTickThreshold = 20;
constexpr float kWallElasticity = 0.2f;
constexpr float kWallFriction = 0.8f;
constexpr float kFruitElasticity = 0.1f;
constexpr float kFruitFriction = 0.6f;
constexpr float kSleepVelocity = 15.0f;
constexpr float kDangerVelocity = 50.0f;
constexpr float kDropReleaseVelocity = 5.0f;
constexpr float kMergeSlack = -1.0f;
constexpr float kDistanceEpsilon = 1e-4f;

constexpr int kFruitRadii[kSuikaNumFruitLevels] = {15, 22, 30, 36, 45, 55, 65, 78, 90, 105, 120};
constexpr int kTriangleScores[kSuikaNumFruitLevels + 1] = {0, 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 66};

inline cpVect toChipmunk(float x, float y) { return cpv(x, y); }

} // namespace

SuikaAction::SuikaAction(const std::vector<std::string>& action_string_args)
{
    if (action_string_args.empty()) { return; }

    const std::string& token = action_string_args[0];
    if (token.rfind("drop@", 0) == 0) {
        action_id_ = std::stoi(token.substr(5));
        player_ = Player::kPlayer1;
    } else if (token.rfind("next@", 0) == 0) {
        action_id_ = kSuikaActionSize + std::stoi(token.substr(5));
        player_ = Player::kPlayerNone;
    } else if (!token.empty() && std::all_of(token.begin(), token.end(), ::isdigit)) {
        action_id_ = std::stoi(token);
        player_ = (action_id_ < kSuikaActionSize ? Player::kPlayer1 : Player::kPlayerNone);
    }
}

std::string SuikaAction::toConsoleString() const
{
    if (action_id_ >= 0 && action_id_ < kSuikaActionSize) {
        return "drop@" + std::to_string(action_id_);
    }
    if (action_id_ >= kSuikaActionSize && action_id_ < kSuikaActionSize + kSuikaChanceEventSize) {
        return "next@" + std::to_string(action_id_ - kSuikaActionSize);
    }
    return "null";
}

SuikaChanceEvent::SuikaChanceEvent(int level)
    : SuikaAction(kSuikaActionSize + level, Player::kPlayerNone), level_(level)
{
}

SuikaChanceEvent::SuikaChanceEvent(const BaseAction& action)
    : SuikaAction(action.getActionID(), action.getPlayer()), level_(action.getActionID() - kSuikaActionSize)
{
    if (level_ < 0 || level_ >= kSuikaChanceEventSize) { level_ = -1; }
}

SuikaEnv::SuikaEnv()
{
    initializePhysics();
}

SuikaEnv::SuikaEnv(const SuikaEnv& env)
{
    copyFrom(env);
}

SuikaEnv& SuikaEnv::operator=(const SuikaEnv& env)
{
    if (this != &env) {
        clearPhysics();
        copyFrom(env);
    }
    return *this;
}

SuikaEnv::~SuikaEnv()
{
    clearPhysics();
}

void SuikaEnv::reset(int seed)
{
    clearPhysics();
    initializePhysics();
    random_.seed(seed_ = seed);
    actions_.clear();
    events_.clear();
    observations_.clear();
    fruits_.clear();
    reward_ = 0;
    total_reward_ = 0;
    game_over_ = false;
    death_timer_ = 0.0f;
    active_drop_id_ = -1;
    next_fruit_id_ = 0;
    next_fruit_level_ = -1;

    turn_ = Player::kPlayerNone;
    actChanceEvent();
}

bool SuikaEnv::act(const SuikaAction& action, bool with_chance /* = true */)
{
    if (!isLegalAction(action)) { return false; }

    const int previous_score = total_reward_;
    const float spawn_x = getActionX(action.getActionID(), next_fruit_level_);
    active_drop_id_ = spawnFruit(next_fruit_level_, spawn_x, kDropSpawnY);
    reward_ = 0;

    // Run the continuous simulation until the board is settled.
    simulateUntilStable();

    actions_.push_back(action);
    reward_ = total_reward_ - previous_score;

    if (!game_over_) {
        turn_ = Player::kPlayerNone;
        if (with_chance) { actChanceEvent(); }
    } else {
        turn_ = Player::kPlayerNone;
        next_fruit_level_ = -1;
    }

    return true;
}

bool SuikaEnv::actChanceEvent(const SuikaAction& action)
{
    if (!isLegalChanceEvent(action)) { return false; }

    const SuikaChanceEvent event(action);
    next_fruit_level_ = event.getLevel();
    events_.push_back(event);
    turn_ = Player::kPlayer1;
    return true;
}

bool SuikaEnv::actChanceEvent()
{
    if (turn_ != Player::kPlayerNone || game_over_) { return false; }
    return actChanceEvent(SuikaChanceEvent(sampleChanceLevel()));
}

std::vector<SuikaAction> SuikaEnv::getLegalActions() const
{
    if (turn_ != Player::kPlayer1 || game_over_) { return {}; }

    std::vector<SuikaAction> actions;
    actions.reserve(kSuikaActionSize);
    for (int action_id = 0; action_id < kSuikaActionSize; ++action_id) {
        actions.emplace_back(action_id, Player::kPlayer1);
    }
    return actions;
}

std::vector<SuikaAction> SuikaEnv::getLegalChanceEvents() const
{
    if (turn_ != Player::kPlayerNone || game_over_) { return {}; }

    std::vector<SuikaAction> events;
    events.reserve(kSuikaChanceEventSize);
    for (int level = 0; level < kSuikaChanceEventSize; ++level) { events.emplace_back(kSuikaActionSize + level, Player::kPlayerNone); }
    return events;
}

bool SuikaEnv::isLegalAction(const SuikaAction& action) const
{
    return turn_ == Player::kPlayer1 && !isTerminal() && action.getPlayer() == Player::kPlayer1 && action.getActionID() >= 0 && action.getActionID() < kSuikaActionSize && next_fruit_level_ >= 0;
}

bool SuikaEnv::isLegalChanceEvent(const SuikaAction& action) const
{
    const int level = action.getActionID() - kSuikaActionSize;
    return turn_ == Player::kPlayerNone && !game_over_ && action.getPlayer() == Player::kPlayerNone && level >= 0 && level < kSuikaChanceEventSize;
}

float SuikaEnv::getChanceEventProbability(const SuikaAction& action) const
{
    return isLegalChanceEvent(action) ? 1.0f / kSuikaChanceEventSize : 0.0f;
}

int SuikaEnv::getRotatePosition(int position, utils::Rotation rotation) const
{
    rotation = normalizeRotation(rotation);
    if (!isHorizontalFlip(rotation) || position < 0 || position >= kSuikaFeatureWidth * kSuikaFeatureHeight) { return position; }
    const int row = position / kSuikaFeatureWidth;
    const int col = position % kSuikaFeatureWidth;
    return row * kSuikaFeatureWidth + (kSuikaFeatureWidth - 1 - col);
}

int SuikaEnv::getRotateAction(int action_id, utils::Rotation rotation) const
{
    rotation = normalizeRotation(rotation);
    if (!isHorizontalFlip(rotation) || action_id < 0 || action_id >= kSuikaActionSize) { return action_id; }
    return kSuikaActionSize - 1 - action_id;
}

std::vector<float> SuikaEnv::getFeatures(utils::Rotation rotation /* = utils::Rotation::kRotationNone */) const
{
    rotation = normalizeRotation(rotation);
    const int hidden_size = kSuikaFeatureWidth * kSuikaFeatureHeight;
    std::vector<float> features(kSuikaInputChannels * hidden_size, 0.0f);
    const bool flip = isHorizontalFlip(rotation);

    // Crop to the playable container. The top row is the death line, so no extra
    // deadline channel is needed.
    for (int row = 0; row < kSuikaFeatureHeight; ++row) {
        for (int col = 0; col < kSuikaFeatureWidth; ++col) {
            const int source_col = flip ? (kSuikaFeatureWidth - 1 - col) : col;
            const float world_x = kLeftWallX + (source_col + 0.5f) * (kRightWallX - kLeftWallX) / kSuikaFeatureWidth;
            const float world_y = kFeatureTopY + (row + 0.5f) * (kFeatureBottomY - kFeatureTopY) / kSuikaFeatureHeight;
            const int offset = row * kSuikaFeatureWidth + col;

            for (const Fruit& fruit : fruits_) {
                if (!fruit.alive) { continue; }
                if (pointInsideFruit(world_x, world_y, fruit)) {
                    features[fruit.level * hidden_size + offset] = 1.0f;
                }
            }
        }
    }

    if (next_fruit_level_ >= 0 && next_fruit_level_ < kSuikaChanceEventSize) {
        const int base = (kSuikaNumFruitLevels + next_fruit_level_) * hidden_size;
        std::fill(features.begin() + base, features.begin() + base + hidden_size, 1.0f);
    }

    return features;
}

std::vector<float> SuikaEnv::getActionFeatures(const SuikaAction& action, utils::Rotation rotation /* = utils::Rotation::kRotationNone */) const
{
    rotation = normalizeRotation(rotation);
    const int hidden_size = kSuikaFeatureWidth * kSuikaFeatureHeight;
    std::vector<float> action_features(kSuikaActionSize * hidden_size, 0.0f);
    if (action.getActionID() < 0 || action.getActionID() >= kSuikaActionSize) { return action_features; }

    const int rotated_action_id = getRotateAction(action.getActionID(), rotation);
    const int column = static_cast<int>(std::lround(rotated_action_id * (kSuikaFeatureWidth - 1.0f) / std::max(1, kSuikaActionSize - 1)));
    const int base = rotated_action_id * hidden_size;
    for (int row = 0; row < kSuikaFeatureHeight; ++row) {
        action_features[base + row * kSuikaFeatureWidth + column] = 1.0f;
    }
    return action_features;
}

std::vector<float> SuikaEnv::getChanceEventFeatures(const SuikaAction& event, utils::Rotation rotation /* = utils::Rotation::kRotationNone */) const
{
    rotation = normalizeRotation(rotation);
    (void)rotation;

    const int hidden_size = kSuikaFeatureWidth * kSuikaFeatureHeight;
    std::vector<float> event_features(kSuikaChanceEventSize * hidden_size, 0.0f);
    const int level = event.getActionID() - kSuikaActionSize;
    if (level < 0 || level >= kSuikaChanceEventSize) { return event_features; }

    const int base = level * hidden_size;
    std::fill(event_features.begin() + base, event_features.begin() + base + hidden_size, 1.0f);
    return event_features;
}

std::string SuikaEnv::toString() const
{
    std::ostringstream oss;
    oss << "SuikaEnv(score=" << total_reward_ << ", next=" << next_fruit_level_ << ", fruits=" << fruits_.size() << ", terminal=" << game_over_ << ")\n";
    oss << "deadline_y=" << kDeadlineY << ", active_drop_id=" << active_drop_id_ << "\n";
    for (const Fruit& fruit : fruits_) {
        if (!fruit.alive) { continue; }
        oss << "  id=" << fruit.id << " level=" << fruit.level << " pos=(" << std::fixed << std::setprecision(1) << fruit.x << "," << fruit.y << ")";
        oss << " vel=(" << fruit.vx << "," << fruit.vy << ") r=" << fruit.radius << "\n";
    }
    return oss.str();
}

void SuikaEnv::copyFrom(const SuikaEnv& env)
{
    seed_ = env.seed_;
    random_ = env.random_;
    turn_ = env.turn_;
    actions_ = env.actions_;
    events_ = env.events_;
    observations_ = env.observations_;
    fruits_ = env.fruits_;
    for (Fruit& fruit : fruits_) {
        fruit.body = nullptr;
        fruit.shape = nullptr;
    }
    next_fruit_level_ = env.next_fruit_level_;
    reward_ = env.reward_;
    total_reward_ = env.total_reward_;
    game_over_ = env.game_over_;
    death_timer_ = env.death_timer_;
    active_drop_id_ = env.active_drop_id_;
    next_fruit_id_ = env.next_fruit_id_;
    initializePhysics();
    rebuildPhysicsFromState();
}

void SuikaEnv::clearPhysics()
{
    if (!space_) { return; }

    for (Fruit& fruit : fruits_) { removeFruitPhysics(fruit); }
    for (cpShape* wall : walls_) {
        if (!wall) { continue; }
        cpSpaceRemoveShape(space_, wall);
        cpShapeFree(wall);
    }
    walls_.clear();
    cpSpaceFree(space_);
    space_ = nullptr;
}

void SuikaEnv::initializePhysics()
{
    if (space_) { return; }
    space_ = cpSpaceNew();
    cpSpaceSetGravity(space_, toChipmunk(0.0f, kGravity));
    cpSpaceSetIterations(space_, 20);
    setupWalls();
}

void SuikaEnv::setupWalls()
{
    const float wall_radius = 10.0f;
    const float left_x = kLeftWallX - wall_radius;
    const float right_x = kRightWallX + wall_radius;
    const float bottom_y = kFloorY + wall_radius;

    std::vector<std::pair<cpVect, cpVect>> wall_segments = {
        {toChipmunk(left_x, -1000.0f), toChipmunk(left_x, bottom_y)},
        {toChipmunk(right_x, -1000.0f), toChipmunk(right_x, bottom_y)},
        {toChipmunk(left_x, bottom_y), toChipmunk(right_x, bottom_y)},
    };

    for (const auto& segment : wall_segments) {
        cpShape* wall = cpSegmentShapeNew(cpSpaceGetStaticBody(space_), segment.first, segment.second, wall_radius);
        cpShapeSetElasticity(wall, kWallElasticity);
        cpShapeSetFriction(wall, kWallFriction);
        cpSpaceAddShape(space_, wall);
        walls_.push_back(wall);
    }
}

void SuikaEnv::rebuildPhysicsFromState()
{
    for (Fruit& fruit : fruits_) {
        if (!fruit.alive) { continue; }
        const float mass = 1.0f + fruit.level * 0.5f;
        const cpFloat moment = cpMomentForCircle(mass, 0.0, fruit.radius, cpvzero);
        fruit.body = cpBodyNew(mass, moment);
        cpBodySetPosition(fruit.body, toChipmunk(fruit.x, fruit.y));
        cpBodySetVelocity(fruit.body, toChipmunk(fruit.vx, fruit.vy));
        fruit.shape = cpCircleShapeNew(fruit.body, fruit.radius, cpvzero);
        cpShapeSetElasticity(fruit.shape, kFruitElasticity);
        cpShapeSetFriction(fruit.shape, kFruitFriction);
        cpSpaceAddBody(space_, fruit.body);
        cpSpaceAddShape(space_, fruit.shape);
    }
}

void SuikaEnv::syncFruitsFromPhysics()
{
    for (Fruit& fruit : fruits_) {
        if (!fruit.alive || !fruit.body) { continue; }
        const cpVect pos = cpBodyGetPosition(fruit.body);
        const cpVect vel = cpBodyGetVelocity(fruit.body);
        fruit.x = static_cast<float>(pos.x);
        fruit.y = static_cast<float>(pos.y);
        fruit.vx = static_cast<float>(vel.x);
        fruit.vy = static_cast<float>(vel.y);
    }
}

void SuikaEnv::removeFruitPhysics(Fruit& fruit)
{
    if (fruit.shape) {
        if (cpSpaceContainsShape(space_, fruit.shape)) { cpSpaceRemoveShape(space_, fruit.shape); }
        cpShapeFree(fruit.shape);
        fruit.shape = nullptr;
    }
    if (fruit.body) {
        if (cpSpaceContainsBody(space_, fruit.body)) { cpSpaceRemoveBody(space_, fruit.body); }
        cpBodyFree(fruit.body);
        fruit.body = nullptr;
    }
}

int SuikaEnv::spawnFruit(int level, float x, float y)
{
    Fruit fruit;
    fruit.id = next_fruit_id_++;
    fruit.level = std::clamp(level, 0, kSuikaNumFruitLevels - 1);
    fruit.radius = static_cast<float>(kFruitRadii[fruit.level]);
    fruit.x = std::clamp(x, getMinX(fruit.level), getMaxX(fruit.level));
    fruit.y = y;
    fruit.vx = 0.0f;
    fruit.vy = 0.0f;
    fruit.alive = true;
    fruits_.push_back(fruit);
    Fruit& stored_fruit = fruits_.back();
    const float mass = 1.0f + stored_fruit.level * 0.5f;
    const cpFloat moment = cpMomentForCircle(mass, 0.0, stored_fruit.radius, cpvzero);
    stored_fruit.body = cpBodyNew(mass, moment);
    cpBodySetPosition(stored_fruit.body, toChipmunk(stored_fruit.x, stored_fruit.y));
    stored_fruit.shape = cpCircleShapeNew(stored_fruit.body, stored_fruit.radius, cpvzero);
    cpShapeSetElasticity(stored_fruit.shape, kFruitElasticity);
    cpShapeSetFriction(stored_fruit.shape, kFruitFriction);
    cpSpaceAddBody(space_, stored_fruit.body);
    cpSpaceAddShape(space_, stored_fruit.shape);
    return fruit.id;
}

void SuikaEnv::simulateUntilStable()
{
    int stable_ticks = 0;
    death_timer_ = 0.0f;

    for (int tick = 0; tick < kMaxSimulationTicks; ++tick) {
        simulateOneStep(kDt);
        if (game_over_) { break; }

        if (areAllBodiesSlow()) {
            ++stable_ticks;
            if (stable_ticks >= kStableTickThreshold) { break; }
        } else {
            stable_ticks = 0;
        }
    }

    active_drop_id_ = -1;
}

void SuikaEnv::simulateOneStep(float dt)
{
    cpSpaceStep(space_, dt);
    syncFruitsFromPhysics();
    applyMerges();
    updateDropState();
    updateDeathState(dt);
}

void SuikaEnv::applyMerges()
{
    std::vector<int> removal_indices;
    std::vector<std::pair<int, std::pair<float, float>>> additions;
    std::vector<bool> reserved(fruits_.size(), false);

    for (size_t i = 0; i < fruits_.size(); ++i) {
        if (!fruits_[i].alive || reserved[i]) { continue; }
        for (size_t j = i + 1; j < fruits_.size(); ++j) {
            if (!fruits_[j].alive || reserved[j]) { continue; }
            if (fruits_[i].level != fruits_[j].level) { continue; }

            const float dx = fruits_[j].x - fruits_[i].x;
            const float dy = fruits_[j].y - fruits_[i].y;
            const float min_dist = fruits_[i].radius + fruits_[j].radius - kMergeSlack;
            if (dx * dx + dy * dy > min_dist * min_dist) { continue; }

            reserved[i] = reserved[j] = true;
            removal_indices.push_back(static_cast<int>(i));
            removal_indices.push_back(static_cast<int>(j));

            const int new_level = fruits_[i].level + 1;
            const float merged_x = (fruits_[i].x + fruits_[j].x) * 0.5f;
            const float merged_y = (fruits_[i].y + fruits_[j].y) * 0.5f;
            if (new_level < kSuikaNumFruitLevels) { additions.push_back({new_level, {merged_x, merged_y}}); }
            if (new_level <= kSuikaNumFruitLevels) { total_reward_ += kTriangleScores[new_level]; }

            if (fruits_[i].id == active_drop_id_ || fruits_[j].id == active_drop_id_) { active_drop_id_ = -1; }
            break;
        }
    }

    if (removal_indices.empty()) { return; }
    std::sort(removal_indices.begin(), removal_indices.end());
    removal_indices.erase(std::unique(removal_indices.begin(), removal_indices.end()), removal_indices.end());
    for (int index : removal_indices) {
        fruits_[index].alive = false;
        removeFruitPhysics(fruits_[index]);
    }

    std::vector<Fruit> next_fruits;
    next_fruits.reserve(fruits_.size() + additions.size());
    for (const Fruit& fruit : fruits_) {
        if (fruit.alive) { next_fruits.push_back(fruit); }
    }
    fruits_.swap(next_fruits);

    for (const auto& [level, pos] : additions) {
        const int id = spawnFruit(level, pos.first, pos.second);
        for (Fruit& fruit : fruits_) {
            if (fruit.id == id) {
                fruit.vx = 0.0f;
                fruit.vy = 0.0f;
                break;
            }
        }
    }
}

void SuikaEnv::updateDropState()
{
    if (active_drop_id_ < 0) { return; }
    auto it = std::find_if(fruits_.begin(), fruits_.end(), [&](const Fruit& fruit) { return fruit.id == active_drop_id_; });
    if (it == fruits_.end()) {
        active_drop_id_ = -1;
        return;
    }

    const float speed = std::sqrt(it->vx * it->vx + it->vy * it->vy);
    if (speed < kDropReleaseVelocity || it->y >= kFloorY - 10.0f || isFruitTouchingSomething(*it)) { active_drop_id_ = -1; }
}

void SuikaEnv::updateDeathState(float dt)
{
    if (hasDangerFruit()) {
        death_timer_ += dt;
        if (death_timer_ > 1.0f) { game_over_ = true; }
    } else {
        death_timer_ = 0.0f;
    }
}

bool SuikaEnv::hasDangerFruit() const
{
    for (const Fruit& fruit : fruits_) {
        if (!fruit.alive) { continue; }
        if (fruit.id == active_drop_id_ && fruit.y - fruit.radius < kDeadlineY) { continue; }
        const float speed = std::sqrt(fruit.vx * fruit.vx + fruit.vy * fruit.vy);
        if (fruit.y - fruit.radius < kDeadlineY && speed < kDangerVelocity) { return true; }
    }
    return false;
}

bool SuikaEnv::isFruitTouchingSomething(const Fruit& fruit) const
{
    if (fruit.x <= getMinX(fruit.level) + 0.5f || fruit.x >= getMaxX(fruit.level) - 0.5f) { return true; }
    if (fruit.y >= kFloorY - fruit.radius - 0.5f) { return true; }

    for (const Fruit& other : fruits_) {
        if (!other.alive || other.id == fruit.id) { continue; }
        const float dx = other.x - fruit.x;
        const float dy = other.y - fruit.y;
        const float min_dist = fruit.radius + other.radius + 0.5f;
        if (dx * dx + dy * dy <= min_dist * min_dist) { return true; }
    }
    return false;
}

float SuikaEnv::getActionX(int action_id, int fruit_level) const
{
    const float min_x = getMinX(fruit_level);
    const float max_x = getMaxX(fruit_level);
    if (kSuikaActionSize == 1) { return (min_x + max_x) * 0.5f; }
    return min_x + (max_x - min_x) * action_id / static_cast<float>(kSuikaActionSize - 1);
}

float SuikaEnv::getMinX(int fruit_level) const
{
    return kLeftWallX + static_cast<float>(kFruitRadii[std::clamp(fruit_level, 0, kSuikaNumFruitLevels - 1)]);
}

float SuikaEnv::getMaxX(int fruit_level) const
{
    return kRightWallX - static_cast<float>(kFruitRadii[std::clamp(fruit_level, 0, kSuikaNumFruitLevels - 1)]);
}

int SuikaEnv::sampleChanceLevel()
{
    return std::uniform_int_distribution<int>(0, kSuikaChanceEventSize - 1)(random_);
}

bool SuikaEnv::areAllBodiesSlow() const
{
    for (const Fruit& fruit : fruits_) {
        if (!fruit.alive) { continue; }
        const float speed = std::sqrt(fruit.vx * fruit.vx + fruit.vy * fruit.vy);
        if (speed > kSleepVelocity) { return false; }
    }
    return true;
}

bool SuikaEnv::pointInsideFruit(float x, float y, const Fruit& fruit) const
{
    const float dx = x - fruit.x;
    const float dy = y - fruit.y;
    return dx * dx + dy * dy <= fruit.radius * fruit.radius;
}

utils::Rotation SuikaEnv::normalizeRotation(utils::Rotation rotation) const
{
    // Suika only has left-right symmetry. Unsupported rotations are no-ops so
    // MiniZero's generic rotation sampler can remain game-agnostic.
    return rotation == utils::Rotation::kHorizontalRotation180 ? rotation : utils::Rotation::kRotationNone;
}

bool SuikaEnv::isHorizontalFlip(utils::Rotation rotation) const
{
    return rotation == utils::Rotation::kHorizontalRotation180;
}

std::vector<float> SuikaEnvLoader::getActionFeatures(const int pos, utils::Rotation rotation /* = utils::Rotation::kRotationNone */) const
{
    SuikaAction action;
    if (pos < static_cast<int>(action_pairs_.size())) { action = action_pairs_[pos].first; }
    return SuikaEnv().getActionFeatures(action, rotation);
}

std::vector<float> SuikaEnvLoader::getValue(const int pos) const
{
    if (pos >= static_cast<int>(action_pairs_.size())) { return toDiscreteValue(0.0f); }
    return toDiscreteValue(utils::transformValue(calculateNStepValue(pos)));
}

std::vector<float> SuikaEnvLoader::getReward(const int pos) const
{
    if (pos >= static_cast<int>(action_pairs_.size())) { return toDiscreteValue(0.0f); }
    return toDiscreteValue(utils::transformValue(BaseEnvLoader::getReward(pos)[0]));
}

float SuikaEnvLoader::getPriority(const int pos) const
{
    return std::fabs(calculateNStepValue(pos) - BaseEnvLoader::getValue(pos)[0]);
}

float SuikaEnvLoader::calculateNStepValue(const int pos) const
{
    const int n_step = config::learner_n_step_return;
    const float discount = config::actor_mcts_reward_discount;
    const size_t bootstrap_index = pos + n_step;
    float value = 0.0f;
    float bootstrap = (bootstrap_index < action_pairs_.size()) ? std::pow(discount, n_step) * BaseEnvLoader::getValue(bootstrap_index)[0] : 0.0f;
    for (size_t index = pos; index < std::min(bootstrap_index, action_pairs_.size()); ++index) {
        value += std::pow(discount, index - pos) * BaseEnvLoader::getReward(index)[0];
    }
    return value + bootstrap;
}

std::vector<float> SuikaEnvLoader::toDiscreteValue(float value) const
{
    std::vector<float> discrete_value(kSuikaDiscreteValueSize, 0.0f);
    int value_floor = static_cast<int>(std::floor(value));
    int value_ceil = static_cast<int>(std::ceil(value));
    int shift = kSuikaDiscreteValueSize / 2;
    int floor_index = std::clamp(value_floor + shift, 0, kSuikaDiscreteValueSize - 1);
    int ceil_index = std::clamp(value_ceil + shift, 0, kSuikaDiscreteValueSize - 1);
    if (value_floor == value_ceil) {
        discrete_value[floor_index] = 1.0f;
    } else {
        discrete_value[floor_index] = value_ceil - value;
        discrete_value[ceil_index] = value - value_floor;
    }
    return discrete_value;
}

} // namespace minizero::env::suika

#endif
