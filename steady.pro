QT += quick quickcontrols2 network
CONFIG += c++17

TARGET = steady
TEMPLATE = app

LIBS += -lLayerShellQtInterface

SOURCES += \
    src/main.cpp \
    src/theme.cpp \
    src/motion_engine.cpp \
    src/overlay_window.cpp \
    src/sensor_server.cpp

HEADERS += \
    src/theme.h \
    src/motion_engine.h \
    src/overlay_window.h \
    src/sensor_server.h

RESOURCES += src/resources.qrc

DESTDIR = .
OBJECTS_DIR = obj
MOC_DIR = moc
RCC_DIR = rcc
