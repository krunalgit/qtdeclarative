/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QQMLVME_P_H
#define QQMLVME_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qqmlerror.h"
#include <private/qbitfield_p.h>
#include "qqmlinstruction_p.h"
#include <private/qrecursionwatcher_p.h>

#include <QtCore/QStack>
#include <QtCore/QString>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qtypeinfo.h>

#include <private/qqmlengine_p.h>
#include <private/qfinitestack_p.h>

#include <private/qqmltrace_p.h>

QT_BEGIN_NAMESPACE

class QObject;
class QJSValue;
class QQmlScriptData;
class QQmlCompiledData;
class QQmlContextData;

namespace QQmlVMETypes {
    struct List
    {
        List() : type(0) {}
        List(int t) : type(t) {}

        int type;
        QQmlListProperty<void> qListProperty;
    };
    struct State {
        enum Flag { Deferred = 0x00000001 };

        State() : flags(0), context(0), compiledData(0), instructionStream(0) {}
        quint32 flags;
        QQmlContextData *context;
        QQmlCompiledData *compiledData;
        const char *instructionStream;
        QBitField bindingSkipList;
    };
}
Q_DECLARE_TYPEINFO(QQmlVMETypes::List, Q_PRIMITIVE_TYPE  | Q_MOVABLE_TYPE);
template<>
class QTypeInfo<QQmlVMETypes::State> : public QTypeInfoMerger<QQmlVMETypes::State, QBitField> {}; //Q_DECLARE_TYPEINFO

class Q_QML_PRIVATE_EXPORT QQmlVME
{
    Q_DECLARE_TR_FUNCTIONS(QQmlVME)
public:
    class Interrupt {
    public:
        inline Interrupt();
        inline Interrupt(volatile bool *runWhile, int nsecs=0);
        inline Interrupt(int nsecs);

        inline void reset();
        inline bool shouldInterrupt() const;
    private:
        enum Mode { None, Time, Flag };
        Mode mode;
        struct {
            QElapsedTimer timer;
            int nsecs;
        };
        volatile bool *runWhile;
    };

    QQmlVME() : data(0), componentAttached(0) {}
    QQmlVME(void *data) : data(data), componentAttached(0) {}

    void *data;
    QQmlComponentAttached *componentAttached;
    QList<QQmlEnginePrivate::FinalizeCallback> finalizeCallbacks;

    void init(QQmlContextData *, QQmlCompiledData *, int start,
              QQmlContextData * = 0);
    bool initDeferred(QObject *);
    void reset();

    QObject *execute(QList<QQmlError> *errors, const Interrupt & = Interrupt());
    QQmlContextData *complete(const Interrupt & = Interrupt());

    static void enableComponentComplete();
    static void disableComponentComplete();
    static bool componentCompleteEnabled();

private:
    friend class QQmlVMEGuard;

    QObject *run(QList<QQmlError> *errors, const Interrupt &
#ifdef QML_THREADED_VME_INTERPRETER
                 , void *const**storeJumpTable = 0
#endif
                );

#ifdef QML_THREADED_VME_INTERPRETER
    static void *const*instructionJumpTable();
    friend class QQmlCompiledData;
#endif

    QQmlEngine *engine;
    QRecursionNode recursion;

#ifdef QML_ENABLE_TRACE
    QQmlCompiledData *rootComponent;
#endif

    QFiniteStack<QObject *> objects;
    QFiniteStack<QQmlVMETypes::List> lists;

    QFiniteStack<QQmlAbstractBinding *> bindValues;
    QFiniteStack<QQmlParserStatus *> parserStatus;
#ifdef QML_ENABLE_TRACE
    QFiniteStack<QQmlData *> parserStatusData;
#endif

    QQmlGuardedContextData rootContext;
    QQmlGuardedContextData creationContext;

    typedef QQmlVMETypes::State State;
    QStack<State> states;

    static void blank(QFiniteStack<QQmlParserStatus *> &);
    static void blank(QFiniteStack<QQmlAbstractBinding *> &);

    static bool s_enableComponentComplete;
};

// Used to check that a QQmlVME that is interrupted mid-execution
// is still valid.  Checks all the objects and contexts have not been 
// deleted.
class QQmlVMEGuard
{
public:
    QQmlVMEGuard();
    ~QQmlVMEGuard();

    void guard(QQmlVME *);
    void clear();

    bool isOK() const;

private:
    int m_objectCount;
    QPointer<QObject> *m_objects;
    int m_contextCount;
    QQmlGuardedContextData *m_contexts;
};

QQmlVME::Interrupt::Interrupt()
    : mode(None), nsecs(0), runWhile(0)
{
}

QQmlVME::Interrupt::Interrupt(volatile bool *runWhile, int nsecs)
    : mode(Flag), nsecs(nsecs), runWhile(runWhile)
{
}

QQmlVME::Interrupt::Interrupt(int nsecs)
    : mode(Time), nsecs(nsecs), runWhile(0)
{
}

void QQmlVME::Interrupt::reset()
{
    if (mode == Time || nsecs)
        timer.start();
}

bool QQmlVME::Interrupt::shouldInterrupt() const
{
    if (mode == None) {
        return false;
    } else if (mode == Time) {
        return timer.nsecsElapsed() > nsecs;
    } else if (mode == Flag) {
        return !*runWhile || (nsecs && timer.nsecsElapsed() > nsecs);
    } else {
        return false;
    }
}

QT_END_NAMESPACE

#endif // QQMLVME_P_H
