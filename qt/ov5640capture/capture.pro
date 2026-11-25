QT += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
	audiothread.cpp \
	impactthread.cpp \
	main.cpp \
	mainwindow.cpp \
	videothread.cpp

HEADERS += \
	audiothread.h \
	impactthread.h \
	mainwindow.h \
	videothread.h

isEmpty(ALSA_STAGING): ALSA_STAGING = /home/zero/linux/tool/alsa/staging/usr
INCLUDEPATH += $$ALSA_STAGING/include
INCLUDEPATH += $$PWD/../../include
LIBS += -L$$ALSA_STAGING/lib -lasound

FORMS += \
	mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
