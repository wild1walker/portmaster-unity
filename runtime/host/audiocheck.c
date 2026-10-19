// Device check: can a 32-bit (armhf) SDL2 program open audio with the given driver while the
// system sound server runs? Plays one second of silence. Usage: SDL_AUDIODRIVER=x ./audiocheck
#include <SDL2/SDL.h>
#include <stdio.h>
int main(void)
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    printf("driver %s\n", SDL_GetCurrentAudioDriver());
    SDL_AudioSpec want = { .freq = 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 512 }, have;
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!d) { printf("open failed: %s\n", SDL_GetError()); return 1; }
    printf("opened: %d Hz, %d ch, %d samples\n", have.freq, have.channels, have.samples);
    static short silence[48000 * 2];
    SDL_QueueAudio(d, silence, sizeof silence);
    SDL_PauseAudioDevice(d, 0);
    SDL_Delay(1100);
    printf("queued left %u bytes (0 = played)\n", SDL_GetQueuedAudioSize(d));
    SDL_CloseAudioDevice(d);
    SDL_Quit();
    return 0;
}
