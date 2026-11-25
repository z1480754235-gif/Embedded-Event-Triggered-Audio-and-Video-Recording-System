#include "audiothread.h"

#include <QDebug>
#include <QSaveFile>

#include <alsa/asoundlib.h>
#include <errno.h>
#include <vector>

audiothread::audiothread(QObject *parent)
	: QThread(parent)
{
}

void audiothread::requestSave(const QString &path)
{
	savePath = path;
	saveRequested.store(true);
}

void audiothread::run()
{
	snd_pcm_t *pcm;

	saveRequested.store(false);

	int ret = snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_CAPTURE,
		SND_PCM_NONBLOCK);
	if (ret < 0) {
		emit errorOccurred(QString("Cannot open microphone: %1")
			.arg(QString::fromLocal8Bit(snd_strerror(ret))));
		return;
	}

	ret = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
		SND_PCM_ACCESS_RW_INTERLEAVED, 2, 16000, 0, 200000);
	if (ret >= 0)
		ret = snd_pcm_start(pcm);

	if (ret < 0) {
		emit errorOccurred(QString("Cannot start audio capture: %1")
			.arg(QString::fromLocal8Bit(snd_strerror(ret))));
		snd_pcm_close(pcm);
		return;
	}

	short samples[320][2];
	const unsigned int prerollFrames = 16000 * 5;
	const unsigned int postrollFrames = 16000 * 5;
	std::vector<short> preroll((prerollFrames + postrollFrames) * 2);
	unsigned int writeIndex = 0;
	unsigned int validFrames = 0;
	unsigned int postFrames = 0;

	while (!isInterruptionRequested() && postFrames < postrollFrames) {
		snd_pcm_sframes_t frames = snd_pcm_readi(pcm, samples, 320);

		/* PCM 就绪时返回 1，超时时返回 0。 */
		if (frames == -EAGAIN || frames == 0)
			ret = snd_pcm_wait(pcm, 100);
		else
			ret = frames;

		if (ret == -EINTR)
			continue;

		if (ret < 0) {
			emit errorOccurred(QString("Cannot read audio: %1")
				.arg(QString::fromLocal8Bit(snd_strerror(ret))));
			break;
		}

		const bool saving = saveRequested.load() && validFrames == prerollFrames;

		for (snd_pcm_sframes_t i = 0;
			i < frames && postFrames < postrollFrames; ++i) {
			if (saving) {
				unsigned int index = prerollFrames + postFrames++;
				preroll[index * 2] = samples[i][0];
				preroll[index * 2 + 1] = samples[i][1];
				continue;
			}

			preroll[writeIndex * 2] = samples[i][0];
			preroll[writeIndex * 2 + 1] = samples[i][1];
			writeIndex = (writeIndex + 1) % prerollFrames;

			if (validFrames < prerollFrames) {
				++validFrames;
				if (validFrames == prerollFrames)
					emit prerollReady();
			}
		}
	}

	snd_pcm_close(pcm);

	if (postFrames != postrollFrames || isInterruptionRequested())
		return;

	QSaveFile file(savePath);
	if (!file.open(QIODevice::WriteOnly)) {
		emit errorOccurred(file.errorString());
		return;
	}

	const char *data = reinterpret_cast<const char *>(preroll.data());
	const qint64 frameBytes = sizeof(samples[0]);
	const qint64 firstBytes = (prerollFrames - writeIndex) * frameBytes;
	const qint64 secondBytes = writeIndex * frameBytes;
	const qint64 postBytes = postrollFrames * frameBytes;

	if (file.write(data + secondBytes, firstBytes) != firstBytes ||
		file.write(data, secondBytes) != secondBytes ||
		file.write(data + prerollFrames * frameBytes, postBytes) != postBytes ||
		!file.commit()) {
		emit errorOccurred(file.errorString());
		return;
	}

	qInfo() << "Saved 160000 audio frames to" << savePath;
	emit prerollSaved();
}
