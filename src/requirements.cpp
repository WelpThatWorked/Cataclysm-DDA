#include "requirements.h"
#include "json.h"
#include "translations.h"
#include "game.h"
#include "player.h"
#include "map.h"
#include "debug.h"
#include "inventory.h"
#include "output.h"
#include "itype.h"
#include <sstream>
#include "calendar.h"
#include <cmath>
#include <algorithm>
#include "generic_factory.h"

static std::map<requirement_id, requirement_data> requirements_all;

template <>
const requirement_id string_id<requirement_data>::NULL_ID( "null" );

template<>
bool string_id<requirement_data>::is_valid() const
{
    return requirements_all.count( *this );
}

template<>
const requirement_data &string_id<requirement_data>::obj() const
{
    const auto found = requirements_all.find( *this );
    if( found == requirements_all.end() ) {
        debugmsg( "Tried to get invalid requirements: %s", c_str() );
        static const requirement_data null_requirement{};
        return null_requirement;
    }
    return found->second;
}

namespace {
generic_factory<quality> quality_factory( "tool quality" );
} // namespace

void quality::reset()
{
    quality_factory.reset();
}

void quality::load_static( JsonObject &jo )
{
    quality_factory.load( jo );
}

void quality::load( JsonObject &jo )
{
    mandatory( jo, was_loaded, "name", name, translated_string_reader );

    JsonArray arr = jo.get_array( "usages" );
    while( arr.has_more() ) {
        auto lvl = arr.next_array();
        auto funcs = lvl.get_array( 1 );
        while( funcs.has_more() ) {
            usages.emplace_back( lvl.get_int( 0 ), funcs.next_string() );
        }
    }
}

template<>
const quality &string_id<quality>::obj() const
{
    return quality_factory.obj( *this );
}

template<>
bool string_id<quality>::is_valid() const
{
    return quality_factory.is_valid( *this );
}

std::string quality_requirement::to_string( int ) const
{
    return string_format( ngettext( "%d tool with %s of %d or more.",
                                    "%d tools with %s of %d or more.", count ),
                          count, type.obj().name.c_str(), level );
}

bool tool_comp::by_charges() const
{
    return count > 0;
}

std::string tool_comp::to_string( int batch ) const
{
    if( by_charges() ) {
        //~ <tool-name> (<numer-of-charges> charges)
        return string_format( ngettext( "%s (%d charge)", "%s (%d charges)", count * batch ),
                              item::nname( type ).c_str(), count * batch );
    } else {
        return item::nname( type, abs( count ) );
    }
}

std::string item_comp::to_string( int batch ) const
{
    const int c = std::abs( count ) * batch;
    //~ <item-count> <item-name>
    return string_format( ngettext( "%d %s", "%d %s", c ), c, item::nname( type, c ).c_str() );
}

void quality_requirement::load( JsonArray &jsarr )
{
    JsonObject quality_data = jsarr.next_object();
    type = quality_id( quality_data.get_string( "id" ) );
    level = quality_data.get_int( "level", 1 );
    count = quality_data.get_int( "amount", 1 );
    if( count <= 0 ) {
        quality_data.throw_error( "quality amount must be a positive number", "amount" );
    }
    // Note: level is not checked, negative values and 0 are allow, see butchering quality.
}

void tool_comp::load( JsonArray &ja )
{
    if( ja.test_string() ) {
        // constructions uses this format: [ "tool", ... ]
        type = ja.next_string();
        count = -1;
    } else {
        JsonArray comp = ja.next_array();
        type = comp.get_string( 0 );
        count = comp.get_int( 1 );
    }
    if( count == 0 ) {
        ja.throw_error( "tool count must not be 0" );
    }
    // Note: negative count means charges (of the tool) should be consumed
}

void item_comp::load( JsonArray &ja )
{
    JsonArray comp = ja.next_array();
    type = comp.get_string( 0 );
    count = comp.get_int( 1 );
    // Recoverable is true by default.
    if( comp.size() > 2 ) {
        recoverable = comp.get_string( 2 ) == "NO_RECOVER" ? false : true;
    }
    if( count <= 0 ) {
        ja.throw_error( "item count must be a positive number" );
    }
}

