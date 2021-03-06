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
#include <qv4engine_p.h>
#include <qv4value_p.h>
#include <qv4object_p.h>
#include <qv4objectproto_p.h>
#include <qv4objectiterator_p.h>
#include <qv4arrayobject_p.h>
#include <qv4booleanobject_p.h>
#include <qv4globalobject_p.h>
#include <qv4errorobject_p.h>
#include <qv4functionobject_p.h>
#include "qv4function_p.h"
#include <qv4mathobject_p.h>
#include <qv4numberobject_p.h>
#include <qv4regexpobject_p.h>
#include <qv4variantobject_p.h>
#include <qv4runtime_p.h>
#include "qv4mm_p.h"
#include <qv4argumentsobject_p.h>
#include <qv4dateobject_p.h>
#include <qv4jsonobject_p.h>
#include <qv4stringobject_p.h>
#include <qv4identifiertable_p.h>
#include "qv4debugging_p.h"
#include "qv4executableallocator_p.h"
#include "qv4sequenceobject_p.h"
#include "qv4qobjectwrapper_p.h"
#include "qv4qmlextensions_p.h"

#ifdef V4_ENABLE_JIT
#include "qv4isel_masm_p.h"
#endif // V4_ENABLE_JIT

#include "qv4isel_moth_p.h"

#if USE(PTHREADS)
#  include <pthread.h>
#endif

QT_BEGIN_NAMESPACE

using namespace QV4;

static QBasicAtomicInt engineSerial = Q_BASIC_ATOMIC_INITIALIZER(1);

static ReturnedValue throwTypeError(CallContext *ctx)
{
    return ctx->throwTypeError();
}

quintptr getStackLimit()
{
    quintptr stackLimit;
#if USE(PTHREADS) && !OS(QNX)
#  if OS(DARWIN)
    pthread_t thread_self = pthread_self();
    void *stackTop = pthread_get_stackaddr_np(thread_self);
    stackLimit = reinterpret_cast<quintptr>(stackTop);
    quintptr size = 0;
    if (pthread_main_np()) {
        rlimit limit;
        getrlimit(RLIMIT_STACK, &limit);
        size = limit.rlim_cur;
    } else
        size = pthread_get_stacksize_np(thread_self);
    stackLimit -= size;
#  else
    void* stackBottom = 0;
    pthread_attr_t attr;
    pthread_getattr_np(pthread_self(), &attr);
    size_t stackSize = 0;
    pthread_attr_getstack(&attr, &stackBottom, &stackSize);
    pthread_attr_destroy(&attr);

    stackLimit = reinterpret_cast<quintptr>(stackBottom);
#  endif
// This is wrong. StackLimit is the currently committed stack size, not the real end.
// only way to get that limit is apparently by using VirtualQuery (Yuck)
//#elif OS(WINDOWS)
//    PNT_TIB tib = (PNT_TIB)NtCurrentTeb();
//    stackLimit = static_cast<quintptr>(tib->StackLimit);
#else
    int dummy;
    // this is inexact, as part of the stack is used when being called here,
    // but let's simply default to 1MB from where the stack is right now
    stackLimit = reinterpret_cast<qintptr>(&dummy) - 1024*1024;
#endif

    // 256k slack
    return stackLimit + 256*1024;
}


