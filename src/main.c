#define _POSIX_C_SOURCE 200809L

#include "microaudio.h"

#include <stdio.h>
#include <time.h>

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s file.wav\n", argv[0]);
		return 2;
	}

	ma_config config = {.sample_rate   = 48000, .buffer_frames = 256};

	ma_context *ctx = ma_init(&config);

	if (!ctx) {
		fprintf(stderr, "tinyaudio: failed to initialize ALSA\n");
		return 1;
	}

	ma_sound *sound = load_sound(ctx, argv[1]);

	if (!sound) {
		fprintf(stderr, "tinyaudio: failed to load WAV\n");
		ma_shutdown(ctx);
		return 1;
	}

	printf("%u Hz, %u channels, %llu frames\n", ma_sound_sample_rate(sound), ma_sound_channels(sound), (unsigned long long)ma_sound_frames(sound));

	ma_voice *voice = ma_play(ctx, sound);

	if (!voice) {
		fprintf(stderr, "tinyaudio: no free voices\n");
		ma_free_sound(sound);
		ma_shutdown(ctx);
		return 1;
	}

	ma_voice_set_volume(voice, 0.85f);

	struct timespec sleep_time = {
		.tv_sec  = 0,
		.tv_nsec = 10 * 1000 * 1000
	};

	while (ma_voice_is_playing(voice)) nanosleep(&sleep_time, NULL);

	ma_free_sound(sound);
	ma_shutdown(ctx);

	return 0;
}