template<typename T>
void requirement_data::load_obj_list( JsonArray &jsarr, std::vector< std::vector<T> > &objs )
{
    while( jsarr.has_more() ) {
        if( jsarr.test_array() ) {
            std::vector<T> choices;
            JsonArray ja = jsarr.next_array();
            while( ja.has_more() ) {
                choices.push_back( T() );
                choices.back().load( ja );
            }
            if( !choices.empty() ) {
                objs.push_back( choices );
            }
        } else {
            // tool qualities don't normally use a list of alternatives
            // each quality is mandatory.
            objs.push_back( std::vector<T>( 1 ) );
            objs.back().back().load( jsarr );
        }
    }
}

requirement_data requirement_data::operator*( unsigned scalar ) const
{
    requirement_data res = *this;
    for( auto &group : res.components ) {
        for( auto &e : group ) {
            e.count = std::max( e.count * int( scalar ), -1 );
        }
    }
    for( auto &group : res.tools ) {
        for( auto &e : group ) {
            e.count = std::max( e.count * int( scalar ), -1 );
        }
    }

    return res;
}

requirement_data requirement_data::operator+( const requirement_data &rhs ) const
{
    requirement_data res = *this;

    res.components.insert( res.components.end(), rhs.components.begin(), rhs.components.end() );
    res.tools.insert( res.tools.end(), rhs.tools.begin(), rhs.tools.end() );
    res.qualities.insert( res.qualities.end(), rhs.qualities.begin(), rhs.qualities.end() );

    // combined result is temporary which caller could store via @ref save_requirement
    res.id_ = requirement_id::NULL_ID;

    // @todo deduplicate qualites and combine other requirements

    return res;
}

void requirement_data::load_requirement( JsonObject &jsobj, const std::string &id )
{
    requirement_data req;

    JsonArray jsarr;
    jsarr = jsobj.get_array( "components" );
    req.load_obj_list( jsarr, req.components );
    jsarr = jsobj.get_array( "qualities" );
    req.load_obj_list( jsarr, req.qualities );
    jsarr = jsobj.get_array( "tools" );
    req.load_obj_list( jsarr, req.tools );

    if( !id.empty() ) {
        req.id_ = requirement_id( id );
    } else if( jsobj.has_string( "id" ) ) {
        req.id_ = requirement_id( jsobj.get_string( "id" ) );
    } else {
        jsobj.throw_error( "id was not specified for requirement" );
    }

    save_requirement( req );
}

void requirement_data::save_requirement( const requirement_data &req, const std::string &id )
{
    auto dup = req;
    if( !id.empty() ) {
        dup.id_ = requirement_id( id );
    }

    if( requirements_all.find( req.id_  ) != requirements_all.end() ) {
        DebugLog( D_INFO, DC_ALL ) << "Updated requirement: " << dup.id_.c_str();
    } else {
        DebugLog( D_INFO, DC_ALL ) << "Added requirement: " << dup.id_.c_str();
    }

    requirements_all[ dup.id_ ] = dup;
}

template<typename T>
bool requirement_data::any_marked_available( const std::vector<T> &comps )
{
    for( const auto &comp : comps ) {
        if( comp.available == a_true ) {
            return true;
        }
    }
    return false;
}

template<typename T>
std::string requirement_data::print_missing_objs( const std::string &header,
        const std::vector< std::vector<T> > &objs )
{
    std::ostringstream buffer;
    for( const auto &list : objs ) {
        if( any_marked_available( list ) ) {
            continue;
        }
        if( !buffer.str().empty() ) {
            buffer << "\n" << _( "and " );
        }
        for( auto it = list.begin(); it != list.end(); ++it ) {
            if( it != list.begin() ) {
                buffer << _( " or " );
            }
            buffer << it->to_string();
        }
    }
    if( buffer.str().empty() ) {
        return std::string();
    }
    return header + "\n" + buffer.str() + "\n";
}

std::string requirement_data::list_missing() const
{
    std::ostringstream buffer;
    buffer << print_missing_objs( _( "These tools are missing:" ), tools );
    buffer << print_missing_objs( _( "These tools are missing:" ), qualities );
    buffer << print_missing_objs( _( "Those components are missing:" ), components );
    return buffer.str();
}

