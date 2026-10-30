/*
 * SDL2 video, audio, keyboard and game-controller input.
 * Keyboard bindings are loaded from controls.cfg.
 * SDL-mapped controllers and generic joysticks are supported.
 * Controllers are assigned to players through their control profiles.
 * Escape quits the game.
 */
#include "ng_internal.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define MAX_INPUT_DEVICES 8
#define AXIS_DEADZONE 8000

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static uint64_t s_next_tick;
static SDL_AudioDeviceID s_audio;
static bool s_menu_open = false;
static int s_menu_selection = 0;

typedef enum {
    MENU_MAIN = 0,
    MENU_CONTROLS,
    MENU_GRAPHICS,
    MENU_KEYBOARD_P1,
    MENU_KEYBOARD_P2,
    MENU_KEYBOARD_SYSTEM,
    MENU_PAD_P1,
    MENU_PAD_P2
} menu_screen_t;

static menu_screen_t s_menu_screen = MENU_MAIN;
static bool s_menu_wait_release = false;
static bool s_menu_capturing = false;
static int s_capture_action = -1;

static SDL_Scancode s_conflict_scancode = SDL_SCANCODE_UNKNOWN;
static int s_conflict_action = -1;

static char s_menu_status[64] = "";


/* ponytail: a fixed layout; remapping belongs to the frontend (ROADMAP.md) */

typedef struct {
    SDL_Scancode p1[8], p2[8];
    SDL_Scancode start1, start2, select1, coin1, coin2, service, test;
    SDL_Scancode overlay_toggle, fullscreen_toggle;
} bindings_t;

typedef struct {
    SDL_GameController *controller;
    SDL_Joystick *joystick;
    SDL_JoystickID instance_id;
    char guid[33], serial[128], path[256];
} input_device_t;

typedef enum {
    PAD_NONE = 0,
    PAD_BUTTON,
    PAD_HAT,
    PAD_AXIS_NEG,
    PAD_AXIS_POS
} pad_type_t;

typedef struct {
    pad_type_t type;
    int index;
    int direction;
} pad_binding_t;

enum {
    PAD_SELECT = 0,
    PAD_SERVICE,
    PAD_TEST,
    PAD_SYSTEM_COUNT
};

static pad_binding_t pad_players[2][10];
static pad_binding_t pad_system[PAD_SYSTEM_COUNT];
static bool s_pad_capturing = false;
static bool s_pad_capture_wait_neutral = true;
static int s_pad_capture_player = -1;
static int s_pad_capture_action = -1;
static int s_pad_conflict_action = -1;
static pad_binding_t s_pad_conflict_binding;
static bindings_t b;

/* Physical inventory of connected devices. */
static input_device_t devices[MAX_INPUT_DEVICES];

/* Physical device slot assigned to each player.
   -1 means that the player has no controller assigned. */
static int player_device[2] = { -1, -1 };
static char player_device_guid[2][33], player_device_serial[2][128];
static char player_device_path[2][256];
static bool s_pad_reassigning = false;

static const char *names[25] = {
    "P1_UP", "P1_DOWN", "P1_LEFT", "P1_RIGHT",
    "P1_A", "P1_B", "P1_C", "P1_D",
    "P2_UP", "P2_DOWN", "P2_LEFT", "P2_RIGHT",
    "P2_A", "P2_B", "P2_C", "P2_D",
    "START1", "START2", "SELECT", "COIN1", "COIN2",
    "SERVICE", "TEST", "OVERLAY", "FULLSCREEN"
};

static SDL_Scancode *slot(const char *n) {
    int i;

    for (i = 0; i < 8; i++)
        if (!strcmp(n, names[i])) return &b.p1[i];

    for (i = 0; i < 8; i++)
        if (!strcmp(n, names[8 + i])) return &b.p2[i];

    if (!strcmp(n, "START1")) return &b.start1;
    if (!strcmp(n, "START2")) return &b.start2;
    if (!strcmp(n, "SELECT")) return &b.select1;
    if (!strcmp(n, "COIN1")) return &b.coin1;
    if (!strcmp(n, "COIN2")) return &b.coin2;
    if (!strcmp(n, "SERVICE")) return &b.service;
    if (!strcmp(n, "TEST")) return &b.test;
    if (!strcmp(n, "OVERLAY")) return &b.overlay_toggle;
    if (!strcmp(n, "FULLSCREEN")) return &b.fullscreen_toggle;

    return NULL;
}

static void defaults(void) {
    SDL_Scancode p1[8] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X,
        SDL_SCANCODE_C, SDL_SCANCODE_V
    };

    SDL_Scancode p2[8] = {
        SDL_SCANCODE_W, SDL_SCANCODE_S,
        SDL_SCANCODE_A, SDL_SCANCODE_D,
        SDL_SCANCODE_F, SDL_SCANCODE_G,
        SDL_SCANCODE_H, SDL_SCANCODE_J
    };

    memcpy(b.p1, p1, sizeof p1);
    memcpy(b.p2, p2, sizeof p2);

    b.start1 = SDL_SCANCODE_1;
    b.start2 = SDL_SCANCODE_2;
    b.select1 = SDL_SCANCODE_3;
    b.coin1 = SDL_SCANCODE_5;
    b.coin2 = SDL_SCANCODE_6;
    b.service = SDL_SCANCODE_9;
    b.test = SDL_SCANCODE_F2;
    b.overlay_toggle = SDL_SCANCODE_F1;
    b.fullscreen_toggle = SDL_SCANCODE_F11;
}

static pad_binding_t *pad_slot(const char *name) {
    static const char *player_names[2][8] = {
        {
            "PAD_P1_UP", "PAD_P1_DOWN",
            "PAD_P1_LEFT", "PAD_P1_RIGHT",
            "PAD_P1_A", "PAD_P1_B",
            "PAD_P1_C", "PAD_P1_D"
        },
        {
            "PAD_P2_UP", "PAD_P2_DOWN",
            "PAD_P2_LEFT", "PAD_P2_RIGHT",
            "PAD_P2_A", "PAD_P2_B",
            "PAD_P2_C", "PAD_P2_D"
        }
    };

    static const char *system_names[PAD_SYSTEM_COUNT] = {
        "PAD_SELECT",
        "PAD_SERVICE",
        "PAD_TEST"
    };

    int p, i;

    for (p = 0; p < 2; p++) {
        for (i = 0; i < 8; i++) {
            if (strcmp(name, player_names[p][i]) == 0)
                return &pad_players[p][i];
        }
    }

    /* START y COIN conservan sus nombres antiguos en controls.cfg. */
    if (strcmp(name, "PAD_START1") == 0)
        return &pad_players[0][8];

    if (strcmp(name, "PAD_COIN1") == 0)
        return &pad_players[0][9];

    if (strcmp(name, "PAD_START2") == 0)
        return &pad_players[1][8];

    if (strcmp(name, "PAD_COIN2") == 0)
        return &pad_players[1][9];

    for (i = 0; i < PAD_SYSTEM_COUNT; i++) {
        if (strcmp(name, system_names[i]) == 0)
            return &pad_system[i];
    }

    return NULL;
}

static int parse_pad_binding(const char *value,
                             pad_binding_t *out) {
    int index;
    char direction[8];
    char extra;

    if (strcmp(value, "NONE") == 0) {
        out->type = PAD_NONE;
        out->index = 0;
        out->direction = 0;
        return 1;
    }

    if (sscanf(value, "BTN%d%c", &index, &extra) == 1 &&
        index >= 0 && index < 128) {
        out->type = PAD_BUTTON;
        out->index = index;
        out->direction = 0;
        return 1;
    }

    if (sscanf(value, "HAT%d_%7s%c",
               &index, direction, &extra) == 2 &&
        index >= 0 && index < 16) {
        out->type = PAD_HAT;
        out->index = index;

        if (strcmp(direction, "UP") == 0)
            out->direction = SDL_HAT_UP;
        else if (strcmp(direction, "DOWN") == 0)
            out->direction = SDL_HAT_DOWN;
        else if (strcmp(direction, "LEFT") == 0)
            out->direction = SDL_HAT_LEFT;
        else if (strcmp(direction, "RIGHT") == 0)
            out->direction = SDL_HAT_RIGHT;
        else
            return 0;

        return 1;
    }

    if (sscanf(value, "AXIS%d_%7s%c",
               &index, direction, &extra) == 2 &&
        index >= 0 && index < 16) {
        out->type = PAD_AXIS_NEG;
        out->index = index;

        if (strcmp(direction, "NEG") == 0)
            out->type = PAD_AXIS_NEG;
        else if (strcmp(direction, "POS") == 0)
            out->type = PAD_AXIS_POS;
        else
            return 0;

        out->direction = 0;
        return 1;
    }

    return 0;
}

static int pad_binding_active(SDL_Joystick *j,
                              const pad_binding_t *binding) {
    if (!j || !SDL_JoystickGetAttached(j))
        return 0;

    switch (binding->type) {
    case PAD_BUTTON:
        return binding->index < SDL_JoystickNumButtons(j) &&
               SDL_JoystickGetButton(j, binding->index);

    case PAD_HAT:
        if (binding->index >= SDL_JoystickNumHats(j))
            return 0;
        return (SDL_JoystickGetHat(j, binding->index) &
                binding->direction) != 0;

    case PAD_AXIS_NEG:
        return binding->index < SDL_JoystickNumAxes(j) &&
               SDL_JoystickGetAxis(j, binding->index)
                   < -AXIS_DEADZONE;

    case PAD_AXIS_POS:
        return binding->index < SDL_JoystickNumAxes(j) &&
               SDL_JoystickGetAxis(j, binding->index)
                   > AXIS_DEADZONE;

    case PAD_NONE:
    default:
        return 0;
    }
}


