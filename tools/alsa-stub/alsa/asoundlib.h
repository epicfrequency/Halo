/* Minimal ALSA stand-in: enough declarations to really compile and link
   halo-daemon off-Linux, so type errors, missing headers and wrong argument
   counts get caught here instead of on the Pi. Runtime behaviour is faked. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_chmap snd_pcm_chmap_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
typedef int snd_pcm_format_t;
typedef int snd_pcm_access_t;
typedef int snd_pcm_stream_t;

#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_ACCESS_MMAP_INTERLEAVED 0
#define SND_PCM_FORMAT_UNKNOWN (-1)
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_FORMAT_S24_LE 6
#define SND_PCM_FORMAT_S24_3LE 32  /* real ALSA's value; 3-byte packed */
#define SND_PCM_FORMAT_S32_LE 10
#define SND_PCM_FORMAT_DSD_U8 48
#define SND_PCM_FORMAT_DSD_U16_LE 49
#define SND_PCM_FORMAT_DSD_U32_LE 50
#define SND_PCM_FORMAT_DSD_U16_BE 51
#define SND_PCM_FORMAT_DSD_U32_BE 52

/* Real ALSA declares this as a macro that stack-allocates. */
#define snd_pcm_hw_params_alloca(ptr) do { *(ptr) = alloca(64); memset(*(ptr), 0, 64); } while (0)

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode);
int snd_pcm_close(snd_pcm_t *pcm);
int snd_pcm_prepare(snd_pcm_t *pcm);
int snd_pcm_drain(snd_pcm_t *pcm);
int snd_pcm_drop(snd_pcm_t *pcm);
int snd_pcm_resume(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size);
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t access);
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t format);
int snd_pcm_hw_params_test_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t format);
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val);
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir);
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val);
int snd_pcm_hw_params_get_rate_min(const snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_get_rate_max(const snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *params, unsigned int *val);
int snd_pcm_set_chmap(snd_pcm_t *pcm, const snd_pcm_chmap_t *map);
const char *snd_pcm_format_name(snd_pcm_format_t format);
int snd_pcm_nonblock(snd_pcm_t *p, int nonblock);
typedef int snd_pcm_state_t;
#define SND_PCM_STATE_RUNNING 3
#define SND_PCM_STATE_DRAINING 5
snd_pcm_state_t snd_pcm_state(snd_pcm_t *p);
const char *snd_strerror(int errnum);

/* Linux-only errno values ALSA callers check for; absent on macOS. */
#ifndef ESTRPIPE
#define ESTRPIPE 86
#endif
#ifndef EBADFD
#define EBADFD 77   /* Linux's value; ALSA returns it for a bad stream state */
#endif