void quality_requirement::check_consistency( const std::string &display_name ) const
{
    if( !type.is_valid() ) {
        debugmsg( "Unknown quality %s in %s", type.c_str(), display_name.c_str() );
    }
}

void component::check_consistency( const std::string &display_name ) const
{
    if( !item::type_is_defined( type ) ) {
        debugmsg( "%s in %s is not a valid item template", type.c_str(), display_name.c_str() );
    }
}

template<typename T>
void requirement_data::check_consistency( const std::vector< std::vector<T> > &vec,
        const std::string &display_name )
{
    for( const auto &list : vec ) {
        for( const auto &comp : list ) {
            comp.check_consistency( display_name );
        }
    }
}

const std::map<requirement_id, requirement_data> &requirement_data::all()
{
    return requirements_all;
}


void requirement_data::check_consistency()
{
    for( const auto &r : all() ) {
        check_consistency( r.second.tools, r.first.str() );
        check_consistency( r.second.components, r.first.str() );
        check_consistency( r.second.qualities, r.first.str() );
    }
}

void requirement_data::reset()
{
    requirements_all.clear();
}


int requirement_data::print_components( WINDOW *w, int ypos, int xpos, int width, nc_color col,
                                        const inventory &crafting_inv, int batch ) const
{
    if( components.empty() ) {
        return 0;
    }
    mvwprintz( w, ypos, xpos, col, _( "Components required:" ) );
    return print_list( w, ypos + 1, xpos, width, col, crafting_inv, components, batch ) + 1;
}

std::vector<std::string> requirement_data::get_folded_components_list( int width, nc_color col,
        const inventory &crafting_inv, int batch ) const
{
    std::vector<std::string> out_buffer;
    if( components.empty() ) {
        return out_buffer;
    }
    std::ostringstream current_line;
    current_line << "<color_" << string_from_color( col ) << ">" << _( "Components required:" ) <<
                 "</color>";
    out_buffer.push_back( current_line.str() );
    current_line.str( "" );

    std::vector<std::string> folded_buffer = get_folded_list( width, crafting_inv, components, batch );
    out_buffer.insert( out_buffer.end(), folded_buffer.begin(), folded_buffer.end() );

    return out_buffer;
}

template<typename T>
int requirement_data::print_list( WINDOW *w, int ypos, int xpos, int width, nc_color col,
                                  const inventory &crafting_inv, const std::vector< std::vector<T> > &objs,
                                  int batch )
{
    const int oldy = ypos;
    for( const auto &comp_list : objs ) {
        const bool has_one = any_marked_available( comp_list );
        std::ostringstream buffer;
        for( auto a = comp_list.begin(); a != comp_list.end(); ++a ) {
            if( a != comp_list.begin() ) {
                buffer << "<color_white> " << _( "OR" ) << "</color> ";
            }
            const std::string col = a->get_color( has_one, crafting_inv, batch );
            buffer << "<color_" << col << ">" << a->to_string( batch ) << "</color>";
        }
        mvwprintz( w, ypos, xpos, col, "> " );
        ypos += fold_and_print( w, ypos, xpos + 2, width - 2, col, buffer.str() );
    }
    return ypos - oldy;
}

template<typename T>
std::vector<std::string> requirement_data::get_folded_list( int width,
        const inventory &crafting_inv, const std::vector< std::vector<T> > &objs,
        int batch )
{
    std::vector<std::string> out_buffer;
    for( const auto &comp_list : objs ) {
        const bool has_one = any_marked_available( comp_list );
        std::ostringstream buffer;
        for( auto a = comp_list.begin(); a != comp_list.end(); ++a ) {
            if( a != comp_list.begin() ) {
                buffer << "<color_white> " << _( "OR" ) << "</color> ";
            }
            const std::string col = a->get_color( has_one, crafting_inv, batch );
            buffer << "<color_" << col << ">" << a->to_string( batch ) << "</color>";
        }
        std::vector<std::string> folded = foldstring( buffer.str(), width - 2 );

        for( size_t i = 0; i < folded.size(); i++ ) {
            if( i == 0 ) {
                out_buffer.push_back( std::string( "> " ).append( folded[i] ) );
            } else {
                out_buffer.push_back( std::string( "  " ).append( folded[i] ) );
            }
        }
    }
    return out_buffer;
}