int window_capture_controller(char *binding, size_t binding_size) {
    SDL_Joystick *joystick = NULL;
    SDL_GameController *controller = NULL;
    SDL_Event event;
    Uint32 started;
    int i, result = 1;
    int waiting_for_release = 1;

    if (!binding || binding_size == 0)
        return 1;

    binding[0] = '\0';

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "[capture] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    /*
     * Open the first available joystick. Keep its instance ID so
     * input from another connected device cannot be captured by mistake.
     */
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i))
            controller = SDL_GameControllerOpen(i);

        if (controller) {
            joystick = SDL_GameControllerGetJoystick(controller);
            if (joystick)
                break;

            SDL_GameControllerClose(controller);
            controller = NULL;
        }

        joystick = SDL_JoystickOpen(i);
        if (joystick)
            break;
    }

    if (!joystick) {
        fprintf(stderr, "[capture] No controller detected.\n");
        goto cleanup;
    }

    {
        SDL_JoystickID instance_id = SDL_JoystickInstanceID(joystick);
        int neutral = 0;

        /*
         * First wait until every button, hat and axis is neutral.
         * This avoids immediately assigning an input held at startup.
         */
        while (!neutral) {
            int buttons_neutral = 1;
            int axes_neutral = 1;
            int hats_neutral = 1;

            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN &&
                     event.key.keysym.sym == SDLK_ESCAPE))
                    goto cleanup;

                if ((event.type == SDL_JOYBUTTONDOWN ||
                     event.type == SDL_JOYHATMOTION ||
                     event.type == SDL_JOYAXISMOTION) &&
                    event.jdevice.which != instance_id) {
                    continue;
                }
            }

            for (i = 0; i < SDL_JoystickNumButtons(joystick); i++) {
                if (SDL_JoystickGetButton(joystick, i)) {
                    buttons_neutral = 0;
                    break;
                }
            }

            for (i = 0; i < SDL_JoystickNumAxes(joystick); i++) {
                if (SDL_JoystickGetAxis(joystick, i) > AXIS_DEADZONE ||
                    SDL_JoystickGetAxis(joystick, i) < -AXIS_DEADZONE) {
                    axes_neutral = 0;
                    break;
                }
            }

            for (i = 0; i < SDL_JoystickNumHats(joystick); i++) {
                if (SDL_JoystickGetHat(joystick, i) != SDL_HAT_CENTERED) {
                    hats_neutral = 0;
                    break;
                }
            }

            neutral = buttons_neutral && axes_neutral && hats_neutral;
            if (!neutral) {
                static Uint32 last_report = 0;
                Uint32 now = SDL_GetTicks();

                if (now - last_report >= 1000) {
                    fprintf(stderr, "[capture] Waiting for neutral: buttons=%d axes=%d hats=%d\n",
                            buttons_neutral, axes_neutral, hats_neutral);

                    for (i = 0; i < SDL_JoystickNumAxes(joystick); i++) {
                        fprintf(stderr, "[capture] Axis %d = %d\n",
                                i, SDL_JoystickGetAxis(joystick, i));
                    }

                    last_report = now;
                }
            }
            SDL_Delay(10);
        }

        waiting_for_release = 0;
        started = SDL_GetTicks();

        /*
         * Capture the next button press, hat direction or axis movement.
         * A 30-second timeout prevents the helper process hanging forever.
         */
        while (SDL_GetTicks() - started < 30000) {
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN &&
                     event.key.keysym.sym == SDLK_ESCAPE))
                    goto cleanup;

                if (event.type == SDL_JOYBUTTONDOWN &&
                    event.jbutton.which == instance_id &&
                    event.jbutton.button < 128) {
                    snprintf(binding, binding_size, "BTN%u",
                             (unsigned)event.jbutton.button);
                    result = 0;
                    goto cleanup;
                }

                if (event.type == SDL_JOYHATMOTION &&
                    event.jhat.which == instance_id &&
                    event.jhat.value != SDL_HAT_CENTERED &&
                    event.jhat.hat < 16) {
                    const char *direction = NULL;

                    if (event.jhat.value & SDL_HAT_UP)
                        direction = "UP";
                    else if (event.jhat.value & SDL_HAT_DOWN)
                        direction = "DOWN";
                    else if (event.jhat.value & SDL_HAT_LEFT)
                        direction = "LEFT";
                    else if (event.jhat.value & SDL_HAT_RIGHT)
                        direction = "RIGHT";

                    if (direction) {
                        snprintf(binding, binding_size, "HAT%u_%s",
                                 (unsigned)event.jhat.hat, direction);
                        result = 0;
                        goto cleanup;
                    }
                }

                if (event.type == SDL_JOYAXISMOTION &&
                    event.jaxis.which == instance_id &&
                    event.jaxis.axis < 16) {
                    if (event.jaxis.value < -AXIS_DEADZONE) {
                        snprintf(binding, binding_size, "AXIS%u_NEG",
                                 (unsigned)event.jaxis.axis);
                        result = 0;
                        goto cleanup;
                    }

                    if (event.jaxis.value > AXIS_DEADZONE) {
                        snprintf(binding, binding_size, "AXIS%u_POS",
                                 (unsigned)event.jaxis.axis);
                        result = 0;
                        goto cleanup;
                    }
                }
            }

            SDL_Delay(5);
        }
    }

cleanup:
    if (controller)
        SDL_GameControllerClose(controller);
    else if (joystick)
        SDL_JoystickClose(joystick);

    SDL_Quit();

    (void)waiting_for_release;
    return result;
}


static void load_bindings(void) {
    FILE *f;
    char line[1024];
    int i, j;

    defaults();
    memset(pad_players, 0, sizeof pad_players);
    memset(pad_system, 0, sizeof pad_system);
    memset(player_device_guid, 0, sizeof player_device_guid);
    memset(player_device_serial, 0, sizeof player_device_serial);
    memset(player_device_path, 0, sizeof player_device_path);

    f = fopen("controls.cfg", "r");
    if (!f) {
        fprintf(stderr,
                "[input] controls.cfg missing; using defaults.\n");
        return;
    }

    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val, *end;
        SDL_Scancode *p;
        SDL_Scancode sc;
        pad_binding_t *pb;
        pad_binding_t parsed;

        key = line;
        while (isspace((unsigned char)*key)) key++;

        if (!*key || *key == '#' || *key == ';') continue;

        eq = strchr(key, '=');
        if (!eq) continue;

        *eq = '\0';

        end = eq - 1;
        while (end >= key && isspace((unsigned char)*end))
            *end-- = '\0';

        val = eq + 1;
        while (isspace((unsigned char)*val)) val++;

        end = val + strlen(val);
        while (end > val && isspace((unsigned char)end[-1]))
            *--end = '\0';
        if (strcmp(key, "PAD_P1_DEVICE_GUID") == 0) {
            snprintf(player_device_guid[0], sizeof player_device_guid[0], "%s", val); continue;
        }
        if (strcmp(key, "PAD_P2_DEVICE_GUID") == 0) {
            snprintf(player_device_guid[1], sizeof player_device_guid[1], "%s", val); continue;
        }
        if (strcmp(key, "PAD_P1_DEVICE_SERIAL") == 0) {
            snprintf(player_device_serial[0], sizeof player_device_serial[0], "%s", val); continue;
        }
        if (strcmp(key, "PAD_P2_DEVICE_SERIAL") == 0) {
            snprintf(player_device_serial[1], sizeof player_device_serial[1], "%s", val); continue;
        }
        if (strcmp(key, "PAD_P1_DEVICE_PATH") == 0) {
            snprintf(player_device_path[0], sizeof player_device_path[0], "%s", val); continue;
        }
        if (strcmp(key, "PAD_P2_DEVICE_PATH") == 0) {
            snprintf(player_device_path[1], sizeof player_device_path[1], "%s", val); continue;
        }
        /* Controller bindings are optional. */
        pb = pad_slot(key);
        if (pb) {
            if (parse_pad_binding(val, &parsed)) {
                *pb = parsed;
            } else {
                fprintf(stderr,
                        "[input] Invalid controller binding %s=%s; "
                        "using automatic mapping.\n", key, val);
            }
            continue;
        }
        p = slot(key);
        if (!p || !*val) continue;

        if (strcmp(val, "NONE") == 0) {
            *p = SDL_SCANCODE_UNKNOWN;
            continue;
        }

        sc = SDL_GetScancodeFromName(val);

        if (sc == SDL_SCANCODE_UNKNOWN ||
            sc == SDL_SCANCODE_ESCAPE) {
            fprintf(stderr,
                    "[input] Invalid/reserved binding %s=%s; "
                    "using default.\n", key, val);
            continue;
        }

        *p = sc;
    }

    fclose(f);

    /* Reject duplicate assigned keys, but allow multiple unassigned actions. */
    for (i = 0; i < 25; i++) {
        if (*slot(names[i]) == SDL_SCANCODE_UNKNOWN)
            continue;

        for (j = 0; j < i; j++) {
            if (*slot(names[j]) != SDL_SCANCODE_UNKNOWN &&
                *slot(names[i]) == *slot(names[j])) {
                defaults();
                fprintf(stderr,
                        "[input] Duplicate controls.cfg assignments; "
                        "using all defaults.\n");
                return;
            }
        }
    }
}

