QT += quick quickcontrols2
macos:contains(CONFIG, steady_bundle) {
    CONFIG += app_bundle
    QMAKE_TARGET_BUNDLE_PREFIX = dev.omarchy
    QMAKE_INFO_PLIST = $$PWD/pkgbuild/Info.plist
} else {
    CONFIG -= app_bundle
}

TARGET = steady
TEMPLATE = app

include(src/core.pri)

linux:!contains(CONFIG, no_layershell) {
    DEFINES += STEADY_LAYER_SHELL
    LIBS += -lLayerShellQtInterface
}
macos {
    OBJECTIVE_SOURCES += src/overlay_window_mac.mm
    LIBS += -framework AppKit
}
win32: LIBS += -luser32

SOURCES += src/main.cpp src/theme.cpp src/overlay_window.cpp
HEADERS += src/theme.h src/overlay_window.h
RESOURCES += src/resources.qrc

# Translations: zh-TW is the source language (all qsTr()/tr() strings).
# The English catalogue is embedded into the binary as :/i18n/steady_en.qm
# (Qt 6.10) and loaded by main() when the language setting asks for it.
TRANSLATIONS += translations/steady_en.ts
CONFIG += embed_translations

DESTDIR = .
OBJECTS_DIR = obj/gui
MOC_DIR = moc/gui
RCC_DIR = rcc
