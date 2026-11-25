#ifndef IMPACTTHREAD_H
#define IMPACTTHREAD_H

#include <QThread>

class impactthread : public QThread
{
	Q_OBJECT

public:
	explicit impactthread(QObject *parent = nullptr);

signals:
	void impactDetected();

protected:
	void run() override;
};

#endif
