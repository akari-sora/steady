QT -= gui
CONFIG += console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = steady-cli

include(src/core.pri)
SOURCES += src/cli.cpp

DESTDIR = .
OBJECTS_DIR = obj/cli
MOC_DIR = moc/cli
