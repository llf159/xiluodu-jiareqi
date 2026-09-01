QT += core testlib

TARGET = controlalgorithm_test
TEMPLATE = app
CONFIG += console testcase c++11

INCLUDEPATH += ../src

SOURCES += \
    controlalgorithm_test.cpp \
    ../src/controlalgorithm.cpp

HEADERS += ../src/controlalgorithm.h
