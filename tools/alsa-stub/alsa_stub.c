#include <alsa/asoundlib.h>
#include <stdio.h>
struct _snd_pcm { int dummy; };
static struct _snd_pcm g_pcm;
static snd_pcm_uframes_t g_written;

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t s, int m) {
    (void)name; (void)s; (void)m; *pcm = &g_pcm; return 0;
}
int snd_pcm_close(snd_pcm_t *p) { (void)p; return 0; }
int snd_pcm_prepare(snd_pcm_t *p) { (void)p; return 0; }
int snd_pcm_drain(snd_pcm_t *p) { (void)p; return 0; }
int snd_pcm_drop(snd_pcm_t *p) { (void)p; return 0; }
int snd_pcm_resume(snd_pcm_t *p) { (void)p; return 0; }
/* Pretend the device consumes everything, instantly. */
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *p, const void *b, snd_pcm_uframes_t n) {
    (void)p; (void)b; g_written += n; return (snd_pcm_sframes_t)n;
}
int snd_pcm_hw_params(snd_pcm_t *p, snd_pcm_hw_params_t *h) { (void)p; (void)h; return 0; }
int snd_pcm_hw_params_any(snd_pcm_t *p, snd_pcm_hw_params_t *h) { (void)p; (void)h; return 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_access_t a) { (void)p;(void)h;(void)a; return 0; }
int snd_pcm_hw_params_set_format(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_format_t f) { (void)p;(void)h;(void)f; return 0; }
int snd_pcm_hw_params_test_format(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_format_t f) { (void)p;(void)h;(void)f; return 0; }
int snd_pcm_hw_params_set_channels(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int v) { (void)p;(void)h;(void)v; return 0; }
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)p;(void)h;(void)d; (void)v; return 0; }
int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)p;(void)h;(void)v;(void)d; return 0; }
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)p;(void)h;(void)v;(void)d; return 0; }
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *d) { (void)h;(void)d; *v = 1024; return 0; }
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v) { (void)h; *v = 8192; return 0; }
int snd_pcm_hw_params_get_rate_min(const snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)h;(void)d; *v = 44100; return 0; }
int snd_pcm_hw_params_get_rate_max(const snd_pcm_hw_params_t *h, unsigned int *v, int *d) { (void)h;(void)d; *v = 768000; return 0; }
int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *h, unsigned int *v) { (void)h; *v = 2; return 0; }
int snd_pcm_set_chmap(snd_pcm_t *p, const snd_pcm_chmap_t *m) { (void)p; (void)m; return 0; }
const char *snd_pcm_format_name(snd_pcm_format_t f) { (void)f; return "STUB_FORMAT"; }
const char *snd_strerror(int e) { (void)e; return "stub error"; }