ExecutionEngine::ExecutionEngine(QQmlJS::EvalISelFactory *factory)
    : memoryManager(new QV4::MemoryManager)
    , executableAllocator(new QV4::ExecutableAllocator)
    , regExpAllocator(new QV4::ExecutableAllocator)
    , bumperPointerAllocator(new WTF::BumpPointerAllocator)
    , jsStack(new WTF::PageAllocation)
    , debugger(0)
    , globalObject(0)
    , globalCode(0)
    , m_engineId(engineSerial.fetchAndAddOrdered(1))
    , regExpCache(0)
    , m_multiplyWrappedQObjects(0)
    , m_qmlExtensions(0)
{
    MemoryManager::GCBlocker gcBlocker(memoryManager);

    exceptionValue = Encode::undefined();
    hasException = false;

    if (!factory) {

#ifdef V4_ENABLE_JIT
        static const bool forceMoth = !qgetenv("QV4_FORCE_INTERPRETER").isEmpty();
        if (forceMoth)
            factory = new QQmlJS::Moth::ISelFactory;
        else
            factory = new QQmlJS::MASM::ISelFactory;
#else // !V4_ENABLE_JIT
        factory = new QQmlJS::Moth::ISelFactory;
#endif // V4_ENABLE_JIT
    }
    iselFactory.reset(factory);

    memoryManager->setExecutionEngine(this);

    // reserve space for the JS stack
    // we allow it to grow to 2 times JSStackLimit, as we can overshoot due to garbage collection
    // and ScopedValues allocated outside of JIT'ed methods.
    *jsStack = WTF::PageAllocation::allocate(2*JSStackLimit, WTF::OSAllocator::JSVMStackPages, true);
    jsStackBase = (SafeValue *)jsStack->base();
    jsStackTop = jsStackBase;

    // set up stack limits
    jsStackLimit = jsStackBase + JSStackLimit/sizeof(SafeValue);
    cStackLimit = getStackLimit();

    Scope scope(this);

    identifierTable = new IdentifierTable(this);

    emptyClass =  new (classPool.allocate(sizeof(InternalClass))) InternalClass(this);

    id_undefined = newIdentifier(QStringLiteral("undefined"));
    id_null = newIdentifier(QStringLiteral("null"));
    id_true = newIdentifier(QStringLiteral("true"));
    id_false = newIdentifier(QStringLiteral("false"));
    id_boolean = newIdentifier(QStringLiteral("boolean"));
    id_number = newIdentifier(QStringLiteral("number"));
    id_string = newIdentifier(QStringLiteral("string"));
    id_object = newIdentifier(QStringLiteral("object"));
    id_function = newIdentifier(QStringLiteral("function"));
    id_length = newIdentifier(QStringLiteral("length"));
    id_prototype = newIdentifier(QStringLiteral("prototype"));
    id_constructor = newIdentifier(QStringLiteral("constructor"));
    id_arguments = newIdentifier(QStringLiteral("arguments"));
    id_caller = newIdentifier(QStringLiteral("caller"));
    id_callee = newIdentifier(QStringLiteral("callee"));
    id_this = newIdentifier(QStringLiteral("this"));
    id___proto__ = newIdentifier(QStringLiteral("__proto__"));
    id_enumerable = newIdentifier(QStringLiteral("enumerable"));
    id_configurable = newIdentifier(QStringLiteral("configurable"));
    id_writable = newIdentifier(QStringLiteral("writable"));
    id_value = newIdentifier(QStringLiteral("value"));
    id_get = newIdentifier(QStringLiteral("get"));
    id_set = newIdentifier(QStringLiteral("set"));
    id_eval = newIdentifier(QStringLiteral("eval"));
    id_uintMax = newIdentifier(QStringLiteral("4294967295"));
    id_name = newIdentifier(QStringLiteral("name"));
    id_index = newIdentifier(QStringLiteral("index"));
    id_input = newIdentifier(QStringLiteral("input"));
    id_toString = newIdentifier(QStringLiteral("toString"));
    id_valueOf = newIdentifier(QStringLiteral("valueOf"));

    ObjectPrototype *objectPrototype = new (memoryManager) ObjectPrototype(emptyClass);
    objectClass = emptyClass->changePrototype(objectPrototype);

    arrayClass = objectClass->addMember(id_length, Attr_NotConfigurable|Attr_NotEnumerable);
    ArrayPrototype *arrayPrototype = new (memoryManager) ArrayPrototype(arrayClass);
    arrayClass = arrayClass->changePrototype(arrayPrototype);

    InternalClass *argsClass = objectClass->addMember(id_length, Attr_NotEnumerable);
    argumentsObjectClass = argsClass->addMember(id_callee, Attr_Data|Attr_NotEnumerable);
    strictArgumentsObjectClass = argsClass->addMember(id_callee, Attr_Accessor|Attr_NotConfigurable|Attr_NotEnumerable);
    strictArgumentsObjectClass = strictArgumentsObjectClass->addMember(id_caller, Attr_Accessor|Attr_NotConfigurable|Attr_NotEnumerable);
    initRootContext();

    StringPrototype *stringPrototype = new (memoryManager) StringPrototype(objectClass);
    stringClass = emptyClass->changePrototype(stringPrototype);

    NumberPrototype *numberPrototype = new (memoryManager) NumberPrototype(objectClass);
    numberClass = emptyClass->changePrototype(numberPrototype);

    BooleanPrototype *booleanPrototype = new (memoryManager) BooleanPrototype(objectClass);
    booleanClass = emptyClass->changePrototype(booleanPrototype);

    DatePrototype *datePrototype = new (memoryManager) DatePrototype(objectClass);
    dateClass = emptyClass->changePrototype(datePrototype);

    FunctionPrototype *functionPrototype = new (memoryManager) FunctionPrototype(objectClass);
    functionClass = emptyClass->changePrototype(functionPrototype);
    uint index;
    functionWithProtoClass = functionClass->addMember(id_prototype, Attr_NotEnumerable|Attr_NotConfigurable, &index);
    Q_ASSERT(index == FunctionObject::Index_Prototype);
    protoClass = objectClass->addMember(id_constructor, Attr_NotEnumerable, &index);
    Q_ASSERT(index == FunctionObject::Index_ProtoConstructor);

    RegExpPrototype *regExpPrototype = new (memoryManager) RegExpPrototype(objectClass);
    regExpClass = emptyClass->changePrototype(regExpPrototype);
    regExpExecArrayClass = arrayClass->addMember(id_index, Attr_Data, &index);
    Q_ASSERT(index == RegExpObject::Index_ArrayIndex);
    regExpExecArrayClass = regExpExecArrayClass->addMember(id_input, Attr_Data, &index);
    Q_ASSERT(index == RegExpObject::Index_ArrayInput);

    ErrorPrototype *errorPrototype = new (memoryManager) ErrorPrototype(objectClass);
    errorClass = emptyClass->changePrototype(errorPrototype);
    EvalErrorPrototype *evalErrorPrototype = new (memoryManager) EvalErrorPrototype(errorClass);
    evalErrorClass = emptyClass->changePrototype(evalErrorPrototype);
    RangeErrorPrototype *rangeErrorPrototype = new (memoryManager) RangeErrorPrototype(errorClass);
    rangeErrorClass = emptyClass->changePrototype(rangeErrorPrototype);
    ReferenceErrorPrototype *referenceErrorPrototype = new (memoryManager) ReferenceErrorPrototype(errorClass);
    referenceErrorClass = emptyClass->changePrototype(referenceErrorPrototype);
    SyntaxErrorPrototype *syntaxErrorPrototype = new (memoryManager) SyntaxErrorPrototype(errorClass);
    syntaxErrorClass = emptyClass->changePrototype(syntaxErrorPrototype);
    TypeErrorPrototype *typeErrorPrototype = new (memoryManager) TypeErrorPrototype(errorClass);
    typeErrorClass = emptyClass->changePrototype(typeErrorPrototype);
    URIErrorPrototype *uRIErrorPrototype = new (memoryManager) URIErrorPrototype(errorClass);
    uriErrorClass = emptyClass->changePrototype(uRIErrorPrototype);

    VariantPrototype *variantPrototype = new (memoryManager) VariantPrototype(objectClass);
    variantClass = emptyClass->changePrototype(variantPrototype);

    SequencePrototype *sequencePrototype = new (memoryManager) SequencePrototype(arrayClass->changePrototype(arrayPrototype));
    sequenceClass = emptyClass->changePrototype(sequencePrototype);

    objectCtor = new (memoryManager) ObjectCtor(rootContext);
    stringCtor = new (memoryManager) StringCtor(rootContext);
    numberCtor = new (memoryManager) NumberCtor(rootContext);
    booleanCtor = new (memoryManager) BooleanCtor(rootContext);
    arrayCtor = new (memoryManager) ArrayCtor(rootContext);
    functionCtor = new (memoryManager) FunctionCtor(rootContext);
    dateCtor = new (memoryManager) DateCtor(rootContext);
    regExpCtor = new (memoryManager) RegExpCtor(rootContext);
    errorCtor = new (memoryManager) ErrorCtor(rootContext);
    evalErrorCtor = new (memoryManager) EvalErrorCtor(rootContext);
    rangeErrorCtor = new (memoryManager) RangeErrorCtor(rootContext);
    referenceErrorCtor = new (memoryManager) ReferenceErrorCtor(rootContext);
    syntaxErrorCtor = new (memoryManager) SyntaxErrorCtor(rootContext);
    typeErrorCtor = new (memoryManager) TypeErrorCtor(rootContext);
    uRIErrorCtor = new (memoryManager) URIErrorCtor(rootContext);

    objectPrototype->init(this, objectCtor);
    stringPrototype->init(this, stringCtor);
    numberPrototype->init(this, numberCtor);
    booleanPrototype->init(this, booleanCtor);
    arrayPrototype->init(this, arrayCtor);
    datePrototype->init(this, dateCtor);
    functionPrototype->init(this, functionCtor);
    regExpPrototype->init(this, regExpCtor);
    errorPrototype->init(this, errorCtor);
    evalErrorPrototype->init(this, evalErrorCtor);
    rangeErrorPrototype->init(this, rangeErrorCtor);
    referenceErrorPrototype->init(this, referenceErrorCtor);
    syntaxErrorPrototype->init(this, syntaxErrorCtor);
    typeErrorPrototype->init(this, typeErrorCtor);
    uRIErrorPrototype->init(this, uRIErrorCtor);

    variantPrototype->init();
    sequencePrototype->init();

    //
    // set up the global object
    //
    globalObject = newObject()->getPointer();
    rootContext->global = globalObject;
    rootContext->callData->thisObject = globalObject;

    globalObject->defineDefaultProperty(QStringLiteral("Object"), objectCtor);
    globalObject->defineDefaultProperty(QStringLiteral("String"), stringCtor);
    globalObject->defineDefaultProperty(QStringLiteral("Number"), numberCtor);
    globalObject->defineDefaultProperty(QStringLiteral("Boolean"), booleanCtor);
    globalObject->defineDefaultProperty(QStringLiteral("Array"), arrayCtor);
    globalObject->defineDefaultProperty(QStringLiteral("Function"), functionCtor);
    globalObject->defineDefaultProperty(QStringLiteral("Date"), dateCtor);
    globalObject->defineDefaultProperty(QStringLiteral("RegExp"), regExpCtor);
    globalObject->defineDefaultProperty(QStringLiteral("Error"), errorCtor);
    globalObject->defineDefaultProperty(QStringLiteral("EvalError"), evalErrorCtor);
    globalObject->defineDefaultProperty(QStringLiteral("RangeError"), rangeErrorCtor);
    globalObject->defineDefaultProperty(QStringLiteral("ReferenceError"), referenceErrorCtor);
    globalObject->defineDefaultProperty(QStringLiteral("SyntaxError"), syntaxErrorCtor);
    globalObject->defineDefaultProperty(QStringLiteral("TypeError"), typeErrorCtor);
    globalObject->defineDefaultProperty(QStringLiteral("URIError"), uRIErrorCtor);
    ScopedObject o(scope);
    globalObject->defineDefaultProperty(QStringLiteral("Math"), (o = new (memoryManager) MathObject(this)));
    globalObject->defineDefaultProperty(QStringLiteral("JSON"), (o = new (memoryManager) JsonObject(this)));

    globalObject->defineReadonlyProperty(QStringLiteral("undefined"), Primitive::undefinedValue());
    globalObject->defineReadonlyProperty(QStringLiteral("NaN"), Primitive::fromDouble(std::numeric_limits<double>::quiet_NaN()));
    globalObject->defineReadonlyProperty(QStringLiteral("Infinity"), Primitive::fromDouble(Q_INFINITY));

    evalFunction = new (memoryManager) EvalFunction(rootContext);
    globalObject->defineDefaultProperty(QStringLiteral("eval"), (o = evalFunction));

    globalObject->defineDefaultProperty(QStringLiteral("parseInt"), GlobalFunctions::method_parseInt, 2);
    globalObject->defineDefaultProperty(QStringLiteral("parseFloat"), GlobalFunctions::method_parseFloat, 1);
    globalObject->defineDefaultProperty(QStringLiteral("isNaN"), GlobalFunctions::method_isNaN, 1);
    globalObject->defineDefaultProperty(QStringLiteral("isFinite"), GlobalFunctions::method_isFinite, 1);
    globalObject->defineDefaultProperty(QStringLiteral("decodeURI"), GlobalFunctions::method_decodeURI, 1);
    globalObject->defineDefaultProperty(QStringLiteral("decodeURIComponent"), GlobalFunctions::method_decodeURIComponent, 1);
    globalObject->defineDefaultProperty(QStringLiteral("encodeURI"), GlobalFunctions::method_encodeURI, 1);
    globalObject->defineDefaultProperty(QStringLiteral("encodeURIComponent"), GlobalFunctions::method_encodeURIComponent, 1);
    globalObject->defineDefaultProperty(QStringLiteral("escape"), GlobalFunctions::method_escape, 1);
    globalObject->defineDefaultProperty(QStringLiteral("unescape"), GlobalFunctions::method_unescape, 1);

    Scoped<String> name(scope, newString(QStringLiteral("thrower")));
    thrower = newBuiltinFunction(rootContext, name, throwTypeError)->getPointer();
}

