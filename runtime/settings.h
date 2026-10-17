// User settings (savagewind.ini in the data root, runtime/paths.h), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: "67%" in the ini now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { PERF_OFF = 0, PERF_FPS = 1, PERF_FULL = 2 };

typedef struct {
    bool fullscreen;
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    int perf_overlay;    // PERF_*: the performance overlay (F10 cycles it)
    bool dpad_move;      // the d-pad (and arrow keys) also act as the left stick while it is at rest
    bool mouse_camera;   // the mouse turns the camera (right stick); the cursor is held in the window while playing
    float mouse_sens;    // mouse camera speed, 1 = default
    bool mouse_invert_y; // mouse up looks down
    bool async_shaders;  // draw on while new shaders compile (no stutter; what needs them is skipped meanwhile)
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
