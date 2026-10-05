#define _POSIX_C_SOURCE 200809L
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "microaudio.h"

#include <alsa/asoundlib.h>
#include <alloca.h>
#include <pthread.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MA_MAX_VOICES 32
#define MA_CHANNELS   2

typedef enum {
	TA_SOUND_WAV,
	TA_SOUND_FLAC
} ta_sound_type;

struct ma_sound {
	ma_context *ctx;

	float    *samples;
	uint64_t  frames;
	uint32_t rate;
	uint32_t channels;

	ta_sound_type type;
};

struct ma_voice {
	ma_context *ctx;
	ma_sound  *sound;

	double pos;

	float volume;
	float pan;

	int looping;
	int active;
};

struct ma_context {
	uint32_t rate;
	uint32_t buffer_frames;

	snd_pcm_t *pcm;
	pthread_t  thread;
	pthread_mutex_t mutex;

	int running;

	ma_voice voices[MA_MAX_VOICES];

	float   *mix;
	int16_t *buffer;
};

/* HELPERS */
static uint16_t ma_u16(const unsigned char *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t ma_u32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ma_read_exact(FILE *f, void *dst, size_t size) {
	return fread(dst, 1, size, f) == size;
}

static int ma_skip(FILE *f, uint32_t size) {
	unsigned char buffer[256];

	while (size > 0) {
		size_t chunk = size < sizeof(buffer) ? size : sizeof(buffer);

		if (fread(buffer, 1, chunk, f) != chunk) {
			return 0;
		}
		size -= (uint32_t)chunk;
	}
	return 1;
}

static float ma_decode_i24(const unsigned char *p) {
	int32_t x = ((int32_t)p[0]) | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
	if (x & 0x800000) {
		x |= ~0xffffff;
	}
	return (float)((double)x / 8388608.0);
}

/* sound loading */
ma_sound *load_sound(ma_context *ctx, const char *path) {
	const char *ext = strrchr(path, '.');

	if (ext && strcmp(ext, ".flac") == 0) {
		return ma_load_flac(ctx, path);
	}

	return ma_load_wav(ctx, path);
}

/* WAV */
static int ma_load_wav_file(FILE *f, ma_sound **out, ma_context *ctx) {
	unsigned char riff[12];

	if (!ma_read_exact(f, riff, sizeof(riff))) {
		return 0;
	}

	if (memcmp(riff, "RIFF", 4) != 0) {
		return 0;
	}

	if (memcmp(riff + 8, "WAVE", 4) != 0) {
		return 0;
	}

	uint16_t format = 0;
	uint16_t channels = 0;
	uint16_t bits = 0;
	uint16_t block_align = 0;
	uint32_t sample_rate = 0;

	uint32_t data_size = 0;
	long data_pos = -1;

	for (;;) {
		unsigned char chunk[8];

		if (!ma_read_exact(f, chunk, sizeof(chunk))) {
			break;
		}

		uint32_t size = ma_u32(chunk + 4);

		if (memcmp(chunk, "fmt ", 4) == 0) {
			unsigned char fmt[16];

			if (size < 16) {
				return 0;
			}

			if (!ma_read_exact(f, fmt, sizeof(fmt))) {
				return 0;
			}

			format      = ma_u16(fmt + 0);
			channels    = ma_u16(fmt + 2);
			sample_rate = ma_u32(fmt + 4);
			block_align = ma_u16(fmt + 12);
			bits        = ma_u16(fmt + 14);

			if (size > 16) {
				if (!ma_skip(f, size - 16)) {
					return 0;
				}
			}
		}
		else if (memcmp(chunk, "data", 4) == 0) {
			data_pos  = ftell(f);
			data_size = size;

			if (data_pos < 0) {
				return 0;
			}

			if (!ma_skip(f, size)) {
				return 0;
			}
		}
		else {
			if (!ma_skip(f, size)) {
				return 0;
			}
		}
	}

	/*
	*   PCM integer   : format 1
	*   IEEE float    : format 3
	*   mono / stereo
	*   16 / 24 / 32-bit
	*/

	if (data_pos < 0) {
		return 0;
	}

	if (channels == 0 || channels > 2) {
		return 0;
	}

	if (sample_rate == 0) {
		return 0;
	}

	if (format != 1 && format != 3) {
		return 0;
	}

	if (format == 3 && bits != 32) {
		return 0;
	}

	if (format == 1 && bits != 16 && bits != 24 && bits != 32) {
		return 0;
	}

	if (block_align != channels * (bits / 8)) {
		return 0;
	}

	if (block_align == 0 || data_size % block_align != 0) {
		return 0;
	}

	uint64_t frames = data_size / block_align;
	uint64_t count  = frames * channels;

	if (frames == 0) {
		return 0;
	}

	if (count > SIZE_MAX / sizeof(float)) {
		return 0;
	}

	ma_sound *sound = calloc(1, sizeof(*sound));
	if (!sound) {
		return 0;
	}

	sound->samples = malloc((size_t)count * sizeof(float));

	if (!sound->samples) {
		free(sound);
		return 0;
	}

	if (fseek(f, data_pos, SEEK_SET) != 0) {
		free(sound->samples);
		free(sound);
		return 0;
	}

	unsigned char *raw = malloc((size_t)data_size);

	if (!raw) {
		free(sound->samples);
		free(sound);
		return 0;
	}

	if (fread(raw, 1, (size_t)data_size, f) != data_size) {
		free(raw);
		free(sound->samples);
		free(sound);
		return 0;
	}

	size_t bytes_per_sample = bits / 8;

	for (uint64_t i = 0; i < count; ++i) {
		const unsigned char *p = raw + i * bytes_per_sample;
		float sample = 0.0f;

		if (format == 3 && bits == 32) {
			uint32_t x = ma_u32(p);
			memcpy(&sample, &x, sizeof(sample));
		} else if (bits == 16) {
			int16_t x = (int16_t)ma_u16(p);
			sample = (float)x / 32768.0f;
		} else if (bits == 24) {
			sample = ma_decode_i24(p);
		} else {
			int32_t x = (int32_t)ma_u32(p);
			sample = (float)((double)x / 2147483648.0);
		}

		if (!isfinite(sample)) {
			sample = 0.0f;
		}

		if (sample > 1.0f) {
			sample = 1.0f;
		}

		if (sample < -1.0f) {
			sample = -1.0f;
		}

		sound->samples[i] = sample;
	}

	free(raw);

	sound->ctx      = ctx;
	sound->frames   = frames;
	sound->rate     = sample_rate;
	sound->channels = channels;

	*out = sound;
	return 1;
}

/* FLAC */
ma_sound *ma_load_flac(ma_context *ctx, const char *path) {
	if (!ctx || !path) {
		return NULL;
	}

	unsigned int channels = 0;
	unsigned int sample_rate = 0;

	drflac_uint64 total_frames = 0;

	float *samples = drflac_open_file_and_read_pcm_frames_f32(path, &channels, &sample_rate, &total_frames, NULL);

	if (!samples) {
		return NULL;
	}

	if (channels == 0 || channels > 2) {
		drflac_free(samples, NULL);
		return NULL;
	}

	ma_sound *sound = calloc(1, sizeof(*sound));

	if (!sound) {
		drflac_free(samples, NULL);
		return NULL;
	}

	sound->ctx      = ctx;
	sound->samples  = samples;
	sound->frames   = total_frames;
	sound->rate     = sample_rate;
	sound->channels = channels;
	sound->type     = TA_SOUND_FLAC;

	return sound;
}

/* ALSA */
static int ma_pcm_setup(snd_pcm_t *pcm, uint32_t rate, uint32_t buffer_frames) {
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	int err = snd_pcm_hw_params_any(pcm, params);

	if (err < 0) {
		return err;
	}

	err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);

	if (err < 0) {
		return err;
	}

	err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);

	if (err < 0) {
		return err;
	}

	err = snd_pcm_hw_params_set_channels(pcm, params, MA_CHANNELS);

	if (err < 0) {
		return err;
	}

	unsigned actual_rate = rate;
	int dir = 0;

	err = snd_pcm_hw_params_set_rate_near(pcm, params, &actual_rate, &dir);

	if (err < 0) {
		return err;
	}

	snd_pcm_uframes_t frames = buffer_frames;

	err = snd_pcm_hw_params_set_period_size_near(pcm, params, &frames, &dir);

	if (err < 0) {
		return err;
	}

	return snd_pcm_hw_params(pcm, params);
}

