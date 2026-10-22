/*
 * SDL2 video, audio, keyboard and game-controller input.
 * Keyboard bindings are loaded from controls.cfg.
 * SDL-mapped controllers and generic joysticks are supported.
 * First connected device is player 1; second is player 2.
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

/* ponytail: a fixed layout; remapping belongs to the frontend (ROADMAP.md) */

typedef struct {
    SDL_Scancode p1[8], p2[8];
    SDL_Scancode start1, start2, select1, coin1, coin2, service, test;
} bindings_t;

typedef struct {
    SDL_GameController *controller;
    SDL_Joystick *joystick;
    SDL_JoystickID instance_id;
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
    PAD_START1 = 0,
    PAD_START2,
    PAD_SELECT,
    PAD_COIN1,
    PAD_COIN2,
    PAD_SERVICE,
    PAD_TEST,
    PAD_SYSTEM_COUNT
};

static pad_binding_t pad_players[2][8];
static pad_binding_t pad_system[PAD_SYSTEM_COUNT];
static bindings_t b;
static input_device_t devices[2];

static const char *names[23] = {
    "P1_UP", "P1_DOWN", "P1_LEFT", "P1_RIGHT",
    "P1_A", "P1_B", "P1_C", "P1_D",
    "P2_UP", "P2_DOWN", "P2_LEFT", "P2_RIGHT",
    "P2_A", "P2_B", "P2_C", "P2_D",
    "START1", "START2", "SELECT", "COIN1", "COIN2",
    "SERVICE", "TEST"
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
        "PAD_START1", "PAD_START2", "PAD_SELECT",
        "PAD_COIN1", "PAD_COIN2", "PAD_SERVICE", "PAD_TEST"
    };

    int p, i;

    for (p = 0; p < 2; p++) {
        for (i = 0; i < 8; i++) {
            if (strcmp(name, player_names[p][i]) == 0)
                return &pad_players[p][i];
        }
    }

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
    char line[256];
    int i, j;

    defaults();
    memset(pad_players, 0, sizeof pad_players);
    memset(pad_system, 0, sizeof pad_system);

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
    for (i = 0; i < 23; i++) {
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

static void close_device(int player) {
    input_device_t *d = &devices[player];

    if (d->controller) {
        SDL_GameControllerClose(d->controller);
    } else if (d->joystick) {
        SDL_JoystickClose(d->joystick);
    }

    d->controller = NULL;
    d->joystick = NULL;
    d->instance_id = -1;
}

static int find_device(SDL_JoystickID instance_id) {
    int i;

    for (i = 0; i < 2; i++) {
        if (devices[i].instance_id == instance_id)
            return i;
    }

    return -1;
}

static int open_device(int device_index) {
    SDL_JoystickID instance_id;
    int player;

    if (device_index < 0 ||
        device_index >= SDL_NumJoysticks()) {
        return -1;
    }

    instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
    if (instance_id < 0) return -1;

    /* SDL may report the same device through controller and joystick
       events. Never open or assign a device twice. */
    if (find_device(instance_id) >= 0) return 0;

    /* Keep player slots stable while a device remains connected. */
    for (player = 0; player < 2; player++) {
        if (!devices[player].controller &&
            !devices[player].joystick) {
            break;
        }
    }

    if (player >= 2) {
        fprintf(stderr,
                "[input] Device ignored: both player slots are occupied.\n");
        return -1;
    }

    if (SDL_IsGameController(device_index)) {
        devices[player].controller =
            SDL_GameControllerOpen(device_index);

        if (devices[player].controller) {
            devices[player].joystick =
                SDL_GameControllerGetJoystick(
                    devices[player].controller);
        }
    }

    /* Fallback for devices without an SDL controller mapping. */
    if (!devices[player].joystick) {
        devices[player].controller = NULL;
        devices[player].joystick = SDL_JoystickOpen(device_index);
    }

    if (!devices[player].joystick) {
        fprintf(stderr, "[input] Could not open device: %s\n",
                SDL_GetError());
        devices[player].controller = NULL;
        devices[player].joystick = NULL;
        devices[player].instance_id = -1;
        return -1;
    }

    devices[player].instance_id =
        SDL_JoystickInstanceID(devices[player].joystick);

    fprintf(stderr, "[input] Player %d connected: %s%s\n",
            player + 1,
            SDL_JoystickName(devices[player].joystick)
                ? SDL_JoystickName(devices[player].joystick)
                : "Unknown device",
            devices[player].controller
                ? " (SDL game controller)"
                : " (generic joystick)");

    return player;
}

static void handle_device_events(const SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED ||
        e->type == SDL_JOYDEVICEADDED) {
        open_device(e->cdevice.which);
    } else if (e->type == SDL_CONTROLLERDEVICEREMOVED ||
               e->type == SDL_JOYDEVICEREMOVED) {
        int player = find_device(e->cdevice.which);

        if (player >= 0) {
            fprintf(stderr, "[input] Player %d disconnected.\n",
                    player + 1);
            close_device(player);
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
    input_device_t *d = &devices[player];
    SDL_GameController *c = d->controller;
    SDL_Joystick *j = d->joystick;
    uint8_t *bits = player == 0 ? &in->p1 : &in->p2;

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
     * System controls.
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

        if (pad_system[PAD_START1].type != PAD_NONE)
            start = pad_binding_active(j, &pad_system[PAD_START1]);

        if (pad_system[PAD_SELECT].type != PAD_NONE)
            select = pad_binding_active(j, &pad_system[PAD_SELECT]);

        if (pad_system[PAD_COIN1].type != PAD_NONE)
            coin = pad_binding_active(j, &pad_system[PAD_COIN1]);

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

        if (pad_system[PAD_START2].type != PAD_NONE)
            start = pad_binding_active(j, &pad_system[PAD_START2]);

        if (pad_system[PAD_COIN2].type != PAD_NONE)
            coin = pad_binding_active(j, &pad_system[PAD_COIN2]);

        in->start2 |= start;
        in->coin2 |= coin;
    }
}

int window_open(const char *title, int scale) {
    int i;

    memset(devices, 0, sizeof devices);
    for (i = 0; i < 2; i++)
        devices[i].instance_id = -1;

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

bool window_frame(const uint32_t *fb, ng_input_t *in) {
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return false;

        if (e.type == SDL_KEYDOWN &&
            e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            return false;

        handle_device_events(&e);
    }

    read_keys(in);
    read_controller(0, in);
    read_controller(1, in);

    if (fb) {
        SDL_UpdateTexture(s_tex, NULL, fb, NG_SCREEN_W * 4);
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

    for (i = 0; i < 2; i++)
        close_device(i);

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