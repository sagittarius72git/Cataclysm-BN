// Assisted-by: Claude:claude-opus-5-5

#include "crafting_missing_parts.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "character.h"
#include "character_functions.h"
#include "color.h"
#include "construction.h"
#include "construction_group.h"
#include "inventory.h"
#include "item.h"
#include "itype.h"
#include "json.h"
#include "messages.h"
#include "output.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "requirements.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "ui.h"

namespace crafting_missing_parts
{
namespace
{

const activity_id ACT_CRAFT( "ACT_CRAFT" );

/// 進行中の状態を保存するキー（Creature の値として保存される）。
/// Key of the saved run state (stored as a Creature value, so it is saved with the game).
const std::string state_key = "craft_missing_parts";

/// 部品の部品…とさかのぼる最大の段数。
/// How many levels of "parts of parts" to search.
constexpr auto max_depth = 6;
/// 1回の実行で部品を作る最大の回数（予想外のくり返しを止めるため）。
/// Maximum number of part crafts in one run (stops unexpected loops).
constexpr auto max_crafts = 50;
/// 同じレシピを作る最大の回数。
/// Maximum number of crafts of the same recipe in one run.
constexpr auto max_same = 20;
/// 作り方を探すときに調べるレシピの最大数（重くなりすぎないように）。
/// Maximum number of recipes checked when searching (keeps it fast).
constexpr auto search_budget = 3000;
/// 確認画面で「作れるか」を調べるときの上限（足りないもの1つごと）。
/// Search limit per missing entry on the confirmation screen.
constexpr auto preview_budget = 500;
/// 確認画面に出す「足りないもの」の最大行数。
/// Maximum number of missing entries listed on the confirmation screen.
constexpr auto max_preview = 12;

enum class need_kind : int {
    component,
    tool,
    quality,
};

/// 足りないものの候補1つ。
/// One alternative of a missing requirement.
struct need_alt {
    need_kind kind = need_kind::component;
    itype_id type = itype_id::NULL_ID();
    quality_id quality = quality_id::NULL_ID();
    int level = 0;
    /// あといくつ足りないか / how many more are needed
    int short_by = 0;
    /// 量（チャージ）で使う道具か / tool used by charges
    bool by_charges = false;
};

/// 足りないもの1つ（候補のどれか1つがあればよい）。
/// One missing requirement (any one of the alternatives is enough).
using need_group = std::vector<need_alt>;

/// 進行中の状態。
/// State of a run in progress.
struct run_state {
    /// "recipe" または "construction" / "recipe" or "construction"
    std::string kind = "";
    std::string target = "";
    int batch = 1;
    /// 製作中のレシピ / recipe being crafted now
    std::string waiting = "";
    /// 目標そのものを作っているところか / crafting the target itself
    bool final_step = false;
    int crafts = 0;
    /// レシピごとの製作回数 / crafts per recipe
    std::map<std::string, int> same = {};
};

auto save_state( Character &who, const run_state &st ) -> void
{
    auto out = std::ostringstream();
    auto jsout = JsonOut( out );
    jsout.start_object();
    jsout.member( "kind", st.kind );
    jsout.member( "target", st.target );
    jsout.member( "batch", st.batch );
    jsout.member( "waiting", st.waiting );
    jsout.member( "final", st.final_step );
    jsout.member( "crafts", st.crafts );
    jsout.member( "same", st.same );
    jsout.end_object();
    who.set_value( state_key, out.str() );
}

auto load_state( const Character &who ) -> std::optional<run_state>
{
    const auto text = who.get_value( state_key );
    if( text.empty() ) {
        return std::nullopt;
    }
    try {
        auto in = std::istringstream( text );
        auto jsin = JsonIn( in );
        auto jo = jsin.get_object();
        jo.allow_omitted_members();
        auto st = run_state();
        jo.read( "kind", st.kind );
        jo.read( "target", st.target );
        jo.read( "batch", st.batch );
        jo.read( "waiting", st.waiting );
        jo.read( "final", st.final_step );
        jo.read( "crafts", st.crafts );
        jo.read( "same", st.same );
        return st;
    } catch( const JsonError & ) {
        return std::nullopt;
    }
}

auto clear_state( Character &who ) -> void { who.remove_value( state_key ); }

/// 完成品 → レシピ、道具の品質 → レシピ の索引。
/// Index of recipes by result item and by tool quality of the result.
struct recipe_index {
    struct quality_entry {
        int level = 0;
        const recipe *rec = nullptr;
    };
    std::unordered_map<itype_id, std::vector<const recipe *>> by_result;
    std::unordered_map<quality_id, std::vector<quality_entry>> by_quality;
};

auto build_index() -> recipe_index
{
    auto idx = recipe_index();
    for( const auto &[id, rec] : recipe_dict ) {
        if( rec.obsolete || rec.is_blacklisted() || rec.is_nested() || rec.never_learn ||
            rec.result().is_null() || !rec.result().is_valid() ) {
            continue;
        }
        idx.by_result[rec.result()].push_back( &rec );
        for( const auto &[qual, level] : rec.result().obj().qualities ) {
            idx.by_quality[qual].push_back( { .level = level, .rec = &rec } );
        }
    }
    return idx;
}

auto alt_key( const need_alt &a ) -> std::string
{
    if( a.kind == need_kind::quality ) {
        return string_format( "q:%s:%d", a.quality.str(), a.level );
    }
    return "i:" + a.type.str();
}

/// 作る対象にしてよい候補か（量で使う道具のチャージは作らない）。
/// Whether the alternative can be crafted (charges of a tool are not crafted).
auto craftable( const need_alt &a ) -> bool { return !( a.kind == need_kind::tool && a.by_charges ); }

/// 足りないものだけを返す。
/// Returns only the requirements that are not met.
auto missing_groups( const requirement_data &req, const inventory &inv,
                     const std::function<bool( const item & )> &filter ) -> std::vector<need_group>
{
    auto out = std::vector<need_group>();

    for( const auto &grp : req.get_components() ) {
        auto alts = need_group();
        auto satisfied = false;
        for( const auto &c : grp ) {
            const auto need = std::abs( c.count );
            const auto have = item::count_by_charges( c.type )
                              ? inv.charges_of( c.type, need, filter )
                              : inv.amount_of( c.type, false, need, filter );
            if( have >= need ) {
                satisfied = true;
                break;
            }
            alts.push_back( { .kind = need_kind::component, .type = c.type, .short_by = need - have } );
        }
        if( !satisfied && !alts.empty() ) {
            out.push_back( std::move( alts ) );
        }
    }

    for( const auto &grp : req.get_tools() ) {
        auto alts = need_group();
        auto satisfied = false;
        for( const auto &t : grp ) {
            if( t.by_charges() ) {
                const auto have = inv.charges_of( t.type, t.count, filter );
                if( have >= t.count ) {
                    satisfied = true;
                    break;
                }
                alts.push_back( { .kind = need_kind::tool, .type = t.type, .short_by = t.count - have,
                                  .by_charges = true } );
            } else {
                if( inv.has_tools( t.type, 1, filter ) ) {
                    satisfied = true;
                    break;
                }
                alts.push_back( { .kind = need_kind::tool, .type = t.type, .short_by = 1 } );
            }
        }
        if( !satisfied && !alts.empty() ) {
            out.push_back( std::move( alts ) );
        }
    }

    for( const auto &grp : req.get_qualities() ) {
        auto alts = need_group();
        auto satisfied = false;
        for( const auto &q : grp ) {
            if( inv.has_quality( q.type, q.level, q.count ) ) {
                satisfied = true;
                break;
            }
            alts.push_back( { .kind = need_kind::quality, .quality = q.type, .level = q.level,
                              .short_by = q.count } );
        }
        if( !satisfied && !alts.empty() ) {
            out.push_back( std::move( alts ) );
        }
    }

    return out;
}

/// 足りない部品のうち、今すぐ作れるものを1つ探す。
/// Searches for one missing part that can be crafted right now.
class planner
{
    public:
        planner( Character &who, const recipe_index &idx, int budget )
            : who( who ), idx( idx ), inv( who.crafting_inventory() ),
              helpers( character_funcs::get_crafting_helpers( who ) ), budget( budget ) {}

