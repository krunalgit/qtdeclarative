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

#include "qv4jsir_p.h"
#include <private/qqmljsast_p.h>

#include <private/qqmlpropertycache_p.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qdebug.h>
#include <QtCore/qset.h>
#include <cmath>
#include <cassert>

#ifdef CONST
#undef CONST
#endif

QT_BEGIN_NAMESPACE

namespace QQmlJS {
namespace V4IR {

QString typeName(Type t)
{
    switch (t) {
    case UnknownType: return QStringLiteral("");
    case MissingType: return QStringLiteral("missing");
    case UndefinedType: return QStringLiteral("undefined");
    case NullType: return QStringLiteral("null");
    case BoolType: return QStringLiteral("bool");
    case UInt32Type: return QStringLiteral("uint32");
    case SInt32Type: return QStringLiteral("int32");
    case DoubleType: return QStringLiteral("double");
    case NumberType: return QStringLiteral("number");
    case StringType: return QStringLiteral("string");
    case VarType: return QStringLiteral("var");
    case QObjectType: return QStringLiteral("qobject");
    default: return QStringLiteral("multiple");
    }
}

const char *opname(AluOp op)
{
    switch (op) {
    case OpInvalid: return "?";

    case OpIfTrue: return "(bool)";
    case OpNot: return "!";
    case OpUMinus: return "-";
    case OpUPlus: return "+";
    case OpCompl: return "~";
    case OpIncrement: return "++";
    case OpDecrement: return "--";

    case OpBitAnd: return "&";
    case OpBitOr: return "|";
    case OpBitXor: return "^";

    case OpAdd: return "+";
    case OpSub: return "-";
    case OpMul: return "*";
    case OpDiv: return "/";
    case OpMod: return "%";

    case OpLShift: return "<<";
    case OpRShift: return ">>";
    case OpURShift: return ">>>";

    case OpGt: return ">";
    case OpLt: return "<";
    case OpGe: return ">=";
    case OpLe: return "<=";
    case OpEqual: return "==";
    case OpNotEqual: return "!=";
    case OpStrictEqual: return "===";
    case OpStrictNotEqual: return "!==";

    case OpInstanceof: return "instanceof";
    case OpIn: return "in";

    case OpAnd: return "&&";
    case OpOr: return "||";

    default: return "?";

    } // switch
}

AluOp binaryOperator(int op)
{
    switch (static_cast<QSOperator::Op>(op)) {
    case QSOperator::Add: return OpAdd;
    case QSOperator::And: return OpAnd;
    case QSOperator::BitAnd: return OpBitAnd;
    case QSOperator::BitOr: return OpBitOr;
    case QSOperator::BitXor: return OpBitXor;
    case QSOperator::Div: return OpDiv;
    case QSOperator::Equal: return OpEqual;
    case QSOperator::Ge: return OpGe;
    case QSOperator::Gt: return OpGt;
    case QSOperator::Le: return OpLe;
    case QSOperator::LShift: return OpLShift;
    case QSOperator::Lt: return OpLt;
    case QSOperator::Mod: return OpMod;
    case QSOperator::Mul: return OpMul;
    case QSOperator::NotEqual: return OpNotEqual;
    case QSOperator::Or: return OpOr;
    case QSOperator::RShift: return OpRShift;
    case QSOperator::StrictEqual: return OpStrictEqual;
    case QSOperator::StrictNotEqual: return OpStrictNotEqual;
    case QSOperator::Sub: return OpSub;
    case QSOperator::URShift: return OpURShift;
    case QSOperator::InstanceOf: return OpInstanceof;
    case QSOperator::In: return OpIn;
    default: return OpInvalid;
    }
}

struct RemoveSharedExpressions: V4IR::StmtVisitor, V4IR::ExprVisitor
{
    CloneExpr clone;
    QSet<Expr *> subexpressions; // contains all the non-cloned subexpressions in the given function
    Expr *uniqueExpr;

    RemoveSharedExpressions(): uniqueExpr(0) {}

    void operator()(V4IR::Function *function)
    {
        subexpressions.clear();

        foreach (BasicBlock *block, function->basicBlocks) {
            clone.setBasicBlock(block);

            foreach (Stmt *s, block->statements) {
                s->accept(this);
            }
        }
    }

