#include "videothread.h"

#include <QFile>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const unsigned int CAPTURE_BUFFER_COUNT = 4;
static const unsigned int PREROLL_FRAME_COUNT = 150;

struct capture_buffer {
	void *start;
	size_t length;
};

static int xioctl(int fd, unsigned long request, void *argument)
{
	int ret;

	do {
		ret = ::ioctl(fd, request, argument);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static QImage yuyvToRgb(const unsigned char *data, int width, int height)
{
	QImage image(width, height, QImage::Format_RGB32);
	if (image.isNull())
		return image;

	for (int row = 0; row < height; row++) {
		QRgb *target = reinterpret_cast<QRgb *>(image.scanLine(row));

		for (int column = 0; column < width; column += 2) {
			int u = data[1] - 128;
			int v = data[3] - 128;
			int red = 359 * v / 256;
			int green = (88 * u + 183 * v) / 256;
			int blue = 454 * u / 256;

			/* 两个像素各有亮度 Y，共用色度 U 和 V。 */
			for (int pixel = 0; pixel < 2; pixel++) {
				int y = data[pixel * 2];

				target[column + pixel] = qRgb(
					qBound(0, y + red, 255),
					qBound(0, y - green, 255),
					qBound(0, y + blue, 255));
			}
			data += 4;
		}
	}

	return image;
}

static int savePreroll(const QString &path, const unsigned char *preroll,
	size_t frameSize, unsigned int oldest, unsigned int count,
	unsigned int postCount)
{
	FILE *file = ::fopen(QFile::encodeName(path).constData(), "wb");
	if (!file) {
		::perror("fopen preroll");
		return -1;
	}

	for (unsigned int i = 0; i < count + postCount; i++) {
		unsigned int index = i < count
			? (oldest + i) % PREROLL_FRAME_COUNT
			: PREROLL_FRAME_COUNT + (i - count);

		const unsigned char *frame =
			preroll + static_cast<size_t>(index) * frameSize;

		if (::fwrite(frame, 1, frameSize, file) != frameSize) {
			::perror("fwrite preroll");
			::fclose(file);
			return -1;
		}
	}

	if (::fclose(file) == EOF) {
		::perror("fclose preroll");
		return -1;
	}

	return 0;
}

videothread::videothread(QObject *parent)
	: QThread(parent)
	, saveRequested(false)
{
}

void videothread::requestSave(const QString &path)
{
	savePath = path;
	saveRequested.store(true);
}

void videothread::run()
{
	struct v4l2_format format;
	struct v4l2_requestbuffers request;
	struct capture_buffer buffers[CAPTURE_BUFFER_COUNT];
	static const unsigned int POSTROLL_FRAME_COUNT = 150;
	struct pollfd waitFd;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bool streaming = false;
	unsigned int mappedCount = 0;
	unsigned char *preroll = nullptr;
	size_t frameSize = 0;
	unsigned int writeIndex = 0;
	unsigned int validCount = 0;
	unsigned int postCount = 0;
	int fd;

	saveRequested.store(false);

	fd = ::open("/dev/video0", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		emit errorOccurred(QString("Cannot open camera: %1")
			.arg(QString::fromLocal8Bit(strerror(errno))));
		return;
	}

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = 640;
	format.fmt.pix.height = 480;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	format.fmt.pix.field = V4L2_FIELD_NONE;

	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
		emit errorOccurred(QString("Cannot set capture format: %1")
			.arg(QString::fromLocal8Bit(strerror(errno))));
		goto out;
	}

	if (format.fmt.pix.width != 640 || format.fmt.pix.height != 480 ||
		format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
		format.fmt.pix.bytesperline != 1280 ||
		format.fmt.pix.sizeimage != 614400) {
		emit errorOccurred("Expected tightly packed 640x480 YUYV frames");
		goto out;
	}

	frameSize = format.fmt.pix.sizeimage;
	preroll = static_cast<unsigned char *>(
		::malloc(frameSize * (PREROLL_FRAME_COUNT + POSTROLL_FRAME_COUNT)));
	if (!preroll) {
		emit errorOccurred("Cannot allocate preroll buffer");
		goto out;
	}

	memset(&request, 0, sizeof(request));
	request.count = CAPTURE_BUFFER_COUNT;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;

	if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0) {
		emit errorOccurred(QString("Cannot allocate capture buffers: %1")
			.arg(QString::fromLocal8Bit(strerror(errno))));
		goto out;
	}

	if (request.count < 2 || request.count > CAPTURE_BUFFER_COUNT) {
		emit errorOccurred("Unsupported capture buffer count");
		goto out;
	}

	for (unsigned int i = 0; i < request.count; i++) {
		struct v4l2_buffer buffer;
		void *start;

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = request.type;
		buffer.memory = request.memory;
		buffer.index = i;

		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			emit errorOccurred(QString("Cannot query capture buffer: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}

		if (buffer.length < format.fmt.pix.sizeimage) {
			emit errorOccurred("Capture buffer is too small");
			goto out;
		}

		start = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, buffer.m.offset);
		if (start == MAP_FAILED) {
			emit errorOccurred(QString("Cannot map capture buffer: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}

		buffers[i].start = start;
		buffers[i].length = buffer.length;
		mappedCount++;

		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			emit errorOccurred(QString("Cannot queue capture buffer: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}
	}

	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		emit errorOccurred(QString("Cannot start capture: %1")
			.arg(QString::fromLocal8Bit(strerror(errno))));
		goto out;
	}
	streaming = true;
	emit formatReady();

	memset(&waitFd, 0, sizeof(waitFd));
	waitFd.fd = fd;
	waitFd.events = POLLIN;

	while (!isInterruptionRequested() && postCount < POSTROLL_FRAME_COUNT) {
		struct v4l2_buffer buffer;
		QImage image;
		int ret;

		ret = ::poll(&waitFd, 1, 100);
		if (ret < 0) {
			if (errno == EINTR)
				continue;

			emit errorOccurred(QString("Cannot wait for a frame: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}

		if (isInterruptionRequested())
			break;

		if (ret == 0)
			continue;

		if (waitFd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			emit errorOccurred("Camera capture error");
			goto out;
		}

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = type;
		buffer.memory = V4L2_MEMORY_MMAP;

		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN)
				continue;

			emit errorOccurred(QString("Cannot dequeue a frame: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}

		if (buffer.index >= mappedCount ||
			buffer.bytesused != format.fmt.pix.sizeimage ||
			buffer.bytesused > buffers[buffer.index].length ||
			(buffer.flags & V4L2_BUF_FLAG_ERROR)) {
			emit errorOccurred("Incomplete camera frame");
			goto out;
		}

		if (buffer.sequence % 2 == 0) {
			image = yuyvToRgb(
				static_cast<const unsigned char *>(buffers[buffer.index].start),
				format.fmt.pix.width, format.fmt.pix.height);
			if (image.isNull()) {
				emit errorOccurred("Cannot allocate preview image");
				goto out;
			}
		}

		if (saveRequested.load()) {
			size_t index = PREROLL_FRAME_COUNT + postCount;

			memcpy(preroll + index * frameSize,
				buffers[buffer.index].start, frameSize);
			postCount++;
		} else {
			memcpy(preroll + static_cast<size_t>(writeIndex) * frameSize,
				buffers[buffer.index].start, frameSize);
			writeIndex = (writeIndex + 1) % PREROLL_FRAME_COUNT;

			if (validCount < PREROLL_FRAME_COUNT) {
				validCount++;
				if (validCount == PREROLL_FRAME_COUNT)
					emit prerollReady();
			}
		}

		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			emit errorOccurred(QString("Cannot requeue capture buffer: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}

		if (!image.isNull())
			emit frameReady(image);
	}

	if (postCount == POSTROLL_FRAME_COUNT && !isInterruptionRequested()) {
		unsigned int oldest =
			(writeIndex + PREROLL_FRAME_COUNT - validCount) %
			PREROLL_FRAME_COUNT;

		if (validCount == 0) {
			emit errorOccurred("No frames to save");
			goto out;
		}

		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
			emit errorOccurred(QString("Cannot stop capture: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
			goto out;
		}
		streaming = false;

		if (savePreroll(savePath, preroll, frameSize, oldest, validCount,
			postCount) < 0) {
			emit errorOccurred("Cannot save video; check terminal output");
			goto out;
		}

		emit prerollSaved(validCount + postCount);
	}

out:
	if (streaming && xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
		emit errorOccurred(QString("Cannot stop capture: %1")
			.arg(QString::fromLocal8Bit(strerror(errno))));

	while (mappedCount > 0) {
		mappedCount--;
		::munmap(buffers[mappedCount].start, buffers[mappedCount].length);
	}

	::free(preroll);
	::close(fd);
}
