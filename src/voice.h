#ifndef ZELL_VOICE_H
#define ZELL_VOICE_H

/* Paradee 8M. bm_fable is the British male storyteller. */
#define ZELL_VOICE_ID "bm_fable"
#define ZELL_VOICE_SCHEMA "kilix.zell.voice/v1"

typedef struct zell_voice_runtime {
    char python[400];
    char synth[400];
    char model[400];
    char config[400];
    char player[400];
} zell_voice_runtime;

const char *zell_voice_state_path(void);

/* 1 when the file was read, 0 when it is missing, -1 when it is unusable.
 * Missing and unusable both leave enabled set to the default, which is on. */
int zell_voice_load(const char *path, int *enabled);
int zell_voice_save(const char *path, int enabled);
int zell_voice_parse(const char *text, int *enabled);

int zell_voice_should_speak(int enabled, const char *text);
int zell_voice_prepare(zell_voice_runtime *runtime);
int zell_voice_synth_argv(const zell_voice_runtime *runtime, const char *text,
                          const char *wav, const char **argv, int capacity);

#endif
