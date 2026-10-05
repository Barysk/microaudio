# microaudio

Micro Linux/ALSA C playback library.

Features: WAV PCM/float input, mono/stereo sources, float32 stereo mixer, 32 voices, volume, pan, looping, and simple linear resampling.

This is intentionally a small learning-oriented playback core, not a miniaudio replacement.

supports wav and flac formats.

## Usage

Initialize audio:

```c
ma_config config = {
	.sample_rate = 48000,
	.buffer_frames = 256
};

ma_context *audio = ma_init(&config);
```

Load and play a sound:

```c
ma_sound *sound = ma_load_wav(audio, "sound.wav");

ma_voice *voice = ma_play(audio, sound);

ma_voice_set_volume(voice, 0.8f);
ma_voice_set_pan(voice, -0.3f);
```

Looping:

```c
ma_voice_set_looping(voice, 1);
```

Stop playback:

```c
ma_stop(voice);
```

Check playback:

```c
while (ma_voice_is_playing(voice)) {
	/* game loop */
}
```

Cleanup:

```c
ma_free_sound(sound);
ma_shutdown(audio);
```

With FLAC support:

```c
ma_sound *sound = ma_load_flac(audio, "music.flac");
```

The mixer supports multiple simultaneous voices.
