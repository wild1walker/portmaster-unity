// glreplay --hash: readbacks hashed where passes end (glreplay_hash.c)
#ifndef GLREPLAY_HASH_H
#define GLREPLAY_HASH_H
int rph_init(const char *out, const char *frames, int draws, const char *dump, void *(*sym)(const char *));
int rph_on(long frame);                 // hashing this frame
int rph_draws(void);                    // also after every draw (--hash-draws)
void rph_read(long frame, unsigned long call, const char *event, unsigned game_fb, int window, int win_w, int win_h);
void rph_close(void);
#endif