int requirement_data::print_tools( WINDOW *w, int ypos, int xpos, int width, nc_color col,
                                   const inventory &crafting_inv, int batch ) const
{
    const int oldy = ypos;
    mvwprintz( w, ypos, xpos, col, _( "Tools required:" ) );
    ypos++;
    if( tools.empty() && qualities.empty() ) {
        mvwprintz( w, ypos, xpos, col, "> " );
        mvwprintz( w, ypos, xpos + 2, c_green, _( "NONE" ) );
        ypos++;
        return ypos - oldy;
    }
    ypos += print_list( w, ypos, xpos, width, col, crafting_inv, qualities );
    ypos += print_list( w, ypos, xpos, width, col, crafting_inv, tools, batch );
    return ypos - oldy;
}

std::vector<std::string> requirement_data::get_folded_tools_list( int width, nc_color col,
        const inventory &crafting_inv, int batch ) const
{
    std::vector<std::string> output_buffer;
    std::ostringstream current_line;
    current_line << "<color_" << string_from_color( col ) << ">" << _( "Tools required:" ) <<
                 "</color>";
    output_buffer.push_back( current_line.str() );
    current_line.str( "" );
    if( tools.empty() && qualities.empty() ) {
        current_line << "<color_" << string_from_color( col ) << ">" << "> " << "</color>";
        current_line << "<color_" << string_from_color( c_green ) << ">" << _( "NONE" ) << "</color>";
        output_buffer.push_back( current_line.str() );
        return output_buffer;
    }

    std::vector<std::string> folded_qualities = get_folded_list( width, crafting_inv, qualities );
    output_buffer.insert( output_buffer.end(), folded_qualities.begin(), folded_qualities.end() );

    std::vector<std::string> folded_tools = get_folded_list( width, crafting_inv, tools, batch );
    output_buffer.insert( output_buffer.end(), folded_tools.begin(), folded_tools.end() );
    return output_buffer;
}

bool requirement_data::can_make_with_inventory( const inventory &crafting_inv, int batch ) const
{
    bool retval = true;
    // All functions must be called to update the available settings in the components.
    if( !has_comps( crafting_inv, qualities ) ) {
        retval = false;
    }
    if( !has_comps( crafting_inv, tools, batch ) ) {
        retval = false;
    }
    if( !has_comps( crafting_inv, components, batch ) ) {
        retval = false;
    }
    if( !check_enough_materials( crafting_inv, batch ) ) {
        retval = false;
    }
    return retval;
}

template<typename T>
bool requirement_data::has_comps( const inventory &crafting_inv,
                                  const std::vector< std::vector<T> > &vec,
                                  int batch )
{
    bool retval = true;
    for( const auto &set_of_tools : vec ) {
        bool has_tool_in_set = false;
        for( const auto &tool : set_of_tools ) {
            if( tool.has( crafting_inv, batch ) ) {
                tool.available = a_true;
            } else {
                tool.available = a_false;
            }
            has_tool_in_set = has_tool_in_set || tool.available == a_true;
        }
        if( !has_tool_in_set ) {
            retval = false;
        }
    }
    return retval;
}

bool quality_requirement::has( const inventory &crafting_inv, int ) const
{
    return crafting_inv.has_quality( type, level, count );
}

std::string quality_requirement::get_color( bool, const inventory &, int ) const
{
    return available == a_true ? "green" : "red";
}

bool tool_comp::has( const inventory &crafting_inv, int batch ) const
{
    if( !by_charges() ) {
        return crafting_inv.has_tools( type, std::abs( count ) );
    } else {
        return crafting_inv.has_charges( type, count * batch );
    }
}

