#include "kotte/application.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include <raymath.h>
#include <string>
#include <utility>

namespace kotte
{
    Application::Application(
        int width,
        int height,
        std::string_view title,
        std::uint64_t seed,
        int target_fps)
        : window_{ width, height, title, target_fps }
        , camera_{ window_.size() }
        , random_{ seed }
        , map_{ make_room(19, 13, random_) }
        , spatial_grid_{ {
            static_cast<float>(map_.width() * tile_size_),
            static_cast<float>(map_.height() * tile_size_)}, spatial_cell_size_ }
            , seed_{ seed } {
        populate_entities();
        initialize_enemies();
    }

    void Application::run() {
        while (!window_.should_close() && !exit_requested_) {
            run_frame(GetFrameTime());
        }
    }

    void Application::run_frame(float delta_time) {
        // Full frame order:
        // 1. Reset frame-local state.
        // 2. Translate input into gameplay intent.
        // 3. Update ordinary gameplay and queue structural requests.
        // 4. Update timed gameplay independently of camera visibility.
        // 5. Resolve gameplay facts into response decisions.
        // 6. Apply structural mutations at one synchronization point.
        // 7. Update presentation state from the synchronized world.
        // 8. Extract, sort, and draw this frame's presentation values.
        //
        // Keeping one call site per phase makes event lifetime and mutation
        // boundaries visible as the simulation grows.
        begin_frame();
        collect_frame_actions();
        update_gameplay(delta_time);
        update_timed_gameplay(delta_time);
        resolve_gameplay_facts();
        apply_structural_mutations();
        update_presentation(delta_time);
        render();
    }

    void Application::begin_frame() noexcept {
        // Every request must be consumed by the previous frame's mutation phase.
        assert(place_bomb_requests_.empty());
        assert(bomb_detonated_facts_.empty());
        assert(destroy_entity_requests_.empty());
        frame_actions_ = {};
        collision_diagnostics_ = {};
    }

