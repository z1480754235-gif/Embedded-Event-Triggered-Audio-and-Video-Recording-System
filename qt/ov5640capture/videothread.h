#ifndef VIDEOTHREAD_H
#define VIDEOTHREAD_H

#include <QImage>
#include <QString>
#include <QThread>

#include <atomic>

class videothread : public QThread
{
	Q_OBJECT

public:
	explicit videothread(QObject *parent = nullptr);
	void requestSave(const QString &path);

signals:
	void frameReady(const QImage &image);
	void formatReady();
	void errorOccurred(const QString &message);
	void prerollReady();
	void prerollSaved(unsigned int count);

protected:
	void run() override;

private:
	std::atomic_bool saveRequested;
	QString savePath;
};

#endif
