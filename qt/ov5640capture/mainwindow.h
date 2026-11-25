#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class videothread;
class audiothread;
class impactthread;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow() override;

private slots:
	void startPreview();
	void saveEvent();

private:
	Ui::MainWindow *ui;
	videothread *videoThread;
	audiothread *audioThread;
	impactthread *impactThread;
	int finishedThreads = 0;
	int savedFiles = 0;
	bool videoPrerollReady = false;
	bool audioPrerollReady = false;
	bool saveInProgress = false;
};

#endif
