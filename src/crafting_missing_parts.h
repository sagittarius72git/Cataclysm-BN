#pragma once

// Assisted-by: Claude:claude-opus-5-5

#include <vector>

class Character;
class recipe;
struct construction;

/// 足りない材料を自動製作する。
/// 目標（製作または建設）に足りない部品を、1回ずつ本体の製作で作ってそろえる。
/// Craft missing parts.
/// Crafts the parts missing for a target (a recipe or a construction), one regular craft at a time.
namespace crafting_missing_parts
{

/// 足りないものを確認画面に出し、了承されたら1回目の製作を始める。始めたら true。
/// Shows what is missing and, if the player agrees, starts the first craft. Returns true if started.
auto start_for_recipe( Character &who, const recipe &target, int batch_size ) -> bool;

/// 建設の段階を選び（複数あるとき）、その材料を集め始める。始めたら true。
/// Picks a stage of the construction (if there are several) and starts gathering its materials.
/// Returns true if started.
auto start_for_construction( Character &who,
                             const std::vector<const construction *> &options ) -> bool;

/// 確認せずに始める（テストで使う）。
/// Starts without asking (used by tests).
auto start_recipe_run( Character &who, const recipe &target, int batch_size ) -> void;

/// 次に作るレシピ。目標がそろっていれば目標そのもの、作れるものがなければ nullptr。
/// The recipe to craft next: the target itself if it is ready, nullptr if nothing can be made.
auto next_part( Character &who, const recipe &target, int batch_size ) -> const recipe *; // *NOPAD*

/// 製作が1回終わったときに呼ぶ。進行中なら次の部品（または目標）を作り始める。
/// Called when a craft is finished. Starts the next part (or the target) if a run is in progress.
auto on_craft_finished( Character &who, const recipe &finished ) -> void;

} // namespace crafting_missing_parts