/* Mixer */
static void ma_mix_buffer(ma_context *ctx) {
	memset(ctx->buffer, 0, (size_t)ctx->buffer_frames * MA_CHANNELS * sizeof(int16_t));

	for (uint32_t frame = 0; frame < ctx->buffer_frames; ++frame) {
		float left  = 0.0f;
		float right = 0.0f;

		for (size_t i = 0; i < MA_MAX_VOICES; ++i) {
			ma_voice *voice = &ctx->voices[i];

			if (!voice->active || !voice->sound) {
				continue;
			}

			ma_sound *sound = voice->sound;

			if (voice->pos >= (double)sound->frames) {
				if (voice->looping) {
					voice->pos = fmod(voice->pos, (double)sound->frames);
				} else {
					voice->active = 0;
					continue;
				}
			}

			uint64_t a = (uint64_t)voice->pos;
			uint64_t b = a + 1;

			if (b >= sound->frames) {
				b = voice->looping ? 0 : a;
			}

			float t = (float)(voice->pos - (double)a);

			float al;
			float ar;
			float bl;
			float br;

			if (sound->channels == 1) {
				al = sound->samples[a];
				ar = al;

				bl = sound->samples[b];
				br = bl;
			} else {
				al = sound->samples[a * 2 + 0];
				ar = sound->samples[a * 2 + 1];

				bl = sound->samples[b * 2 + 0];
				br = sound->samples[b * 2 + 1];
			}

			float sample_left = al + (bl - al) * t;

			float sample_right = ar + (br - ar) * t;

			float pan = voice->pan;

			if (pan < -1.0f) {
				pan = -1.0f;
			}

			if (pan > 1.0f) {
				pan = 1.0f;
			}

			/*
			 * -1 = left
			 *  0 = center
			 * +1 = right
			 */

			float gain_left = pan > 0.0f ? 1.0f - pan : 1.0f;
			float gain_right = pan < 0.0f ? 1.0f + pan : 1.0f;

			left += sample_left * voice->volume * gain_left;
			right += sample_right * voice->volume * gain_right;

			voice->pos += (double)sound->rate / (double)ctx->rate;

			if (!voice->looping && voice->pos >= (double)sound->frames) {
				voice->active = 0;
			}
		}

		if (left > 1.0f) {
			left = 1.0f;
		}

		if (left < -1.0f) {
			left = -1.0f;
		}

		if (right > 1.0f) {
			right = 1.0f;
		}

		if (right < -1.0f) {
			right = -1.0f;
		}

		ctx->mix[frame * 2 + 0] = left;
		ctx->mix[frame * 2 + 1] = right;

		ctx->buffer[frame * 2 + 0] = (int16_t)lrintf(left * 32767.0f);
		ctx->buffer[frame * 2 + 1] = (int16_t)lrintf(right * 32767.0f);
	}
}