static const char *pad_names[23] = {
    "PAD_P1_UP", "PAD_P1_DOWN", "PAD_P1_LEFT", "PAD_P1_RIGHT",
    "PAD_P1_A", "PAD_P1_B", "PAD_P1_C", "PAD_P1_D",
    "PAD_P2_UP", "PAD_P2_DOWN", "PAD_P2_LEFT", "PAD_P2_RIGHT",
    "PAD_P2_A", "PAD_P2_B", "PAD_P2_C", "PAD_P2_D",
    "PAD_START1", "PAD_START2", "PAD_SELECT",
    "PAD_COIN1", "PAD_COIN2", "PAD_SERVICE", "PAD_TEST"
};

static void format_pad_binding(
    const pad_binding_t *binding,
    char *out,
    size_t out_size)
{
    switch (binding->type) {
    case PAD_BUTTON:
        snprintf(out, out_size, "BTN%d", binding->index);
        break;

    case PAD_HAT:
        switch (binding->direction) {
        case SDL_HAT_UP:
            snprintf(out, out_size, "HAT%d_UP", binding->index);
            break;
        case SDL_HAT_DOWN:
            snprintf(out, out_size, "HAT%d_DOWN", binding->index);
            break;
        case SDL_HAT_LEFT:
            snprintf(out, out_size, "HAT%d_LEFT", binding->index);
            break;
        case SDL_HAT_RIGHT:
            snprintf(out, out_size, "HAT%d_RIGHT", binding->index);
            break;
        default:
            snprintf(out, out_size, "NONE");
            break;
        }
        break;

    case PAD_AXIS_NEG:
        snprintf(out, out_size, "AXIS%d_NEG", binding->index);
        break;

    case PAD_AXIS_POS:
        snprintf(out, out_size, "AXIS%d_POS", binding->index);
        break;

    case PAD_NONE:
    default:
        snprintf(out, out_size, "NONE");
        break;
    }
}

static int save_bindings(void)
{
    FILE *f;
    int i;
    char value[32];
    int ok = 1;

    f = fopen("controls.cfg", "w");
    if (!f) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "SAVE ERROR");
        return 0;
    }

    if (fprintf(f, "# Keyboard bindings\n") < 0)
        ok = 0;

    for (i = 0; i < 25; i++) {
        SDL_Scancode *key = slot(names[i]);

        if (!key || *key == SDL_SCANCODE_UNKNOWN) {
            snprintf(value, sizeof value, "NONE");
        } else {
            const char *key_name = SDL_GetScancodeName(*key);

            if (!key_name || !*key_name) {
                snprintf(value, sizeof value, "NONE");
            } else {
                snprintf(value, sizeof value, "%s", key_name);
            }
        }

        if (fprintf(f, "%s=%s\n", names[i], value) < 0)
            ok = 0;
    }

    if (fprintf(f, "\n# Controller bindings\n") < 0)
        ok = 0;

    for (i = 0; i < 23; i++) {
        pad_binding_t *binding = pad_slot(pad_names[i]);

        if (!binding)
            continue;

        format_pad_binding(binding, value, sizeof value);

        if (fprintf(f, "%s=%s\n", pad_names[i], value) < 0)
            ok = 0;
    }

    if (fprintf(f,
                "PAD_P1_DEVICE_GUID=%s\nPAD_P1_DEVICE_SERIAL=%s\n"
                "PAD_P1_DEVICE_PATH=%s\nPAD_P2_DEVICE_GUID=%s\n"
                "PAD_P2_DEVICE_SERIAL=%s\nPAD_P2_DEVICE_PATH=%s\n",
                player_device_guid[0], player_device_serial[0],
                player_device_path[0], player_device_guid[1],
                player_device_serial[1], player_device_path[1]) < 0)
        ok = 0;

    if (fclose(f) != 0)
        ok = 0;

    if (!ok) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "SAVE ERROR");
        return 0;
    }

    snprintf(s_menu_status, sizeof s_menu_status,
             "SETTINGS SAVED");
    return 1;
}

static void read_device_identity(SDL_Joystick *j, input_device_t *d)
{
    SDL_JoystickGUID guid;
    const char *value;
    d->guid[0] = d->serial[0] = d->path[0] = '\0';
    if (!j) return;
    guid = SDL_JoystickGetGUID(j);
    SDL_JoystickGetGUIDString(guid, d->guid, sizeof d->guid);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    value = SDL_JoystickGetSerial(j);
    if (value) snprintf(d->serial, sizeof d->serial, "%s", value);
#endif
#if SDL_VERSION_ATLEAST(2, 24, 0)
    value = SDL_JoystickPath(j);
    if (value) snprintf(d->path, sizeof d->path, "%s", value);
#endif
}

static int device_matches_player_identity(int player, const input_device_t *d)
{
    int i, count = 0;
    if (player < 0 || player > 1 || !d ||
        !player_device_guid[player][0] ||
        strcmp(player_device_guid[player], d->guid) != 0) return 0;
    if (player_device_serial[player][0])
        return d->serial[0] && strcmp(player_device_serial[player], d->serial) == 0;
    if (player_device_path[player][0])
        return d->path[0] && strcmp(player_device_path[player], d->path) == 0;
    for (i = 0; i < MAX_INPUT_DEVICES; i++)
        if (devices[i].joystick && SDL_JoystickGetAttached(devices[i].joystick) &&
            strcmp(devices[i].guid, d->guid) == 0) count++;
    return count == 1;
}

static int device_assigned_to_other_player(int player, int slot)
{
    int other = player == 0 ? 1 : 0;
    return player_device[other] == slot ||
           device_matches_player_identity(other, &devices[slot]);
}

static void assign_device_to_player(int player, int slot)
{
    input_device_t *d = &devices[slot];
    player_device[player] = slot;
    snprintf(player_device_guid[player], sizeof player_device_guid[player], "%s", d->guid);
    snprintf(player_device_serial[player], sizeof player_device_serial[player], "%s", d->serial);
    snprintf(player_device_path[player], sizeof player_device_path[player], "%s", d->path);
}

static void close_device(int device_slot) {
    input_device_t *d;

    if (device_slot < 0 || device_slot >= MAX_INPUT_DEVICES)
        return;

    d = &devices[device_slot];

    /* No player can retain a reference to a disconnected device. */
    if (player_device[0] == device_slot)
        player_device[0] = -1;

    if (player_device[1] == device_slot)
        player_device[1] = -1;

    if (d->controller) {
        SDL_GameControllerClose(d->controller);
    } else if (d->joystick) {
        SDL_JoystickClose(d->joystick);
    }

    d->controller = NULL;
    d->joystick = NULL;
    d->instance_id = -1;
    d->guid[0] = d->serial[0] = d->path[0] = '\0';
}

static int find_device(SDL_JoystickID instance_id) {
    int i;

    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
        if (devices[i].instance_id == instance_id)
            return i;
    }

    return -1;
}

static int player_device_index(int player) {
    int slot;

    if (player < 0 || player > 1)
        return -1;

    slot = player_device[player];

    if (slot < 0 || slot >= MAX_INPUT_DEVICES)
        return -1;

    if (!devices[slot].joystick ||
        !SDL_JoystickGetAttached(devices[slot].joystick))
        return -1;

    return slot;
}

static int open_device(int device_index) {
    SDL_JoystickID instance_id;
    int slot;
    int player = -1;

    if (device_index < 0 ||
        device_index >= SDL_NumJoysticks()) {
        return -1;
    }

    instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
    if (instance_id < 0)
        return -1;

    /* Evitar registrar dos veces el mismo dispositivo. */
    if (find_device(instance_id) >= 0)
        return 0;

    /* Find a free slot in the physical device inventory. */
    for (slot = 0; slot < MAX_INPUT_DEVICES; slot++) {
        if (!devices[slot].controller &&
            !devices[slot].joystick) {
            break;
        }
    }

    if (slot >= MAX_INPUT_DEVICES) {
        fprintf(stderr,
                "[input] Device ignored: inventory is full.\n");
        return -1;
    }

    if (SDL_IsGameController(device_index)) {
        devices[slot].controller =
            SDL_GameControllerOpen(device_index);

        if (devices[slot].controller) {
            devices[slot].joystick =
                SDL_GameControllerGetJoystick(
                    devices[slot].controller);
        }
    }

    /* Fallback para dispositivos sin mapping de SDL Controller. */
    if (!devices[slot].joystick) {
        if (devices[slot].controller) {
            SDL_GameControllerClose(devices[slot].controller);
            devices[slot].controller = NULL;
        }

        devices[slot].joystick = SDL_JoystickOpen(device_index);
    }

    if (!devices[slot].joystick) {
        fprintf(stderr, "[input] Could not open device: %s\n",
                SDL_GetError());

        devices[slot].controller = NULL;
        devices[slot].joystick = NULL;
        devices[slot].instance_id = -1;
        return -1;
    }

    devices[slot].instance_id = SDL_JoystickInstanceID(devices[slot].joystick);
    read_device_identity(devices[slot].joystick, &devices[slot]);
    for (player = 0; player < 2; player++) {
        int other = player == 0 ? 1 : 0;
        if (player_device[player] < 0 && player_device[other] != slot &&
            !device_assigned_to_other_player(player, slot) &&
            device_matches_player_identity(player, &devices[slot])) {
            player_device[player] = slot;
            break;
        }
    }

    fprintf(stderr,
            "[input] Device connected: %s%s (slot %d)",
            SDL_JoystickName(devices[slot].joystick)
                ? SDL_JoystickName(devices[slot].joystick)
                : "Unknown device",
            devices[slot].controller
                ? " (SDL game controller)"
                : " (generic joystick)",
            slot);

    if (player < 2) fprintf(stderr, ", restored for P%d\n", player + 1);
    else fprintf(stderr, ", not assigned\n");

    return slot;
}

