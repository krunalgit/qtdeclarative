CONFIG += testcase
TARGET = tst_qquickvisualdatamodel
macx:CONFIG -= app_bundle

SOURCES += tst_qquickvisualdatamodel.cpp \
           ../../shared/util.cpp
HEADERS += ../../shared/util.h

testDataFiles.files = data
testDataFiles.path = .
DEPLOYMENT += testDataFiles

CONFIG += parallel_test

QT += core-private gui-private v8-private declarative-private quick-private widgets testlib
