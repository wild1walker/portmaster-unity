// Device check: does 32-bit SDL2 see the built-in pad and open audio? (no window: SDL_INIT_VIDEO off)
#include <SDL2/SDL.h>
#include <stdio.h>
int main(void)
{
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_version v; SDL_GetVersion(&v);
    printf("SDL %d.%d.%d, audio driver %s, joysticks %d\n", v.major, v.minor, v.patch, SDL_GetCurrentAudioDriver(), SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        printf("  %d: %s, game controller %s (%s)\n", i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i) ? "yes" : "no",
               SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i) : "-");
    SDL_AudioSpec want = {0}, have;
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    printf("audio: %s (%d Hz, %d ch, %d samples)\n", dev ? "open" : SDL_GetError(), have.freq, have.channels, have.samples);
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