static void handle_device_events(const SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED ||
        e->type == SDL_JOYDEVICEADDED) {

        open_device(e->cdevice.which);

    } else if (e->type == SDL_CONTROLLERDEVICEREMOVED ||
               e->type == SDL_JOYDEVICEREMOVED) {

        int slot = find_device(e->cdevice.which);

        if (slot >= 0) {
            int player = -1;

            if (player_device[0] == slot)
                player = 0;
            else if (player_device[1] == slot)
                player = 1;

            if (player >= 0) {
                fprintf(stderr,
                        "[input] Player %d device disconnected.\n",
                        player + 1);
            } else {
                fprintf(stderr,
                        "[input] Unassigned device disconnected.\n");
            }

            close_device(slot);
        }
    }
}

static int controller_button(SDL_GameController *c,
                             SDL_GameControllerButton button) {
    return c && SDL_GameControllerGetButton(c, button);
}

static int joystick_button(SDL_Joystick *j, int button) {
    return j && button >= 0 &&
           button < SDL_JoystickNumButtons(j) &&
           SDL_JoystickGetButton(j, button);
}

static int joystick_axis(SDL_Joystick *j, int axis) {
    if (!j || axis < 0 || axis >= SDL_JoystickNumAxes(j))
        return 0;

    return SDL_JoystickGetAxis(j, axis);
}

static int generic_direction(SDL_Joystick *j,
                             int direction) {
    Uint8 hat;

    if (!j) return 0;

    /* direction: 0=up, 1=down, 2=left, 3=right */
    if (SDL_JoystickNumHats(j) > 0) {
        hat = SDL_JoystickGetHat(j, 0);

        if (direction == 0 && (hat & SDL_HAT_UP)) return 1;
        if (direction == 1 && (hat & SDL_HAT_DOWN)) return 1;
        if (direction == 2 && (hat & SDL_HAT_LEFT)) return 1;
        if (direction == 3 && (hat & SDL_HAT_RIGHT)) return 1;
    }

    if (direction == 0)
        return joystick_axis(j, 1) < -AXIS_DEADZONE;
    if (direction == 1)
        return joystick_axis(j, 1) > AXIS_DEADZONE;
    if (direction == 2)
        return joystick_axis(j, 0) < -AXIS_DEADZONE;

    return joystick_axis(j, 0) > AXIS_DEADZONE;
}