std::string tool_comp::get_color( bool has_one, const inventory &crafting_inv, int batch ) const
{
    if( available == a_insufficent ) {
        return "brown";
    } else if( !by_charges() && crafting_inv.has_tools( type, std::abs( count ) ) ) {
        return "green";
    } else if( by_charges() && crafting_inv.has_charges( type, count * batch ) ) {
        return "green";
    }
    return has_one ? "dkgray" : "red";
}

bool item_comp::has( const inventory &crafting_inv, int batch ) const
{
    // If you've Rope Webs, you can spin up the webbing to replace any amount of
    // rope your projects may require.  But you need to be somewhat nourished:
    // Famished or worse stops it.
    if( type == "rope_30" || type == "rope_6" ) {
        // NPC don't craft?
        // TODO: what about the amount of ropes vs the hunger?
        if( g->u.has_trait( "WEB_ROPE" ) && g->u.get_hunger() <= 300 ) {
            return true;
        }
    }
    const int cnt = std::abs( count ) * batch;
    if( item::count_by_charges( type ) ) {
        return crafting_inv.has_charges( type, cnt );
    } else {
        return crafting_inv.has_components( type, cnt );
    }
}

std::string item_comp::get_color( bool has_one, const inventory &crafting_inv, int batch ) const
{
    if( type == "rope_30" || type == "rope_6" ) {
        if( g->u.has_trait( "WEB_ROPE" ) && g->u.get_hunger() <= 300 ) {
            return "ltgreen"; // Show that WEB_ROPE is on the job!
        }
    }
    const int cnt = std::abs( count ) * batch;
    if( available == a_insufficent ) {
        return "brown";
    } else if( item::count_by_charges( type ) ) {
        if( crafting_inv.has_charges( type, cnt ) ) {
            return "green";
        }
    } else if( crafting_inv.has_components( type, cnt ) ) {
        return "green";
    }
    return has_one ? "dkgray" : "red";
}

template<typename T, typename ID>
const T *requirement_data::find_by_type( const std::vector< std::vector<T> > &vec,
        const ID &type )
{
    for( const auto &list : vec ) {
        for( const auto &comp : list ) {
            if( comp.type == type ) {
                return &comp;
            }
        }
    }
    return nullptr;
}

bool requirement_data::check_enough_materials( const inventory &crafting_inv, int batch ) const
{
    bool retval = true;
    for( const auto &component_choices : components ) {
        bool atleast_one_available = false;
        for( const auto &comp : component_choices ) {
            if( check_enough_materials( comp, crafting_inv, batch ) ) {
                atleast_one_available = true;
            }
        }
        if( !atleast_one_available ) {
            retval = false;
        }
    }
    return retval;
}

bool requirement_data::check_enough_materials( const item_comp &comp,
        const inventory &crafting_inv, int batch ) const
{
    if( comp.available != a_true ) {
        return false;
    }
    const int cnt = std::abs( comp.count ) * batch;
    const tool_comp *tq = find_by_type( tools, comp.type );
    if( tq != nullptr && tq->available == a_true ) {
        // The very same item type is also needed as tool!
        // Use charges of it, or use it by count?
        const int tc = tq->by_charges() ? 1 : std::abs( tq->count );
        // Check for components + tool count. Check item amount (excludes
        // pseudo items) and tool amount (includes pseudo items)
        // Imagine: required = 1 welder (component) + 1 welder (tool),
        // available = 1 welder (real item), 1 welding rig (creates
        // a pseudo welder item). has_components(welder,2) returns false
        // as there is only one real welder available, but has_tools(welder,2)
        // returns true.
        // Keep in mind that both requirements (tool+component) are checked
        // before this. That assures that one real item is actually available,
        // two welding rigs (and no real welder) would make this component
        // non-available even before this function is called.
        // Only ammo and (some) food is counted by charges, both are unlikely
        // to appear as tool, but it's possible /-:
        const item_comp i_tmp( comp.type, cnt + tc );
        const tool_comp t_tmp( comp.type, -( cnt + tc ) ); // not by charges!
        // batch factor is explicitly 1, because it's already included in the count.
        if( !i_tmp.has( crafting_inv, 1 ) && !t_tmp.has( crafting_inv, 1 ) ) {
            comp.available = a_insufficent;
        }
    }
    const itype *it = item::find_type( comp.type );
    for( const auto &ql : it->qualities ) {
        const quality_requirement *qr = find_by_type( qualities, ql.first );
        if( qr == nullptr || qr->level > ql.second ) {
            continue;
        }
        // This item can be used for the quality requirement, same as above for specific
        // tools applies.
        if( !crafting_inv.has_quality( qr->type, qr->level, qr->count + abs( comp.count ) ) ) {
            comp.available = a_insufficent;
        }
    }
    return comp.available == a_true;
}

