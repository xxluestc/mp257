#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

#define PCM_DEVICE "default"

int main(int argc, char *argv[])
{
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    unsigned int rate = 48000;
    int channels = 2;
    int err;
    char *buffer;
    int size;
    snd_pcm_uframes_t frames;
    FILE *fp;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <wav_file>\n", argv[0]);
        return 1;
    }

    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    fseek(fp, 44, SEEK_SET);

    err = snd_pcm_open(&handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open error: %s\n", snd_strerror(err));
        fclose(fp);
        return 1;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, params, channels);
    snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0);

    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params error: %s\n", snd_strerror(err));
        snd_pcm_close(handle);
        fclose(fp);
        return 1;
    }

    snd_pcm_hw_params_get_period_size(params, &frames, 0);
    size = frames * channels * 2;
    buffer = malloc(size);
    if (!buffer) {
        fprintf(stderr, "malloc failed\n");
        snd_pcm_close(handle);
        fclose(fp);
        return 1;
    }

    printf("Playing %s: %uHz, %d channels, %d-bit\n",
           argv[1], rate, channels, 16);
    printf("Period size: %lu frames, buffer: %d bytes\n", frames, size);

    while (1) {
        int n = fread(buffer, 1, size, fp);
        if (n <= 0)
            break;

        err = snd_pcm_writei(handle, buffer, n / (channels * 2));
        if (err < 0) {
            err = snd_pcm_recover(handle, err, 0);
            if (err < 0) {
                fprintf(stderr, "snd_pcm_writei error: %s\n", snd_strerror(err));
                break;
            }
        }
    }

    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    free(buffer);
    fclose(fp);
    printf("Done.\n");
    return 0;
}