ExecutionEngine::~ExecutionEngine()
{
    delete debugger;
    delete m_multiplyWrappedQObjects;
    m_multiplyWrappedQObjects = 0;
    delete identifierTable;
    delete memoryManager;

    QSet<QV4::CompiledData::CompilationUnit*> remainingUnits;
    qSwap(compilationUnits, remainingUnits);
    foreach (QV4::CompiledData::CompilationUnit *unit, remainingUnits)
        unit->unlink();

    delete m_qmlExtensions;
    emptyClass->destroy();
    delete bumperPointerAllocator;
    delete regExpCache;
    delete regExpAllocator;
    delete executableAllocator;
    jsStack->deallocate();
    delete jsStack;
}

void ExecutionEngine::enableDebugger()
{
    Q_ASSERT(!debugger);
    debugger = new Debugging::Debugger(this);
    iselFactory.reset(new QQmlJS::Moth::ISelFactory);
}

void ExecutionEngine::initRootContext()
{
    rootContext = static_cast<GlobalContext *>(memoryManager->allocManaged(sizeof(GlobalContext) + sizeof(CallData)));
    rootContext->init();
    current = rootContext;
    current->parent = 0;
    rootContext->initGlobalContext(this);
}

InternalClass *ExecutionEngine::newClass(const InternalClass &other)
{
    return new (classPool.allocate(sizeof(InternalClass))) InternalClass(other);
}