template<typename T>
void requirement_data::remove_item( const std::string &type, std::vector< std::vector<T> > &vec )
{
    // remove all instances of @ref type from each of the options
    for( auto &opts : vec ) {
        opts.erase( std::remove_if( opts.begin(), opts.end(), [&type]( const T &e ) {
            return e.type == type;
        } ), opts.end() );
    }

    // if an option group is left empty then it can be removed
    vec.erase( std::remove_if( vec.begin(), vec.end(), []( const std::vector<T> &e ) {
        return e.empty();
    } ), vec.end() );
}

void requirement_data::remove_item( const std::string &type )
{
    remove_item( type, tools );
    remove_item( type, components );
}

const requirement_data::alter_tool_comp_vector &requirement_data::get_tools() const
{
    return tools;
}

const requirement_data::alter_quali_req_vector &requirement_data::get_qualities() const
{
    return qualities;
}

const requirement_data::alter_item_comp_vector &requirement_data::get_components() const
{
    return components;
}

requirement_data::alter_item_comp_vector &requirement_data::get_components()
{
    return components;
}

const requirement_data requirement_data::disassembly_requirements() const
{
    // TODO:
    // Allow jsonizing those tool replacements

    // Make a copy
    // Maybe TODO: Cache it somewhere and return a reference instead
    requirement_data ret = *this;
    auto new_qualities = std::vector<quality_requirement>();
    for( auto &it : ret.tools ) {
        bool replaced = false;
        for( const auto &tool : it ) {
            const itype_id &type = tool.type;

            // If crafting required a welder or forge then disassembly requires metal sawing
            if( type == "welder" || type == "welder_crude" || type == "oxy_torch" ||
                type == "forge" || type == "char_forge" ) {
                new_qualities.emplace_back( quality_id( "SAW_M_FINE" ), 1, 1 );
                replaced = true;
                break;
            }

            if( type == "sewing_kit" ||
                type == "mold_plastic" ) {
                new_qualities.emplace_back( quality_id( "CUT" ), 1, 1 );
                replaced = true;
                break;
            }

            if( type == "crucible" ) {
                replaced = true;
                break;
            }
        }

        if( replaced ) {
            // Replace the entire block of variants
            // This avoids the pesky integrated toolset
            it.clear();
        }
    }

    // Warning: This depends on the fact that tool qualities
    // are all mandatory (don't use variants)
    // If that ever changes, this will be wrong!
    if( ret.qualities.empty() ) {
        ret.qualities.resize( 1 );
    }

    auto &qualities = ret.qualities[0];
    qualities.insert( qualities.end(), new_qualities.begin(), new_qualities.end() );
    // Remove duplicate qualities
    {
        auto itr = std::unique( qualities.begin(), qualities.end(),
                                []( const quality_requirement & a, const quality_requirement & b ) {
                                    return a.type == b.type;
                                } );
        qualities.resize( std::distance( qualities.begin(), itr ) );
    }

    // Remove empty variant sections
    ret.tools.erase( std::remove_if( ret.tools.begin(), ret.tools.end(),
                                     []( const std::vector<tool_comp> &tcv ) {
                                         return tcv.empty();
                                     } ), ret.tools.end() );
    // Remove unrecoverable components
    ret.components.erase( std::remove_if( ret.components.begin(), ret.components.end(),
        []( std::vector<item_comp> &cov ) {
            cov.erase( std::remove_if( cov.begin(), cov.end(),
                []( const item_comp &comp ) {
                    return !comp.recoverable || item( comp.type ).has_flag( "UNRECOVERABLE" );
                } ), cov.end() );
            return cov.empty();
        } ), ret.components.end() );

    return ret;
}