/* Audio thread */

static void *ma_audio_thread(void *arg) {
	ma_context *ctx = arg;

	for (;;) {
		pthread_mutex_lock(&ctx->mutex);
		int running = ctx->running;

		if (running) {
			ma_mix_buffer(ctx);
		}

		pthread_mutex_unlock(&ctx->mutex);

		if (!running) {
			break;
		}

		snd_pcm_sframes_t written = snd_pcm_writei(ctx->pcm, ctx->buffer, ctx->buffer_frames);

		if (written < 0) {
			int err = snd_pcm_recover(ctx->pcm, (int)written, 1);

			if (err < 0) {
				pthread_mutex_lock(&ctx->mutex);
				ctx->running = 0;
				pthread_mutex_unlock(&ctx->mutex);

				break;
			}
		}
	}
	return NULL;
}

/* API */
ma_context *ma_init(const ma_config *config) {
	ma_context *ctx = calloc(1, sizeof(*ctx));

	if (!ctx) {
		return NULL;
	}

	ctx->rate = config && config->sample_rate ? config->sample_rate : 48000;

	ctx->buffer_frames = config && config->buffer_frames ? config->buffer_frames : 256;

	if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
		goto fail_context;
	}

	int err = snd_pcm_open(&ctx->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);

	if (err < 0) {
		goto fail_mutex;
	}

	if (ma_pcm_setup(ctx->pcm, ctx->rate, ctx->buffer_frames) < 0) {
		goto fail_pcm;
	}

	ctx->mix = calloc((size_t)ctx->buffer_frames * 2, sizeof(float));

	ctx->buffer = calloc((size_t)ctx->buffer_frames * 2, sizeof(int16_t));

	if (!ctx->mix || !ctx->buffer) {
		goto fail_buffers;
	}

	for (size_t i = 0; i < MA_MAX_VOICES; ++i) {
		ctx->voices[i].ctx = ctx;
		ctx->voices[i].volume = 1.0f;
	}

	ctx->running = 1;

	if (pthread_create(&ctx->thread, NULL, ma_audio_thread, ctx) != 0) {
		goto fail_buffers;
	}

	return ctx;

	fail_buffers:
	free(ctx->mix);
	free(ctx->buffer);

	fail_pcm:
	snd_pcm_close(ctx->pcm);

	fail_mutex:
	pthread_mutex_destroy(&ctx->mutex);

	fail_context:
	free(ctx);
	return NULL;
}