        /// この品をたどらない（目標の品を材料にするレシピなど）。
        /// Never search through this item (e.g. recipes that use the target itself).
        auto skip_result( const itype_id &type ) -> void { visiting.insert( type ); }

        auto missing( const requirement_data &req,
                      const std::function<bool( const item & )> &filter ) const -> std::vector<need_group> {
            return missing_groups( req, inv, filter );
        }

        /// 1. 足りない部品そのもので、今すぐ作れるレシピ
        /// 2. なければ、知っているレシピの材料（部品の部品）へさかのぼる
        /// 1. a recipe for a missing part that can be crafted now
        /// 2. otherwise, go back through the parts of known recipes
        auto find_step( const std::vector<need_group> &missing, int depth ) -> const recipe * { // *NOPAD*
            if( depth > max_depth ) {
                return nullptr;
            }
            for( const auto &grp : missing ) {
                for( const auto &a : grp ) {
                    if( !craftable( a ) ) {
                        continue;
                    }
                    for( const auto *r : producers( a ) ) {
                        if( visiting.contains( r->result() ) ) {
                            continue;
                        }
                        if( --budget < 0 ) {
                            return nullptr;
                        }
                        if( who.can_make( r, 1 ) ) {
                            return r;
                        }
                    }
                }
            }
            for( const auto &grp : missing ) {
                for( const auto &a : grp ) {
                    const auto key = alt_key( a );
                    if( !craftable( a ) || dead.contains( key ) ) {
                        continue;
                    }
                    for( const auto *r : producers( a ) ) {
                        if( budget < 0 ) {
                            return nullptr;
                        }
                        if( visiting.contains( r->result() ) || who.has_recipe( r, inv, helpers ) < 0 ) {
                            continue;
                        }
                        visiting.insert( r->result() );
                        const auto *found = find_step( missing_groups( r->simple_requirements(), inv,
                                                       r->get_component_filter() ), depth + 1 );
                        visiting.erase( r->result() );
                        if( found ) {
                            return found;
                        }
                    }
                    dead.insert( key );
                }
            }
            return nullptr;
        }

