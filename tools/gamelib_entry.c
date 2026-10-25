// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the module layout,
// function tables and the game version's sites (the generated layout.c), found by name when the app loads the library.
// Everything else in it stays hidden.
#include "gamelib.h"

extern const GameLibModule game_modules[];
extern const uint32_t game_module_count;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) const char game_version[] = "1.8.2";
__attribute__((weak)) const GameLibSite game_sites[] = {
    {"controller_step", 0x7001c4ba40ull},
    {"dialog_answer", 0x7001c4b308ull},
    {"event_mgr", 0x7001cd8db8ull},
    {"frame_loop_interval_ret", 0x7001012a34ull},
    {"main_data", 0x7001a1f000ull},
    {"main_end", 0x7001d37000ull},
    {"opt_screen_rows", 0x3698ull},
    {"opt_table", 0x7001cccd98ull},
    {"pause_state", 0x7001cb48a4ull},
    {"player_info", 0x7001cb2658ull},
    {"screen_mgr", 0x7001cd5c38ull},
    {"title_options_state", 0x363cull},
    {"title_version_format", 0x70014b854eull},
    {"world_mgr", 0x7001cf5d00ull},
};
__attribute__((weak)) const uint32_t game_site_count = 14;
#else
extern const char game_version[];
extern const GameLibSite game_sites[];
extern const uint32_t game_site_count;
#endif

GAMELIB_EXPORT const GameLibTables *swgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, game_modules, &game_module_count,
                                    game_version, game_sites, &game_site_count};
    return &t;
}
