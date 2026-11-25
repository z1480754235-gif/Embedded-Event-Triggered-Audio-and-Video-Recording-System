#include "mainwindow.h"
#include "audiothread.h"
#include "impactthread.h"
#include "ui_mainwindow.h"
#include "videothread.h"

#include <QDir>
#include <QPixmap>
#include <QTemporaryDir>

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
	, videoThread(new videothread(this))
	, audioThread(new audiothread(this))
	, impactThread(new impactthread(this))
{
	ui->setupUi(this);
	ui->startButton->setEnabled(true);

	connect(ui->startButton, &QPushButton::clicked,
		this, &MainWindow::startPreview);

	connect(videoThread, &videothread::formatReady, this, [this]() {
		ui->statusLabel->setText("Previewing 640x480 YUYV");
	});

	connect(videoThread, &videothread::frameReady, this,
		[this](const QImage &image) {
			ui->videoLabel->setPixmap(QPixmap::fromImage(image).scaled(
				ui->videoLabel->size(), Qt::KeepAspectRatio,
				Qt::FastTransformation));
		});

	auto updateSaveButton = [this]() {
		if (videoPrerollReady && audioPrerollReady &&
			ui->stopButton->isEnabled()) {
			ui->statusLabel->setText("Audio and video buffers ready");
			ui->saveButton->setEnabled(true);
		}
	};

	connect(videoThread, &videothread::prerollReady, this,
		[this, updateSaveButton]() {
			videoPrerollReady = true;
			updateSaveButton();
		});

	connect(audioThread, &audiothread::prerollReady, this,
		[this, updateSaveButton]() {
			audioPrerollReady = true;
			updateSaveButton();
		});

	connect(ui->saveButton, &QPushButton::clicked,
		this, &MainWindow::saveEvent);

	auto fileSaved = [this]() {
		if (++savedFiles == 2)
			ui->statusLabel->setText("Audio and video saved");
	};

	connect(videoThread, &videothread::prerollSaved, this, fileSaved);
	connect(audioThread, &audiothread::prerollSaved, this, fileSaved);

	connect(videoThread, &videothread::errorOccurred,
		ui->statusLabel, &QLabel::setText);

	connect(audioThread, &audiothread::errorOccurred,
		ui->statusLabel, &QLabel::setText);

	auto threadFinished = [this]() {
		if (++finishedThreads != 2)
			return;

		if (saveInProgress && savedFiles == 2) {
			startPreview();
			ui->statusLabel->setText("Audio and video saved. Restarting preview...");
			return;
		}

		ui->startButton->setEnabled(true);
	};

	connect(videoThread, &QThread::finished, this, [this, threadFinished]() {
		videoPrerollReady = false;
		if (!saveInProgress)
			audioThread->requestInterruption();
		ui->stopButton->setEnabled(false);
		ui->saveButton->setEnabled(false);
		threadFinished();
	});

	connect(audioThread, &QThread::finished, this, [this, threadFinished]() {
		audioPrerollReady = false;
		if (!saveInProgress)
			videoThread->requestInterruption();
		ui->stopButton->setEnabled(false);
		ui->saveButton->setEnabled(false);
		threadFinished();
	});

	connect(ui->stopButton, &QPushButton::clicked, this, [this]() {
		videoThread->requestInterruption();
		audioThread->requestInterruption();
		ui->stopButton->setEnabled(false);
		ui->saveButton->setEnabled(false);
		ui->statusLabel->setText("Stop requested");
	});

	connect(ui->exitButton, &QPushButton::clicked,
		this, &QWidget::close);

	connect(impactThread, &impactthread::impactDetected,
		this, &MainWindow::saveEvent);
	impactThread->start();
}

void MainWindow::startPreview()
{
	ui->saveButton->setEnabled(false);
	ui->startButton->setEnabled(false);
	ui->stopButton->setEnabled(true);
	ui->statusLabel->setText("Opening camera...");
	finishedThreads = 0;
	savedFiles = 0;
	videoPrerollReady = false;
	audioPrerollReady = false;
	saveInProgress = false;

	videoThread->start();
	audioThread->start();
}

void MainWindow::saveEvent()
{
	if (!ui->saveButton->isEnabled())
		return;

	if (!QDir().mkpath("/av-blackbox/events")) {
		ui->statusLabel->setText("Cannot create events directory");
		return;
	}

	QTemporaryDir eventDir("/av-blackbox/events/event-XXXXXX");
	if (!eventDir.isValid()) {
		ui->statusLabel->setText(eventDir.errorString());
		return;
	}
	eventDir.setAutoRemove(false);

	saveInProgress = true;
	ui->saveButton->setEnabled(false);
	ui->stopButton->setEnabled(false);
	ui->statusLabel->setText("Recording event...");
	videoThread->requestSave(eventDir.path() + "/video.yuyv");
	audioThread->requestSave(eventDir.path() + "/audio.pcm");
}

MainWindow::~MainWindow()
{
	impactThread->requestInterruption();
	videoThread->requestInterruption();
	audioThread->requestInterruption();

	impactThread->wait();
	videoThread->wait();
	audioThread->wait();

	delete ui;
}