    void Application::collect_frame_actions() noexcept {
        if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_Q)) {
            request_exit();
        }
        frame_actions_.restart = IsKeyPressed(KEY_R);
        if (round_state_ != RoundState::playing) {
            return;
        }

        frame_actions_.place_bomb = IsKeyPressed(KEY_SPACE);
        frame_actions_.remote_detonate = IsKeyPressed(KEY_E);

        Vector2& movement_direction = frame_actions_.player_movement_direction;
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) {
            movement_direction.x -= 1.0f;
        }
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) {
            movement_direction.x += 1.0f;
        }
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) {
            movement_direction.y -= 1.0f;
        }
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) {
            movement_direction.y += 1.0f;
        }

        // Normalize intent before simulation so bindings cannot change movement speed.
        if (movement_direction.x != 0.0f || movement_direction.y != 0.0f) {
            movement_direction = Vector2Normalize(movement_direction);
        }
    }

    void Application::update_gameplay(float delta_time) {
        if (round_state_ != RoundState::playing) {
            if (frame_actions_.restart) {
                restart_round();
            }
            return;
        }

        // The player and every enemy are movers; each asks its own collision question.
        update_player(delta_time);
        update_enemies(delta_time);
        if (round_state_ != RoundState::playing) {
            return;
        }
        queue_bomb_placement();
        queue_remote_detonations();
        check_player_contacts();
        check_pickup_contact();
        update_bomb_owner_escape();
    }

    void Application::update_timed_gameplay(float delta_time) {
        if (round_state_ != RoundState::playing) {
            return;
        }
        // This list is independent of camera queries, so off-screen fuses advance.
        for (const EntityHandle bomb_handle : active_bomb_handles_) {
            Entity* bomb = entities_.try_get(bomb_handle);
            if (bomb == nullptr || !bomb->bomb_state.has_value()
                || bomb->bomb_state->detonation_requested) {
                continue;
            }

            BombState& state = *bomb->bomb_state;
            state.fuse_remaining -= delta_time;
            if (state.fuse_remaining > 0.0f) {
                continue;
            }

            state.fuse_remaining = 0.0f;
            state.detonation_requested = true;

            // The fact owns every value needed by later phases; it does not
            // borrow the bomb that will eventually request its own destruction.
            bomb_detonated_facts_.push_back({
                bomb_handle,
                state.owner,
                bomb->world_position,
                state.blast_range
                });
        }
    }

    void Application::resolve_gameplay_facts() {
        for (std::size_t index = 0; index < bomb_detonated_facts_.size(); ++index) {
            const BombDetonated fact = bomb_detonated_facts_[index];
            BlastEffect effect;
            effect.remaining_seconds = blast_effect_seconds_;
            effect.tile_centres.reserve(static_cast<std::size_t>(fact.blast_range) * 4 + 1);

            const float tile_extent = static_cast<float>(tile_size_);
            const int origin_column = static_cast<int>(std::floor(fact.world_position.x / tile_extent));
            const int origin_row = static_cast<int>(std::floor(fact.world_position.y / tile_extent));
            (void)resolve_blast_tile(effect, origin_column, origin_row);

            // The detonating bomb must disappear even if its presentation or
            // collision bounds change independently of blast-tile detection.
            destroy_entity_requests_.push_back({ fact.bomb });

            append_blast_ray(effect, origin_column, origin_row, 0, -1, fact.blast_range);
            append_blast_ray(effect, origin_column, origin_row, 1, 0, fact.blast_range);
            append_blast_ray(effect, origin_column, origin_row, 0, 1, fact.blast_range);
            append_blast_ray(effect, origin_column, origin_row, -1, 0, fact.blast_range);

            blast_effects_.push_back(std::move(effect));
        }

        bomb_detonated_facts_.clear();
    }

    void Application::apply_structural_mutations() {
        // Entity storage and every derived index change only at this boundary.
        apply_entity_destructions();
        apply_bomb_placements();
        update_round_result();
    }

    void Application::update_presentation(float delta_time) {
        for (BlastEffect& effect : blast_effects_) {
            effect.remaining_seconds -= delta_time;
        }
        std::erase_if(blast_effects_, [](const BlastEffect& effect) {
            return effect.remaining_seconds <= 0.0f;
            });

        update_camera(delta_time);
    }

    void Application::update_player(float delta_time) {
        const Vector2 direction = frame_actions_.player_movement_direction;

        Entity& player_entity = player();
        const Rectangle old_world_bounds = entity_world_bounds(player_entity);
        const Vector2 displacement = direction * (player_speed_ * delta_time);

        // Separate proposals let a clear axis slide along a blocked one.
        if (displacement.x != 0.0f) {
            try_move_player({ displacement.x, 0.0f });
        }
        if (displacement.y != 0.0f) {
            try_move_player({ 0.0f, displacement.y });
        }

        const Rectangle player_bounds = entity_world_bounds(player_entity);
        const Vector2 bounds_offset = player_entity.world_position - Vector2{ player_bounds.x, player_bounds.y };
        const float floor_left = static_cast<float>(tile_size_);
        const float floor_top = static_cast<float>(tile_size_);
        const float floor_right = static_cast<float>((map_.width() - 1) * tile_size_);
        const float floor_bottom = static_cast<float>((map_.height() - 1) * tile_size_);
        const Vector2 minimum_position{ floor_left + bounds_offset.x, floor_top + bounds_offset.y };
        const Vector2 maximum_position{ floor_right - bounds_offset.x, floor_bottom };
        player_entity.world_position = Vector2Clamp(player_entity.world_position, minimum_position, maximum_position);

        const Rectangle new_world_bounds = entity_world_bounds(player_entity);
        spatial_grid_.update(player_handle_, old_world_bounds, new_world_bounds);
    }

    void Application::try_move_player(Vector2 axis_displacement) {
        // Propose one horizontal or vertical player movement.
        ++collision_diagnostics_.movement_attempts;

        Entity& player_entity = player();
        const Vector2 proposed_position = player_entity.world_position + axis_displacement;

        // Detect possible contacts using the mover's proposed collision bounds.
        if (entity_movement_is_blocked(player_handle_, proposed_position)) {
            // Respond by rejecting only this blocked player axis.
            ++collision_diagnostics_.blocked_moves;
            return;
        }

        // Accept the clear proposal; grid synchronization happens after both axes.
        player_entity.world_position = proposed_position;
    }

    void Application::update_enemies(float delta_time) {
        // The update list contains every enemy, not only those returned by the
        // camera query. Off-screen enemies therefore keep simulating normally.
        static float wandering_turn_seconds = 0.0f;
        wandering_turn_seconds -= delta_time;
        const bool choose_wandering_direction = wandering_turn_seconds <= 0.0f;
        if (choose_wandering_direction) {
            wandering_turn_seconds = 0.6f;
        }

        for (const EntityHandle enemy_handle : enemy_handles_) {
            // Each enemy is one mover with one cardinal movement attempt.
            ++collision_diagnostics_.enemy_updates;
            ++collision_diagnostics_.movement_attempts;

            Entity& enemy = entity(enemy_handle);
            const Rectangle old_world_bounds = entity_world_bounds(enemy);

            if (choose_wandering_direction) {
                enemy.movement_heading = static_cast<CardinalDirection>(
                    random_.next<4, std::uint8_t>());
            }

            // Propose one cardinal movement for this enemy.
            const Vector2 direction = cardinal_vector(enemy.movement_heading);
            const Vector2 displacement = direction * (enemy_speed_ * delta_time);
            const Vector2 proposed_position = enemy.world_position + displacement;

            // Detect a possible contact using this enemy's proposed bounds.
            if (entity_movement_is_blocked(enemy_handle, proposed_position)) {
                ++collision_diagnostics_.blocked_moves;
                ++collision_diagnostics_.enemy_turns;
                enemy.movement_heading = different_enemy_direction(enemy.movement_heading);
                continue;
            }

            enemy.world_position = proposed_position;
            const Rectangle new_world_bounds = entity_world_bounds(enemy);

            // Synchronize this accepted move before the next enemy query.
            spatial_grid_.update(enemy_handle, old_world_bounds, new_world_bounds);
        }
    }

    void Application::queue_bomb_placement() {
        if (!frame_actions_.place_bomb) {
            return;
        }

        const Entity& player_entity = player();
        const Rectangle player_bounds = entity_collision_bounds(player_entity);
        const Vector2 placement_reference{
            player_entity.world_position.x,
            player_bounds.y + player_bounds.height / 2.0f
        };
        // Capture values rather than an Entity reference because the request
        // deliberately outlives this simulation phase.
        place_bomb_requests_.push_back({
            player_handle_,
            tile_centre(placement_reference)
            });
    }

    void Application::queue_remote_detonations() {
        if (!frame_actions_.remote_detonate) {
            return;
        }

        for (const EntityHandle handle : active_bomb_handles_) {
            Entity* bomb = entities_.try_get(handle);
            if (bomb == nullptr || !bomb->bomb_state.has_value()) {
                continue;
            }

            BombState& state = *bomb->bomb_state;
            if (state.owner != player_handle_ || state.detonation_requested) {
                continue;
            }

            state.detonation_requested = true;
            bomb_detonated_facts_.push_back({ handle, state.owner, bomb->world_position, state.blast_range });
        }
    }

    void Application::check_player_contacts() {
        const Entity& player_entity = player();
        const Rectangle player_bounds = entity_collision_bounds(player_entity);
        const SpatialQuery query = spatial_grid_.query(player_bounds);

        for (const EntityHandle handle : query.entity_handles) {
            const Entity* candidate = entities_.try_get(handle);
            if (candidate != nullptr && candidate->kind == EntityKind::enemy
                && CheckCollisionRecs(player_bounds, entity_collision_bounds(*candidate))) {
                request_player_death();
                return;
            }
        }
    }

    void Application::check_pickup_contact() {
        Entity& player_entity = player();
        const Rectangle player_bounds = entity_collision_bounds(player_entity);
        const SpatialQuery query = spatial_grid_.query(player_bounds);

        for (const EntityHandle handle : query.entity_handles) {
            const Entity* pickup = entities_.try_get(handle);
            if (pickup == nullptr || pickup->kind != EntityKind::blast_range_pickup
                || !CheckCollisionRecs(player_bounds, entity_collision_bounds(*pickup))) {
                continue;
            }

            assert(player_entity.player_state.has_value());
            PlayerState& state = *player_entity.player_state;
            state.blast_range = std::min(maximum_blast_range_, static_cast<std::uint8_t>(state.blast_range + 1));
            state.pickup_collected = true;
            destroy_entity_requests_.push_back({ handle });
            return;
        }
    }

    void Application::update_bomb_owner_escape() {
        const Rectangle player_bounds = entity_collision_bounds(player());
        for (const EntityHandle handle : active_bomb_handles_) {
            Entity* bomb = entities_.try_get(handle);
            if (bomb == nullptr || !bomb->bomb_state.has_value()) {
                continue;
            }
            BombState& state = *bomb->bomb_state;
            if (state.owner == player_handle_ && state.owner_may_escape
                && !CheckCollisionRecs(player_bounds, entity_collision_bounds(*bomb))) {
                state.owner_may_escape = false;
            }
        }
    }

    void Application::request_player_death() noexcept {
        if (round_state_ == RoundState::playing) {
            round_state_ = RoundState::lost;
        }
    }

    void Application::update_round_result() {
        if (round_state_ == RoundState::playing && enemy_handles_.empty()) {
            round_state_ = RoundState::won;
        }
    }

    void Application::restart_round() {
        random_ = Random{ seed_ };
        map_ = make_room(19, 13, random_);
        entities_ = EntityStore{};
        enemy_handles_.clear();
        active_bomb_handles_.clear();
        place_bomb_requests_.clear();
        bomb_detonated_facts_.clear();
        destroy_entity_requests_.clear();
        blast_effects_.clear();
        spatial_grid_ = SpatialGrid{ {
            static_cast<float>(map_.width() * tile_size_),
            static_cast<float>(map_.height() * tile_size_)}, spatial_cell_size_ };
        round_state_ = RoundState::playing;
        populate_entities();
        initialize_enemies();
    }

    void Application::apply_bomb_placements() {
        for (const PlaceBombRequest& request : place_bomb_requests_) {
            const Entity* owner = entities_.try_get(request.owner);
            const float tile_extent = static_cast<float>(tile_size_);
            const Rectangle target_tile{
                request.world_position.x - tile_extent / 2.0f,
                request.world_position.y - tile_extent / 2.0f,
                tile_extent,
                tile_extent
            };
            const SpatialQuery target_query = spatial_grid_.query(target_tile);
            const bool crate_occupies_target = std::ranges::any_of(
                target_query.entity_handles,
                [this, request](EntityHandle handle) {
                    const Entity* candidate = entities_.try_get(handle);
                    return candidate != nullptr
                        && candidate->kind == EntityKind::crate
                        && candidate->world_position.x == request.world_position.x
                        && candidate->world_position.y == request.world_position.y;
                });
            if (owner == nullptr || owner->kind != EntityKind::player
                || owner_has_active_bomb(request.owner)
                || bomb_occupies(request.world_position)
                || crate_occupies_target) {
                continue;
            }

            assert(owner->player_state.has_value());

            const Entity bomb{
                .kind = EntityKind::bomb,
                .world_position = request.world_position,
                .bomb_state = BombState{
                    .owner = request.owner,
                    .fuse_remaining = bomb_fuse_seconds_,
                    .blast_range = owner->player_state->blast_range
                }
            };
            const EntityHandle bomb_handle = entities_.create(bomb);

            // The store owns the bomb; these handles are synchronized, non-owning
            // views used by spatial queries and timed gameplay.
            spatial_grid_.insert(bomb_handle, entity_world_bounds(bomb));
            active_bomb_handles_.push_back(bomb_handle);
        }

        place_bomb_requests_.clear();
    }

    void Application::apply_entity_destructions() {
        // A moving entity may overlap several blast tiles, and several bombs may
        // reach it in one frame. Canonicalizing requests makes that multiplicity
        // explicit instead of depending on a forgiving destruction order.
        std::ranges::sort(destroy_entity_requests_, {}, &DestroyEntityRequest::entity);
        const auto duplicate_tail = std::ranges::unique(
            destroy_entity_requests_, {}, &DestroyEntityRequest::entity);
        destroy_entity_requests_.erase(duplicate_tail.begin(), duplicate_tail.end());

        for (const DestroyEntityRequest& request : destroy_entity_requests_) {
            const Entity* doomed = entities_.try_get(request.entity);
            if (doomed == nullptr) {
                continue;
            }

            // Spatial membership is derived from the live entity, so remove it
            // while the handle can still resolve to the bounds used at insertion.
            spatial_grid_.remove(request.entity, entity_world_bounds(*doomed));
            const bool destroyed = entities_.destroy(request.entity);
            assert(destroyed);
            (void)destroyed;
        }

        // These update lists are non-owning views. A single cleanup pass keeps
        // their iteration dense without teaching each destruction source about
        // every list that happens to reference an entity.
        std::erase_if(enemy_handles_, [this](EntityHandle handle) {
            return !entities_.contains(handle);
            });
        std::erase_if(active_bomb_handles_, [this](EntityHandle handle) {
            return !entities_.contains(handle);
            });

        destroy_entity_requests_.clear();
    }

    bool Application::resolve_blast_tile(
        BlastEffect& effect,
        int column,
        int row) {
        const Vector2 centre = tile_centre(column, row);
        effect.tile_centres.push_back(centre);

        const float tile_extent = static_cast<float>(tile_size_);
        const Rectangle tile_bounds{
            static_cast<float>(column) * tile_extent,
            static_cast<float>(row) * tile_extent,
            tile_extent,
            tile_extent
        };
        const SpatialQuery contacts = spatial_grid_.query(tile_bounds);
        bool blocked_by_crate = false;

        for (const EntityHandle handle : contacts.entity_handles) {
            const Entity* candidate = entities_.try_get(handle);
            if (candidate == nullptr
                || !CheckCollisionRecs(tile_bounds, entity_collision_bounds(*candidate))) {
                continue;
            }

            switch (candidate->kind) {
            case EntityKind::bomb:
                if (candidate->bomb_state.has_value() && !candidate->bomb_state->detonation_requested) {
                    Entity* chained_bomb = entities_.try_get(handle);
                    assert(chained_bomb != nullptr);
                    chained_bomb->bomb_state->detonation_requested = true;
                    bomb_detonated_facts_.push_back({
                        handle,
                        chained_bomb->bomb_state->owner,
                        chained_bomb->world_position,
                        chained_bomb->bomb_state->blast_range
                        });
                }
                destroy_entity_requests_.push_back({ handle });
                break;
            case EntityKind::enemy:
                destroy_entity_requests_.push_back({ handle });
                break;
            case EntityKind::crate:
                destroy_entity_requests_.push_back({ handle });
                blocked_by_crate = true;
                break;
            case EntityKind::player:
                request_player_death();
                break;
            case EntityKind::blast_range_pickup:
                break;
            }
        }

        return blocked_by_crate;
    }

    void Application::append_blast_ray(
        BlastEffect& effect,
        int origin_column,
        int origin_row,
        int column_step,
        int row_step,
        std::uint8_t range) {
        for (int distance = 1; distance <= static_cast<int>(range); ++distance) {
            const int column = origin_column + column_step * distance;
            const int row = origin_row + row_step * distance;
            if (!map_.contains(column, row) || map_.at(column, row).type == TileType::wall) {
                break;
            }

            if (resolve_blast_tile(effect, column, row)) {
                break;
            }
        }
    }

    void Application::update_camera(float delta_time) noexcept {
        (void)delta_time; // name the argument to avoid the static analyzer yelling at us. :) We'll animate the camera later.
        camera_.set_viewport_size(window_.size());
        camera_.set_centre(player().world_position);
    }

    void Application::render() {
        Frame frame{ background_color_ };

        const Rectangle world_view = camera_.world_view();
        const TileRange visible_tiles = map_.tiles_overlapping(world_view, tile_size_);

        for (int y = visible_tiles.first_row; y < visible_tiles.past_last_row; ++y) {
            for (int x = visible_tiles.first_column; x < visible_tiles.past_last_column; ++x) {
                const Tile& tile = map_.at(x, y);
                const Color color = tile.type == TileType::wall
                    ? wall_color_
                    : floor_colors_[tile.variation % 3];

                const Vector2 tile_world_position{
                    static_cast<float>(x * tile_size_),
                    static_cast<float>(y * tile_size_)
                };
                const Vector2 tile_screen_position = camera_.world_to_screen(tile_world_position);

                DrawRectangle(
                    static_cast<int>(std::floor(tile_screen_position.x)),
                    static_cast<int>(std::floor(tile_screen_position.y)),
                    tile_size_ - 1,
                    tile_size_ - 1,
                    color);
            }
        }

        const SpatialQuery spatial_query = spatial_grid_.query(world_view);
        renderer_.clear();

        // Blast effects are short-lived presentation values, not authoritative
        // entities. Rebuilding commands here keeps their lifetime out of gameplay.
        constexpr float blast_inset = 3.0f;
        for (const BlastEffect& effect : blast_effects_) {
            for (const Vector2 tile_world_centre : effect.tile_centres) {
                Rectangle bounds{
                    tile_world_centre.x - static_cast<float>(tile_size_) / 2.0f + blast_inset,
                    tile_world_centre.y - static_cast<float>(tile_size_) / 2.0f + blast_inset,
                    static_cast<float>(tile_size_) - blast_inset * 2.0f,
                    static_cast<float>(tile_size_) - blast_inset * 2.0f
                };
                if (!CheckCollisionRecs(bounds, world_view)) {
                    continue;
                }

                const Vector2 screen_position = camera_.world_to_screen({ bounds.x, bounds.y });
                bounds.x = screen_position.x;
                bounds.y = screen_position.y;
                renderer_.submit({
                    bounds,
                    blast_color_,
                    RenderLayer::effect,
                    tile_world_centre.y,
                    0.3f
                    });
            }
        }

        // The spatial query replaces Week 3's complete entity scan. Its result is
        // only a candidate list, so every candidate still needs an exact test.
        for (const EntityHandle entity_handle : spatial_query.entity_handles) {
            const Entity* visible_candidate = entities_.try_get(entity_handle);
            if (visible_candidate == nullptr) {
                continue;
            }

            Rectangle bounds = entity_world_bounds(*visible_candidate);
            if (!CheckCollisionRecs(bounds, world_view)) {
                continue;
            }
            const Vector2 screen_position = camera_.world_to_screen({ bounds.x, bounds.y });
            bounds.x = screen_position.x;
            bounds.y = screen_position.y;
            const RenderLayer layer = visible_candidate->kind == EntityKind::bomb
                ? RenderLayer::ground
                : RenderLayer::world;
            renderer_.submit({
                bounds,
                entity_color(visible_candidate->kind),
                layer,
                visible_candidate->world_position.y,
                entity_roundness(visible_candidate->kind)
                });
        }
        renderer_.sort();
        renderer_.execute();

        DrawRectangle(10, 10, 320, 126, Color{ 0x0b, 0x14, 0x20, 0xdd });
        DrawText("REMOTE BLAST ARENA", 22, 20, 24, pickup_color_);
        const char* round_text = "PLAYING - defeat both enemies";
        Color round_color = RAYWHITE;
        if (round_state_ == RoundState::won) {
            round_text = "WON! Press R to restart";
            round_color = GREEN;
        }
        else if (round_state_ == RoundState::lost) {
            round_text = "LOST! Press R to restart";
            round_color = RED;
        }
        DrawText(round_text, 22, 52, 20, round_color);
        const PlayerState& player_state = *player().player_state;
        const std::string status = std::format(
            "Range: {}/{}    Bombs: {}/{}",
            player_state.blast_range,
            maximum_blast_range_,
            active_bomb_handles_.size(),
            bomb_capacity_);
        DrawText(status.c_str(), 22, 80, 20, RAYWHITE);
        DrawText("Move: WASD / Arrows", 22, 106, 18, LIGHTGRAY);
        DrawText("Bomb: Space    Detonate: E    Restart: R", 22, window_.height() - 32, 18, LIGHTGRAY);
    }

    void Application::populate_entities() {
        entities_.reserve(28);

        Entity player_entity{ EntityKind::player, tile_centre(2, 2) };
        player_entity.player_state = PlayerState{ initial_blast_range_ };
        player_handle_ = entities_.create(player_entity);
        spatial_grid_.insert(player_handle_, entity_world_bounds(player_entity));

        const auto add = [this](EntityKind kind, int column, int row) {
            const Entity new_entity{ kind, tile_centre(column, row) };
            const EntityHandle handle = entities_.create(new_entity);
            spatial_grid_.insert(handle, entity_world_bounds(new_entity));
            if (kind == EntityKind::enemy) {
                enemy_handles_.push_back(handle);
            }
            };

        add(EntityKind::blast_range_pickup, 4, 2);
        add(EntityKind::crate, 6, 3);
        add(EntityKind::crate, 7, 3);
        add(EntityKind::crate, 10, 7);
        add(EntityKind::crate, 14, 8);
        add(EntityKind::crate, 3, 5);
        add(EntityKind::crate, 6, 6);
        add(EntityKind::crate, 12, 3);
        add(EntityKind::crate, 15, 4);
        add(EntityKind::crate, 5, 9);
        add(EntityKind::crate, 9, 9);
        add(EntityKind::crate, 12, 10);
        add(EntityKind::crate, 16, 6);
        add(EntityKind::enemy, 9, 3);
        add(EntityKind::enemy, 14, 9);
    }

    void Application::initialize_enemies() {
        // Finish every world-generation roll before drawing runtime headings.
        // This preserves the complete Week 4 layout for the reference seed.
        for (const EntityHandle enemy_handle : enemy_handles_) {
            Entity& enemy = entity(enemy_handle);
            enemy.movement_heading = static_cast<CardinalDirection>(
                random_.next<4, std::uint8_t>());
        }
    }

    Entity& Application::entity(EntityHandle handle) noexcept {
        Entity* resolved = entities_.try_get(handle);
        assert(resolved != nullptr);
        return *resolved;
    }

    const Entity& Application::entity(EntityHandle handle) const noexcept {
        const Entity* resolved = entities_.try_get(handle);
        assert(resolved != nullptr);
        return *resolved;
    }

    Entity& Application::player() noexcept {
        return entity(player_handle_);
    }

    const Entity& Application::player() const noexcept {
        return entity(player_handle_);
    }

    Rectangle Application::entity_world_bounds(const Entity& entity) const noexcept {
        Vector2 size{};
        switch (entity.kind) {
        case EntityKind::player: size = { 24.0f, 32.0f }; break;
        case EntityKind::bomb: size = { 22.0f, 22.0f }; break;
        case EntityKind::crate: size = { 32.0f, 32.0f }; break;
        case EntityKind::enemy: size = { 28.0f, 30.0f }; break;
        case EntityKind::blast_range_pickup: size = { 18.0f, 18.0f }; break;
        }

        const float vertical_offset = entity.kind == EntityKind::crate
            ? size.y / 2.0f
            : size.y;
        const Vector2 top_left = entity.world_position
            - Vector2{ size.x / 2.0f, vertical_offset };
        return { top_left.x, top_left.y, size.x, size.y };
    }

    Rectangle Application::entity_collision_bounds(const Entity& entity) const noexcept {
        // Collision uses a smaller footprint around the part touching the
        // floor. The complete visual rectangle remains in the spatial grid so
        // broad-phase queries conservatively find every possible contact.
        Vector2 size{};
        switch (entity.kind) {
        case EntityKind::player: size = { 20.0f, 16.0f }; break;
        case EntityKind::bomb: size = { 18.0f, 12.0f }; break;
        case EntityKind::crate: size = { 32.0f, 32.0f }; break;
        case EntityKind::enemy: size = { 24.0f, 14.0f }; break;
        case EntityKind::blast_range_pickup: size = { 16.0f, 16.0f }; break;
        }

        const float vertical_offset = entity.kind == EntityKind::crate
            ? size.y / 2.0f
            : size.y;
        const Vector2 top_left = entity.world_position
            - Vector2{ size.x / 2.0f, vertical_offset };
        return { top_left.x, top_left.y, size.x, size.y };
    }

    bool Application::entity_movement_is_blocked(
        EntityHandle moving_entity_handle,
        Vector2 proposed_position) {
        Entity proposed_entity = entity(moving_entity_handle);
        proposed_entity.world_position = proposed_position;

        // Room walls are a simple boundary rule in the current map. Test the
        // complete visual bounds directly before asking the grid about entities.
        const Rectangle proposed_world_bounds = entity_world_bounds(proposed_entity);
        const float floor_left = static_cast<float>(tile_size_);
        const float floor_top = static_cast<float>(tile_size_);
        const float floor_right = static_cast<float>((map_.width() - 1) * tile_size_);
        const float floor_bottom = static_cast<float>((map_.height() - 1) * tile_size_);
        const bool leaves_room = proposed_world_bounds.x < floor_left
            || proposed_world_bounds.y < floor_top
            || proposed_world_bounds.x + proposed_world_bounds.width > floor_right
            || proposed_world_bounds.y + proposed_world_bounds.height > floor_bottom;
        if (leaves_room) {
            ++collision_diagnostics_.boundary_blocks;
            return true;
        }

        const Rectangle proposed_collision_bounds = entity_collision_bounds(proposed_entity);

        // Broad phase: query nearby candidates using the proposed bounds.
        const SpatialQuery spatial_query = spatial_grid_.query(proposed_collision_bounds);
        ++collision_diagnostics_.spatial_queries;
        collision_diagnostics_.cells_visited += spatial_query.cells_visited;
        collision_diagnostics_.candidate_references += spatial_query.candidate_references;
        // These counters aggregate work across movers, not globally unique entities.
        collision_diagnostics_.unique_candidates += spatial_query.entity_handles.size();

        bool blocked = false;

        // Filter: remove the mover and non-solid kinds before exact testing.
        for (const EntityHandle candidate_handle : spatial_query.entity_handles) {
            if (candidate_handle == moving_entity_handle) {
                continue;
            }

            const Entity* candidate = entities_.try_get(candidate_handle);
            if (candidate == nullptr || !is_solid(candidate->kind)) {
                continue;
            }

            if (candidate->kind == EntityKind::bomb
                && player_can_pass_bomb(moving_entity_handle, *candidate)) {
                continue;
            }

            // Narrow phase: exact-test the remaining solid candidate.
            ++collision_diagnostics_.exact_tests;
            if (CheckCollisionRecs(proposed_collision_bounds, entity_collision_bounds(*candidate))) {
                ++collision_diagnostics_.contacts;
                const bool player_enemy_contact =
                    (proposed_entity.kind == EntityKind::enemy
                        && candidate->kind == EntityKind::player)
                    || (proposed_entity.kind == EntityKind::player
                        && candidate->kind == EntityKind::enemy);
                if (player_enemy_contact) {
                    request_player_death();
                }
                blocked = true;
            }
        }

        return blocked;
    }

    bool Application::is_solid(EntityKind kind) noexcept {
        switch (kind) {
        case EntityKind::player:
        case EntityKind::crate:
        case EntityKind::enemy:
            return true;
        case EntityKind::bomb:
            return true;
        case EntityKind::blast_range_pickup:
            return false;
        }

        return false;
    }

    Vector2 Application::cardinal_vector(CardinalDirection direction) noexcept {
        switch (direction) {
        case CardinalDirection::up: return { 0.0f, -1.0f };
        case CardinalDirection::right: return { 1.0f, 0.0f };
        case CardinalDirection::down: return { 0.0f, 1.0f };
        case CardinalDirection::left: return { -1.0f, 0.0f };
        }

        return {};
    }

    CardinalDirection Application::different_enemy_direction(CardinalDirection current_direction) {
        constexpr std::uint8_t direction_count = 4;
        const std::uint8_t current_value = static_cast<std::uint8_t>(current_direction);

        // Adding 1, 2, or 3 modulo four reaches every heading except the one
        // that was just blocked.
        const std::uint8_t offset = static_cast<std::uint8_t>(
            random_.next<3, std::uint8_t>() + 1);
        const std::uint8_t new_value = static_cast<std::uint8_t>(
            (current_value + offset) % direction_count);
        return static_cast<CardinalDirection>(new_value);
    }

    bool Application::owner_has_active_bomb(EntityHandle owner) const noexcept {
        const std::size_t owned_bomb_count = static_cast<std::size_t>(std::ranges::count_if(
            active_bomb_handles_, [this, owner](EntityHandle bomb_handle) {
                const Entity* bomb = entities_.try_get(bomb_handle);
                return bomb != nullptr
                    && bomb->bomb_state.has_value()
                    && bomb->bomb_state->owner == owner;
            }));
        return owned_bomb_count >= bomb_capacity_;
    }

    bool Application::bomb_occupies(Vector2 world_position) const noexcept {
        return std::ranges::any_of(active_bomb_handles_, [this, world_position](EntityHandle handle) {
            const Entity* bomb = entities_.try_get(handle);
            return bomb != nullptr && bomb->kind == EntityKind::bomb
                && bomb->world_position.x == world_position.x
                && bomb->world_position.y == world_position.y;
            });
    }

    bool Application::player_can_pass_bomb(EntityHandle player_handle, const Entity& bomb) const noexcept {
        return player_handle == player_handle_
            && bomb.bomb_state.has_value()
            && bomb.bomb_state->owner == player_handle
            && bomb.bomb_state->owner_may_escape;
    }

    Vector2 Application::tile_centre(Vector2 world_position) noexcept {
        const float tile_extent = static_cast<float>(tile_size_);
        const int column = static_cast<int>(std::floor(world_position.x / tile_extent));
        const int row = static_cast<int>(std::floor(world_position.y / tile_extent));
        return tile_centre(column, row);
    }

    Vector2 Application::tile_centre(int column, int row) noexcept {
        const float tile_extent = static_cast<float>(tile_size_);
        return {
            static_cast<float>(column) * tile_extent + tile_extent / 2.0f,
            static_cast<float>(row) * tile_extent + tile_extent / 2.0f
        };
    }

    Color Application::entity_color(EntityKind kind) noexcept {
        switch (kind) {
        case EntityKind::player: return player_color_;
        case EntityKind::bomb: return bomb_color_;
        case EntityKind::crate: return crate_color_;
        case EntityKind::enemy: return enemy_color_;
        case EntityKind::blast_range_pickup: return pickup_color_;
        }
        return MAGENTA;
    }

    float Application::entity_roundness(EntityKind kind) noexcept {
        return kind == EntityKind::crate ? 0.15f : 0.8f;
    }

    void Application::request_exit() noexcept {
        exit_requested_ = true;
    }

} //kotte
