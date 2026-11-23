/* stick_record.c - 5d: graba con SDL3 lo que dan los sticks, para elegir la zona muerta.
 *
 * Compilar (salida en work\5d, junto a una copia de SDL3.dll):
 *   clang-cl /O2 /Iwork\sdl3\include tools\stick_record.c work\sdl3\lib\SDL3.lib /Fe:work\5d\stick_record.exe /Fo:work\5d\
 *
 * Uso:  work\5d\stick_record.exe work\5d\sticks.csv
 *
 * Una linea por cambio: ms, mando, lx, ly, rx, ry, en crudo (-32768 a 32767). Lee cada 1 ms.
 * Acaba con START en cualquier mando o a los 180 s. Solo mira los mandos conectados.
 */
#include <stdio.h>
#include <SDL3/SDL.h>

#define MAX_PADS 4

int main(int argc, char** argv)
{
    FILE* f = fopen(argc > 1 ? argv[1] : "sticks.csv", "w");
    if (!f) { fprintf(stderr, "no se puede escribir el archivo\n"); return 1; }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    fprintf(f, "ms,pad,lx,ly,rx,ry\n");

    SDL_Gamepad* pad[MAX_PADS] = { 0 };
    Sint16 last[MAX_PADS][4] = { { 0 } };
    const Uint64 t0 = SDL_GetTicksNS();
    int lines = 0, stop = 0, shown = -1;
    printf("Grabando. START en el mando para acabar (maximo 180 s).\n");
    while (!stop && SDL_GetTicksNS() - t0 < 180000000000ull) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED)
                for (int i = 0; i < MAX_PADS; i++)
                    if (!pad[i]) {
                        pad[i] = SDL_OpenGamepad(ev.gdevice.which);
                        printf("mando %d: %s (%s)\n", i, pad[i] ? SDL_GetGamepadName(pad[i]) : "?",
                               pad[i] ? SDL_GetGamepadStringForType(SDL_GetGamepadType(pad[i])) : "?");
                        break;
                    }
            if (ev.type == SDL_EVENT_GAMEPAD_REMOVED)
                for (int i = 0; i < MAX_PADS; i++)
                    if (pad[i] && SDL_GetGamepadID(pad[i]) == ev.gdevice.which) {
                        SDL_CloseGamepad(pad[i]);
                        pad[i] = NULL;
                        printf("mando %d desconectado\n", i);
                    }
        }
        const double ms = (double)(SDL_GetTicksNS() - t0) / 1e6;
        for (int i = 0; i < MAX_PADS; i++) {
            if (!pad[i]) continue;
            const Sint16 v[4] = { SDL_GetGamepadAxis(pad[i], SDL_GAMEPAD_AXIS_LEFTX),
                                  SDL_GetGamepadAxis(pad[i], SDL_GAMEPAD_AXIS_LEFTY),
                                  SDL_GetGamepadAxis(pad[i], SDL_GAMEPAD_AXIS_RIGHTX),
                                  SDL_GetGamepadAxis(pad[i], SDL_GAMEPAD_AXIS_RIGHTY) };
            if (SDL_memcmp(v, last[i], sizeof v)) {
                SDL_memcpy(last[i], v, sizeof v);
                fprintf(f, "%.1f,%d,%d,%d,%d,%d\n", ms, i, v[0], v[1], v[2], v[3]);
                lines++;
            }
            if (SDL_GetGamepadButton(pad[i], SDL_GAMEPAD_BUTTON_START)) stop = 1;
        }
        if ((int)(ms / 5000) != shown) { shown = (int)(ms / 5000); printf("%d s, %d lineas\n", shown * 5, lines); fflush(stdout); }
        SDL_DelayNS(1000000);
    }
    fclose(f);
    printf("Listo: %d lineas.\n", lines);
    SDL_Quit();
    return 0;
}
