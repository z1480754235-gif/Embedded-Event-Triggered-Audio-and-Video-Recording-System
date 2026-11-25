#ifndef AUDIOTHREAD_H
#define AUDIOTHREAD_H

#include <QString>
#include <QThread>

#include <atomic>

class audiothread : public QThread
{
	Q_OBJECT

public:
	explicit audiothread(QObject *parent = nullptr);
	void requestSave(const QString &path);

signals:
	void errorOccurred(const QString &message);
	void prerollReady();
	void prerollSaved();

protected:
	void run() override;

private:
	std::atomic_bool saveRequested{false};
	QString savePath;
};

#endif
