#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <alsa/asoundlib.h>

#define SAMPLE_RATE     16000
#define CHANNELS        2
#define PREROLL_FRAMES  (SAMPLE_RATE * 5)
#define BLOCK_FRAMES    320 /* 每次读320帧，每帧为两个声道数据4个字节，总共约20ms */

static short preroll[PREROLL_FRAMES][CHANNELS];
static volatile sig_atomic_t save_request;

static void request_save(int signo)
{
    (void)signo;
    save_request = 1;
}

static int save_preroll(size_t write_index, size_t valid_frames)
{
	size_t oldest;
	size_t i;
	FILE *file;

	if (valid_frames == 0) {
		fprintf(stderr, "NO captured audio to save\n");
		return 1;
	}

	oldest = (write_index + PREROLL_FRAMES - valid_frames) % PREROLL_FRAMES;

	file = fopen("/preroll-audio.pcm", "wb");
	if (!file) {
		perror("fopen preroll");
		return 1;
	}

	for (i = 0; i < valid_frames; i++) {
		size_t index = (oldest + i) % PREROLL_FRAMES;

		if (fwrite(preroll[index], sizeof(preroll[0]), 1, file) != 1) {
			perror("fwrite preroll");
			fclose(file);
			return 1;
		}
	}

	if (fclose(file) == EOF) {
		perror("fclose preroll");
		return 1;
	}

	printf("Saved %zu audio frames to /preroll-audio.pcm\n", valid_frames);
	return 0;
}

int main(void)
{
    struct sigaction action = { .sa_handler = request_save };
    snd_pcm_t *pcm;
    size_t write_index = 0;
    size_t valid_frames = 0;
    int status = 1;
    int ret;

    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    /* 第二个参数是选声卡0 PCM设备0，第三个参数表示是录音，第四个参数是非阻塞等待 */
    ret = snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(ret));
        return 1;
    }

    /* 第三个参数表示内存顺序 L0 R0 L1 R1....， 最后一个参数是PCM缓冲区可以装下0.2s的音频数据 */
    ret = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                            CHANNELS, SAMPLE_RATE, 0, 200000);
    if (ret < 0) {
        fprintf(stderr, "snd_pcm_set_params: %s\n", snd_strerror(ret));
        goto close_pcm;
    }

    ret = snd_pcm_start(pcm);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_start: %s\n", snd_strerror(ret));
		goto close_pcm;
	}

    printf("Capturing audio. To save preroll: kill -USR1 %ld\n", (long)getpid());
    fflush(stdout);

    while (!save_request) {
		snd_pcm_uframes_t wanted = BLOCK_FRAMES;
		snd_pcm_sframes_t frames;

		if (wanted > PREROLL_FRAMES - write_index)
			wanted = PREROLL_FRAMES - write_index;

        /* 录音设备指向 hw:0,0 采集流，接收数据的数组，请求读取多少个音频 */
        frames = snd_pcm_readi(pcm, &preroll[write_index], wanted);
		if (frames == -EINTR)
			continue;

		if (frames == -EAGAIN || frames == 0) {
			ret = snd_pcm_wait(pcm, 100); /* 等待PCM就绪，最长等100ms */
			if (ret >= 0 || ret == -EINTR)
				continue;

			fprintf(stderr, "snd_pcm_wait: %s\n", snd_strerror(ret));
			goto close_pcm;
		}

        if (frames < 0) {
			fprintf(stderr, "snd_pcm_readi: %s\n",
				snd_strerror((int)frames));
			goto close_pcm;
		}

		write_index = (write_index + frames) % PREROLL_FRAMES;
		valid_frames += frames;
		if (valid_frames > PREROLL_FRAMES)
			valid_frames = PREROLL_FRAMES;
    }

    status = 0;

close_pcm:
    snd_pcm_close(pcm);
	if (status)
		return status;
	
	return save_preroll(write_index, valid_frames);
}
