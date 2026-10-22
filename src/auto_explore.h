#pragma once

// Assisted-by: Claude:claude-opus-5-5

#include <optional>

#include "coordinates.h"

class avatar;
class map;

/// 自動探索。まだ見ていない場所の手前まで自動移動でくり返し歩く。
/// 敵が見えたとき、けがをしたとき、自動移動が止まったとき、行ける場所がなくなったときに止まる。
/// Auto explore. Repeatedly auto-moves to the nearest place next to unexplored tiles.
/// Stops when a hostile is seen, when hurt, when auto-move stops, or when nothing is left to explore.
namespace auto_explore
{

/// 探索を始める。最初の移動先を決められたら true。
/// Starts exploring. Returns true if the first leg was set.
auto start( avatar &you, map &here ) -> bool;

/// 自動移動が終わったときに呼ぶ。探索中なら次の移動先を決めて true を返す。
/// Called when there is no auto-move destination. If exploring, sets the next leg and returns true.
auto continue_exploring( avatar &you, map &here ) -> bool;

/// 探索中か。
/// Whether exploring is in progress.
auto is_active() -> bool;

/// 探索をやめる（メッセージは出さない）。
/// Stops exploring without a message.
auto stop() -> void;

/// 次に向かう場所（見ていない場所に隣接する、歩ける既知の場所のうち最も近いもの）。
/// The next place to go: the nearest known walkable tile next to an unexplored tile.
auto find_target( avatar &you, map &here ) -> std::optional<tripoint_bub_ms>;

} // namespace auto_explore