    template <typename _Expr>
    _Expr *cleanup(_Expr *expr)
    {
        if (subexpressions.contains(expr)) {
             // the cloned expression is unique by definition
            // so we don't need to add it to `subexpressions'.
            return clone(expr);
        }

        subexpressions.insert(expr);
        V4IR::Expr *e = expr;
        qSwap(uniqueExpr, e);
        expr->accept(this);
        qSwap(uniqueExpr, e);
        return static_cast<_Expr *>(e);
    }

    // statements
    virtual void visitExp(Exp *s)
    {
        s->expr = cleanup(s->expr);
    }

    virtual void visitMove(Move *s)
    {
        s->target = cleanup(s->target);
        s->source = cleanup(s->source);
    }

    virtual void visitJump(Jump *)
    {
        // nothing to do for Jump statements
    }

    virtual void visitCJump(CJump *s)
    {
        s->cond = cleanup(s->cond);
    }

    virtual void visitRet(Ret *s)
    {
        s->expr = cleanup(s->expr);
    }

    virtual void visitPhi(V4IR::Phi *) { Q_UNIMPLEMENTED(); }

    // expressions
    virtual void visitConst(Const *) {}
    virtual void visitString(String *) {}
    virtual void visitRegExp(RegExp *) {}
    virtual void visitName(Name *) {}
    virtual void visitTemp(Temp *) {}
    virtual void visitClosure(Closure *) {}

    virtual void visitConvert(Convert *e)
    {
        e->expr = cleanup(e->expr);
    }

    virtual void visitUnop(Unop *e)
    {
        e->expr = cleanup(e->expr);
    }

    virtual void visitBinop(Binop *e)
    {
        e->left = cleanup(e->left);
        e->right = cleanup(e->right);
    }

    virtual void visitCall(Call *e)
    {
        e->base = cleanup(e->base);
        for (V4IR::ExprList *it = e->args; it; it = it->next)
            it->expr = cleanup(it->expr);
    }

    virtual void visitNew(New *e)
    {
        e->base = cleanup(e->base);
        for (V4IR::ExprList *it = e->args; it; it = it->next)
            it->expr = cleanup(it->expr);
    }

    virtual void visitSubscript(Subscript *e)
    {
        e->base = cleanup(e->base);
        e->index = cleanup(e->index);
    }

