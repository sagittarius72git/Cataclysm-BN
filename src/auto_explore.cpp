// Assisted-by: Claude:claude-opus-5-5

#include "auto_explore.h"

#include <array>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "avatar.h"
#include "creature.h"
#include "enums.h"
#include "game.h"
#include "game_constants.h"
#include "map/map.h"
#include "map/mapdata.h"
#include "messages.h"
#include "point.h"
#include "translations.h"

namespace auto_explore
{
namespace
{

/// 1回の探索で決める移動先の最大数（止まらずに歩き続けないように）。
/// Maximum number of legs in one exploration (so it doesn't walk forever).
constexpr auto max_legs = 100;
/// 経路を調べる候補の最大数。
/// Maximum number of candidate tiles tried for a route.
constexpr auto max_route_tries = 20;

/// 探索中の状態（セーブはしない）。
/// State of the exploration in progress (not saved).
struct explore_state {
    bool active = false;
    /// 今の移動先 / current destination
    tripoint_abs_ms target = tripoint_abs_ms::zero();
    /// 前の移動先を決めたときのHP / HP when the last leg was set
    int last_hp = 0;
    int legs = 0;
    /// 行ってみたが見えるようにならなかった場所、行けなかった場所
    /// Places already tried (reached without revealing anything, or unreachable)
    std::set<tripoint_abs_ms> tried = {};
};

auto state() -> explore_state & // *NOPAD*
{
    static auto st = explore_state();
    return st;
}

constexpr auto neighbors = std::array<point_rel_ms, 8>
{
    point_rel_ms( -1, -1 ), point_rel_ms( 0, -1 ), point_rel_ms( 1, -1 ),
                  point_rel_ms( -1, 0 ), point_rel_ms( 1, 0 ),
                  point_rel_ms( -1, 1 ), point_rel_ms( 0, 1 ), point_rel_ms( 1, 1 ),
};

/// 見たことがある場所か（マップの記憶にあるか、今見えているか）。
/// Whether the tile was explored (remembered on the map, or visible now).
auto explored( const avatar &you, const tripoint_bub_ms &p ) -> bool
{
    return you.has_memorized_tile_for_autodrive( bub_to_abs( p ) ) || you.sees( p );
}

/// 探索で通ってよい場所か（歩ける・開けられる扉で、分かっている危険がない）。
/// Whether exploring may pass through the tile (walkable or an openable door, no known danger).
auto walkable( avatar &you, map &here, const tripoint_bub_ms &p ) -> bool
{
    const auto inside = !here.is_outside( you.bub_pos() );
    if( !here.passable( p ) && !here.can_open_door( &you, p, inside ) ) {
        return false;
    }
    return !here.can_see_trap_at( p, you ) &&
           !here.dangerous_field_at( p ) &&
           !here.has_flag( TFLAG_DEEP_WATER, p ) &&
           !here.has_flag( TFLAG_NO_FLOOR, p );
}

/// 見ていない場所に隣接する、歩ける既知の場所を近い順に返す。
/// Known walkable tiles next to unexplored tiles, nearest first.
auto frontier_tiles( avatar &you, map &here ) -> std::vector<tripoint_bub_ms>
{
    const auto start = you.bub_pos();
    auto seen = std::vector<bool>( MAPSIZE_X * MAPSIZE_Y, false );
    const auto index = [&]( const tripoint_bub_ms & p ) { return p.y() * MAPSIZE_X + p.x(); };

    auto out = std::vector<tripoint_bub_ms>();
    auto queue = std::deque<tripoint_bub_ms> { start };
    seen[index( start )] = true;
    while( !queue.empty() ) {
        const auto cur = queue.front();
        queue.pop_front();
        auto next_to_unexplored = false;
        for( const auto &d : neighbors ) {
            const auto n = cur + d;
            if( !here.inbounds( n ) ) {
                continue;
            }
            if( !explored( you, n ) ) {
                next_to_unexplored = true;
                continue;
            }
            if( seen[index( n )] || !walkable( you, here, n ) ) {
                continue;
            }
            seen[index( n )] = true;
            queue.push_back( n );
        }
        if( next_to_unexplored && cur != start ) {
            out.push_back( cur );
        }
    }
    return out;
}

/// 見えている敵（いれば最初の1体）。
/// A visible hostile creature, if any.
auto visible_hostile( avatar &you ) -> Creature * // *NOPAD*
{
    const auto found = g->get_creatures_if( [&]( const Creature & c ) {
        return &c != &you && you.attitude_to( c ) == Attitude::A_HOSTILE && you.sees( c );
    } );
    return found.empty() ? nullptr : found.front();
}

auto stop_with( const std::string &msg ) -> bool
{
    add_msg( m_info, msg );
    state() = explore_state();
    return false;
}

/// 次の移動先を決めて自動移動を始める。決められたら true。
/// Decides the next leg and starts auto-moving. Returns true if a leg was set.
auto plan_leg( avatar &you, map &here ) -> bool
{
    auto &st = state();
    if( const auto *hostile = visible_hostile( you ) ) {
        return stop_with( string_format( _( "You see %s.  Stopped exploring." ), hostile->disp_name() ) );
    }
    if( you.get_hp() < st.last_hp ) {
        return stop_with( _( "You are hurt.  Stopped exploring." ) );
    }
    if( st.legs >= max_legs ) {
        return stop_with( _( "Explored for a while.  Press the key again to go on." ) );
    }

    auto tries = 0;
    for( const auto &p : frontier_tiles( you, here ) ) {
        const auto abs_p = bub_to_abs( p );
        if( st.tried.contains( abs_p ) ) {
            continue;
        }
        if( tries++ >= max_route_tries ) {
            break;
        }
        auto route = here.route( you.bub_pos(), p, you.get_legacy_pathfinding_settings(),
                                 you.get_legacy_path_avoid() );
        if( route.empty() ) {
            st.tried.insert( abs_p );
            continue;
        }
        you.set_destination( route );
        st.target = abs_p;
        st.last_hp = you.get_hp();
        st.legs++;
        return true;
    }
    return stop_with( _( "Nothing left to explore nearby." ) );
}

} // namespace

auto start( avatar &you, map &here ) -> bool
{
    if( you.controlling_vehicle ) {
        add_msg( m_info, _( "You can't explore while driving." ) );
        return false;
    }
    auto &st = state();
    st = explore_state();
    st.active = true;
    st.last_hp = you.get_hp();
    return plan_leg( you, here );
}

auto continue_exploring( avatar &you, map &here ) -> bool
{
    auto &st = state();
    if( !st.active ) {
        return false;
    }
    if( you.has_destination() ) {
        return false;
    }
    if( you.abs_pos() != st.target ) {
        // 自動移動が途中で止まった（敵・障害物・中断など）。
        // Auto-move stopped on the way (a hostile, an obstacle, an interruption...).
        return stop_with( _( "Stopped exploring." ) );
    }
    // 着いた場所はもう候補にしない（暗くて見えないままでもくり返し向かわないように）。
    // Don't pick the reached tile again (even if it stays dark and unexplored).
    st.tried.insert( st.target );
    return plan_leg( you, here );
}

auto is_active() -> bool { return state().active; }

auto stop() -> void { state() = explore_state(); }

auto find_target( avatar &you, map &here ) -> std::optional<tripoint_bub_ms>
{
    for( const auto &p : frontier_tiles( you, here ) ) {
        if( !here.route( you.bub_pos(), p, you.get_legacy_pathfinding_settings(),
                         you.get_legacy_path_avoid() ).empty() ) {
            return p;
        }
    }
    return std::nullopt;
}

} // namespace auto_explore