    private:
        auto producers( const need_alt &a ) const -> std::vector<const recipe *> {
            if( a.kind != need_kind::quality ) {
                const auto it = idx.by_result.find( a.type );
                return it == idx.by_result.end() ? std::vector<const recipe *>() : it->second;
            }
            const auto it = idx.by_quality.find( a.quality );
            if( it == idx.by_quality.end() ) {
                return {};
            }
            auto out = std::vector<const recipe *>();
            for( const auto &e : it->second ) {
                if( e.level >= a.level ) {
                    out.push_back( e.rec );
                }
            }
            return out;
        }

        Character &who;
        const recipe_index &idx;
        const inventory &inv;
        std::vector<npc *> helpers;
        int budget;
        std::set<itype_id> visiting;
        std::set<std::string> dead;
};

// ---------------------------------------------------------------------------
// 表示用の文 / Text for display
// ---------------------------------------------------------------------------

/// 候補1つ：「革 x6」「はさみ」「切断 2」
/// One alternative: "leather x6", "scissors", "cutting 2"
auto alt_label( const need_alt &a ) -> std::string
{
    switch( a.kind ) {
        case need_kind::quality:
            //~ %1$s: tool quality name, %2$d: quality level
            return string_format( pgettext( "craft missing parts", "%1$s %2$d" ),
                                  a.quality->name.translated(), a.level );
        case need_kind::tool:
            if( !a.by_charges ) {
                return item::nname( a.type, 1 );
            }
            break;
        case need_kind::component:
            break;
    }
    //~ %1$s: item name, %2$d: amount
    return string_format( pgettext( "craft missing parts", "%1$s x%2$d" ),
                          item::nname( a.type, a.short_by ), a.short_by );
}

/// 足りないもの1つ：「革 x6 / なめし革 x6」
/// One missing requirement: "leather x6 / tanned leather x6"
auto group_label( const need_group &grp ) -> std::string
{
    constexpr auto max_alts = 3;
    auto parts = std::vector<std::string>();
    for( const auto &a : grp ) {
        if( static_cast<int>( parts.size() ) >= max_alts ) {
            parts.push_back( string_format( _( "(+%d more)" ),
                                            static_cast<int>( grp.size() ) - max_alts ) );
            break;
        }
        parts.push_back( alt_label( a ) );
    }
    auto out = std::string();
    for( const auto &p : parts ) {
        if( !out.empty() ) {
            out += " / ";
        }
        out += p;
    }
    return out;
}

auto join_labels( const std::vector<need_group> &groups ) -> std::string
{
    auto out = std::string();
    for( const auto &grp : groups ) {
        if( !out.empty() ) {
            out += ", ";
        }
        out += group_label( grp );
    }
    return out;
}

// ---------------------------------------------------------------------------
// 目標 / Target
// ---------------------------------------------------------------------------

/// 目標の情報（レシピまたは建設）。
/// Information about the target (a recipe or a construction).
struct target_info {
    std::string name = "";
    requirement_data req = {};
    std::function<bool( const item & )> filter = nullptr;
    /// 製作のときの完成品（建設では空）/ result item for a recipe (empty for a construction)
    std::optional<itype_id> result = std::nullopt;
    const recipe *rec = nullptr;
    const construction *con = nullptr;
};

auto target_of( const run_state &st ) -> std::optional<target_info>
{
    if( st.kind == "recipe" ) {
        const auto rid = recipe_id( st.target );
        if( !rid.is_valid() ) {
            return std::nullopt;
        }
        const auto &r = rid.obj();
        return target_info{
            .name = r.result_name(),
            .req = r.simple_requirements() * std::max( st.batch, 1 ),
            .filter = r.get_component_filter(),
            .result = r.result(),
            .rec = &r,
        };
    }
    if( st.kind == "construction" ) {
        const auto cid = construction_str_id( st.target );
        if( !cid.is_valid() ) {
            return std::nullopt;
        }
        const auto &c = cid.obj();
        return target_info{
            .name = c.group->name(),
            .req = c.requirements.obj(),
            .filter = is_crafting_component,
            .con = &c,
        };
    }
    return std::nullopt;
}

/// 目標がもうそろっているか（建設は材料と道具だけを見る）。
/// Whether the target is ready (for a construction, only materials and tools are checked).
auto target_ready( Character &who, const target_info &t, int batch ) -> bool
{
    if( t.rec ) {
        return who.can_make( t.rec, batch );
    }
    return t.req.can_make_with_inventory( who.crafting_inventory(), t.filter );
}

auto make_planner( Character &who, const recipe_index &idx, const target_info &t,
                   int budget ) -> planner
{
    auto p = planner( who, idx, budget );
    if( t.result ) {
        p.skip_result( *t.result );
    }
    return p;
}

/// 行き詰まったとき、何が足りないかを知らせて止める。
/// Tells what is still missing and stops.
auto report_stuck( Character &who, const target_info &t ) -> void
{
    const auto missing = missing_groups( t.req, who.crafting_inventory(), t.filter );
    if( !missing.empty() ) {
        who.add_msg_if_player( m_bad, _( "Can't make the missing parts for %1$s.  Still missing: %2$s" ),
                               t.name, join_labels( missing ) );
    } else {
        who.add_msg_if_player( m_bad,
                               _( "The parts for %s are ready, but it still can't be made.  Check your skills and the recipe." ),
                               t.name );
    }
    clear_state( who );
}

/// 次の1回を決めて始める。
/// Decides the next craft and starts it.
auto next_step( Character &who, run_state st ) -> void
{
    const auto t = target_of( st );
    if( !t ) {
        clear_state( who );
        return;
    }
    who.invalidate_crafting_inventory();

    if( target_ready( who, *t, st.batch ) ) {
        if( t->con ) {
            who.add_msg_if_player( m_good,
                                   _( "All materials for %s are ready.  Build it from the construction menu." ),
                                   t->name );
            clear_state( who );
            return;
        }
        if( !who.making_would_work( t->rec->ident(), st.batch ) ) {
            clear_state( who );
            return;
        }
        who.add_msg_if_player( m_good, _( "All parts are ready.  Crafting %s." ), t->name );
        st.waiting = t->rec->ident().str();
        st.final_step = true;
        save_state( who, st );
        who.make_craft( t->rec->ident(), st.batch );
        if( !who.has_activity( ACT_CRAFT ) ) {
            clear_state( who );
        }
        return;
    }

    if( st.crafts >= max_crafts ) {
        who.add_msg_if_player( m_info, _( "Stopped after making parts %d times." ), st.crafts );
        clear_state( who );
        return;
    }

    const auto idx = build_index();
    auto p = make_planner( who, idx, *t, search_budget );
    const auto *step = p.find_step( p.missing( t->req, t->filter ), 1 );
    if( !step ) {
        report_stuck( who, *t );
        return;
    }

    const auto step_id = step->ident().str();
    if( ++st.same[step_id] > max_same ) {
        who.add_msg_if_player( m_info, _( "Stopped: kept making %s over and over." ),
                               step->result_name() );
        clear_state( who );
        return;
    }
    if( !who.making_would_work( step->ident(), 1 ) ) {
        clear_state( who );
        return;
    }

    who.add_msg_if_player( m_info, _( "Making %1$s for %2$s." ), step->result_name(), t->name );
    st.waiting = step_id;
    st.final_step = false;
    st.crafts++;
    save_state( who, st );
    who.make_craft( step->ident(), 1 );
    if( !who.has_activity( ACT_CRAFT ) ) {
        who.add_msg_if_player( m_info, _( "Stopped gathering the parts for %s." ), t->name );
        clear_state( who );
    }
}

/// 確認画面を出して、了承されたら始める。
/// Shows the confirmation screen and starts if the player agrees.
auto confirm_and_start( Character &who, const run_state &st ) -> bool
{
    const auto t = target_of( st );
    if( !t ) {
        return false;
    }
    who.invalidate_crafting_inventory();
    if( target_ready( who, *t, st.batch ) ) {
        popup( _( "Nothing is missing for %s." ), t->name );
        return false;
    }

    const auto idx = build_index();
    auto p = make_planner( who, idx, *t, search_budget );
    const auto missing = p.missing( t->req, t->filter );
    if( missing.empty() ) {
        popup( _( "Nothing is missing for %s, but it still can't be made.  Check your skills and the recipe." ),
               t->name );
        return false;
    }

    auto text = string_format( _( "Craft the missing parts for %s?" ), t->name ) + "\n\n";
    text += _( "Missing:" );
    text += "\n";
    auto shown = 0;
    for( const auto &grp : missing ) {
        if( shown >= max_preview ) {
            text += string_format( _( "  ...and %d more" ), static_cast<int>( missing.size() ) - shown );
            text += "\n";
            break;
        }
        // このグループだけを対象に作り方を探して、作れそうかを添える。
        // Search only for this entry to show whether it can be made.
        auto preview = make_planner( who, idx, *t, preview_budget );
        const auto can = preview.find_step( { grp }, 1 ) != nullptr;
        text += string_format( "  %s  %s\n", group_label( grp ),
                               can ? colorize( _( "[can make]" ), c_light_green )
                               : colorize( _( "[no way to make]" ), c_light_red ) );
        shown++;
    }
    text += "\n";

    if( p.find_step( missing, 1 ) == nullptr ) {
        text += _( "None of the missing parts can be made right now." );
        popup( text );
        return false;
    }
    text += _( "Parts are crafted one at a time, the same as crafting from this menu." );
    if( !query_yn( text ) ) {
        return false;
    }

    who.add_msg_if_player( m_info, _( "Started gathering the parts for %s." ), t->name );
    next_step( who, st );
    return true;
}

} // namespace

auto start_for_recipe( Character &who, const recipe &target, int batch_size ) -> bool
{
    if( target.is_nested() ) {
        return false;
    }
    return confirm_and_start( who, {
        .kind = "recipe",
        .target = target.ident().str(),
        .batch = std::max( batch_size, 1 ),
    } );
}

auto start_recipe_run( Character &who, const recipe &target, int batch_size ) -> void
{
    next_step( who, {
        .kind = "recipe",
        .target = target.ident().str(),
        .batch = std::max( batch_size, 1 ),
    } );
}

auto next_part( Character &who, const recipe &target, int batch_size ) -> const recipe * // *NOPAD*
{
    const auto batch = std::max( batch_size, 1 );
    who.invalidate_crafting_inventory();
    if( who.can_make( &target, batch ) ) {
        return &target;
    }
    const auto t = target_of( { .kind = "recipe", .target = target.ident().str(), .batch = batch } );
    if( !t ) {
        return nullptr;
    }
    const auto idx = build_index();
    auto p = make_planner( who, idx, *t, search_budget );
    return p.find_step( p.missing( t->req, t->filter ), 1 );
}

auto start_for_construction( Character &who,
                             const std::vector<const construction *> &options ) -> bool
{
    if( options.empty() ) {
        return false;
    }
    const auto *chosen = options.front();
    if( options.size() > 1 ) {
        // 同じ建設に材料違いの段階があるので、材料を添えて選ばせる。
        // A construction can have stages with different materials, so list them with materials.
        const auto &inv = who.crafting_inventory();
        auto menu = uilist();
        menu.text = _( "Gather the materials for which one?" );
        for( size_t i = 0; i < options.size(); i++ ) {
            const auto *c = options[i];
            auto all = std::vector<need_group>();
            for( const auto &grp : c->requirements->get_components() ) {
                auto alts = need_group();
                for( const auto &comp : grp ) {
                    alts.push_back( { .kind = need_kind::component, .type = comp.type,
                                      .short_by = std::abs( comp.count ) } );
                }
                all.push_back( std::move( alts ) );
            }
            auto label = c->group->name();
            if( !all.empty() ) {
                label += "  (" + join_labels( all ) + ")";
            }
            if( c->requirements->can_make_with_inventory( inv, is_crafting_component ) ) {
                label += "  " + colorize( _( "[ready]" ), c_light_green );
            }
            menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, label );
        }
        menu.query();
        if( menu.ret < 0 || menu.ret >= static_cast<int>( options.size() ) ) {
            return false;
        }
        chosen = options[menu.ret];
    }
    return confirm_and_start( who, {
        .kind = "construction",
        .target = chosen->id.str(),
    } );
}

auto on_craft_finished( Character &who, const recipe &finished ) -> void
{
    const auto st = load_state( who );
    if( !st ) {
        return;
    }
    if( st->waiting != finished.ident().str() ) {
        // 別の製作が終わった＝前の実行は途中でやめられている。
        // A different craft finished, so the earlier run was abandoned.
        clear_state( who );
        return;
    }
    if( st->final_step ) {
        who.add_msg_if_player( m_good, _( "Finished crafting %s." ), finished.result_name() );
        clear_state( who );
        return;
    }
    next_step( who, *st );
}

} // namespace crafting_missing_parts
