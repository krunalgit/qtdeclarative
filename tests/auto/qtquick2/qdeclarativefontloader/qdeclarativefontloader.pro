CONFIG += testcase
TARGET = tst_qdeclarativefontloader
macx:CONFIG -= app_bundle

HEADERS += ../../shared/testhttpserver.h \
           ../../shared/util.h
SOURCES += tst_qdeclarativefontloader.cpp \
           ../../shared/testhttpserver.cpp \
           ../../shared/util.cpp

testDataFiles.files = data
testDataFiles.path = .
DEPLOYMENT += testDataFiles

CONFIG += parallel_test

QT += core-private gui-private declarative-private quick-private network testlib
