QT += gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test_overlay

SOURCES += \
    test_overlay.cpp \
    ../src/motion_engine.cpp \
    ../src/overlay_window.cpp \
    ../src/theme.cpp

HEADERS += \
    ../src/motion_engine.h \
    ../src/overlay_window.h \
    ../src/theme.h

macos {
    OBJECTIVE_SOURCES += ../src/overlay_window_mac.mm
    LIBS += -framework AppKit
}
win32: LIBS += -luser32

unix:contains(CONFIG, sanitize) {
    QMAKE_CXXFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}
