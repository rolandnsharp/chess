/* sound.h - synthesised sound effects, played on a background thread.
 * The sound card is opened only while a sound plays, so other programs
 * (notification chimes, voice engines) can still use it. */
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>

typedef enum {
    SFX_MOVE, SFX_CAPTURE, SFX_CASTLE, SFX_CHECK, SFX_PROMOTE,
    SFX_ERROR,   /* a typed move that isn't legal or isn't notation */
    SFX_CORRECT, SFX_WRONG, SFX_TICK,
    SFX_START,   /* an online opponent was found */
    SFX_WIN, SFX_END,
    SFX_COUNT
} Sfx;

void sound_init(void);
void sound_shutdown(void);
void sound_play(Sfx s);
void sound_set_enabled(bool on);
bool sound_enabled(void);
const char *sound_device(void); /* the ALSA device in use */

#endif
