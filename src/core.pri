QT += core network
win32: LIBS += -ladvapi32
CONFIG += c++17
VERSION = $$cat($$PWD/../version.txt)
DEFINES += STEADY_VERSION=\\\"$$VERSION\\\"
msvc: QMAKE_CXXFLAGS += /utf-8

INCLUDEPATH += $$PWD $$PWD/../third_party/qrcodegen
SOURCES += \
    $$PWD/app_paths.cpp \
    $$PWD/local_control.cpp \
    $$PWD/motion_engine.cpp \
    $$PWD/qr_matrix.cpp \
    $$PWD/sensor_server.cpp \
    $$PWD/../third_party/qrcodegen/qrcodegen.c

HEADERS += \
    $$PWD/app_paths.h \
    $$PWD/local_control.h \
    $$PWD/motion_engine.h \
    $$PWD/qr_matrix.h \
    $$PWD/sensor_server.h \
    $$PWD/../third_party/qrcodegen/qrcodegen.h