    virtual void visitMember(Member *e)
    {
        e->base = cleanup(e->base);
    }
};

static QString dumpStart(const Expr *e) {
    if (e->type == UnknownType)
//        return QStringLiteral("**UNKNOWN**");
        return QString();

    QString result = typeName(e->type);
    const Temp *temp = const_cast<Expr*>(e)->asTemp();
    if (e->type == QObjectType && temp && temp->memberResolver.data) {
        result += QLatin1Char('<');
        result += QString::fromUtf8(static_cast<QQmlPropertyCache*>(temp->memberResolver.data)->className());
        result += QLatin1Char('>');
    }
    result += QLatin1Char('{');
    return result;
}

static const char *dumpEnd(const Expr *e) {
    if (e->type == UnknownType)
        return "";
    else
        return "}";
}

void Const::dump(QTextStream &out) const
{
    if (type != UndefinedType && type != NullType)
        out << dumpStart(this);
    switch (type) {
    case QQmlJS::V4IR::UndefinedType:
        out << "undefined";
        break;
    case QQmlJS::V4IR::NullType:
        out << "null";
        break;
    case QQmlJS::V4IR::BoolType:
        out << (value ? "true" : "false");
        break;
    case QQmlJS::V4IR::MissingType:
        out << "missing";
        break;
    default:
        if (int(value) == 0 && int(value) == value) {
            if (isNegative(value))
                out << "-0";
            else
                out << "0";
        } else {
            out << QString::number(value, 'g', 16);
        }
        break;
    }
    if (type != UndefinedType && type != NullType)
        out << dumpEnd(this);
}

void String::dump(QTextStream &out) const
{
    out << '"' << escape(*value) << '"';
}

QString String::escape(const QString &s)
{
    QString r;
    for (int i = 0; i < s.length(); ++i) {
        const QChar ch = s.at(i);
        if (ch == QLatin1Char('\n'))
            r += QStringLiteral("\\n");
        else if (ch == QLatin1Char('\r'))
            r += QStringLiteral("\\r");
        else if (ch == QLatin1Char('\\'))
            r += QStringLiteral("\\\\");
        else if (ch == QLatin1Char('"'))
            r += QStringLiteral("\\\"");
        else if (ch == QLatin1Char('\''))
            r += QStringLiteral("\\'");
        else
            r += ch;
    }
    return r;
}

void RegExp::dump(QTextStream &out) const
{
    char f[3];
    int i = 0;
    if (flags & RegExp_Global)
        f[i++] = 'g';
    if (flags & RegExp_IgnoreCase)
        f[i++] = 'i';
    if (flags & RegExp_Multiline)
        f[i++] = 'm';
    f[i] = 0;

    out << '/' << *value << '/' << f;
}

void Name::initGlobal(const QString *id, quint32 line, quint32 column)
{
    this->id = id;
    this->builtin = builtin_invalid;
    this->global = true;
    this->qmlSingleton = false;
    this->freeOfSideEffects = false;
    this->line = line;
    this->column = column;
}

void Name::init(const QString *id, quint32 line, quint32 column)
{
    this->id = id;
    this->builtin = builtin_invalid;
    this->global = false;
    this->qmlSingleton = false;
    this->freeOfSideEffects = false;
    this->line = line;
    this->column = column;
}

void Name::init(Builtin builtin, quint32 line, quint32 column)
{
    this->id = 0;
    this->builtin = builtin;
    this->global = false;
    this->qmlSingleton = false;
    this->freeOfSideEffects = false;
    this->line = line;
    this->column = column;
}

static const char *builtin_to_string(Name::Builtin b)
{
    switch (b) {
    case Name::builtin_invalid:
        return "builtin_invalid";
    case Name::builtin_typeof:
        return "builtin_typeof";
    case Name::builtin_delete:
        return "builtin_delete";
    case Name::builtin_throw:
        return "builtin_throw";
    case Name::builtin_rethrow:
        return "builtin_rethrow";
    case Name::builtin_unwind_exception:
        return "builtin_unwind_exception";
    case Name::builtin_push_catch_scope:
        return "builtin_push_catch_scope";
    case V4IR::Name::builtin_foreach_iterator_object:
        return "builtin_foreach_iterator_object";
    case V4IR::Name::builtin_foreach_next_property_name:
        return "builtin_foreach_next_property_name";
    case V4IR::Name::builtin_push_with_scope:
        return "builtin_push_with_scope";
    case V4IR::Name::builtin_pop_scope:
        return "builtin_pop_scope";
    case V4IR::Name::builtin_declare_vars:
        return "builtin_declare_vars";
    case V4IR::Name::builtin_define_property:
        return "builtin_define_property";
    case V4IR::Name::builtin_define_array:
        return "builtin_define_array";
    case V4IR::Name::builtin_define_getter_setter:
        return "builtin_define_getter_setter";
    case V4IR::Name::builtin_define_object_literal:
        return "builtin_define_object_literal";
    case V4IR::Name::builtin_setup_argument_object:
        return "builtin_setup_argument_object";
    case V4IR::Name::builtin_convert_this_to_object:
        return "builtin_convert_this_to_object";
    case V4IR::Name::builtin_qml_id_array:
        return "builtin_qml_id_array";
    case V4IR::Name::builtin_qml_imported_scripts_object:
        return "builtin_qml_imported_scripts_object";
    case V4IR::Name::builtin_qml_scope_object:
        return "builtin_qml_scope_object";
    case V4IR::Name::builtin_qml_context_object:
        return "builtin_qml_context_object";
    }
    return "builtin_(###FIXME)";
};

void Name::dump(QTextStream &out) const
{
    if (id)
        out << *id;
    else
        out << builtin_to_string(builtin);
}

void Temp::dump(QTextStream &out) const
{
    out << dumpStart(this);
    switch (kind) {
    case Formal:           out << '#' << index; break;
    case ScopedFormal:     out << '#' << index
                               << '@' << scope; break;
    case Local:            out << '$' << index; break;
    case ScopedLocal:      out << '$' << index
                               << '@' << scope; break;
    case VirtualRegister:  out << '%' << index; break;
    case PhysicalRegister: out << (type == DoubleType ? "fp" : "r")
                               << index; break;
    case StackSlot:        out << '&' << index; break;
    default:               out << "INVALID";
    }
    out << dumpEnd(this);
}

bool operator<(const Temp &t1, const Temp &t2) Q_DECL_NOTHROW
{
    if (t1.kind < t2.kind) return true;
    if (t1.kind > t2.kind) return false;
    if (t1.index < t2.index) return true;
    if (t1.index > t2.index) return false;
    return t1.scope < t2.scope;
}

void Closure::dump(QTextStream &out) const
{
    QString name = functionName ? *functionName : QString();
    if (name.isEmpty())
        name.sprintf("%x", value);
    out << "closure(" << name << ')';
}

void Convert::dump(QTextStream &out) const
{
    out << dumpStart(this);
    out << "convert(";
    expr->dump(out);
    out << ')' << dumpEnd(this);
}

void Unop::dump(QTextStream &out) const
{
    out << dumpStart(this) << opname(op);
    expr->dump(out);
    out << dumpEnd(this);
}

void Binop::dump(QTextStream &out) const
{
    out << dumpStart(this);
    left->dump(out);
    out << ' ' << opname(op) << ' ';
    right->dump(out);
    out << dumpEnd(this);
}

void Call::dump(QTextStream &out) const
{
    base->dump(out);
    out << '(';
    for (ExprList *it = args; it; it = it->next) {
        if (it != args)
            out << ", ";
        it->expr->dump(out);
    }
    out << ')';
}

void New::dump(QTextStream &out) const
{
    out << "new ";
    base->dump(out);
    out << '(';
    for (ExprList *it = args; it; it = it->next) {
        if (it != args)
            out << ", ";
        it->expr->dump(out);
    }
    out << ')';
}

void Subscript::dump(QTextStream &out) const
{
    base->dump(out);
    out << '[';
    index->dump(out);
    out << ']';
}

void Member::dump(QTextStream &out) const
{
    base->dump(out);
    out << '.' << *name;
    if (property)
        out << " (meta-property " << property->coreIndex << " <" << QMetaType::typeName(property->propType) << ">)";
}

void Exp::dump(QTextStream &out, Mode)
{
    out << "(void) ";
    expr->dump(out);
    out << ';';
}

void Move::dump(QTextStream &out, Mode mode)
{
    Q_UNUSED(mode);

    target->dump(out);
    out << ' ';
    if (swap)
        out << "<=> ";
    else
        out << "= ";
//    if (source->type != target->type)
//        out << typeName(source->type) << "_to_" << typeName(target->type) << '(';
    source->dump(out);
//    if (source->type != target->type)
//        out << ')';
    out << ';';
}

void Jump::dump(QTextStream &out, Mode mode)
{
    Q_UNUSED(mode);
    out << "goto " << 'L' << target->index << ';';
}

void CJump::dump(QTextStream &out, Mode mode)
{
    Q_UNUSED(mode);
    out << "if (";
    cond->dump(out);
    if (mode == HIR)
        out << ") goto " << 'L' << iftrue->index << "; else goto " << 'L' << iffalse->index << ';';
    else
        out << ") goto " << 'L' << iftrue->index << ";";
}

void Ret::dump(QTextStream &out, Mode)
{
    out << "return";
    if (expr) {
        out << ' ';
        expr->dump(out);
    }
    out << ';';
}

void Phi::dump(QTextStream &out, Stmt::Mode mode)
{
    Q_UNUSED(mode);

    targetTemp->dump(out);
    out << " = phi(";
    for (int i = 0, ei = d->incoming.size(); i < ei; ++i) {
        if (i > 0)
            out << ", ";
        if (d->incoming[i])
            d->incoming[i]->dump(out);
    }
    out << ");";
}

Function *Module::newFunction(const QString &name, Function *outer)
{
    Function *f = new Function(this, outer, name);
    functions.append(f);
    if (!outer) {
        if (!isQmlModule) {
            assert(!rootFunction);
            rootFunction = f;
        }
    } else {
        outer->nestedFunctions.append(f);
    }
    return f;
}

Module::~Module()
{
    qDeleteAll(functions);
}

void Module::setFileName(const QString &name)
{
    if (fileName.isEmpty())
        fileName = name;
    else {
        Q_ASSERT(fileName == name);
    }
}

Function::~Function()
{
    // destroy the Stmt::Data blocks manually, because memory pool cleanup won't
    // call the Stmt destructors.
    foreach (V4IR::BasicBlock *b, basicBlocks)
        foreach (V4IR::Stmt *s, b->statements)
            s->destroyData();

    qDeleteAll(basicBlocks);
    pool = 0;
    module = 0;
}


const QString *Function::newString(const QString &text)
{
    return &*strings.insert(text);
}

BasicBlock *Function::newBasicBlock(BasicBlock *containingLoop, BasicBlock *catchBlock, BasicBlockInsertMode mode)
{
    BasicBlock *block = new BasicBlock(this, containingLoop, catchBlock);
    return mode == InsertBlock ? insertBasicBlock(block) : block;
}

void Function::dump(QTextStream &out, Stmt::Mode mode)
{
    QString n = name ? *name : QString();
    if (n.isEmpty())
        n.sprintf("%p", this);
    out << "function " << n << "() {" << endl;
    foreach (const QString *formal, formals)
        out << "\treceive " << *formal << ';' << endl;
    foreach (const QString *local, locals)
        out << "\tlocal " << *local << ';' << endl;
    foreach (BasicBlock *bb, basicBlocks)
        bb->dump(out, mode);
    out << '}' << endl;
}

void Function::removeSharedExpressions()
{
    RemoveSharedExpressions removeSharedExpressions;
    removeSharedExpressions(this);
}

int Function::indexOfArgument(const QStringRef &string) const
{
    for (int i = formals.size() - 1; i >= 0; --i) {
        if (*formals.at(i) == string)
            return i;
    }
    return -1;
}
unsigned BasicBlock::newTemp()
{
    return function->tempCount++;
}

Temp *BasicBlock::TEMP(unsigned index)
{
    Temp *e = function->New<Temp>();
    e->init(Temp::VirtualRegister, index, 0);
    return e;
}

Temp *BasicBlock::ARG(unsigned index, unsigned scope)
{
    Temp *e = function->New<Temp>();
    e->init(scope ? Temp::ScopedFormal : Temp::Formal, index, scope);
    return e;
}

Temp *BasicBlock::LOCAL(unsigned index, unsigned scope)
{
    Temp *e = function->New<Temp>();
    e->init(scope ? Temp::ScopedLocal : Temp::Local, index, scope);
    return e;
}

Expr *BasicBlock::CONST(Type type, double value)
{ 
    Const *e = function->New<Const>();
    if (type == NumberType) {
        int ival = (int)value;
        // +0 != -0, so we need to convert to double when negating 0
        if (ival == value && !(value == 0 && isNegative(value)))
            type = SInt32Type;
        else
            type = DoubleType;
    }
    e->init(type, value);
    return e;
}

Expr *BasicBlock::STRING(const QString *value)
{
    String *e = function->New<String>();
    e->init(value);
    return e;
}

Expr *BasicBlock::REGEXP(const QString *value, int flags)
{
    RegExp *e = function->New<RegExp>();
    e->init(value, flags);
    return e;
}

Name *BasicBlock::NAME(const QString &id, quint32 line, quint32 column)
{ 
    Name *e = function->New<Name>();
    e->init(function->newString(id), line, column);
    return e;
}

Name *BasicBlock::GLOBALNAME(const QString &id, quint32 line, quint32 column)
{
    Name *e = function->New<Name>();
    e->initGlobal(function->newString(id), line, column);
    return e;
}


Name *BasicBlock::NAME(Name::Builtin builtin, quint32 line, quint32 column)
{
    Name *e = function->New<Name>();
    e->init(builtin, line, column);
    return e;
}

Closure *BasicBlock::CLOSURE(int functionInModule)
{
    Closure *clos = function->New<Closure>();
    clos->init(functionInModule, function->module->functions.at(functionInModule)->name);
    return clos;
}

Expr *BasicBlock::CONVERT(Expr *expr, Type type)
{
    Convert *e = function->New<Convert>();
    e->init(expr, type);
    return e;
}

Expr *BasicBlock::UNOP(AluOp op, Expr *expr)
{ 
    Unop *e = function->New<Unop>();
    e->init(op, expr);
    return e;
}

Expr *BasicBlock::BINOP(AluOp op, Expr *left, Expr *right)
{
    Binop *e = function->New<Binop>();
    e->init(op, left, right);
    return e;
}

Expr *BasicBlock::CALL(Expr *base, ExprList *args)
{ 
    Call *e = function->New<Call>();
    e->init(base, args);
    int argc = 0;
    for (ExprList *it = args; it; it = it->next)
        ++argc;
    function->maxNumberOfArguments = qMax(function->maxNumberOfArguments, argc);
    return e;
}

Expr *BasicBlock::NEW(Expr *base, ExprList *args)
{
    New *e = function->New<New>();
    e->init(base, args);
    return e;
}

Expr *BasicBlock::SUBSCRIPT(Expr *base, Expr *index)
{
    Subscript *e = function->New<Subscript>();
    e->init(base, index);
    return e;
}

Expr *BasicBlock::MEMBER(Expr *base, const QString *name, QQmlPropertyData *property)
{
    Member*e = function->New<Member>();
    e->init(base, name, property);
    return e;
}

Stmt *BasicBlock::EXP(Expr *expr)
{ 
    if (isTerminated())
        return 0;

    Exp *s = function->New<Exp>();
    s->init(expr);
    appendStatement(s);
    return s;
}

Stmt *BasicBlock::MOVE(Expr *target, Expr *source)
{ 
    if (isTerminated())
        return 0;

    Move *s = function->New<Move>();
    s->init(target, source);
    appendStatement(s);
    return s;
}

Stmt *BasicBlock::JUMP(BasicBlock *target) 
{
    if (isTerminated())
        return 0;

    Jump *s = function->New<Jump>();
    s->init(target);
    appendStatement(s);

    assert(! out.contains(target));
    out.append(target);

    assert(! target->in.contains(this));
    target->in.append(this);

    return s;
}

Stmt *BasicBlock::CJUMP(Expr *cond, BasicBlock *iftrue, BasicBlock *iffalse) 
{
    if (isTerminated())
        return 0;

    if (iftrue == iffalse) {
        MOVE(TEMP(newTemp()), cond);
        return JUMP(iftrue);
    }

    CJump *s = function->New<CJump>();
    s->init(cond, iftrue, iffalse);
    appendStatement(s);

    assert(! out.contains(iftrue));
    out.append(iftrue);

    assert(! iftrue->in.contains(this));
    iftrue->in.append(this);

    assert(! out.contains(iffalse));
    out.append(iffalse);

    assert(! iffalse->in.contains(this));
    iffalse->in.append(this);

    return s;
}

Stmt *BasicBlock::RET(Temp *expr)
{
    if (isTerminated())
        return 0;

    Ret *s = function->New<Ret>();
    s->init(expr);
    appendStatement(s);
    return s;
}

void BasicBlock::dump(QTextStream &out, Stmt::Mode mode)
{
    out << 'L' << index << ':';
    if (catchBlock)
        out << " (catchBlock L" << catchBlock->index << ")";
    out << endl;
    foreach (Stmt *s, statements) {
        out << '\t';
        s->dump(out, mode);

        if (s->location.isValid())
            out << " // line: " << s->location.startLine << " ; column: " << s->location.startColumn;

        out << endl;
    }
}

void BasicBlock::appendStatement(Stmt *statement)
{
    if (nextLocation.isValid())
        statement->location = nextLocation;
    statements.append(statement);
}

CloneExpr::CloneExpr(BasicBlock *block)
    : block(block), cloned(0)
{
}

void CloneExpr::setBasicBlock(BasicBlock *block)
{
    this->block = block;
}

ExprList *CloneExpr::clone(ExprList *list)
{
    if (! list)
        return 0;

    ExprList *clonedList = block->function->New<V4IR::ExprList>();
    clonedList->init(clone(list->expr), clone(list->next));
    return clonedList;
}

void CloneExpr::visitConst(Const *e)
{
    cloned = cloneConst(e, block->function);
}

void CloneExpr::visitString(String *e)
{
    cloned = block->STRING(e->value);
}

void CloneExpr::visitRegExp(RegExp *e)
{
    cloned = block->REGEXP(e->value, e->flags);
}

void CloneExpr::visitName(Name *e)
{
    cloned = cloneName(e, block->function);
}

void CloneExpr::visitTemp(Temp *e)
{
    cloned = cloneTemp(e, block->function);
}

void CloneExpr::visitClosure(Closure *e)
{
    cloned = block->CLOSURE(e->value);
}

void CloneExpr::visitConvert(Convert *e)
{
    cloned = block->CONVERT(clone(e->expr), e->type);
}

void CloneExpr::visitUnop(Unop *e)
{
    cloned = block->UNOP(e->op, clone(e->expr));
}

void CloneExpr::visitBinop(Binop *e)
{
    cloned = block->BINOP(e->op, clone(e->left), clone(e->right));
}

void CloneExpr::visitCall(Call *e)
{
    cloned = block->CALL(clone(e->base), clone(e->args));
}

void CloneExpr::visitNew(New *e)
{
    cloned = block->NEW(clone(e->base), clone(e->args));
}

void CloneExpr::visitSubscript(Subscript *e)
{
    cloned = block->SUBSCRIPT(clone(e->base), clone(e->index));
}

void CloneExpr::visitMember(Member *e)
{
    cloned = block->MEMBER(clone(e->base), e->name, e->property);
}

} // end of namespace IR
} // end of namespace QQmlJS

QT_END_NAMESPACE
