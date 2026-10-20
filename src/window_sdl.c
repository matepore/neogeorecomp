/*
 * window_sdl.c — the SDL2 window: presentation, sound, keyboard, frame pacing.
 * Only built when SDL2 is found; see platform.c for the headless stubs.
 *
 * Keys: arrows, Z/X/C/V = A/B/C/D, 1/2 = start P1/P2, 3 = select,
 * 5/6 = coin 1/2, 9 = service, F2 = test switch, Esc = quit.
 */
#include "ng_internal.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static uint64_t s_next_tick;
static SDL_AudioDeviceID s_audio;

int window_open(const char *title, int scale) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             NG_SCREEN_W * scale, NG_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(s_ren, NG_SCREEN_W, NG_SCREEN_H);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              NG_SCREEN_W, NG_SCREEN_H);
    s_next_tick = SDL_GetPerformanceCounter();
    /* Sound at the YM2610's own rate; SDL converts it for the device. A
     * missing audio device leaves the game silent, not stopped. */
    SDL_AudioSpec want = {0};
    want.freq = NG_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
        s_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!s_audio) fprintf(stderr, "warning: no sound: %s\n", SDL_GetError());
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
    if (k[SDL_SCANCODE_UP]) in->p1 |= NG_UP;
    if (k[SDL_SCANCODE_DOWN]) in->p1 |= NG_DOWN;
    if (k[SDL_SCANCODE_LEFT]) in->p1 |= NG_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) in->p1 |= NG_RIGHT;
    if (k[SDL_SCANCODE_Z]) in->p1 |= NG_A;
    if (k[SDL_SCANCODE_X]) in->p1 |= NG_B;
    if (k[SDL_SCANCODE_C]) in->p1 |= NG_C;
    if (k[SDL_SCANCODE_V]) in->p1 |= NG_D;
    in->start1 = k[SDL_SCANCODE_1];
    in->start2 = k[SDL_SCANCODE_2];
    in->select1 = k[SDL_SCANCODE_3];
    in->coin1 = k[SDL_SCANCODE_5];
    in->coin2 = k[SDL_SCANCODE_6];
    in->service = k[SDL_SCANCODE_9];
    in->test = k[SDL_SCANCODE_F2];
}

bool window_frame(const uint32_t *fb, ng_input_t *in) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
    }
    read_keys(in);
    if (fb) {
        SDL_UpdateTexture(s_tex, NULL, fb, NG_SCREEN_W * 4);
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }
    /* Pace to the MVS refresh rate, 59.1856 Hz. */
    uint64_t freq = SDL_GetPerformanceFrequency();
    s_next_tick += (uint64_t)((double)freq / 59.185606);
    uint64_t now = SDL_GetPerformanceCounter();
    if (now < s_next_tick) SDL_Delay((Uint32)((s_next_tick - now) * 1000 / freq));
    else if (now - s_next_tick > freq / 10) s_next_tick = now;
    return true;
}

void window_close(void) {
    if (s_audio) SDL_CloseAudioDevice(s_audio);
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    SDL_Quit();
}