ExecutionContext *ExecutionEngine::pushGlobalContext()
{
    GlobalContext *g = new (memoryManager) GlobalContext;
    ExecutionContext *oldNext = g->next;
    memcpy(g, rootContext, sizeof(GlobalContext));
    g->next = oldNext;
    g->parent = current;
    current = g;

    return current;
}

Returned<FunctionObject> *ExecutionEngine::newBuiltinFunction(ExecutionContext *scope, const StringRef name, ReturnedValue (*code)(CallContext *))
{
    BuiltinFunction *f = new (memoryManager) BuiltinFunction(scope, name, code);
    return f->asReturned<FunctionObject>();
}

Returned<BoundFunction> *ExecutionEngine::newBoundFunction(ExecutionContext *scope, FunctionObjectRef target, const ValueRef boundThis, const QVector<SafeValue> &boundArgs)
{
    Q_ASSERT(target);

    BoundFunction *f = new (memoryManager) BoundFunction(scope, target, boundThis, boundArgs);
    return f->asReturned<BoundFunction>();
}


Returned<Object> *ExecutionEngine::newObject()
{
    Object *object = new (memoryManager) Object(this);
    return object->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newObject(InternalClass *internalClass)
{
    Object *object = new (memoryManager) Object(internalClass);
    return object->asReturned<Object>();
}

Returned<String> *ExecutionEngine::newString(const QString &s)
{
    return (new (memoryManager) String(this, s))->asReturned<String>();
}

String *ExecutionEngine::newIdentifier(const QString &text)
{
    return identifierTable->insertString(text);
}

Returned<Object> *ExecutionEngine::newStringObject(const ValueRef value)
{
    StringObject *object = new (memoryManager) StringObject(this, value);
    return object->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newNumberObject(const ValueRef value)
{
    NumberObject *object = new (memoryManager) NumberObject(this, value);
    return object->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newBooleanObject(const ValueRef value)
{
    Object *object = new (memoryManager) BooleanObject(this, value);
    return object->asReturned<Object>();
}

Returned<ArrayObject> *ExecutionEngine::newArrayObject(int count)
{
    ArrayObject *object = new (memoryManager) ArrayObject(this);

    if (count) {
        if (count < 0x1000)
            object->arrayReserve(count);
        object->setArrayLengthUnchecked(count);
    }
    return object->asReturned<ArrayObject>();
}

Returned<ArrayObject> *ExecutionEngine::newArrayObject(const QStringList &list)
{
    ArrayObject *object = new (memoryManager) ArrayObject(this, list);
    return object->asReturned<ArrayObject>();
}

Returned<ArrayObject> *ExecutionEngine::newArrayObject(InternalClass *ic)
{
    ArrayObject *object = new (memoryManager) ArrayObject(ic);
    return object->asReturned<ArrayObject>();
}


Returned<DateObject> *ExecutionEngine::newDateObject(const ValueRef value)
{
    DateObject *object = new (memoryManager) DateObject(this, value);
    return object->asReturned<DateObject>();
}

Returned<DateObject> *ExecutionEngine::newDateObject(const QDateTime &dt)
{
    DateObject *object = new (memoryManager) DateObject(this, dt);
    return object->asReturned<DateObject>();
}

Returned<RegExpObject> *ExecutionEngine::newRegExpObject(const QString &pattern, int flags)
{
    bool global = (flags & QQmlJS::V4IR::RegExp::RegExp_Global);
    bool ignoreCase = false;
    bool multiline = false;
    if (flags & QQmlJS::V4IR::RegExp::RegExp_IgnoreCase)
        ignoreCase = true;
    if (flags & QQmlJS::V4IR::RegExp::RegExp_Multiline)
        multiline = true;

    Scope scope(this);
    Scoped<RegExp> re(scope, RegExp::create(this, pattern, ignoreCase, multiline));
    return newRegExpObject(re, global);
}

Returned<RegExpObject> *ExecutionEngine::newRegExpObject(Referenced<RegExp> re, bool global)
{
    RegExpObject *object = new (memoryManager) RegExpObject(this, re, global);
    return object->asReturned<RegExpObject>();
}

Returned<RegExpObject> *ExecutionEngine::newRegExpObject(const QRegExp &re)
{
    RegExpObject *object = new (memoryManager) RegExpObject(this, re);
    return object->asReturned<RegExpObject>();
}

Returned<Object> *ExecutionEngine::newErrorObject(const ValueRef value)
{
    ErrorObject *object = new (memoryManager) ErrorObject(errorClass, value);
    return object->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newSyntaxErrorObject(const QString &message)
{
    Scope scope(this);
    ScopedString s(scope, newString(message));
    Object *error = new (memoryManager) SyntaxErrorObject(this, s);
    return error->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newSyntaxErrorObject(const QString &message, const QString &fileName, int line, int column)
{
    Object *error = new (memoryManager) SyntaxErrorObject(this, message, fileName, line, column);
    return error->asReturned<Object>();
}


Returned<Object> *ExecutionEngine::newReferenceErrorObject(const QString &message)
{
    Object *o = new (memoryManager) ReferenceErrorObject(this, message);
    return o->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newReferenceErrorObject(const QString &message, const QString &fileName, int lineNumber, int columnNumber)
{
    Object *o = new (memoryManager) ReferenceErrorObject(this, message, fileName, lineNumber, columnNumber);
    return o->asReturned<Object>();
}


Returned<Object> *ExecutionEngine::newTypeErrorObject(const QString &message)
{
    Object *o = new (memoryManager) TypeErrorObject(this, message);
    return o->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newRangeErrorObject(const QString &message)
{
    Object *o = new (memoryManager) RangeErrorObject(this, message);
    return o->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newURIErrorObject(const ValueRef message)
{
    Object *o = new (memoryManager) URIErrorObject(this, message);
    return o->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newVariantObject(const QVariant &v)
{
    Object *o = new (memoryManager) VariantObject(this, v);
    return o->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::newForEachIteratorObject(ExecutionContext *ctx, const ObjectRef o)
{
    Object *obj = new (memoryManager) ForEachIteratorObject(ctx, o);
    return obj->asReturned<Object>();
}

Returned<Object> *ExecutionEngine::qmlContextObject() const
{
    ExecutionContext *ctx = current;

    if (ctx->type == QV4::ExecutionContext::Type_SimpleCallContext && !ctx->outer)
        ctx = ctx->parent;

    if (!ctx->outer)
        return 0;

    while (ctx->outer && ctx->outer->type != ExecutionContext::Type_GlobalContext)
        ctx = ctx->outer;

    Q_ASSERT(ctx);
    if (ctx->type != ExecutionContext::Type_QmlContext)
        return 0;

    return static_cast<CallContext *>(ctx)->activation->asReturned<Object>();
}

namespace {
    struct LineNumberResolver {
        const ExecutionEngine* engine;

        LineNumberResolver(const ExecutionEngine *engine)
            : engine(engine)
        {
        }

        void resolve(StackFrame *frame, ExecutionContext *context, Function *function)
        {
            qptrdiff offset;
            if (context->interpreterInstructionPointer) {
                offset = *context->interpreterInstructionPointer - 1 - function->codeData;
                frame->line = function->lineNumberForProgramCounter(offset);
            } else {
                frame->line = context->lineNumber;
            }
        }
    };
}

QVector<StackFrame> ExecutionEngine::stackTrace(int frameLimit) const
{
    LineNumberResolver lineNumbers(this);

    QVector<StackFrame> stack;

    QV4::ExecutionContext *c = current;
    while (c && frameLimit) {
        CallContext *callCtx = c->asCallContext();
        if (callCtx && callCtx->function) {
            StackFrame frame;
            if (callCtx->function->function)
                frame.source = callCtx->function->function->sourceFile();
            frame.function = callCtx->function->name->toQString();
            frame.line = -1;
            frame.column = -1;

            if (callCtx->function->function)
                lineNumbers.resolve(&frame, callCtx, callCtx->function->function);

            stack.append(frame);
            --frameLimit;
        }
        c = c->parent;
    }

    if (frameLimit && globalCode) {
        StackFrame frame;
        frame.source = globalCode->sourceFile();
        frame.function = globalCode->name->toQString();
        frame.line = -1;
        frame.column = -1;

        lineNumbers.resolve(&frame, rootContext, globalCode);

        stack.append(frame);
    }
    return stack;
}

StackFrame ExecutionEngine::currentStackFrame() const
{
    StackFrame frame;
    frame.line = -1;
    frame.column = -1;

    QVector<StackFrame> trace = stackTrace(/*limit*/ 1);
    if (!trace.isEmpty())
        frame = trace.first();

    return frame;
}

QUrl ExecutionEngine::resolvedUrl(const QString &file)
{
    QUrl src(file);
    if (!src.isRelative())
        return src;

    QUrl base;
    QV4::ExecutionContext *c = current;
    while (c) {
        CallContext *callCtx = c->asCallContext();
        if (callCtx && callCtx->function) {
            if (callCtx->function->function)
                base.setUrl(callCtx->function->function->sourceFile());
            break;
        }
        c = c->parent;
    }

    if (base.isEmpty() && globalCode)
        base.setUrl(globalCode->sourceFile());

    if (base.isEmpty())
        return src;

    return base.resolved(src);
}

void ExecutionEngine::requireArgumentsAccessors(int n)
{
    if (n <= argumentsAccessors.size())
        return;

    Scope scope(this);
    ScopedFunctionObject get(scope);
    ScopedFunctionObject set(scope);

    uint oldSize = argumentsAccessors.size();
    argumentsAccessors.resize(n);
    for (int i = oldSize; i < n; ++i) {
        get = new (memoryManager) ArgumentsGetterFunction(rootContext, i);
        set = new (memoryManager) ArgumentsSetterFunction(rootContext, i);
        Property pd = Property::fromAccessor(get.getPointer(), set.getPointer());
        argumentsAccessors[i] = pd;
    }
}

void ExecutionEngine::markObjects()
{
    identifierTable->mark(this);

    globalObject->mark(this);

    if (globalCode)
        globalCode->mark(this);

    for (int i = 0; i < argumentsAccessors.size(); ++i) {
        const Property &pd = argumentsAccessors.at(i);
        if (FunctionObject *getter = pd.getter())
            getter->mark(this);
        if (FunctionObject *setter = pd.setter())
            setter->mark(this);
    }

    ExecutionContext *c = current;
    while (c) {
        c->mark(this);
        c = c->parent;
    }

    id_length->mark(this);
    id_prototype->mark(this);
    id_constructor->mark(this);
    id_arguments->mark(this);
    id_caller->mark(this);
    id_this->mark(this);
    id___proto__->mark(this);
    id_enumerable->mark(this);
    id_configurable->mark(this);
    id_writable->mark(this);
    id_value->mark(this);
    id_get->mark(this);
    id_set->mark(this);
    id_eval->mark(this);
    id_uintMax->mark(this);
    id_name->mark(this);
    id_index->mark(this);
    id_input->mark(this);
    id_toString->mark(this);
    id_valueOf->mark(this);

    objectCtor.mark(this);
    stringCtor.mark(this);
    numberCtor.mark(this);
    booleanCtor.mark(this);
    arrayCtor.mark(this);
    functionCtor.mark(this);
    dateCtor.mark(this);
    regExpCtor.mark(this);
    errorCtor.mark(this);
    evalErrorCtor.mark(this);
    rangeErrorCtor.mark(this);
    referenceErrorCtor.mark(this);
    syntaxErrorCtor.mark(this);
    typeErrorCtor.mark(this);
    uRIErrorCtor.mark(this);

    exceptionValue.mark(this);

    thrower->mark(this);

    if (m_qmlExtensions)
        m_qmlExtensions->markObjects(this);

    emptyClass->markObjects();

    for (QSet<CompiledData::CompilationUnit*>::ConstIterator it = compilationUnits.constBegin(), end = compilationUnits.constEnd();
         it != end; ++it)
        (*it)->markObjects(this);
}

namespace {
    struct FindHelper
    {
        bool operator()(Function *function, quintptr pc)
        {
            return reinterpret_cast<quintptr>(function->codePtr) < pc
                   && (reinterpret_cast<quintptr>(function->codePtr) + function->codeSize) < pc;
        }

        bool operator()(quintptr pc, Function *function)
        {
            return pc < reinterpret_cast<quintptr>(function->codePtr);
        }
    };
}

Function *ExecutionEngine::functionForProgramCounter(quintptr pc) const
{
    // ### Use this code path instead of the "else" when the number of compilation units went down to
    // one per (qml) file.
#if 0
    for (QSet<QV4::CompiledData::CompilationUnit*>::ConstIterator unitIt = compilationUnits.constBegin(), unitEnd = compilationUnits.constEnd();
         unitIt != unitEnd; ++unitIt) {
        const QVector<Function*> &functions = (*unitIt)->runtimeFunctionsSortedByAddress;
        QVector<Function*>::ConstIterator it = qBinaryFind(functions.constBegin(),
                                                           functions.constEnd(),
                                                           pc, FindHelper());
        if (it != functions.constEnd())
            return *it;
    }
    return 0;
#else
    QMap<quintptr, Function*>::ConstIterator it = allFunctions.lowerBound(pc);
    if (it != allFunctions.begin() && allFunctions.count() > 0)
        --it;
    if (it == allFunctions.end())
        return 0;

    if (pc < it.key() || pc >= it.key() + (*it)->codeSize)
        return 0;
    return *it;
#endif
}

QmlExtensions *ExecutionEngine::qmlExtensions()
{
    if (!m_qmlExtensions)
        m_qmlExtensions = new QmlExtensions;
    return m_qmlExtensions;
}

ReturnedValue ExecutionEngine::throwException(const ValueRef value)
{
    // we can get in here with an exception already set, as the runtime
    // doesn't check after every operation that can throw.
    // in this case preserve the first exception to give correct error
    // information
    if (hasException)
        return Encode::undefined();

    hasException = true;
    exceptionValue = value;
    QV4::Scope scope(this);
    QV4::Scoped<ErrorObject> error(scope, value);
    if (!!error)
        exceptionStackTrace = error->stackTrace;
    else
        exceptionStackTrace = stackTrace();

    if (debugger)
        debugger->aboutToThrow();

    return Encode::undefined();
}

ReturnedValue ExecutionEngine::catchException(ExecutionContext *catchingContext, StackTrace *trace)
{
    Q_ASSERT(hasException);
    while (current != catchingContext)
        popContext();
    if (trace)
        *trace = exceptionStackTrace;
    exceptionStackTrace.clear();
    hasException = false;
    ReturnedValue res = exceptionValue.asReturnedValue();
    exceptionValue = Primitive::emptyValue();
    return res;
}

QQmlError ExecutionEngine::catchExceptionAsQmlError(ExecutionContext *context)
{
    QV4::StackTrace trace;
    QV4::Scope scope(context);
    QV4::ScopedValue exception(scope, context->catchException(&trace));
    QQmlError error;
    if (!trace.isEmpty()) {
        QV4::StackFrame frame = trace.first();
        error.setUrl(QUrl(frame.source));
        error.setLine(frame.line);
        error.setColumn(frame.column);
    }
    QV4::Scoped<QV4::ErrorObject> errorObj(scope, exception);
    if (!!errorObj && errorObj->asSyntaxError()) {
        QV4::ScopedString m(scope, errorObj->engine()->newString(QStringLiteral("message")));
        QV4::ScopedValue v(scope, errorObj->get(m));
        error.setDescription(v->toQStringNoThrow());
    } else
        error.setDescription(exception->toQStringNoThrow());
    return error;
}

bool ExecutionEngine::recheckCStackLimits()
{
    int dummy;
#ifdef Q_OS_WIN
    // ### this is only required on windows, where we currently use heuristics to get the stack limit
    if (cStackLimit - reinterpret_cast<quintptr>(&dummy) > 128*1024)
        // we're more then 128k away from our stack limit, assume the thread has changed, and
        // call getStackLimit
#endif
    // this can happen after a thread change
    cStackLimit = getStackLimit();

    return (reinterpret_cast<quintptr>(&dummy) >= cStackLimit);
}

QT_END_NAMESPACE
