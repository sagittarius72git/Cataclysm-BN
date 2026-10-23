// Assisted-by: Claude:claude-opus-5-5

#include "action.h"
#include "auto_explore.h"
#include "avatar.h"
#include "avatar_action.h"
#include "calendar.h"
#include "catch/catch.hpp"
#include "game.h"
#include "map/map.h"
#include "map_helpers.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "state_helpers.h"
#include "type_id.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace {

const auto room_center = tripoint_bub_ms(60, 60, 0);

auto refresh_vision(avatar& you, map& here) -> void {
    here.invalidate_map_cache(0);
    here.build_map_cache(0, true);
    you.recalc_sight_limits();
    here.update_visibility_cache(0);
}

// 中心のまわりに壁で囲んだ部屋（内側 9x9）を作る。open_east なら東の壁に1マスの出口を開ける。
// Builds a walled room (9x9 inside) around the room_center. With open_east, leaves a gap in the
// east wall.
auto prepare_room(bool open_east) -> avatar& {
    clear_all_state();
    clear_map();
    clear_avatar();
    set_time(calendar::turn_zero + 12_hours);
    auto& you = get_avatar();
    auto& here = get_map();
    you.clear_map_memory();
    auto_explore::stop();
    for (auto x = -5; x <= 5; x++) {
        for (auto y = -5; y <= 5; y++) {
            if (std::abs(x) == 5 || std::abs(y) == 5) {
                here.ter_set(room_center + point_rel_ms(x, y), ter_id("t_wall"));
            }
        }
    }
    if (open_east) { here.ter_set(room_center + point_rel_ms(5, 0), ter_id("t_grass")); }
    g->place_player(room_center);
    refresh_vision(you, here);
    return you;
}

// 今の移動先へ一気に移って、自動移動を終えた状態にする。
// Jumps to the end of the current auto-move route, as if the walk finished.
auto finish_leg(avatar& you, map& here) -> void {
    REQUIRE(you.has_destination());
    const auto dest = you.get_auto_move_route().back();
    you.clear_destination();
    g->place_player(dest);
    refresh_vision(you, here);
}

} // namespace

TEST_CASE("auto_explore_closed_room_has_nothing_to_explore", "[auto_explore]") {
    auto& you = prepare_room(false);
    auto& here = get_map();

    CHECK_FALSE(auto_explore::find_target(you, here).has_value());
    CHECK_FALSE(auto_explore::start(you, here));
    CHECK_FALSE(auto_explore::is_active());
    CHECK_FALSE(you.has_destination());
}

TEST_CASE("auto_explore_walks_out_through_an_opening", "[auto_explore]") {
    auto& you = prepare_room(true);
    auto& here = get_map();

    const auto target = auto_explore::find_target(you, here);
    REQUIRE(target.has_value());
    CHECK(target->x() >= room_center.x() + 5);

    REQUIRE(auto_explore::start(you, here));
    CHECK(auto_explore::is_active());
    CHECK(you.has_destination());

    SECTION("keeps going after a leg is finished") {
        finish_leg(you, here);
        CHECK(auto_explore::continue_exploring(you, here));
        CHECK(auto_explore::is_active());
        CHECK(you.has_destination());
    }

    SECTION("stops when auto-move was stopped on the way") {
        you.clear_destination();
        CHECK_FALSE(auto_explore::continue_exploring(you, here));
        CHECK_FALSE(auto_explore::is_active());
    }

    SECTION("stops when a hostile is in sight") {
        finish_leg(you, here);
        const auto dirs = std::
            array{point_rel_ms(-1, 0), point_rel_ms(1, 0), point_rel_ms(0, -1), point_rel_ms(0, 1)};
        const auto spot = std::ranges::find_if(dirs, [&](const auto& d) {
            return here.passable(you.bub_pos() + d);
        });
        REQUIRE(spot != dirs.end());
        spawn_test_monster("mon_zombie", you.bub_pos() + *spot);
        refresh_vision(you, here);
        CHECK_FALSE(auto_explore::continue_exploring(you, here));
        CHECK_FALSE(auto_explore::is_active());
    }
}

TEST_CASE("auto_explore_does_not_start_with_a_hostile_in_sight", "[auto_explore]") {
    auto& you = prepare_room(true);
    auto& here = get_map();
    spawn_test_monster("mon_zombie", room_center + point_rel_ms(-2, -2));
    refresh_vision(you, here);

    CHECK_FALSE(auto_explore::start(you, here));
    CHECK_FALSE(auto_explore::is_active());
}

TEST_CASE("auto_explore_walks_with_the_game_loop", "[auto_explore]") {
    auto& you = prepare_room(true);
    auto& here = get_map();
    REQUIRE(auto_explore::start(you, here));

    // game::handle_action と同じように、自動移動の向きを1歩ずつ avatar_action::move に渡す。
    // Steps along the auto-move route the same way game::handle_action does.
    auto left_room = false;
    for (auto i = 0; i < 300; i++) {
        if (!you.has_destination()) {
            refresh_vision(you, here);
            if (!auto_explore::continue_exploring(you, here)) { break; }
        }
        you.set_moves(100);
        const auto act = you.get_next_auto_move_direction();
        const auto delta = get_delta_from_movement_action(act, iso_rotate::no);
        if (delta == point_rel_ms::zero() || !avatar_action::move(you, here, delta)) {
            you.clear_destination();
        }
        refresh_vision(you, here);
        if (you.bub_pos().x() > room_center.x() + 5) { left_room = true; }
    }
    CHECK(left_room);
    auto_explore::stop();
}