void ma_shutdown(ma_context *ctx) {
	if (!ctx) {
		return;
	}

	pthread_mutex_lock(&ctx->mutex);
	ctx->running = 0;
	pthread_mutex_unlock(&ctx->mutex);

	pthread_join(ctx->thread, NULL);

	snd_pcm_drop(ctx->pcm);
	snd_pcm_close(ctx->pcm);

	pthread_mutex_destroy(&ctx->mutex);

	free(ctx->mix);
	free(ctx->buffer);
	free(ctx);
}

ma_sound *ma_load_wav(ma_context *ctx, const char *path) {
	if (!ctx || !path) {
		return NULL;
	}

	FILE *f = fopen(path, "rb");

	if (!f) {
		return NULL;
	}

	ma_sound *sound = NULL;

	int ok = ma_load_wav_file(f, &sound, ctx);

	fclose(f);
	sound->type = TA_SOUND_WAV;
	return ok ? sound : NULL;
}

void ma_free_sound(ma_sound *sound) {
	if (!sound) {
		return;
	}

	ma_context *ctx = sound->ctx;

	if (ctx) {
		pthread_mutex_lock(&ctx->mutex);

		for (size_t i = 0; i < MA_MAX_VOICES; ++i) {
			if (ctx->voices[i].sound == sound) {
				ctx->voices[i].active = 0;
			}
		}

		pthread_mutex_unlock(&ctx->mutex);
	}

	if (sound->type == TA_SOUND_FLAC) {
		drflac_free(sound->samples, NULL);
	} else {
		free(sound->samples);
	}

	free(sound);
}

ma_voice *ma_play(ma_context *ctx, ma_sound *sound) {
	if (!ctx || !sound) {
		return NULL;
	}

	if (sound->ctx != ctx) {
		return NULL;
	}

	pthread_mutex_lock(&ctx->mutex);

	for (size_t i = 0; i < MA_MAX_VOICES; ++i) {
		ma_voice *voice = &ctx->voices[i];

		if (!voice->active) {
			voice->sound   = sound;
			voice->pos     = 0.0;
			voice->volume  = 1.0f;
			voice->pan     = 0.0f;
			voice->looping = 0;
			voice->active  = 1;

			pthread_mutex_unlock(&ctx->mutex);

			return voice;
		}
	}

	pthread_mutex_unlock(&ctx->mutex);
	return NULL;
}

void ma_stop(ma_voice *voice) {
	if (!voice) {
		return;
	}

	pthread_mutex_lock(&voice->ctx->mutex);
	voice->active = 0;
	pthread_mutex_unlock(&voice->ctx->mutex);
}

void ma_voice_set_volume(ma_voice *voice, float volume) {
	if (!voice) {
		return;
	}

	if (volume < 0.0f) {
		volume = 0.0f;
	}

	pthread_mutex_lock(&voice->ctx->mutex);

	voice->volume = volume;

	pthread_mutex_unlock(&voice->ctx->mutex);
}

void ma_voice_set_pan(ma_voice *voice, float pan) {
	if (!voice) {
		return;
	}

	if (pan < -1.0f) {
		pan = -1.0f;
	}

	if (pan > 1.0f) {
		pan = 1.0f;
	}

	pthread_mutex_lock(&voice->ctx->mutex);

	voice->pan = pan;

	pthread_mutex_unlock(&voice->ctx->mutex);
}

void ma_voice_set_looping(ma_voice *voice, int looping) {
	if (!voice) {
		return;
	}
	pthread_mutex_lock(&voice->ctx->mutex);
	voice->looping = looping != 0;
	pthread_mutex_unlock(&voice->ctx->mutex);
}

int ma_voice_is_playing(const ma_voice *voice) {
	if (!voice) {
		return 0;
	}
	pthread_mutex_lock(&voice->ctx->mutex);
	int active = voice->active;
	pthread_mutex_unlock(&voice->ctx->mutex);
	return active;
}

uint32_t ma_sound_sample_rate(const ma_sound *sound) {
	return sound ? sound->rate : 0;
}

uint32_t ma_sound_channels(const ma_sound *sound) {
	return sound ? sound->channels : 0;
}

uint64_t ma_sound_frames(const ma_sound *sound) {
	return sound ? sound->frames : 0;
}
