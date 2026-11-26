QT -= gui
CONFIG += console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test_steady

include(../src/core.pri)
SOURCES += test_steady.cpp

unix:contains(CONFIG, sanitize) {
    QMAKE_CXXFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_LFLAGS += -fsanitize=address,undefined
}

RESOURCES += ../src/resources.qrc
