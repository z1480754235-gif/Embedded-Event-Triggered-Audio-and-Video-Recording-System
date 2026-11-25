#include "impactthread.h"
#include "icm20608_data.h"

#include <QDebug>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

impactthread::impactthread(QObject *parent)
	: QThread(parent)
{
}

void impactthread::run()
{
	const int fd = ::open("/dev/icm20608", O_RDONLY);
	if (fd < 0) {
		qWarning() << "Cannot open /dev/icm20608, errno:" << errno;
		return;
	}

	const double threshold = 1.1 * ICM20608_ACCEL_LSB_PER_G;
	bool wasAboveThreshold = false;

	while (!isInterruptionRequested()) {
		icm20608_data data;
		const ssize_t bytes = ::read(fd, &data, sizeof(data));

		if (bytes < 0 && errno == EINTR)
			continue;

		if (bytes != static_cast<ssize_t>(sizeof(data))) {
			qWarning() << "Cannot read ICM20608, result:" << bytes;
			break;
		}

		const double x = data.accel_x;
		const double y = data.accel_y;
		const double z = data.accel_z;
		const bool aboveThreshold =
			x * x + y * y + z * z >= threshold * threshold;

		if (aboveThreshold && !wasAboveThreshold) {
			qInfo() << "ICM20608 acceleration crossed 1.1 g";
			emit impactDetected();
		}

		wasAboveThreshold = aboveThreshold;
		msleep(20);
	}

	::close(fd);
}
