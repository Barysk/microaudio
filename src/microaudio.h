#ifndef TINYAUDIO_H
#define TINYAUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
	#endif

	typedef struct ma_context ma_context;
	typedef struct ma_sound   ma_sound;
	typedef struct ma_voice   ma_voice;

	typedef struct {
		uint32_t sample_rate;
		uint32_t buffer_frames;
	} ma_config;

	/* Context */
	ma_context *ma_init(const ma_config *config);
	void ma_shutdown(ma_context *ctx);

	/* Sounds */
	ma_sound * load_sound(ma_context *ctx, const char *path);
	ma_sound *ma_load_wav(ma_context *ctx, const char *path);
	void ma_free_sound(ma_sound *sound);
	ma_sound *ma_load_flac(ma_context *ctx, const char *path);

	uint32_t ma_sound_sample_rate(const ma_sound *sound);
	uint32_t ma_sound_channels(const ma_sound *sound);
	uint64_t ma_sound_frames(const ma_sound *sound);

	/* Voices */
	ma_voice *ma_play(ma_context *ctx, ma_sound *sound);
	void ma_stop(ma_voice *voice);

	void ma_voice_set_volume(ma_voice *voice, float volume);
	void ma_voice_set_pan(ma_voice *voice, float pan);
	void ma_voice_set_looping(ma_voice *voice, int looping);
	int ma_voice_is_playing(const ma_voice *voice);

	#ifdef __cplusplus
}
#endif

#endif