static void read_controller(int player, ng_input_t *in) {
    uint8_t *bits = player == 0 ? &in->p1 : &in->p2;
    int slot = player_device_index(player);
    input_device_t *d;
    SDL_GameController *c;
    SDL_Joystick *j;

    if (slot < 0)
        return;

    d = &devices[slot];
    c = d->controller;
    j = d->joystick;

    static const uint32_t action_bits[8] = {
        NG_UP, NG_DOWN, NG_LEFT, NG_RIGHT,
        NG_A, NG_B, NG_C, NG_D
    };

    int active[8];
    int i;

    if (!j || !SDL_JoystickGetAttached(j))
        return;

    /*
     * Determine the existing automatic mappings first.
     * Custom entries below override the corresponding action.
     */
    if (c) {
        active[0] =
            controller_button(c, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
            SDL_GameControllerGetAxis(
                c, SDL_CONTROLLER_AXIS_LEFTY) < -AXIS_DEADZONE;

        active[1] =
            controller_button(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
            SDL_GameControllerGetAxis(
                c, SDL_CONTROLLER_AXIS_LEFTY) > AXIS_DEADZONE;

        active[2] =
            controller_button(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
            SDL_GameControllerGetAxis(
                c, SDL_CONTROLLER_AXIS_LEFTX) < -AXIS_DEADZONE;

        active[3] =
            controller_button(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            SDL_GameControllerGetAxis(
                c, SDL_CONTROLLER_AXIS_LEFTX) > AXIS_DEADZONE;

        active[4] = controller_button(c, SDL_CONTROLLER_BUTTON_A);
        active[5] = controller_button(c, SDL_CONTROLLER_BUTTON_B);
        active[6] = controller_button(c, SDL_CONTROLLER_BUTTON_X);
        active[7] = controller_button(c, SDL_CONTROLLER_BUTTON_Y);
    } else {
        active[0] = generic_direction(j, 0);
        active[1] = generic_direction(j, 1);
        active[2] = generic_direction(j, 2);
        active[3] = generic_direction(j, 3);

        active[4] = joystick_button(j, 0);
        active[5] = joystick_button(j, 1);
        active[6] = joystick_button(j, 2);
        active[7] = joystick_button(j, 3);
    }

    for (i = 0; i < 8; i++) {
        int pressed = active[i];

        if (pad_players[player][i].type != PAD_NONE) {
            pressed = pad_binding_active(
                j, &pad_players[player][i]);
        }

        if (pressed)
            *bits |= action_bits[i];
    }

    /*
     * Player controls.
     * Keyboard inputs remain active because these are ORed into them.
     */
    if (player == 0) {
        int start = c
            ? controller_button(c, SDL_CONTROLLER_BUTTON_START)
            : joystick_button(j, 7);

        int select = c
            ? controller_button(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            : joystick_button(j, 6);

        int coin = c
            ? controller_button(c, SDL_CONTROLLER_BUTTON_BACK)
            : joystick_button(j, 4);

        /* START and COIN belong to the player profile. */
        if (pad_players[0][8].type != PAD_NONE)
            start = pad_binding_active(j, &pad_players[0][8]);

        if (pad_system[PAD_SELECT].type != PAD_NONE)
            select = pad_binding_active(j, &pad_system[PAD_SELECT]);

        if (pad_players[0][9].type != PAD_NONE)
            coin = pad_binding_active(j, &pad_players[0][9]);

        in->start1 |= start;
        in->select1 |= select;
        in->coin1 |= coin;

        if (pad_system[PAD_SERVICE].type != PAD_NONE)
            in->service |= pad_binding_active(
                j, &pad_system[PAD_SERVICE]);

        if (pad_system[PAD_TEST].type != PAD_NONE)
            in->test |= pad_binding_active(
                j, &pad_system[PAD_TEST]);

    } else {
        int start = c
            ? controller_button(c, SDL_CONTROLLER_BUTTON_START)
            : joystick_button(j, 7);

        int coin = c
            ? controller_button(c, SDL_CONTROLLER_BUTTON_BACK)
            : joystick_button(j, 4);

        if (pad_players[1][8].type != PAD_NONE)
            start = pad_binding_active(j, &pad_players[1][8]);

        if (pad_players[1][9].type != PAD_NONE)
            coin = pad_binding_active(j, &pad_players[1][9]);

        in->start2 |= start;
        in->coin2 |= coin;
    }
}

int window_open(const char *title, int scale) {
    int i;

    memset(devices, 0, sizeof devices);

    for (i = 0; i < MAX_INPUT_DEVICES; i++)
        devices[i].instance_id = -1;

    player_device[0] = -1;
    player_device[1] = -1;

    if (SDL_Init(SDL_INIT_VIDEO |
                 SDL_INIT_GAMECONTROLLER |
                 SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    /* Allow SDL to generate device-added events. */
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    load_bindings();

    s_win = SDL_CreateWindow(
        title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        NG_SCREEN_W * scale, NG_SCREEN_H * scale,
        SDL_WINDOW_RESIZABLE);

    if (!s_win) {
        fprintf(stderr, "error: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_ren)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);

    if (!s_ren) {
        SDL_DestroyWindow(s_win);
        s_win = NULL;
        SDL_Quit();
        return -1;
    }

    SDL_RenderSetLogicalSize(s_ren, NG_SCREEN_W, NG_SCREEN_H);

    s_tex = SDL_CreateTexture(
        s_ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, NG_SCREEN_W, NG_SCREEN_H);

    if (!s_tex) {
        SDL_DestroyRenderer(s_ren);
        SDL_DestroyWindow(s_win);
        s_ren = NULL;
        s_win = NULL;
        SDL_Quit();
        return -1;
    }

    /* Pick up controllers already connected before the game launched. */
    for (i = 0; i < SDL_NumJoysticks(); i++)
        open_device(i);

    s_next_tick = SDL_GetPerformanceCounter();

    /* Sound at the YM2610's own rate; SDL converts it for the device. */
    {
        SDL_AudioSpec want = {0};

        want.freq = NG_AUDIO_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;

        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
            s_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);

        if (!s_audio)
            fprintf(stderr, "warning: no sound: %s\n", SDL_GetError());
    }

    return 0;
}

/* The frame loop and the sound card both run at the board's rates, so the
 * queue stays level; it is kept between about 30 and 120 ms, dropping a
 * frame's samples when it runs long (after a stall) and padding with
 * silence when it runs dry. */
void window_audio(const int16_t *samples, size_t frames) {
    if (!s_audio) return;
    const Uint32 bytes_per_ms = NG_AUDIO_RATE * 4 / 1000;
    Uint32 queued = SDL_GetQueuedAudioSize(s_audio);
    if (queued > 120 * bytes_per_ms) return;
    if (queued < 30 * bytes_per_ms) {
        static const int16_t silence[2 * 2048];   /* 37 ms */
        SDL_QueueAudio(s_audio, silence, (Uint32)(30 * bytes_per_ms - queued) & ~3u);
    }
    SDL_QueueAudio(s_audio, samples, (Uint32)(frames * 4));
    SDL_PauseAudioDevice(s_audio, 0);
}

static void read_keys(ng_input_t *in) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    int i;

    in->p1 = 0;
    in->p2 = 0;

    if (k[b.p1[0]]) in->p1 |= NG_UP;
    if (k[b.p1[1]]) in->p1 |= NG_DOWN;
    if (k[b.p1[2]]) in->p1 |= NG_LEFT;
    if (k[b.p1[3]]) in->p1 |= NG_RIGHT;
    if (k[b.p1[4]]) in->p1 |= NG_A;
    if (k[b.p1[5]]) in->p1 |= NG_B;
    if (k[b.p1[6]]) in->p1 |= NG_C;
    if (k[b.p1[7]]) in->p1 |= NG_D;

    if (k[b.p2[0]]) in->p2 |= NG_UP;
    if (k[b.p2[1]]) in->p2 |= NG_DOWN;
    if (k[b.p2[2]]) in->p2 |= NG_LEFT;
    if (k[b.p2[3]]) in->p2 |= NG_RIGHT;
    if (k[b.p2[4]]) in->p2 |= NG_A;
    if (k[b.p2[5]]) in->p2 |= NG_B;
    if (k[b.p2[6]]) in->p2 |= NG_C;
    if (k[b.p2[7]]) in->p2 |= NG_D;

    in->start1 = k[b.start1];
    in->start2 = k[b.start2];
    in->select1 = k[b.select1];
    in->coin1 = k[b.coin1];
    in->coin2 = k[b.coin2];
    in->service = k[b.service];
    in->test = k[b.test];

    (void)i;
}

static void menu_toggle_fullscreen(void) {
    Uint32 flags;
    bool fullscreen;

    if (!s_win)
        return;

    flags = SDL_GetWindowFlags(s_win);
    fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;

    if (SDL_SetWindowFullscreen(
            s_win,
            fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
        fprintf(stderr,
                "Failed to change display mode: %s\n",
                SDL_GetError());
    }
}

static const char *keyboard_labels[25] = {
    "P1 UP", "P1 DOWN", "P1 LEFT", "P1 RIGHT",
    "P1 A", "P1 B", "P1 C", "P1 D",
    "P2 UP", "P2 DOWN", "P2 LEFT", "P2 RIGHT",
    "P2 A", "P2 B", "P2 C", "P2 D",
    "START 1", "START 2", "SELECT",
    "COIN 1", "COIN 2", "SERVICE", "TEST",
    "OPEN OVERLAY", "FULLSCREEN"
};

static int menu_is_keyboard_screen(void)
{
    return s_menu_screen == MENU_KEYBOARD_P1 ||
           s_menu_screen == MENU_KEYBOARD_P2 ||
           s_menu_screen == MENU_KEYBOARD_SYSTEM;
}

static int menu_keyboard_action_count(void)
{
    if (s_menu_screen == MENU_KEYBOARD_P1 ||
        s_menu_screen == MENU_KEYBOARD_P2)
        return 10;

    if (s_menu_screen == MENU_KEYBOARD_SYSTEM)
        return 5;

    return 0;
}

static int menu_keyboard_action_index(void)
{
    static const int system_actions[5] = {
        18, /* SELECT */
        21, /* SERVICE */
        22, /* TEST */
        23, /* OPEN OVERLAY */
        24  /* FULLSCREEN */
    };

    if (s_menu_screen == MENU_KEYBOARD_P1) {
        if (s_menu_selection < 8)
            return s_menu_selection;

        if (s_menu_selection == 8)
            return 16; /* START 1 */

        if (s_menu_selection == 9)
            return 19; /* COIN 1 */
    }

    if (s_menu_screen == MENU_KEYBOARD_P2) {
        if (s_menu_selection < 8)
            return 8 + s_menu_selection;

        if (s_menu_selection == 8)
            return 17; /* START 2 */

        if (s_menu_selection == 9)
            return 20; /* COIN 2 */
    }

    if (s_menu_screen == MENU_KEYBOARD_SYSTEM &&
        s_menu_selection >= 0 && s_menu_selection < 5)
        return system_actions[s_menu_selection];

    return -1;
}

static const char *menu_key_name(int action)
{
    SDL_Scancode *key;

    if (action < 0 || action >= 25)
        return "NONE";

    key = slot(names[action]);

    if (!key || *key == SDL_SCANCODE_UNKNOWN)
        return "NONE";

    return SDL_GetScancodeName(*key);
}

static int menu_find_key_conflict(
    SDL_Scancode scancode,
    int except_action)
{
    int i;

    for (i = 0; i < 25; i++) {
        SDL_Scancode *key = slot(names[i]);

        if (i != except_action &&
            key &&
            *key != SDL_SCANCODE_UNKNOWN &&
            *key == scancode) {
            return i;
        }
    }

    return -1;
}

static void menu_start_capture(int action)
{
    if (action < 0 || action >= 25)
        return;

    s_capture_action = action;
    s_menu_capturing = true;

    s_conflict_action = -1;
    s_conflict_scancode = SDL_SCANCODE_UNKNOWN;

    snprintf(s_menu_status, sizeof s_menu_status,
             "PRESS A KEY");
}

static void menu_cancel_capture(void)
{
    s_menu_capturing = false;
    s_capture_action = -1;
    s_conflict_action = -1;
    s_conflict_scancode = SDL_SCANCODE_UNKNOWN;

    snprintf(s_menu_status, sizeof s_menu_status, "CANCELLED");
}

static void menu_capture_key(SDL_Scancode scancode)
{
    int conflict;
    SDL_Scancode *target;

    if (!s_menu_capturing || s_capture_action < 0)
        return;

    /* Escape is reserved for canceling the operation. */
    if (scancode == SDL_SCANCODE_ESCAPE) {
        menu_cancel_capture();
        return;
    }

    target = slot(names[s_capture_action]);
    if (!target) {
        menu_cancel_capture();
        return;
    }

    conflict = menu_find_key_conflict(scancode, s_capture_action);

    if (conflict >= 0) {
        s_menu_capturing = false;
        s_conflict_action = conflict;
        s_conflict_scancode = scancode;

        snprintf(s_menu_status, sizeof s_menu_status,
                 "IN USE: ENTER TO SWAP");
        return;
    }

    *target = scancode;
    s_menu_capturing = false;
    s_capture_action = -1;

    save_bindings();
}

static void menu_resolve_conflict(void)
{
    SDL_Scancode *target;
    SDL_Scancode *other;

    if (s_conflict_action < 0 ||
        s_capture_action < 0 ||
        s_conflict_scancode == SDL_SCANCODE_UNKNOWN) {
        return;
    }

    target = slot(names[s_capture_action]);
    other = slot(names[s_conflict_action]);

    if (!target || !other) {
        menu_cancel_capture();
        return;
    }

    /*
     * Swap the two assignments rather than creating a duplicate.
     * This keeps both actions usable.
     */
    {
        SDL_Scancode old_target = *target;

        *target = s_conflict_scancode;
        *other = old_target;
    }

    s_menu_capturing = false;
    s_capture_action = -1;
    s_conflict_action = -1;
    s_conflict_scancode = SDL_SCANCODE_UNKNOWN;
    save_bindings();
}

static int menu_is_pad_screen(void)
{
    return s_menu_screen == MENU_PAD_P1 ||
           s_menu_screen == MENU_PAD_P2;
}

static int menu_pad_player(void)
{
    if (s_menu_screen == MENU_PAD_P1) return 0;
    if (s_menu_screen == MENU_PAD_P2) return 1;
    return -1;
}

static const char *pad_action_labels[10] = {
    "UP", "DOWN", "LEFT", "RIGHT",
    "BUTTON A", "BUTTON B", "BUTTON C", "BUTTON D",
    "START", "COIN"
};

static int pad_binding_equal(const pad_binding_t *a,
                             const pad_binding_t *bnd)
{
    return a->type == bnd->type &&
           a->index == bnd->index &&
           a->direction == bnd->direction;
}

static int menu_find_pad_conflict(int player, int action,
                                  const pad_binding_t *binding)
{
    int i;

    if (binding->type == PAD_NONE)
        return -1;

    for (i = 0; i < 10; i++) {
        if (i != action &&
            pad_players[player][i].type != PAD_NONE &&
            pad_binding_equal(&pad_players[player][i], binding))
            return i;
    }

    return -1;
}

static int pad_device_neutral(SDL_Joystick *j)
{
    int i;

    if (!j || !SDL_JoystickGetAttached(j))
        return 0;

    for (i = 0; i < SDL_JoystickNumButtons(j); i++)
        if (SDL_JoystickGetButton(j, i))
            return 0;

    for (i = 0; i < SDL_JoystickNumAxes(j); i++) {
        int value = SDL_JoystickGetAxis(j, i);
        if (value > AXIS_DEADZONE || value < -AXIS_DEADZONE)
            return 0;
    }

    for (i = 0; i < SDL_JoystickNumHats(j); i++)
        if (SDL_JoystickGetHat(j, i) != SDL_HAT_CENTERED)
            return 0;

    return 1;
}

static void menu_cancel_pad_capture(void)
{
    s_pad_capturing = false;
    s_pad_capture_wait_neutral = true;
    s_pad_capture_player = -1;
    s_pad_capture_action = -1;
    s_pad_reassigning = false;
    s_pad_conflict_action = -1;
    snprintf(s_menu_status, sizeof s_menu_status, "CANCELLED");
}

static void menu_start_pad_capture(int action)
{
    int player = menu_pad_player();

    if (player < 0 || action < 0 || action >= 10)
        return;

    s_pad_capture_player = player;
    s_pad_capture_action = action;
    s_pad_reassigning = false;
    s_pad_conflict_action = -1;
    s_pad_capturing = true;
    s_pad_capture_wait_neutral = true;

    snprintf(s_menu_status, sizeof s_menu_status,
             player_device_index(player) < 0 ? "CONNECT A CONTROLLER" : "RELEASE THE CONTROLLER");
}

static void menu_start_pad_reassignment(void)
{
    int player = menu_pad_player();
    if (player < 0 || player > 1) return;
    s_pad_capture_player = player;
    s_pad_capture_action = -1;
    s_pad_reassigning = true;
    s_pad_conflict_action = -1;
    s_pad_capturing = true;
    s_pad_capture_wait_neutral = true;
    snprintf(s_menu_status, sizeof s_menu_status, "CONNECT OR RELEASE THE CONTROLLER");
}

static void menu_finish_pad_capture(const pad_binding_t *binding)
{
    int conflict;
    int player = s_pad_capture_player;
    int action = s_pad_capture_action;

    if (player < 0 || player > 1 || action < 0 || action >= 10) {
        menu_cancel_pad_capture();
        return;
    }

    conflict = menu_find_pad_conflict(player, action, binding);
    if (conflict >= 0) {
        s_pad_capturing = false;
        s_pad_conflict_action = conflict;
        s_pad_conflict_binding = *binding;
        snprintf(s_menu_status, sizeof s_menu_status,
                 "IN USE: ENTER TO SWAP");
        return;
    }

    pad_players[player][action] = *binding;
    s_pad_capturing = false;
    s_pad_capture_wait_neutral = true;
    s_pad_capture_player = -1;
    s_pad_capture_action = -1;
    save_bindings();
}

static void menu_capture_pad_event(const SDL_Event *e)
{
    SDL_Joystick *j;
    SDL_JoystickID event_id;
    pad_binding_t binding;
    int slot;

    if (!s_pad_capturing || s_pad_capture_player < 0 ||
        s_pad_capture_player > 1)
        return;

    /* Get the identifier of the controller that generated the event. */
    if (e->type == SDL_JOYBUTTONDOWN)
        event_id = e->jbutton.which;
    else if (e->type == SDL_JOYHATMOTION)
        event_id = e->jhat.which;
    else if (e->type == SDL_JOYAXISMOTION)
        event_id = e->jaxis.which;
    else
        return;

    /* Find the controller in the physical device inventory, not by player. */
    slot = find_device(event_id);
    if (slot < 0)
        return;

    j = devices[slot].joystick;
    if (!j || !SDL_JoystickGetAttached(j))
        return;

    binding.type = PAD_NONE;
    binding.index = 0;
    binding.direction = 0;

    if (s_pad_capture_wait_neutral)
        return;

    if (e->type == SDL_JOYBUTTONDOWN &&
        e->jbutton.button < 128) {
        binding.type = PAD_BUTTON;
        binding.index = (int)e->jbutton.button;

    } else if (e->type == SDL_JOYHATMOTION &&
               e->jhat.value != SDL_HAT_CENTERED &&
               e->jhat.hat < 16) {
        binding.type = PAD_HAT;
        binding.index = (int)e->jhat.hat;

        if (e->jhat.value & SDL_HAT_UP)
            binding.direction = SDL_HAT_UP;
        else if (e->jhat.value & SDL_HAT_DOWN)
            binding.direction = SDL_HAT_DOWN;
        else if (e->jhat.value & SDL_HAT_LEFT)
            binding.direction = SDL_HAT_LEFT;
        else if (e->jhat.value & SDL_HAT_RIGHT)
            binding.direction = SDL_HAT_RIGHT;
        else
            return;

    } else if (e->type == SDL_JOYAXISMOTION &&
               e->jaxis.axis < 16) {
        if (e->jaxis.value < -AXIS_DEADZONE)
            binding.type = PAD_AXIS_NEG;
        else if (e->jaxis.value > AXIS_DEADZONE)
            binding.type = PAD_AXIS_POS;
        else
            return;

        binding.index = (int)e->jaxis.axis;

    } else {
        return;
    }

    if (device_assigned_to_other_player(s_pad_capture_player, slot)) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "CONTROLLER ALREADY ASSIGNED");
        return;
    }

    if (s_pad_reassigning) {
        assign_device_to_player(s_pad_capture_player, slot);
        s_pad_reassigning = false;
        s_pad_capturing = false;
        s_pad_capture_wait_neutral = true;
        s_pad_capture_player = -1;
        s_pad_capture_action = -1;
        snprintf(s_menu_status, sizeof s_menu_status,
                 "CONTROLLER ASSIGNED");
        save_bindings();
        return;
    }

    if (player_device[s_pad_capture_player] < 0) {
        assign_device_to_player(s_pad_capture_player, slot);
        save_bindings();
    } else if (player_device[s_pad_capture_player] != slot) {
        return;
    }

    menu_finish_pad_capture(&binding);
}

static void menu_update_pad_capture(void)
{
    int i;
    int connected = 0;
    int all_neutral = 1;

    if (!s_pad_capturing || !s_pad_capture_wait_neutral)
        return;

    if (s_pad_capture_player < 0 || s_pad_capture_player > 1) {
        menu_cancel_pad_capture();
        return;
    }

    /* Wait for all connected controllers to return to neutral. */
    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
        SDL_Joystick *j = devices[i].joystick;

        if (!j || !SDL_JoystickGetAttached(j))
            continue;

        connected++;

        if (!pad_device_neutral(j))
            all_neutral = 0;
    }

    if (connected == 0) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "CONNECT A CONTROLLER");
        return;
    }

    if (!all_neutral) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "RELEASE THE CONTROLLER");
        return;
    }

    s_pad_capture_wait_neutral = false;

    snprintf(s_menu_status, sizeof s_menu_status,
             s_pad_reassigning ? "PRESS A BUTTON ON NEW CONTROLLER" : "PRESS DELETE: AUTO");
}

static void menu_resolve_pad_conflict(void)
{
    pad_binding_t *target;
    pad_binding_t *other;
    pad_binding_t old_target;

    if (s_pad_conflict_action < 0 ||
        s_pad_capture_player < 0 || s_pad_capture_player > 1 ||
        s_pad_capture_action < 0 || s_pad_capture_action >= 10)
        return;

    target = &pad_players[s_pad_capture_player][s_pad_capture_action];
    other = &pad_players[s_pad_capture_player][s_pad_conflict_action];
    old_target = *target;

    *target = s_pad_conflict_binding;
    *other = old_target;

    s_pad_capturing = false;
    s_pad_capture_wait_neutral = true;
    s_pad_capture_player = -1;
    s_pad_capture_action = -1;
    s_pad_reassigning = false;
    s_pad_conflict_action = -1;
    save_bindings();
}

static void menu_draw_text(int x, int y, const char *text,
                           int scale, SDL_Color color) {
    static const struct {
        char c;
        unsigned char rows[7];
    } glyphs[] = {
        {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
        {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
        {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
        {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
        {'I',{14,4,4,4,4,4,14}},     {'J',{7,2,2,2,18,18,12}},
        {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
        {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
        {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
        {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
        {'S',{15,16,16,14,1,1,30}},   {'T',{31,4,4,4,4,4,4}},
        {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
        {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
        {'Y',{17,17,10,4,4,4,4}},     {'Z',{31,1,2,4,8,16,31}},
        {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
        {'2',{14,17,1,2,4,8,31}},     {'3',{30,1,1,14,1,1,30}},
        {'4',{2,6,10,18,31,2,2}},     {'5',{31,16,16,30,1,1,30}},
        {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
        {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
        {'-',{0,0,0,31,0,0,0}}, {':',{0,4,4,0,4,4,0}},
        {' ',{0,0,0,0,0,0,0}}
    };
    int cursor = x;

    SDL_SetRenderDrawColor(s_ren, color.r, color.g, color.b, color.a);

    while (*text) {
        char ch = (char)toupper((unsigned char)*text++);
        size_t i;
        const unsigned char *rows = NULL;

        for (i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++) {
            if (glyphs[i].c == ch) {
                rows = glyphs[i].rows;
                break;
            }
        }

        if (rows) {
            int row, col;
            for (row = 0; row < 7; row++) {
                for (col = 0; col < 5; col++) {
                    if (rows[row] & (1u << (4 - col))) {
                        SDL_Rect pixel = {
                            cursor + col * scale,
                            y + row * scale,
                            scale, scale
                        };
                        SDL_RenderFillRect(s_ren, &pixel);
                    }
                }
            }
        }

        cursor += 6 * scale;
    }
}


static int menu_item_count(void)
{
    switch (s_menu_screen) {
    case MENU_MAIN:
        return 3;

    case MENU_CONTROLS:
        return 6;

    case MENU_GRAPHICS:
        return 2;

    case MENU_KEYBOARD_P1:
    case MENU_KEYBOARD_P2:
        return 12; /* 10 actions + restore + back */
    case MENU_KEYBOARD_SYSTEM:
        return 6;  /* 5 actions + back */
    case MENU_PAD_P1:
    case MENU_PAD_P2:
        return 13; /* 10 actions + change controller + restore + back */

    default:
        return 0;
    }
}

static int menu_first_item_y(int panel_y)
{
    if (menu_is_keyboard_screen() || menu_is_pad_screen())
        return panel_y + 28;

    if (s_menu_screen == MENU_MAIN)
        return panel_y + 42;

    if (s_menu_screen == MENU_CONTROLS)
        return panel_y + 32;

    return panel_y + 48;
}

static void menu_back(void)
{
    if (s_menu_capturing || s_conflict_action >= 0 ||
        s_pad_capturing || s_pad_conflict_action >= 0) {
        if (s_pad_capturing || s_pad_conflict_action >= 0)
            menu_cancel_pad_capture();
        else
            menu_cancel_capture();
        return;
    }

    if (s_menu_screen == MENU_KEYBOARD_P1 ||
        s_menu_screen == MENU_KEYBOARD_P2 ||
        s_menu_screen == MENU_KEYBOARD_SYSTEM ||
        s_menu_screen == MENU_PAD_P1 ||
        s_menu_screen == MENU_PAD_P2) {
        s_menu_screen = MENU_CONTROLS;
        s_menu_selection = 0;
    } else if (s_menu_screen != MENU_MAIN) {
        s_menu_screen = MENU_MAIN;
        s_menu_selection = 0;
    } else {
        s_menu_open = false;
    }

    s_menu_status[0] = '\0';
}

static void menu_restore_keyboard_defaults(int player)
{
    static const SDL_Scancode p1_defaults[8] = {
        SDL_SCANCODE_UP,
        SDL_SCANCODE_DOWN,
        SDL_SCANCODE_LEFT,
        SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z,
        SDL_SCANCODE_X,
        SDL_SCANCODE_C,
        SDL_SCANCODE_V
    };

    static const SDL_Scancode p2_defaults[8] = {
        SDL_SCANCODE_W,
        SDL_SCANCODE_S,
        SDL_SCANCODE_A,
        SDL_SCANCODE_D,
        SDL_SCANCODE_F,
        SDL_SCANCODE_G,
        SDL_SCANCODE_H,
        SDL_SCANCODE_J
    };

    if (player == 0) {
        memcpy(b.p1, p1_defaults, sizeof p1_defaults);
        b.start1 = SDL_SCANCODE_1;
        b.coin1 = SDL_SCANCODE_5;
    } else if (player == 1) {
        memcpy(b.p2, p2_defaults, sizeof p2_defaults);
        b.start2 = SDL_SCANCODE_2;
        b.coin2 = SDL_SCANCODE_6;
    } else {
        return;
    }

    if (save_bindings()) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "DEFAULTS RESTORED");
    }
}

static void menu_restore_pad_defaults(void)
{
    int player = menu_pad_player();

    if (player < 0 || player > 1)
        return;

    memset(pad_players[player], 0, sizeof pad_players[player]);

    if (save_bindings()) {
        snprintf(s_menu_status, sizeof s_menu_status,
                 "DEFAULTS RESTORED");
    }
}

static void menu_activate(void)
{
    if (s_menu_capturing || s_pad_capturing)
        return;

    if (s_conflict_action >= 0) {
        menu_resolve_conflict();
        return;
    }

    if (s_pad_conflict_action >= 0) {
        menu_resolve_pad_conflict();
        return;
    }

    s_menu_status[0] = '\0';

    switch (s_menu_screen) {
    case MENU_MAIN:
        if (s_menu_selection == 0) {
            s_menu_screen = MENU_CONTROLS;
            s_menu_selection = 0;
        } else if (s_menu_selection == 1) {
            s_menu_screen = MENU_GRAPHICS;
            s_menu_selection = 0;
        } else {
            s_menu_open = false;
        }
        break;

    case MENU_CONTROLS:
        if (s_menu_selection == 0)
            s_menu_screen = MENU_KEYBOARD_P1;
        else if (s_menu_selection == 1)
            s_menu_screen = MENU_KEYBOARD_P2;
        else if (s_menu_selection == 2)
            s_menu_screen = MENU_PAD_P1;
        else if (s_menu_selection == 3)
            s_menu_screen = MENU_PAD_P2;
        else if (s_menu_selection == 4)
            s_menu_screen = MENU_KEYBOARD_SYSTEM;
        else {
            menu_back();
            return;
        }

        s_menu_selection = 0;
        s_menu_status[0] = '\0';
        break;

    case MENU_GRAPHICS:
        if (s_menu_selection == 0) {
            menu_toggle_fullscreen();
        } else {
            menu_back();
        }
        break;

    case MENU_KEYBOARD_P1:
    case MENU_KEYBOARD_P2:
        if (s_menu_selection < 10) {
            menu_start_capture(menu_keyboard_action_index());
        } else if (s_menu_selection == 10) {
            menu_restore_keyboard_defaults(
                s_menu_screen == MENU_KEYBOARD_P1 ? 0 : 1);
        } else {
            menu_back();
        }
        break;

    case MENU_KEYBOARD_SYSTEM:
        if (s_menu_selection < 5) {
            menu_start_capture(menu_keyboard_action_index());
        } else {
            menu_back();
        }
        break;

    case MENU_PAD_P1:
    case MENU_PAD_P2:
        if (s_menu_selection < 10) {
            menu_start_pad_capture(s_menu_selection);
        } else if (s_menu_selection == 10) {
            menu_start_pad_reassignment();
        } else if (s_menu_selection == 11) {
            menu_restore_pad_defaults();
        } else {
            menu_back();
        }
        break;
    }
}

static void menu_move(int direction) {
    int count = menu_item_count();

    if (count <= 0)
        return;

    s_menu_selection += direction;

    if (s_menu_selection < 0)
        s_menu_selection = count - 1;
    else if (s_menu_selection >= count)
        s_menu_selection = 0;
}

static int menu_is_profile_screen(void)
{
    return s_menu_screen == MENU_KEYBOARD_P1 ||
           s_menu_screen == MENU_KEYBOARD_P2 ||
           s_menu_screen == MENU_PAD_P1 ||
           s_menu_screen == MENU_PAD_P2;
}

static void menu_draw(void)
{
    int keyboard_screen = menu_is_keyboard_screen();
    int pad_screen = menu_is_pad_screen();
    int controls_screen = s_menu_screen == MENU_CONTROLS;
    int panel_w = keyboard_screen ? 240 :
                  pad_screen ? 260 :
                  controls_screen ? 230 : 200;
    int panel_h = keyboard_screen ? 210 :
                  pad_screen ? 230 :
                  controls_screen ? 190 : 140;

    SDL_Rect panel = {
        NG_SCREEN_W / 2 - panel_w / 2,
        NG_SCREEN_H / 2 - panel_h / 2,
        panel_w, panel_h
    };

    SDL_Color white = { 240, 240, 240, 255 };
    SDL_Color cyan = { 90, 220, 255, 255 };
    SDL_Color muted = { 170, 180, 195, 255 };

    const char *title = "";
    const char *items[9] = { "", "", "", "", "", "", "", "", "" };

    int count;
    int first_y;
    int row_step;
    int i;

    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 165);
    SDL_RenderFillRect(s_ren, NULL);

    SDL_SetRenderDrawColor(s_ren, 18, 24, 44, 245);
    SDL_RenderFillRect(s_ren, &panel);

    SDL_SetRenderDrawColor(s_ren, 70, 170, 220, 255);
    SDL_RenderDrawRect(s_ren, &panel);

    switch (s_menu_screen) {
    case MENU_MAIN:
        title = "MAGICAL DROP III";
        items[0] = "CONTROLS";
        items[1] = "GRAPHICS";
        items[2] = "RESUME GAME";
        break;

    case MENU_CONTROLS:
        title = "CONTROLS";
        items[0] = "KEYBOARD P1";
        items[1] = "KEYBOARD P2";
        items[2] = "CONTROLLER P1";
        items[3] = "CONTROLLER P2";
        items[4] = "SYSTEM KEYS";
        items[5] = "BACK";
        break;

    case MENU_GRAPHICS:
        title = "GRAPHICS";
        items[0] = "TOGGLE FULLSCREEN";
        items[1] = "BACK";
        break;

    case MENU_KEYBOARD_P1:
        title = "KEYBOARD P1";
        break;

    case MENU_KEYBOARD_P2:
        title = "KEYBOARD P2";
        break;

    case MENU_KEYBOARD_SYSTEM:
        title = "SYSTEM KEYS";
        break;

    case MENU_PAD_P1:
        title = "CONTROLLER P1";
        break;

    case MENU_PAD_P2:
        title = "CONTROLLER P2";
        break;

    default:
        return;
    }

    menu_draw_text(panel.x + 12, panel.y + 8,
                   title, 2, white);

    count = menu_item_count();
    first_y = menu_first_item_y(panel.y);
    row_step = menu_is_profile_screen() ? 13 :
           keyboard_screen ? 17 : 22;

    for (i = 0; i < count; i++) {
        char action_text[64];
        const char *label;
        SDL_Rect option;
        SDL_Color text_color;

        if (keyboard_screen) {
            int action_count = menu_keyboard_action_count();

            if (i < action_count) {
                int action;
                int previous_selection = s_menu_selection;

                s_menu_selection = i;
                action = menu_keyboard_action_index();
                s_menu_selection = previous_selection;

                if (action >= 0) {
                    snprintf(action_text, sizeof action_text,
                            "%s: %s",
                            keyboard_labels[action],
                            menu_key_name(action));
                    label = action_text;
                } else {
                    label = "";
                }
            } else if (menu_is_profile_screen() && i == 10) {
                label = "RESTORE DEFAULTS";
            } else {
                label = "BACK";
            }
        } else if (pad_screen) {
            int player = menu_pad_player();

            if (i < 10) {
                char binding_text[32];

                format_pad_binding(&pad_players[player][i],
                                binding_text, sizeof binding_text);

                snprintf(action_text, sizeof action_text,
                        "%s: %s", pad_action_labels[i],
                        pad_players[player][i].type == PAD_NONE
                            ? "AUTO" : binding_text);

                label = action_text;
            } else if (i == 10) {
                label = "CHANGE CONTROLLER";
            } else if (i == 11) {
                label = "RESTORE DEFAULTS";
            } else {
                label = "BACK";
            }
        } else {
            label = items[i];
        }

        option.x = panel.x + 8;
        option.y = first_y + i * row_step;
        option.w = panel.w - 16;
        option.h = menu_is_profile_screen() ? 12 :
           keyboard_screen ? 15 : 18;

        text_color = s_menu_selection == i ? white : muted;

        if (s_menu_selection == i) {
            SDL_Rect marker = {
                option.x + 3, option.y + 5, 4, 4
            };

            SDL_SetRenderDrawColor(s_ren, 40, 100, 145, 255);
            SDL_RenderFillRect(s_ren, &option);

            SDL_SetRenderDrawColor(s_ren, cyan.r, cyan.g,
                                   cyan.b, cyan.a);
            SDL_RenderFillRect(s_ren, &marker);
        }

        menu_draw_text(option.x + 12, option.y + 4,
                       label, 1, text_color);
    }

    if (s_menu_status[0]) {
        menu_draw_text(panel.x + 10,
                       panel.y + panel.h - 12,
                       s_menu_status, 1, cyan);
    }
}

static bool menu_run(const uint32_t *fb) {
    SDL_Event e;

    if (fb)
        SDL_UpdateTexture(s_tex, NULL, fb, NG_SCREEN_W * 4);

    while (s_menu_open) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                return false;

            handle_device_events(&e);

            if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
                SDL_Scancode sc = e.key.keysym.scancode;

                if (s_pad_capturing) {
                    if (sc == SDL_SCANCODE_ESCAPE) menu_cancel_pad_capture();
                    else if (!s_pad_reassigning &&
                             (sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE)) {
                        pad_binding_t automatic = { PAD_NONE, 0, 0 };
                        menu_finish_pad_capture(&automatic);
                    }
                    continue;
                }

                if (s_pad_conflict_action >= 0) {
                    if (sc == SDL_SCANCODE_ESCAPE) {
                        menu_cancel_pad_capture();
                    } else if (sc == SDL_SCANCODE_RETURN ||
                               sc == SDL_SCANCODE_SPACE) {
                        menu_resolve_pad_conflict();
                    }
                    continue;
                }

                if (s_menu_capturing) {
                    menu_capture_key(e.key.keysym.scancode);
                    continue;
                }

                if (s_conflict_action >= 0) {
                    if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        menu_cancel_capture();
                    } else if (e.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                            e.key.keysym.scancode == SDL_SCANCODE_SPACE) {
                        menu_resolve_conflict();
                    }
                    continue;
                }

                if (sc == b.fullscreen_toggle ||
                    (sc == SDL_SCANCODE_RETURN &&
                    (e.key.keysym.mod & KMOD_ALT))) {
                    menu_toggle_fullscreen();
                    continue;
                }

                if (sc == b.overlay_toggle) {
                    s_menu_open = false;
                    break;
                }

                if (sc == SDL_SCANCODE_ESCAPE) {
                    menu_back();
                    break;
                }

                if (sc == SDL_SCANCODE_UP) {
                    menu_move(-1);
                } else if (sc == SDL_SCANCODE_DOWN) {
                    menu_move(1);
                } else if (sc == SDL_SCANCODE_RETURN ||
                           sc == SDL_SCANCODE_SPACE) {
                    menu_activate();
                }
            }

            if (s_pad_capturing) {
                if (e.type == SDL_JOYBUTTONDOWN ||
                    e.type == SDL_JOYHATMOTION ||
                    e.type == SDL_JOYAXISMOTION) {
                    menu_capture_pad_event(&e);
                }
                continue;
            }

            if (s_pad_conflict_action >= 0)
                continue;

            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                SDL_GameControllerButton button =
                    (SDL_GameControllerButton)e.cbutton.button;

                if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    menu_move(-1);
                } else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    menu_move(1);
                } else if (button == SDL_CONTROLLER_BUTTON_A ||
                           button == SDL_CONTROLLER_BUTTON_START) {
                    menu_activate();
                } else if (button == SDL_CONTROLLER_BUTTON_B) {
                    menu_back();
                }
            }

            if (e.type == SDL_JOYHATMOTION) {
                int player = find_device(e.jhat.which);

                /* Controller-mapped devices are handled above. */
                if (player >= 0 && !devices[player].controller) {
                    if (e.jhat.value & SDL_HAT_UP)
                        menu_move(-1);
                    else if (e.jhat.value & SDL_HAT_DOWN)
                        menu_move(1);
                }
            }

            if (e.type == SDL_JOYBUTTONDOWN) {
                int player = find_device(e.jbutton.which);

                /* Avoid processing controller buttons twice. */
                if (player >= 0 && !devices[player].controller) {
                    if (e.jbutton.button == 0)
                        menu_activate();
                    else if (e.jbutton.button == 1)
                        menu_back();
                }
            }

            if (e.type == SDL_MOUSEMOTION ||
                e.type == SDL_MOUSEBUTTONDOWN) {
                int mx = e.type == SDL_MOUSEMOTION
                    ? e.motion.x : e.button.x;
                int my = e.type == SDL_MOUSEMOTION
                    ? e.motion.y : e.button.y;
                int keyboard_screen = menu_is_keyboard_screen();
                int pad_screen = menu_is_pad_screen();
                int controls_screen = s_menu_screen == MENU_CONTROLS;
                int panel_h = keyboard_screen ? 210 :
                              pad_screen ? 230 :
                              controls_screen ? 190 : 140;
                int panel_y = NG_SCREEN_H / 2 - panel_h / 2;
                int first_y = menu_first_item_y(panel_y);
                int row_step = menu_is_profile_screen() ? 13 :
                    keyboard_screen ? 17 : 22;
                int count = menu_item_count();
                int panel_w = keyboard_screen ? 240 :
                              pad_screen ? 260 :
                              controls_screen ? 230 : 200;
                int i;

                if (mx >= NG_SCREEN_W / 2 - (panel_w / 2 - 8) &&
                    mx <= NG_SCREEN_W / 2 + (panel_w / 2 - 8)) {
                    for (i = 0; i < count; i++) {
                        int top = first_y + i * row_step;

                        int row_height = menu_is_profile_screen() ? 12 :
                            keyboard_screen ? 15 : 18;

                        if (my >= top && my < top + row_height) {
                            s_menu_selection = i;

                            if (e.type == SDL_MOUSEBUTTONDOWN &&
                                e.button.button == SDL_BUTTON_LEFT) {
                                menu_activate();
                            }
                            break;
                        }
                    }
                }
            }
        }

        menu_update_pad_capture();

        if (!s_menu_open)
            break;

        SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
        SDL_RenderClear(s_ren);

        if (s_tex)
            SDL_RenderCopy(s_ren, s_tex, NULL, NULL);

        menu_draw();
        SDL_RenderPresent(s_ren);
        SDL_Delay(16);
    }

    s_menu_wait_release = true;
    return true;
}

static bool menu_any_controller_button_down(void) {
    int i;

    SDL_PumpEvents();

    for (i = 0; i < MAX_INPUT_DEVICES; i++) {
        if (devices[i].controller) {
            int b;

            for (b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
                if (SDL_GameControllerGetButton(
                        devices[i].controller,
                        (SDL_GameControllerButton)b)) {
                    return true;
                }
            }
        } else if (devices[i].joystick) {
            int b;
            int count = SDL_JoystickNumButtons(
                devices[i].joystick);

            for (b = 0; b < count; b++) {
                if (SDL_JoystickGetButton(
                        devices[i].joystick, b)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool window_frame(const uint32_t *fb, ng_input_t *in) {
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return false;

        if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            SDL_Scancode sc = e.key.keysym.scancode;

            if (sc == b.overlay_toggle) {
                s_menu_open = true;
                s_menu_screen = MENU_MAIN;
                s_menu_selection = 0;
                break;
            }

            if (sc == SDL_SCANCODE_ESCAPE)
                return false;

            if (sc == b.fullscreen_toggle ||
                (sc == SDL_SCANCODE_RETURN &&
                (e.key.keysym.mod & KMOD_ALT))) {
                menu_toggle_fullscreen();
            }
        }

        handle_device_events(&e);
    }

    if (s_menu_open)
        return menu_run(fb);

    read_keys(in);
    read_controller(0, in);
    read_controller(1, in);

    if (s_menu_wait_release) {
        bool button_down = menu_any_controller_button_down();

        memset(in, 0, sizeof(*in));

        if (!button_down) {
            s_menu_wait_release = false;
        }
    }

    if (fb) {
        SDL_UpdateTexture(s_tex, NULL, fb, NG_SCREEN_W * 4);

        SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
        SDL_RenderClear(s_ren);

        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }

    {
        uint64_t f = SDL_GetPerformanceFrequency();
        s_next_tick += (uint64_t)((double)f / 59.185606);
        uint64_t n = SDL_GetPerformanceCounter();

        if (n < s_next_tick)
            SDL_Delay((Uint32)((s_next_tick - n) * 1000 / f));
        else if (n - s_next_tick > f / 10)
            s_next_tick = n;
    }

    return true;
}

void window_close(void) {
    int i;
    for (i = 0; i < MAX_INPUT_DEVICES; i++)
        close_device(i);

    player_device[0] = -1;
    player_device[1] = -1;

    if (s_audio) {
        SDL_CloseAudioDevice(s_audio);
        s_audio = 0;
    }

    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);

    s_tex = NULL;
    s_ren = NULL;
    s_win = NULL;

    SDL_Quit();
}