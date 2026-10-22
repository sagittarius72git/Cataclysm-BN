// Assisted-by: Claude:claude-opus-5-5

#include "avatar.h"
#include "calendar.h"
#include "catch/catch.hpp"
#include "crafting_missing_parts.h"
#include "item.h"
#include "itype.h"
#include "map_helpers.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "recipe.h"
#include "state_helpers.h"
#include "type_id.h"

namespace {

const activity_id ACT_CRAFT("ACT_CRAFT");

// 晴れた昼間に、リュックを着けた何もない状態から始める。
// Start from an empty avatar wearing a backpack, at midday.
auto prepare_avatar() -> avatar& {
    clear_all_state();
    clear_map();
    clear_avatar();
    set_time(calendar::turn_zero + 12_hours);
    auto& you = get_avatar();
    you.setpos(tripoint_bub_ms(60, 60, 0));
    you.wear_item(item::spawn("backpack"), false);
    return you;
}

auto give(avatar& you, const char* type, int count) -> void {
    auto it = item::spawn(itype_id(type), calendar::turn);
    if (it->count_by_charges()) {
        it->charges = count;
        you.i_add(std::move(it));
        return;
    }
    you.i_add(std::move(it));
    for (auto i = 1; i < count; i++) { you.i_add(item::spawn(itype_id(type), calendar::turn)); }
}

// 製作の活動が終わるまで進める（部品の製作から目標の製作へ続く分も含む）。
// Runs craft activities until none is left (including the ones started by the run).
auto run_crafts(avatar& you) -> int {
    auto turns = 0;
    while (you.activity && you.activity->id() == ACT_CRAFT && turns < 100'000) {
        turns++;
        you.set_moves(100);
        you.activity->do_turn(you);
    }
    return turns;
}

} // namespace

TEST_CASE("craft_missing_parts_picks_a_part_that_can_be_made_now", "[crafting][missing_parts]") {
    auto& you = prepare_avatar();
    const auto& target = recipe_id("string_36").obj();
    const auto& part = recipe_id("string_6").obj();
    you.learn_recipe(&target);
    you.learn_recipe(&part);

    SECTION("nothing to make it from") {
        REQUIRE_FALSE(you.can_make(&target, 1));
        CHECK(crafting_missing_parts::next_part(you, target, 1) == nullptr);
    }

    SECTION("one part short, and thread to make it") {
        give(you, "string_6", 5);
        give(you, "thread", 50);
        REQUIRE_FALSE(you.can_make(&target, 1));
        CHECK(crafting_missing_parts::next_part(you, target, 1) == &part);
    }

    SECTION("everything is there") {
        give(you, "string_6", 6);
        REQUIRE(you.can_make(&target, 1));
        CHECK(crafting_missing_parts::next_part(you, target, 1) == &target);
    }
}

TEST_CASE("craft_missing_parts_makes_a_tool_for_a_missing_quality", "[crafting][missing_parts]") {
    auto& you = prepare_avatar();
    const auto& target = recipe_id("rope_6_from_cloth").obj();
    const auto& machete = recipe_id("makeshift_machete").obj();
    you.learn_recipe(&target);
    you.learn_recipe(&machete);
    give(you, "rag", 30);
    give(you, "duct_tape", 50);
    give(you, "blade", 1);
    REQUIRE_FALSE(you.can_make(&target, 1));

    const auto* step = crafting_missing_parts::next_part(you, target, 1);
    REQUIRE(step != nullptr);
    const auto& qualities = step->result().obj().qualities;
    const auto cut = qualities.find(quality_id("CUT"));
    REQUIRE(cut != qualities.end());
    CHECK(cut->second >= 1);
}

TEST_CASE("craft_missing_parts_crafts_the_part_then_the_target", "[crafting][missing_parts]") {
    auto& you = prepare_avatar();
    const auto& target = recipe_id("string_36").obj();
    const auto& part = recipe_id("string_6").obj();
    you.learn_recipe(&target);
    you.learn_recipe(&part);
    give(you, "string_6", 5);
    give(you, "thread", 50);
    REQUIRE_FALSE(you.can_make(&target, 1));

    crafting_missing_parts::start_recipe_run(you, target, 1);
    REQUIRE(you.activity);
    REQUIRE(you.activity->id() == ACT_CRAFT);
    CHECK_FALSE(you.get_value("craft_missing_parts").empty());

    run_crafts(you);

    CHECK(you.amount_of(itype_id("string_36")) == 1);
    CHECK(you.amount_of(itype_id("string_6")) == 0);
    CHECK(you.get_value("craft_missing_parts").empty());
}

TEST_CASE("craft_missing_parts_stops_when_another_craft_finishes", "[crafting][missing_parts]") {
    auto& you = prepare_avatar();
    const auto& target = recipe_id("string_36").obj();
    const auto& part = recipe_id("string_6").obj();
    you.learn_recipe(&target);
    you.learn_recipe(&part);
    give(you, "string_6", 5);
    give(you, "thread", 50);

    crafting_missing_parts::start_recipe_run(you, target, 1);
    REQUIRE_FALSE(you.get_value("craft_missing_parts").empty());
    you.cancel_activity();

    crafting_missing_parts::on_craft_finished(you, target);
    CHECK(you.get_value("craft_missing_parts").empty());
}
