/*
 * shim.cpp — the Odin III SystemVerilog adapter: slang (third_party/slang) elaborates the
 * sources, this file walks slang's elaborated AST (post-parameter, post-generate) and writes the
 * Odin III elaborated AST (docs/specs/2026-10-11-2D-elaboration-design.md, EL-4) through the
 * public odin3_ast_* builders, then drives stage 2 with odin3_ast_elaborate (DESIGN §4.2, §12.1).
 *
 * The mapping (agent defaults are listed in adapters/slang/README.md):
 *   packages, enum values, parameters, $bits and constant expressions are folded to NUMBER leaves
 *   (stage 2 never sees a package or enum name);
 *   packed and unpacked structs are flattened to one packed vector, member accesses to selects;
 *   interface instances are flattened into the module's nets, named "<instance>.<signal>";
 *   an interface port becomes one module port per modport signal, named "<port>.<signal>";
 *   block-scoped variables are hoisted to module scope, named "blk$<k>$<name>";
 *   generate scopes are flattened into names ("genblk1.x"), since slang already expanded them;
 *   always_ff/comb/latch become ALWAYS with that sub; always @(...) becomes ALWAYS over
 *   TIMING_STMT[EVENT_CONTROL, S].
 * Unsupported constructs are located errors, never silent drops.
 */
#include "odin3_slang.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/HierarchicalReference.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/SystemSubroutine.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/Operator.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace slang;
using namespace slang::ast;

namespace {

constexpr int NAME_BUF = 512;

/* The spelling of a unique/priority check, for the refusal message. */
const char *checkName(UniquePriorityCheck check) {
    switch (check) {
    case UniquePriorityCheck::Unique:
        return "unique";
    case UniquePriorityCheck::Unique0:
        return "unique0";
    case UniquePriorityCheck::Priority:
        return "priority";
    default:
        return "";
    }
}

/* The Odin NET_DECL sub for a slang net type; false for a kind with no Odin net declaration. */
bool netSub(const NetType &netType, uint32_t &sub) {
    switch (netType.netKind) {
    case NetType::Wire:
        sub = ODIN3_AST_NET_DECL_WIRE;
        return true;
    case NetType::Tri:
        sub = ODIN3_AST_NET_DECL_TRI;
        return true;
    case NetType::WAnd:
        sub = ODIN3_AST_NET_DECL_WAND;
        return true;
    case NetType::WOr:
        sub = ODIN3_AST_NET_DECL_WOR;
        return true;
    case NetType::TriAnd:
        sub = ODIN3_AST_NET_DECL_TRIAND;
        return true;
    case NetType::TriOr:
        sub = ODIN3_AST_NET_DECL_TRIOR;
        return true;
    case NetType::Tri0:
        sub = ODIN3_AST_NET_DECL_TRI0;
        return true;
    case NetType::Tri1:
        sub = ODIN3_AST_NET_DECL_TRI1;
        return true;
    case NetType::TriReg:
        sub = ODIN3_AST_NET_DECL_TRIREG;
        return true;
    case NetType::UWire:
        sub = ODIN3_AST_NET_DECL_UWIRE;
        return true;
    case NetType::Supply0:
        sub = ODIN3_AST_NET_DECL_SUPPLY0;
        return true;
    case NetType::Supply1:
        sub = ODIN3_AST_NET_DECL_SUPPLY1;
        return true;
    default:
        return false;
    }
}

/* One AST node's fields, so node() takes two parameters (the code standard's limit is five). */
struct NodeFields {
    uint32_t kind;
    uint32_t sub = 0;
    uint32_t flags = 0;
    uint32_t loc = 0;
    uint32_t end = 0;
    uint32_t name = 0;
    uint32_t payload = 0;
};

/* A module plan: the slang instance body and the Odin module name it becomes. */
struct ModulePlan {
    const InstanceBodySymbol *body;
    std::string name;
    uint32_t node = 0;
};

/* A port connection's parent expression and its source range (loc/end 0 when unconnected). */
struct ConnectionValue {
    uint32_t value = 0;
    uint32_t loc = 0;
    uint32_t end = 0;
};

/* One flattened module port (an interface port yields one per modport signal). */
struct PortInfo {
    std::string name;
    const Type *type = nullptr;
    ArgumentDirection dir = ArgumentDirection::InOut;
    bool is_var = false;
    bool is_integer = false;
    bool is_interface = false;
    const Symbol *internal = nullptr;
    const PortSymbol *port = nullptr;
    const InterfacePortSymbol *iface = nullptr;
    std::string iface_signal; /* for an interface port: the signal name in the interface */
    SourceLocation loc;       /* the port's or modport signal's declaration */
};

class Adapter {
  public:
    Adapter(odin3_design *design, odin3_ast *ast, odin3_srcman *sm, SourceManager &slsm,
            const odin3_elab_options &opts)
        : design(design), ast(ast), sm(sm), slsm(slsm), opts(opts) {
    }

    odin3_status run(const RootSymbol &root, const char *top);
    odin3_status st = ODIN3_OK;

  private:
    /* --- diagnostics and locations --- */
    void failf(odin3_status status, uint32_t loc, const char *fmt, ...);
    uint32_t locOf(SourceLocation l);
    uint32_t endOf(SourceRange r);
    uint32_t ensureBuffer(BufferID b);
    uint32_t intern(std::string_view text);
    std::string_view textAt(BufferID b);

    /* --- nodes --- */
    uint32_t node(const NodeFields &fields, const std::vector<uint32_t> &kids);
    uint32_t number(const SVInt &value, bool unsized, uint32_t loc, uint32_t end);
    uint32_t numberI64(int64_t value, uint32_t loc, uint32_t end);
    uint32_t constant(const ConstantValue &cv, bool unsized, uint32_t loc, uint32_t end);
    uint32_t rangeNode(const ConstantRange &range, uint32_t loc, uint32_t end);
    uint32_t ident(const std::string &name, uint32_t loc, uint32_t end);
    uint32_t listNode(const std::vector<uint32_t> &items, uint32_t loc, uint32_t end);
    uint32_t unary(uint32_t op, uint32_t operand, uint32_t loc, uint32_t end);
    uint32_t binary(uint32_t op, uint32_t lhs, uint32_t rhs, uint32_t loc, uint32_t end);

    /* --- types and names --- */
    const Type &stripArrays(const Type &type, std::vector<const Type *> &dims);
    bool isIntegerVar(const Type &type);
    std::string symName(const Symbol &sym);
    std::string hoistName(const Symbol &sym);
    void collectModuleNames(const InstanceBodySymbol &body);
    uint32_t declRanges(const Type &type, std::vector<uint32_t> &unpacked, uint32_t loc,
                        uint32_t end);
    bool structMember(const Type &type, const Symbol &member, uint32_t &offset, uint32_t &width);
    uint32_t memberSelect(const Expression &base, const Symbol &member, uint32_t loc, uint32_t end);

    /* --- expressions and statements --- */
    uint32_t expr(const Expression &e);
    uint32_t exprRaw(const Expression &e);
    bool exprLeaf(const Expression &e, uint32_t &out);
    bool exprOper(const Expression &e, uint32_t &out);
    uint32_t exprNamed(const NamedValueExpression &nv, uint32_t loc, uint32_t end);
    uint32_t exprHierarchical(const HierarchicalValueExpression &hv, uint32_t loc, uint32_t end);
    uint32_t exprMember(const MemberAccessExpression &ma, uint32_t loc, uint32_t end);
    uint32_t exprSelect(const Expression &e, uint32_t loc, uint32_t end);
    uint32_t exprLiteral(const Expression &e, uint32_t loc, uint32_t end);
    uint32_t exprUnary(const UnaryExpression &ue, uint32_t loc, uint32_t end);
    uint32_t exprBinary(const BinaryExpression &be, uint32_t loc, uint32_t end);
    uint32_t exprConditional(const ConditionalExpression &ce, uint32_t loc, uint32_t end);
    uint32_t exprConcat(const ConcatenationExpression &cc, uint32_t loc, uint32_t end);
    uint32_t exprReplication(const ReplicationExpression &re, uint32_t loc, uint32_t end);
    uint32_t exprConversion(const ConversionExpression &cv, uint32_t loc, uint32_t end);
    uint32_t exprCall(const CallExpression &call, uint32_t loc, uint32_t end);
    uint32_t stmt(const Statement &s);
    uint32_t stmtList(const StatementList &sl, uint32_t loc, uint32_t end);
    uint32_t stmtExpr(const ExpressionStatement &es, uint32_t loc, uint32_t end);
    uint32_t stmtDecl(const VariableDeclStatement &vd, uint32_t loc, uint32_t end);
    uint32_t stmtConditional(const ConditionalStatement &cs, uint32_t loc, uint32_t end);
    bool caseSub(CaseStatementCondition condition, uint32_t loc, uint32_t &sub);
    uint32_t stmtCase(const CaseStatement &cs, uint32_t loc, uint32_t end);
    uint32_t stmtFor(const ForLoopStatement &fs, uint32_t loc, uint32_t end);
    uint32_t stmtRepeat(const RepeatLoopStatement &rs, uint32_t loc, uint32_t end);
    uint32_t stmtWhile(const WhileLoopStatement &ws, uint32_t loc, uint32_t end);
    uint32_t stmtTimed(const TimedStatement &ts, uint32_t loc, uint32_t end);
    uint32_t assignStmt(const Expression &e, bool nonblocking);
    uint32_t assignExpression(const AssignmentExpression &ae);
    uint32_t incDecStmt(const UnaryExpression &ue, bool nonblocking);
    uint32_t blockStmt(const BlockStatement &b);
    uint32_t timing(const TimingControl &t, uint32_t loc, uint32_t end);
    uint32_t eventExpr(const SignalEventControl &se, uint32_t loc, uint32_t end);

    /* --- modules and items --- */
    std::vector<PortInfo> ports(const InstanceBodySymbol &body);
    bool plainPort(const PortSymbol &p, PortInfo &info);
    bool modportPort(const InterfacePortSymbol &ip, const ModportPortSymbol &mp, PortInfo &info);
    PortInfo ifaceSignal(const InterfacePortSymbol &ip, const Symbol &m);
    void ifacePorts(const InterfacePortSymbol &ip, std::vector<PortInfo> &out);
    void queueBody(const InstanceBodySymbol &body);
    uint32_t emitModule(ModulePlan plan);
    uint32_t moduleLoc(const InstanceBodySymbol &body);
    uint32_t emitPortList(const std::vector<PortInfo> &portList, uint32_t loc);
    uint32_t emitParamList(const InstanceBodySymbol &body, uint32_t loc);
    void emitModuleItems(const InstanceBodySymbol &body, const std::vector<PortInfo> &portList,
                         std::vector<uint32_t> &items);
    void emitMember(const Symbol &m, std::vector<uint32_t> &items);
    void emitDeclItem(const Symbol &m, std::vector<uint32_t> &items);
    void emitInstanceItem(const InstanceSymbol &inst, std::vector<uint32_t> &items);
    void emitInterfaceItem(const InstanceSymbol &inst, uint32_t loc, std::vector<uint32_t> &items);
    void emitInstanceArrayItem(const InstanceArraySymbol &arr, uint32_t loc,
                               std::vector<uint32_t> &items);
    void emitBlockItem(const ProceduralBlockSymbol &pb, uint32_t loc, std::vector<uint32_t> &items);
    void emitHoisted(std::vector<uint32_t> &items);
    uint32_t emitPort(const PortInfo &p, uint32_t loc, uint32_t end);
    uint32_t emitParamDecl(const ParameterSymbol &p, uint32_t loc, uint32_t end);
    uint32_t emitNetOrVar(const Symbol &sym, bool force_net = false);
    bool netAttributesOk(const NetSymbol &net, uint32_t loc);
    uint32_t emitInterfaces(const InstanceBodySymbol &body);
    uint32_t emitInstance(const InstanceSymbol &inst, uint32_t loc, uint32_t end);
    uint32_t emitInstantiation(const std::string &moduleName,
                               const std::vector<const Symbol *> &insts,
                               const InstanceBodySymbol *child, uint32_t loc, uint32_t end);
    std::vector<uint32_t> emitConnections(const InstanceBodySymbol &body,
                                          const InstanceSymbol *inst,
                                          const std::vector<PortInfo> &portList, uint32_t loc);
    ConnectionValue
    portConnectionValue(const PortInfo &p, const InstanceSymbol *inst,
                        const std::unordered_map<std::string_view, const PortConnection *> &byName,
                        uint32_t loc);
    ConnectionValue ifaceConnectionValue(const PortInfo &p, const InstanceSymbol &inst,
                                         uint32_t loc);
    uint32_t emitAlways(const ProceduralBlockSymbol &pb, uint32_t loc, uint32_t end);
    uint32_t emitInitial(const ProceduralBlockSymbol &pb, uint32_t loc, uint32_t end);
    uint32_t emitContinuous(const ContinuousAssignSymbol &ca, uint32_t loc, uint32_t end);
    std::vector<uint32_t> emitInterfaceAssigns(const InstanceSymbol &ifaceInst, uint32_t loc);
    std::string moduleNameFor(const InstanceBodySymbol &body);

    odin3_design *design;
    odin3_ast *ast;
    odin3_srcman *sm;
    SourceManager &slsm;
    odin3_elab_options opts;

    std::unordered_map<uint32_t, uint32_t> buffers; /* slang buffer id -> Odin FILE buffer */
    std::unordered_map<const Symbol *, std::string> names;
    std::unordered_map<const Symbol *, std::string> hoisted;
    std::vector<const VariableSymbol *> module_hoisted;
    std::vector<ModulePlan> plans;
    std::deque<const InstanceBodySymbol *> queue;
    std::unordered_set<std::string> taken;
    int next_block = 1;
    int next_special = 2;
};

/* --- diagnostics and locations ------------------------------------------------------------- */

void Adapter::failf(odin3_status status, uint32_t loc, const char *fmt, ...) {
    if (st != ODIN3_OK) {
        return;
    }
    char body[NAME_BUF];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    char msg[NAME_BUF + 256];
    if (loc != 0) {
        const char *where = NULL;
        if (odin3_srcman_get_format(sm, loc, &where) == ODIN3_OK && where != NULL) {
            (void)snprintf(msg, sizeof msg, "read_slang: %s: %s", where, body);
        } else {
            (void)snprintf(msg, sizeof msg, "read_slang: %s", body);
        }
    } else {
        (void)snprintf(msg, sizeof msg, "read_slang: %s", body);
    }
    (void)odin3_log_write(ODIN3_LOG_ERROR, msg);
    st = status;
}

std::string_view Adapter::textAt(BufferID b) {
    return slsm.getSourceText(b);
}

uint32_t Adapter::intern(std::string_view text) {
    std::string copy(text);
    uint32_t id = 0;
    odin3_status rc = odin3_ast_build_intern(ast, copy.c_str(), &id);
    if (rc != ODIN3_OK) {
        st = rc;
        return 0;
    }
    return id;
}

uint32_t Adapter::ensureBuffer(BufferID b) {
    auto it = buffers.find(b.getId());
    if (it != buffers.end()) {
        return it->second;
    }
    std::string raw(slsm.getRawFileName(b));
    std::filesystem::path full = slsm.getFullPath(b);
    std::string resolved = full.empty() ? raw : full.string();
    uint32_t name = intern(raw.empty() ? "<unknown>" : raw);
    uint32_t res = intern(resolved);
    uint32_t parent = 0;
    SourceLocation from = slsm.getIncludedFrom(b);
    if (from) {
        parent = locOf(from);
    }
    std::string_view text = textAt(b);
    odin3_srcfile_spec spec = {};
    spec.name = name;
    spec.resolved = res;
    spec.parent = parent;
    spec.len = (uint32_t)text.size();
    uint32_t buffer = 0;
    odin3_status rc = odin3_srcman_build_file(sm, &spec, &buffer);
    if (rc != ODIN3_OK) {
        st = rc;
        return 0;
    }
    buffers[b.getId()] = buffer;
    /* Line starts after the first: offset 0 is line 1 implicitly (odin3_srcman_build_line). */
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n' && i + 1 < text.size()) {
            rc = odin3_srcman_build_line(sm, buffer, (uint32_t)(i + 1));
            if (rc != ODIN3_OK) {
                st = rc;
                return 0;
            }
        }
    }
    return buffer;
}

uint32_t Adapter::locOf(SourceLocation l) {
    if (!l || st != ODIN3_OK) {
        return 0;
    }
    SourceLocation file = slsm.getFullyExpandedLoc(l);
    BufferID b = file.buffer();
    if (!b) {
        return 0;
    }
    uint32_t buffer = ensureBuffer(b);
    if (buffer == 0) {
        return 0;
    }
    uint32_t loc = 0;
    odin3_status rc = odin3_srcman_get_loc(sm, buffer, (uint32_t)file.offset(), &loc);
    if (rc != ODIN3_OK) {
        st = rc;
        return 0;
    }
    return loc;
}

uint32_t Adapter::endOf(SourceRange r) {
    return r.end() ? locOf(r.end()) : 0;
}

/* --- nodes ---------------------------------------------------------------------------------- */

uint32_t Adapter::node(const NodeFields &fields, const std::vector<uint32_t> &kids) {
    uint32_t loc = fields.loc;
    uint32_t end = fields.end;
    if (st != ODIN3_OK) {
        return 0;
    }
    /* loc <= end holds only when both name one buffer; a synthesized end (a module's, a
     * parent's) is dropped rather than risk the AST check's rule 1. */
    if (end != 0) {
        odin3_srcpos a = {};
        odin3_srcpos b = {};
        if (loc == 0 || odin3_srcman_get_position(sm, loc, &a) != ODIN3_OK ||
            odin3_srcman_get_position(sm, end, &b) != ODIN3_OK || a.buffer != b.buffer ||
            end < loc) {
            end = 0;
        }
    }
    odin3_ast_spec spec = {};
    spec.kind = (uint8_t)fields.kind;
    spec.sub = (uint8_t)fields.sub;
    spec.flags = (uint16_t)fields.flags;
    spec.loc = loc;
    spec.end = end;
    spec.name = fields.name;
    spec.payload = fields.payload;
    uint32_t made = 0;
    odin3_status rc = odin3_ast_build_node(ast, &spec, kids.empty() ? NULL : kids.data(),
                                           (uint32_t)kids.size(), &made);
    if (rc != ODIN3_OK) {
        st = rc;
        return 0;
    }
    return made;
}

uint32_t Adapter::number(const SVInt &value, bool unsized, uint32_t loc, uint32_t end) {
    uint32_t width = value.getBitWidth();
    std::string bits;
    bits.reserve(width);
    for (int i = (int)width - 1; i >= 0; i--) {
        logic_t bit = value[(int32_t)i];
        if (bit == logic_t::x) {
            bits.push_back('x');
        } else if (bit == logic_t::z) {
            bits.push_back('z');
        } else {
            bits.push_back(bit.value != 0 ? '1' : '0');
        }
    }
    uint32_t payload = 0;
    odin3_status rc =
        odin3_ast_number_new_str(ast, loc, unsized ? 0U : width, bits.c_str(), &payload);
    if (rc != ODIN3_OK) {
        if (st == ODIN3_OK) {
            st = rc;
        }
        return 0;
    }
    uint32_t flags = value.isSigned() ? (uint32_t)ODIN3_AST_F_NUMBER_SIGNED : 0U;
    return node({ODIN3_AST_NUMBER, ODIN3_AST_NUMBER_BIN, flags, loc, end, 0, payload}, {});
}

uint32_t Adapter::numberI64(int64_t value, uint32_t loc, uint32_t end) {
    SVInt v = SVInt(32, (uint64_t)(int64_t)(int32_t)value, true);
    return number(v, false, loc, end);
}

uint32_t Adapter::constant(const ConstantValue &cv, bool unsized, uint32_t loc, uint32_t end) {
    if (cv.isInteger()) {
        return number(cv.integer(), unsized, loc, end);
    }
    if (cv.isString()) {
        std::string text = cv.str();
        uint32_t name = intern(text);
        if (st != ODIN3_OK) {
            return 0;
        }
        uint32_t flags = text.empty() ? (uint32_t)ODIN3_AST_F_STRING_EMPTY : 0U;
        return node({ODIN3_AST_STRING, 0, flags, loc, end, name, 0}, {});
    }
    failf(ODIN3_ERR_PARSE, loc, "a constant that is not an integer or string");
    return 0;
}

uint32_t Adapter::rangeNode(const ConstantRange &range, uint32_t loc, uint32_t end) {
    uint32_t left = numberI64(range.left, loc, end);
    uint32_t right = numberI64(range.right, loc, end);
    return node({ODIN3_AST_RANGE, 0, 0, loc, end, 0, 0}, {left, right});
}

uint32_t Adapter::ident(const std::string &name, uint32_t loc, uint32_t end) {
    uint32_t id = intern(name);
    if (st != ODIN3_OK) {
        return 0;
    }
    return node({ODIN3_AST_IDENT, 0, 0, loc, end, id, 0}, {});
}

uint32_t Adapter::listNode(const std::vector<uint32_t> &items, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_LIST, 0, 0, loc, end, 0, 0}, items);
}

uint32_t Adapter::unary(uint32_t op, uint32_t operand, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_UNARY, op, 0, loc, end, 0, 0}, {operand});
}

uint32_t Adapter::binary(uint32_t op, uint32_t lhs, uint32_t rhs, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_BINARY, op, 0, loc, end, 0, 0}, {lhs, rhs});
}

/* --- types and names ----------------------------------------------------------------------- */

const Type &Adapter::stripArrays(const Type &type, std::vector<const Type *> &dims) {
    const Type *t = &type;
    while (t->isUnpackedArray()) {
        dims.push_back(t);
        t = &t->getCanonicalType().as<FixedSizeUnpackedArrayType>().elementType;
    }
    return *t;
}

bool Adapter::isIntegerVar(const Type &type) {
    const Type &t = type.getCanonicalType();
    return t.isIntegral() && t.getBitWidth() == 32 && t.isSigned() && t.isPredefinedInteger() &&
           t.kind != SymbolKind::EnumType;
}

/* The name a value symbol has in the module: its own name, prefixed by the generate blocks,
 * named blocks and interface instances it is nested in (slang expanded generate, so the nesting
 * is a name, not a scope, in the Odin AST). */
std::string Adapter::symName(const Symbol &sym) {
    auto ho = hoisted.find(&sym);
    if (ho != hoisted.end()) {
        return ho->second;
    }
    auto it = names.find(&sym);
    if (it != names.end()) {
        return it->second;
    }
    std::vector<std::string> parts;
    parts.emplace_back(sym.name);
    const Scope *scope = sym.getParentScope();
    while (scope != NULL) {
        const Symbol &owner = scope->asSymbol();
        if (owner.kind == SymbolKind::InstanceBody) {
            const InstanceBodySymbol &body = owner.as<InstanceBodySymbol>();
            if (body.parentInstance != NULL && body.parentInstance->isInterface()) {
                parts.insert(parts.begin(), std::string(body.parentInstance->name));
            }
            break;
        }
        if (owner.kind == SymbolKind::GenerateBlock || owner.kind == SymbolKind::StatementBlock) {
            if (!owner.name.empty()) {
                parts.insert(parts.begin(), std::string(owner.name));
            }
        }
        scope = owner.getParentScope();
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i != 0) {
            out += '.';
        }
        out += parts[i];
    }
    names[&sym] = out;
    return out;
}

void Adapter::collectModuleNames(const InstanceBodySymbol &body) {
    for (const Symbol &m : body.members()) {
        if (m.kind == SymbolKind::Net || m.kind == SymbolKind::Variable) {
            names[&m] = symName(m);
        } else if ((m.kind == SymbolKind::Instance || m.kind == SymbolKind::InstanceArray) &&
                   m.kind == SymbolKind::Instance) {
            /* Interface instances are flattened: their signals get "<inst>.<signal>". */
            const InstanceSymbol &inst = m.as<InstanceSymbol>();
            if (!inst.isInterface()) {
                continue;
            }
            const InstanceBodySymbol &ib = inst.body;
            for (const Symbol &inner : ib.members()) {
                if (inner.kind == SymbolKind::Net || inner.kind == SymbolKind::Variable) {
                    names[&inner] = std::string(inst.name) + "." + std::string(inner.name);
                }
            }
        }
    }
}

uint32_t Adapter::declRanges(const Type &type, std::vector<uint32_t> &unpacked, uint32_t loc,
                             uint32_t end) {
    std::vector<const Type *> dims;
    const Type &base = stripArrays(type, dims);
    std::transform(dims.begin(), dims.end(), std::back_inserter(unpacked), [&](const Type *dim) {
        return rangeNode(dim->getCanonicalType().as<FixedSizeUnpackedArrayType>().range, loc, end);
    });
    const Type &t = base.getCanonicalType();
    if (t.isPredefinedInteger()) {
        return 0; /* int/integer: the declaration's DT or VAR_DECL sub gives [31:0] */
    }
    if (t.kind == SymbolKind::UnpackedStructType || t.kind == SymbolKind::UnpackedUnionType) {
        uint64_t width = t.getSelectableWidth();
        if (width > 1) {
            return rangeNode(ConstantRange((int32_t)(width - 1), 0), loc, end);
        }
        return 0;
    }
    if (t.isIntegral() && t.getBitWidth() > 1) {
        ConstantRange range = t.getFixedRange();
        if (range.width() > 1) {
            return rangeNode(range, loc, end);
        }
    }
    return 0;
}

/* The bit offset (from the flattened vector's LSB) and width of struct member `member`. */
bool Adapter::structMember(const Type &type, const Symbol &member, uint32_t &offset,
                           uint32_t &width) {
    const Type &t = type.getCanonicalType();
    std::vector<const FieldSymbol *> fields;
    if (t.kind == SymbolKind::PackedStructType) {
        const PackedStructType &packedType = t.as<PackedStructType>();
        for (const Symbol &m : packedType.members()) {
            if (m.kind == SymbolKind::Field) {
                fields.push_back(&m.as<FieldSymbol>());
            }
        }
    } else if (t.kind == SymbolKind::UnpackedStructType) {
        const UnpackedStructType &unpackedType = t.as<UnpackedStructType>();
        for (const Symbol &m : unpackedType.members()) {
            if (m.kind == SymbolKind::Field) {
                fields.push_back(&m.as<FieldSymbol>());
            }
        }
    } else {
        return false;
    }
    uint32_t total =
        std::accumulate(fields.begin(), fields.end(), 0U, [](uint32_t sum, const FieldSymbol *f) {
            return sum + (uint32_t)f->getType().getBitWidth();
        });
    uint32_t at = total;
    for (const FieldSymbol *f : fields) {
        uint32_t w = (uint32_t)f->getType().getBitWidth();
        at -= w;
        if (std::string_view(f->name) == std::string_view(member.name)) {
            offset = at;
            width = w;
            return true;
        }
    }
    return false;
}

uint32_t Adapter::memberSelect(const Expression &base, const Symbol &member, uint32_t loc,
                               uint32_t end) {
    uint32_t value = expr(base);
    uint32_t offset = 0;
    uint32_t width = 0;
    if (!structMember(base.type->getCanonicalType(), member, offset, width)) {
        failf(ODIN3_ERR_PARSE, loc, "member '%s' of a type with no bit layout",
              std::string(member.name).c_str());
        return 0;
    }
    if (width == 0) {
        return 0;
    }
    uint32_t msb = numberI64((int64_t)(offset + width - 1), loc, end);
    uint32_t lsb = numberI64((int64_t)offset, loc, end);
    return node({ODIN3_AST_SELECT, ODIN3_AST_SELECT_PART, 0, loc, end, 0, 0}, {value, msb, lsb});
}

/* --- expressions --------------------------------------------------------------------------- */

uint32_t Adapter::expr(const Expression &e) {
    if (st != ODIN3_OK) {
        return 0;
    }
    uint32_t loc = locOf(e.sourceRange.start());
    uint32_t end = endOf(e.sourceRange);
    if (const ConstantValue *cv = e.getConstant()) {
        if (!cv->bad() && (cv->isInteger() || cv->isString())) {
            return constant(*cv, false, loc, end);
        }
    }
    return exprRaw(e);
}

/* The operator tables: one entry per slang operator the adapter has an Odin AST node for. */
struct UnaryOpEntry {
    UnaryOperator op;
    uint32_t code;
};
constexpr UnaryOpEntry kUnaryOps[] = {
    {UnaryOperator::Plus, ODIN3_AST_UNARY_PLUS},
    {UnaryOperator::Minus, ODIN3_AST_UNARY_MINUS},
    {UnaryOperator::LogicalNot, ODIN3_AST_UNARY_LNOT},
    {UnaryOperator::BitwiseNot, ODIN3_AST_UNARY_NOT},
    {UnaryOperator::BitwiseAnd, ODIN3_AST_UNARY_AND},
    {UnaryOperator::BitwiseNand, ODIN3_AST_UNARY_NAND},
    {UnaryOperator::BitwiseOr, ODIN3_AST_UNARY_OR},
    {UnaryOperator::BitwiseNor, ODIN3_AST_UNARY_NOR},
    {UnaryOperator::BitwiseXor, ODIN3_AST_UNARY_XOR},
    {UnaryOperator::BitwiseXnor, ODIN3_AST_UNARY_XNOR},
};

struct BinaryOpEntry {
    BinaryOperator op;
    uint32_t code;
};
constexpr BinaryOpEntry kBinaryOps[] = {
    {BinaryOperator::Add, ODIN3_AST_BINARY_ADD},
    {BinaryOperator::Subtract, ODIN3_AST_BINARY_SUB},
    {BinaryOperator::Multiply, ODIN3_AST_BINARY_MUL},
    {BinaryOperator::Divide, ODIN3_AST_BINARY_DIV},
    {BinaryOperator::Mod, ODIN3_AST_BINARY_MOD},
    {BinaryOperator::Power, ODIN3_AST_BINARY_POW},
    {BinaryOperator::Equality, ODIN3_AST_BINARY_EQ},
    {BinaryOperator::Inequality, ODIN3_AST_BINARY_NE},
    {BinaryOperator::CaseEquality, ODIN3_AST_BINARY_CASE_EQ},
    {BinaryOperator::CaseInequality, ODIN3_AST_BINARY_CASE_NE},
    {BinaryOperator::LogicalAnd, ODIN3_AST_BINARY_LAND},
    {BinaryOperator::LogicalOr, ODIN3_AST_BINARY_LOR},
    {BinaryOperator::LessThan, ODIN3_AST_BINARY_LT},
    {BinaryOperator::LessThanEqual, ODIN3_AST_BINARY_LE},
    {BinaryOperator::GreaterThan, ODIN3_AST_BINARY_GT},
    {BinaryOperator::GreaterThanEqual, ODIN3_AST_BINARY_GE},
    {BinaryOperator::BinaryAnd, ODIN3_AST_BINARY_AND},
    {BinaryOperator::BinaryOr, ODIN3_AST_BINARY_OR},
    {BinaryOperator::BinaryXor, ODIN3_AST_BINARY_XOR},
    {BinaryOperator::BinaryXnor, ODIN3_AST_BINARY_XNOR},
    {BinaryOperator::LogicalShiftLeft, ODIN3_AST_BINARY_SHL},
    {BinaryOperator::LogicalShiftRight, ODIN3_AST_BINARY_SHR},
    {BinaryOperator::ArithmeticShiftLeft, ODIN3_AST_BINARY_ASHL},
    {BinaryOperator::ArithmeticShiftRight, ODIN3_AST_BINARY_ASHR},
};

/* The Odin code of op, or false when the adapter has no node for it. */
bool unaryCode(UnaryOperator op, uint32_t &code) {
    const auto *end = std::end(kUnaryOps);
    const auto *entry = std::find_if(std::begin(kUnaryOps), end,
                                     [op](const UnaryOpEntry &one) { return one.op == op; });
    if (entry == end) {
        return false;
    }
    code = entry->code;
    return true;
}

bool binaryCode(BinaryOperator op, uint32_t &code) {
    const auto *end = std::end(kBinaryOps);
    const auto *entry = std::find_if(std::begin(kBinaryOps), end,
                                     [op](const BinaryOpEntry &one) { return one.op == op; });
    if (entry == end) {
        return false;
    }
    code = entry->code;
    return true;
}

/* True for the increment and decrement operators (statements, not expressions). */
bool isIncDec(UnaryOperator op) {
    return op == UnaryOperator::Preincrement || op == UnaryOperator::Postincrement ||
           op == UnaryOperator::Predecrement || op == UnaryOperator::Postdecrement;
}

uint32_t Adapter::exprRaw(const Expression &e) {
    uint32_t made = 0;
    if (exprLeaf(e, made) || exprOper(e, made)) {
        return made;
    }
    failf(ODIN3_ERR_PARSE, locOf(e.sourceRange.start()), "expression kind %d is not supported",
          (int)e.kind);
    return 0;
}

/* The value, select and literal kinds; false when e is none of them. */
bool Adapter::exprLeaf(const Expression &e, uint32_t &out) {
    uint32_t loc = locOf(e.sourceRange.start());
    uint32_t end = endOf(e.sourceRange);
    switch (e.kind) {
    case ExpressionKind::NamedValue:
        out = exprNamed(e.as<NamedValueExpression>(), loc, end);
        return true;
    case ExpressionKind::HierarchicalValue:
        out = exprHierarchical(e.as<HierarchicalValueExpression>(), loc, end);
        return true;
    case ExpressionKind::MemberAccess:
        out = exprMember(e.as<MemberAccessExpression>(), loc, end);
        return true;
    case ExpressionKind::ElementSelect:
    case ExpressionKind::RangeSelect:
        out = exprSelect(e, loc, end);
        return true;
    case ExpressionKind::IntegerLiteral:
    case ExpressionKind::UnbasedUnsizedIntegerLiteral:
    case ExpressionKind::StringLiteral:
        out = exprLiteral(e, loc, end);
        return true;
    default:
        return false;
    }
}

/* The operator and call kinds; false when e is none of them. */
bool Adapter::exprOper(const Expression &e, uint32_t &out) {
    uint32_t loc = locOf(e.sourceRange.start());
    uint32_t end = endOf(e.sourceRange);
    switch (e.kind) {
    case ExpressionKind::UnaryOp:
        out = exprUnary(e.as<UnaryExpression>(), loc, end);
        return true;
    case ExpressionKind::BinaryOp:
        out = exprBinary(e.as<BinaryExpression>(), loc, end);
        return true;
    case ExpressionKind::ConditionalOp:
        out = exprConditional(e.as<ConditionalExpression>(), loc, end);
        return true;
    case ExpressionKind::Concatenation:
        out = exprConcat(e.as<ConcatenationExpression>(), loc, end);
        return true;
    case ExpressionKind::Replication:
        out = exprReplication(e.as<ReplicationExpression>(), loc, end);
        return true;
    case ExpressionKind::Conversion:
        out = exprConversion(e.as<ConversionExpression>(), loc, end);
        return true;
    case ExpressionKind::Call:
        out = exprCall(e.as<CallExpression>(), loc, end);
        return true;
    default:
        return false;
    }
}

uint32_t Adapter::exprNamed(const NamedValueExpression &nv, uint32_t loc, uint32_t end) {
    if (nv.symbol.kind == SymbolKind::Parameter) {
        const ConstantValue &cv = nv.symbol.as<ParameterSymbol>().getValue();
        if (!cv.bad()) {
            return constant(cv, false, loc, end);
        }
        failf(ODIN3_ERR_PARSE, loc, "'%s' has no constant value",
              std::string(nv.symbol.name).c_str());
        return 0;
    }
    if (nv.symbol.kind == SymbolKind::EnumValue) {
        const ConstantValue &cv = nv.symbol.as<EnumValueSymbol>().getValue();
        if (!cv.bad()) {
            return constant(cv, false, loc, end);
        }
        failf(ODIN3_ERR_PARSE, loc, "'%s' has no value", std::string(nv.symbol.name).c_str());
        return 0;
    }
    if (nv.symbol.kind == SymbolKind::Net || nv.symbol.kind == SymbolKind::Variable ||
        nv.symbol.kind == SymbolKind::ModportPort || VariableSymbol::isKind(nv.symbol.kind)) {
        return ident(symName(nv.symbol), loc, end);
    }
    failf(ODIN3_ERR_PARSE, loc, "'%s' is not a net or variable",
          std::string(nv.symbol.name).c_str());
    return 0;
}

uint32_t Adapter::exprHierarchical(const HierarchicalValueExpression &hv, uint32_t loc,
                                   uint32_t end) {
    if (const ConstantValue *cv = hv.getConstant()) {
        return constant(*cv, false, loc, end);
    }
    if (hv.symbol.kind == SymbolKind::Net || hv.symbol.kind == SymbolKind::Variable ||
        hv.symbol.kind == SymbolKind::ModportPort || VariableSymbol::isKind(hv.symbol.kind)) {
        /* Path names give the flattened port/instance names ("s.valid", "bus.data"). */
        std::string out;
        for (const HierarchicalReference::Element &el : hv.ref.path) {
            if (!out.empty()) {
                out += '.';
            }
            out += el.symbol->name;
            if (std::holds_alternative<int32_t>(el.selector)) {
                out += '[' + std::to_string(std::get<int32_t>(el.selector)) + ']';
            }
        }
        if (out.empty()) {
            out = symName(hv.symbol);
        }
        return ident(out, loc, end);
    }
    failf(ODIN3_ERR_PARSE, loc, "hierarchical reference '%s' to a non-value",
          std::string(hv.symbol.name).c_str());
    return 0;
}

uint32_t Adapter::exprMember(const MemberAccessExpression &ma, uint32_t loc, uint32_t end) {
    if (ma.member.kind == SymbolKind::Field) {
        return memberSelect(ma.value(), ma.member, loc, end);
    }
    if (ma.member.kind == SymbolKind::Net || ma.member.kind == SymbolKind::Variable) {
        return ident(symName(ma.member), loc, end);
    }
    failf(ODIN3_ERR_PARSE, loc, "member access '%s' is not supported",
          std::string(ma.member.name).c_str());
    return 0;
}

uint32_t Adapter::exprSelect(const Expression &e, uint32_t loc, uint32_t end) {
    if (e.kind == ExpressionKind::ElementSelect) {
        const auto &se = e.as<ElementSelectExpression>();
        uint32_t value = expr(se.value());
        uint32_t index = expr(se.selector());
        return node({ODIN3_AST_SELECT, ODIN3_AST_SELECT_BIT, 0, loc, end, 0, 0},
                    std::vector<uint32_t>{value, index, 0});
    }
    const auto &rs = e.as<RangeSelectExpression>();
    uint32_t value = expr(rs.value());
    uint32_t left = expr(rs.left());
    uint32_t right = expr(rs.right());
    uint32_t sub = ODIN3_AST_SELECT_PART;
    if (rs.getSelectionKind() == RangeSelectionKind::IndexedUp) {
        sub = ODIN3_AST_SELECT_PART_PLUS;
    } else if (rs.getSelectionKind() == RangeSelectionKind::IndexedDown) {
        sub = ODIN3_AST_SELECT_PART_MINUS;
    }
    return node({ODIN3_AST_SELECT, sub, 0, loc, end, 0, 0}, {value, left, right});
}

uint32_t Adapter::exprLiteral(const Expression &e, uint32_t loc, uint32_t end) {
    if (e.kind == ExpressionKind::IntegerLiteral) {
        const auto &il = e.as<IntegerLiteral>();
        return number(il.getValue(), il.isDeclaredUnsized, loc, end);
    }
    if (e.kind == ExpressionKind::UnbasedUnsizedIntegerLiteral) {
        const auto &ul = e.as<UnbasedUnsizedIntegerLiteral>();
        return number(ul.getValue(), false, loc, end);
    }
    const auto &sl = e.as<StringLiteral>();
    std::string text(sl.getValue());
    uint32_t name = intern(text);
    uint32_t flags = text.empty() ? (uint32_t)ODIN3_AST_F_STRING_EMPTY : 0U;
    return node({ODIN3_AST_STRING, 0, flags, loc, end, name, 0}, {});
}

uint32_t Adapter::exprUnary(const UnaryExpression &ue, uint32_t loc, uint32_t end) {
    uint32_t op = 0;
    if (!unaryCode(ue.op, op)) {
        failf(ODIN3_ERR_PARSE, loc,
              "unary operator %d is not supported (only statements carry increments)", (int)ue.op);
        return 0;
    }
    return unary(op, expr(ue.operand()), loc, end);
}

uint32_t Adapter::exprBinary(const BinaryExpression &be, uint32_t loc, uint32_t end) {
    uint32_t op = 0;
    if (!binaryCode(be.op, op)) {
        failf(ODIN3_ERR_PARSE, loc, "binary operator %d is not supported", (int)be.op);
        return 0;
    }
    return binary(op, expr(be.left()), expr(be.right()), loc, end);
}

uint32_t Adapter::exprConditional(const ConditionalExpression &ce, uint32_t loc, uint32_t end) {
    if (ce.conditions.size() != 1 || ce.conditions[0].pattern != nullptr) {
        failf(ODIN3_ERR_PARSE, loc, "a conditional with several conditions is not supported");
        return 0;
    }
    return node({ODIN3_AST_TERNARY, 0, 0, loc, end, 0, 0},
                {expr(*ce.conditions[0].expr), expr(ce.left()), expr(ce.right())});
}

uint32_t Adapter::exprConcat(const ConcatenationExpression &cc, uint32_t loc, uint32_t end) {
    std::vector<uint32_t> ops;
    std::transform(cc.operands().begin(), cc.operands().end(), std::back_inserter(ops),
                   [this](const Expression *op) { return expr(*op); });
    return node({ODIN3_AST_CONCAT, 0, 0, loc, end, 0, 0}, ops);
}

uint32_t Adapter::exprReplication(const ReplicationExpression &re, uint32_t loc, uint32_t end) {
    uint32_t count = expr(re.count());
    uint32_t concat = node({ODIN3_AST_CONCAT, 0, 0, loc, end, 0, 0}, {expr(re.concat())});
    return node({ODIN3_AST_REPLICATE, 0, 0, loc, end, 0, 0}, {count, concat});
}

uint32_t Adapter::exprConversion(const ConversionExpression &cv, uint32_t loc, uint32_t end) {
    if (!cv.isImplicit()) {
        /* An explicit cast (sized, signed, streaming) carries a width or sign the Odin AST has
         * no node for; dropping it would change the value silently. */
        failf(ODIN3_ERR_PARSE, loc, "an explicit %d-bit cast is not supported",
              (int)cv.type->getBitWidth());
        return 0;
    }
    uint32_t sub = expr(cv.operand());
    bool from_signed = cv.operand().type->isSigned();
    bool to_signed = cv.type->isSigned();
    if (!from_signed && to_signed) {
        uint32_t name = intern("$signed");
        return node({ODIN3_AST_CALL, 0, ODIN3_AST_F_CALL_SYSTEM, loc, end, name, 0}, {sub});
    }
    return sub;
}

uint32_t Adapter::exprCall(const CallExpression &call, uint32_t loc, uint32_t end) {
    if (!std::holds_alternative<CallExpression::SystemCallInfo>(call.subroutine)) {
        failf(ODIN3_ERR_PARSE, loc, "function and task calls are not supported yet");
        return 0;
    }
    const auto &sys = std::get<CallExpression::SystemCallInfo>(call.subroutine);
    std::string_view name = sys.subroutine->name;
    if (name == "$bits" && call.arguments().size() == 1) {
        const Type &arg = *call.arguments()[0]->type;
        return numberI64((int64_t)arg.getSelectableWidth(), loc, end);
    }
    if (name != "$signed" && name != "$unsigned") {
        failf(ODIN3_ERR_PARSE, loc, "system function '%s' is not supported",
              std::string(name).c_str());
        return 0;
    }
    uint32_t id = intern(name);
    std::vector<uint32_t> args;
    std::transform(call.arguments().begin(), call.arguments().end(), std::back_inserter(args),
                   [this](const Expression *arg) { return expr(*arg); });
    return node({ODIN3_AST_CALL, 0, ODIN3_AST_F_CALL_SYSTEM, loc, end, id, 0}, args);
}

/* --- statements ---------------------------------------------------------------------------- */

uint32_t Adapter::timing(const TimingControl &t, uint32_t loc, uint32_t end) {
    switch (t.kind) {
    case TimingControlKind::SignalEvent: {
        const auto &se = t.as<SignalEventControl>();
        uint32_t event = eventExpr(se, loc, end);
        return node({ODIN3_AST_EVENT_CONTROL, 0, 0, loc, end, 0, 0},
                    std::vector<uint32_t>{0, event});
    }
    case TimingControlKind::EventList: {
        const auto &el = t.as<EventListControl>();
        std::vector<uint32_t> events = {0};
        for (const TimingControl *ev : el.events) {
            if (ev->kind == TimingControlKind::SignalEvent) {
                events.push_back(eventExpr(ev->as<SignalEventControl>(), loc, end));
            } else {
                failf(ODIN3_ERR_PARSE, loc, "this event list entry is not supported");
            }
        }
        return node({ODIN3_AST_EVENT_CONTROL, 0, 0, loc, end, 0, 0}, events);
    }
    case TimingControlKind::ImplicitEvent:
        return node({ODIN3_AST_EVENT_CONTROL, 0, ODIN3_AST_F_EVENT_CONTROL_STAR, loc, end, 0, 0},
                    std::vector<uint32_t>{0});
    default:
        failf(ODIN3_ERR_PARSE, loc, "this timing control is not supported in an always");
        return 0;
    }
}

uint32_t Adapter::eventExpr(const SignalEventControl &se, uint32_t loc, uint32_t end) {
    uint32_t sub = ODIN3_AST_EVENT_EXPR_NONE;
    if (se.edge == EdgeKind::PosEdge) {
        sub = ODIN3_AST_EVENT_EXPR_POSEDGE;
    } else if (se.edge == EdgeKind::NegEdge) {
        sub = ODIN3_AST_EVENT_EXPR_NEGEDGE;
    }
    return node({ODIN3_AST_EVENT_EXPR, sub, 0, loc, end, 0, 0}, {expr(se.expr)});
}

uint32_t Adapter::blockStmt(const BlockStatement &b) {
    uint32_t loc = locOf(b.sourceRange.start());
    uint32_t end = endOf(b.sourceRange);
    std::vector<uint32_t> locals;
    std::string name;
    if (b.blockSymbol != NULL) {
        name = std::string(b.blockSymbol->name);
        for (const Symbol &m : b.blockSymbol->members()) {
            if (VariableSymbol::isKind(m.kind)) {
                const auto &v = m.as<VariableSymbol>();
                std::string full = "blk$" + std::to_string(next_block) + "$" + std::string(v.name);
                hoisted[&v] = full;
                std::vector<uint32_t> unpacked;
                uint32_t range = declRanges(v.getType(), unpacked, loc, end);
                uint32_t decl = ident(full, loc, end);
                std::vector<uint32_t> dkids = {0};
                if (const Expression *init = v.getInitializer()) {
                    dkids[0] = expr(*init);
                }
                dkids.insert(dkids.end(), unpacked.begin(), unpacked.end());
                uint32_t declr =
                    node({ODIN3_AST_DECLARATOR, 0, 0, loc, end, intern(full), 0}, dkids);
                uint32_t sub =
                    isIntegerVar(v.getType()) ? ODIN3_AST_VAR_DECL_INTEGER : ODIN3_AST_VAR_DECL_REG;
                uint32_t flags =
                    v.getType().isSigned() ? (uint32_t)ODIN3_AST_F_VAR_DECL_SIGNED : 0U;
                locals.push_back(node({ODIN3_AST_VAR_DECL, sub, flags, loc, end, 0, 0},
                                      std::vector<uint32_t>{range, declr}));
                (void)decl;
                next_block++;
            }
        }
        if (name.empty() && !locals.empty()) {
            name = "$unnamed_block$" + std::to_string(next_block);
        }
    }
    uint32_t localList = locals.empty() ? 0 : listNode(locals, loc, end);
    uint32_t body = stmt(b.body);
    return node({ODIN3_AST_SEQ_BLOCK, 0, 0, loc, end, name.empty() ? 0 : intern(name), 0},
                {localList, body});
}

uint32_t Adapter::assignStmt(const Expression &e, bool nonblocking) {
    if (e.kind == ExpressionKind::Assignment) {
        return assignExpression(e.as<AssignmentExpression>());
    }
    if (e.kind == ExpressionKind::UnaryOp && isIncDec(e.as<UnaryExpression>().op)) {
        return incDecStmt(e.as<UnaryExpression>(), nonblocking);
    }
    failf(ODIN3_ERR_PARSE, locOf(e.sourceRange.start()), "this assignment form is not supported");
    return 0;
}

uint32_t Adapter::assignExpression(const AssignmentExpression &ae) {
    uint32_t loc = locOf(ae.sourceRange.start());
    uint32_t end = endOf(ae.sourceRange);
    if (ae.timingControl != nullptr) {
        failf(ODIN3_ERR_PARSE, loc, "an intra-assignment timing control is not supported");
        return 0;
    }
    uint32_t lhs = expr(ae.left());
    uint32_t rhs = expr(ae.right());
    if (ae.isCompound()) {
        uint32_t op = 0;
        if (!binaryCode(*ae.op, op)) {
            failf(ODIN3_ERR_PARSE, loc, "this compound assignment is not supported");
            return 0;
        }
        uint32_t again = expr(ae.left());
        rhs = binary(op, again, rhs, loc, end);
    }
    uint32_t kind = ae.isNonBlocking() ? ODIN3_AST_NONBLOCKING_ASSIGN : ODIN3_AST_BLOCKING_ASSIGN;
    return node({kind, 0, 0, loc, end, 0, 0}, {lhs, 0, rhs});
}

/* `++x`, `x++`, `--x`, `x--` in a statement: one assignment of x +- 1. */
uint32_t Adapter::incDecStmt(const UnaryExpression &ue, bool nonblocking) {
    uint32_t loc = locOf(ue.sourceRange.start());
    uint32_t end = endOf(ue.sourceRange);
    bool dec = ue.op == UnaryOperator::Predecrement || ue.op == UnaryOperator::Postdecrement;
    uint32_t lhs = expr(ue.operand());
    uint32_t read = expr(ue.operand()); /* a tree: the read is another node */
    uint32_t one = numberI64(1, loc, end);
    uint32_t sum = binary(dec ? ODIN3_AST_BINARY_SUB : ODIN3_AST_BINARY_ADD, read, one, loc, end);
    uint32_t kind = nonblocking ? ODIN3_AST_NONBLOCKING_ASSIGN : ODIN3_AST_BLOCKING_ASSIGN;
    return node({kind, 0, 0, loc, end, 0, 0}, {lhs, 0, sum});
}

uint32_t Adapter::stmt(const Statement &s) {
    uint32_t loc = locOf(s.sourceRange.start());
    uint32_t end = endOf(s.sourceRange);
    if (st != ODIN3_OK) {
        return 0;
    }
    switch (s.kind) {
    case StatementKind::Empty:
        return node({ODIN3_AST_NULL_STMT, 0, 0, loc, end, 0, 0}, {});
    case StatementKind::Block:
        return blockStmt(s.as<BlockStatement>());
    case StatementKind::List:
        return stmtList(s.as<StatementList>(), loc, end);
    case StatementKind::ExpressionStatement:
        return stmtExpr(s.as<ExpressionStatement>(), loc, end);
    case StatementKind::VariableDeclaration:
        return stmtDecl(s.as<VariableDeclStatement>(), loc, end);
    case StatementKind::Conditional:
        return stmtConditional(s.as<ConditionalStatement>(), loc, end);
    case StatementKind::Case:
        return stmtCase(s.as<CaseStatement>(), loc, end);
    case StatementKind::ForLoop:
        return stmtFor(s.as<ForLoopStatement>(), loc, end);
    case StatementKind::RepeatLoop:
        return stmtRepeat(s.as<RepeatLoopStatement>(), loc, end);
    case StatementKind::WhileLoop:
        return stmtWhile(s.as<WhileLoopStatement>(), loc, end);
    case StatementKind::Timed:
        return stmtTimed(s.as<TimedStatement>(), loc, end);
    default:
        failf(ODIN3_ERR_PARSE, loc, "statement kind %d is not supported", (int)s.kind);
        return 0;
    }
}

uint32_t Adapter::stmtList(const StatementList &sl, uint32_t loc, uint32_t end) {
    std::vector<uint32_t> kids = {0};
    std::transform(sl.list.begin(), sl.list.end(), std::back_inserter(kids),
                   [this](const Statement *one) { return stmt(*one); });
    return node({ODIN3_AST_SEQ_BLOCK, 0, 0, loc, end, 0, 0}, kids);
}

uint32_t Adapter::stmtExpr(const ExpressionStatement &es, uint32_t loc, uint32_t end) {
    if (es.expr.kind == ExpressionKind::Assignment || es.expr.kind == ExpressionKind::UnaryOp) {
        return assignStmt(es.expr, false);
    }
    if (es.expr.kind == ExpressionKind::Call) {
        const auto &call = es.expr.as<CallExpression>();
        if (std::holds_alternative<CallExpression::SystemCallInfo>(call.subroutine)) {
            const auto &sys = std::get<CallExpression::SystemCallInfo>(call.subroutine);
            uint32_t id = intern(sys.subroutine->name);
            std::vector<uint32_t> args;
            std::transform(call.arguments().begin(), call.arguments().end(),
                           std::back_inserter(args),
                           [this](const Expression *arg) { return expr(*arg); });
            return node({ODIN3_AST_TASK_CALL, 0, ODIN3_AST_F_TASK_CALL_SYSTEM, loc, end, id, 0},
                        args);
        }
    }
    failf(ODIN3_ERR_PARSE, loc, "this expression statement is not supported");
    return 0;
}

uint32_t Adapter::stmtDecl(const VariableDeclStatement &vd, uint32_t loc, uint32_t end) {
    auto inserted = hoisted.try_emplace(&vd.symbol);
    if (inserted.second) {
        inserted.first->second =
            "blk$" + std::to_string(next_block++) + "$" + std::string(vd.symbol.name);
    }
    return node({ODIN3_AST_NULL_STMT, 0, 0, loc, end, 0, 0}, {});
}

uint32_t Adapter::stmtConditional(const ConditionalStatement &cs, uint32_t loc, uint32_t end) {
    if (cs.check != UniquePriorityCheck::None) {
        failf(ODIN3_ERR_PARSE, loc, "'%s if' is not supported", checkName(cs.check));
        return 0;
    }
    if (cs.conditions.size() != 1 || cs.conditions[0].pattern != nullptr) {
        failf(ODIN3_ERR_PARSE, loc, "if with a pattern or several conditions is not supported");
        return 0;
    }
    uint32_t cond = expr(*cs.conditions[0].expr);
    uint32_t yes = stmt(cs.ifTrue);
    uint32_t no = cs.ifFalse != nullptr ? stmt(*cs.ifFalse) : 0;
    return node({ODIN3_AST_IF, 0, 0, loc, end, 0, 0}, {cond, yes, no});
}

/* The CASE sub for a case statement's condition kind; 0 for `case inside` (refused). */
bool Adapter::caseSub(CaseStatementCondition condition, uint32_t loc, uint32_t &sub) {
    sub = ODIN3_AST_CASE_CASE;
    if (condition == CaseStatementCondition::WildcardXOrZ) {
        sub = ODIN3_AST_CASE_CASEX;
    } else if (condition == CaseStatementCondition::WildcardJustZ) {
        sub = ODIN3_AST_CASE_CASEZ;
    } else if (condition == CaseStatementCondition::Inside) {
        failf(ODIN3_ERR_PARSE, loc, "case inside is not supported");
        return false;
    }
    return true;
}

uint32_t Adapter::stmtCase(const CaseStatement &cs, uint32_t loc, uint32_t end) {
    if (cs.check != UniquePriorityCheck::None) {
        failf(ODIN3_ERR_PARSE, loc, "'%s case' is not supported", checkName(cs.check));
        return 0;
    }
    uint32_t sub = 0;
    if (!caseSub(cs.condition, loc, sub)) {
        return 0;
    }
    std::vector<uint32_t> kids;
    kids.push_back(expr(cs.expr));
    for (const CaseStatement::ItemGroup &group : cs.items) {
        std::vector<uint32_t> values;
        std::transform(group.expressions.begin(), group.expressions.end(),
                       std::back_inserter(values),
                       [this](const Expression *v) { return expr(*v); });
        uint32_t body = stmt(*group.stmt);
        values.insert(values.begin(), body);
        kids.push_back(node({ODIN3_AST_CASE_ITEM, 0, 0, loc, end, 0, 0}, values));
    }
    if (cs.defaultCase != nullptr) {
        uint32_t body = stmt(*cs.defaultCase);
        kids.push_back(
            node({ODIN3_AST_CASE_ITEM, 0, ODIN3_AST_F_CASE_ITEM_DEFAULT, loc, end, 0, 0}, {body}));
    }
    return node({ODIN3_AST_CASE, sub, 0, loc, end, 0, 0}, kids);
}

uint32_t Adapter::stmtFor(const ForLoopStatement &fs, uint32_t loc, uint32_t end) {
    uint32_t init = 0;
    if (!fs.loopVars.empty()) {
        const VariableSymbol &v = *fs.loopVars[0];
        auto inserted = hoisted.try_emplace(&v);
        if (inserted.second) {
            inserted.first->second =
                "blk$" + std::to_string(next_block++) + "$" + std::string(v.name);
            module_hoisted.push_back(&v);
        }
        uint32_t lhs = ident(hoisted[&v], loc, end);
        uint32_t rhs = 0;
        if (const Expression *ie = v.getInitializer()) {
            rhs = expr(*ie);
        } else {
            rhs = numberI64(0, loc, end);
        }
        init = node({ODIN3_AST_BLOCKING_ASSIGN, 0, 0, loc, end, 0, 0}, {lhs, 0, rhs});
    } else if (!fs.initializers.empty()) {
        init = assignStmt(*fs.initializers[0], false);
    }
    uint32_t cond = fs.stopExpr != nullptr ? expr(*fs.stopExpr) : 0;
    uint32_t step = 0;
    if (!fs.steps.empty()) {
        step = assignStmt(*fs.steps[0], false);
    }
    uint32_t body = stmt(fs.body);
    return node({ODIN3_AST_FOR, 0, 0, loc, end, 0, 0}, {init, cond, step, body});
}

uint32_t Adapter::stmtRepeat(const RepeatLoopStatement &rs, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_REPEAT, 0, 0, loc, end, 0, 0}, {expr(rs.count), stmt(rs.body)});
}

uint32_t Adapter::stmtWhile(const WhileLoopStatement &ws, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_WHILE, 0, 0, loc, end, 0, 0}, {expr(ws.cond), stmt(ws.body)});
}

uint32_t Adapter::stmtTimed(const TimedStatement &ts, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_TIMING_STMT, 0, 0, loc, end, 0, 0},
                {timing(ts.timing, loc, end), stmt(ts.stmt)});
}

/* Refuses a net's drive strength, charge strength or delay with a located error; false when
 * one is present. */
bool Adapter::netAttributesOk(const NetSymbol &net, uint32_t loc) {
    auto strength = net.getDriveStrength();
    if (strength.first.has_value() || strength.second.has_value()) {
        failf(ODIN3_ERR_PARSE, loc, "a drive strength on net '%s' is not supported",
              std::string(net.name).c_str());
        return false;
    }
    if (net.getChargeStrength().has_value() || net.getDelay() != nullptr) {
        failf(ODIN3_ERR_PARSE, loc, "a charge strength or delay on net '%s' is not supported",
              std::string(net.name).c_str());
        return false;
    }
    return true;
}

/* --- modules and items --------------------------------------------------------------------- */

std::vector<PortInfo> Adapter::ports(const InstanceBodySymbol &body) {
    std::vector<PortInfo> out;
    for (const Symbol *port : body.getPortList()) {
        if (port->kind == SymbolKind::Port) {
            PortInfo info;
            if (plainPort(port->as<PortSymbol>(), info)) {
                out.push_back(std::move(info));
            }
        } else if (port->kind == SymbolKind::InterfacePort) {
            ifacePorts(port->as<InterfacePortSymbol>(), out);
        } else if (port->kind == SymbolKind::MultiPort) {
            failf(ODIN3_ERR_PARSE, locOf(port->location), "multi-port '%s' is not supported",
                  std::string(port->name).c_str());
        }
    }
    return out;
}

/* One `input/output/inout` port's info; false (and a located error) for a ref port. */
bool Adapter::plainPort(const PortSymbol &p, PortInfo &info) {
    if (p.direction == ArgumentDirection::Ref) {
        failf(ODIN3_ERR_PARSE, locOf(p.location), "ref port '%s' is not supported",
              std::string(p.name).c_str());
        return false;
    }
    info.name = std::string(p.name);
    info.type = &p.getType();
    info.dir = p.direction;
    info.port = &p;
    info.internal = p.internalSymbol;
    info.loc = p.location;
    if (p.internalSymbol != nullptr) {
        const Symbol &in = *p.internalSymbol;
        if (in.kind == SymbolKind::Variable) {
            info.is_var = true;
            info.is_integer = isIntegerVar(in.as<VariableSymbol>().getType());
        }
    }
    return true;
}

/* One modport signal's port info (a ref modport port is refused). */
bool Adapter::modportPort(const InterfacePortSymbol &ip, const ModportPortSymbol &mp,
                          PortInfo &info) {
    if (mp.direction == ArgumentDirection::Ref) {
        failf(ODIN3_ERR_PARSE, locOf(mp.location), "ref modport port '%s' is not supported",
              std::string(mp.name).c_str());
        return false;
    }
    info.name = std::string(ip.name) + "." + std::string(mp.name);
    info.iface_signal = std::string(mp.name);
    info.dir = mp.direction;
    info.is_interface = true;
    info.iface = &ip;
    info.internal = mp.internalSymbol;
    info.loc = mp.location;
    if (mp.internalSymbol != nullptr && ValueSymbol::isKind(mp.internalSymbol->kind)) {
        info.type = &mp.internalSymbol->as<ValueSymbol>().getType();
    }
    return true;
}

/* One of an interface's own signals as a port, when the interface port has no modport. */
PortInfo Adapter::ifaceSignal(const InterfacePortSymbol &ip, const Symbol &m) {
    PortInfo info;
    info.name = std::string(ip.name) + "." + std::string(m.name);
    info.iface_signal = std::string(m.name);
    info.dir = ArgumentDirection::InOut;
    info.is_interface = true;
    info.iface = &ip;
    info.internal = &m;
    info.type = &m.as<ValueSymbol>().getType();
    info.loc = m.location;
    return info;
}

/* Appends the ports of one interface port: one per modport signal, else one per signal. */
void Adapter::ifacePorts(const InterfacePortSymbol &ip, std::vector<PortInfo> &out) {
    auto conn = ip.getConnection();
    const InstanceSymbol *inst = conn.first != nullptr && conn.first->kind == SymbolKind::Instance
                                     ? &conn.first->as<InstanceSymbol>()
                                     : nullptr;
    if (inst == nullptr) {
        failf(ODIN3_ERR_PARSE, locOf(ip.location),
              "interface port '%s' has no interface instance connection",
              std::string(ip.name).c_str());
        return;
    }
    const ModportSymbol *modport = conn.second;
    if (modport != nullptr) {
        for (const Symbol &m : modport->members()) {
            if (m.kind != SymbolKind::ModportPort) {
                continue;
            }
            PortInfo info;
            if (modportPort(ip, m.as<ModportPortSymbol>(), info)) {
                out.push_back(std::move(info));
            }
        }
        return;
    }
    for (const Symbol &m : inst->body.members()) {
        if (m.kind == SymbolKind::Net || m.kind == SymbolKind::Variable) {
            out.push_back(ifaceSignal(ip, m));
        }
    }
}

void Adapter::queueBody(const InstanceBodySymbol &body) {
    bool planned = std::any_of(plans.begin(), plans.end(),
                               [&body](const ModulePlan &p) { return p.body->hasSameType(body); });
    if (planned) {
        return;
    }
    ModulePlan plan;
    plan.body = &body;
    std::string base(body.getDefinition().name);
    if (plans.empty()) {
        plan.name = base; /* the top keeps its definition's name (EL-9) */
    } else {
        /* Loop until the name is free: a module literally named base$2 may be queued already. */
        plan.name = base;
        while (taken.count(plan.name) != 0) {
            plan.name = base + "$" + std::to_string(next_special++);
        }
    }
    taken.insert(plan.name);
    plans.push_back(plan);
    queue.push_back(&body);
}

uint32_t Adapter::emitPort(const PortInfo &p, uint32_t loc, uint32_t end) {
    std::vector<uint32_t> unpacked;
    uint32_t range = p.type != nullptr ? declRanges(*p.type, unpacked, loc, end) : 0;
    uint32_t flags = (uint32_t)ODIN3_AST_F_PORT_DECL_IN_HEADER;
    if (p.type != nullptr && p.type->isSigned()) {
        flags |= ODIN3_AST_F_PORT_DECL_SIGNED;
    }
    if (p.is_var) {
        flags |= (p.is_integer ? ODIN3_AST_DT_INTEGER : ODIN3_AST_DT_REG)
                 << ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    } else {
        flags |= (1U + ODIN3_AST_NET_DECL_WIRE) << ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    }
    uint32_t sub = ODIN3_AST_PORT_DECL_INPUT;
    if (p.dir == ArgumentDirection::Out) {
        sub = ODIN3_AST_PORT_DECL_OUTPUT;
    } else if (p.dir == ArgumentDirection::InOut) {
        sub = ODIN3_AST_PORT_DECL_INOUT;
    }
    uint32_t name = intern(p.name);
    uint32_t init = 0;
    if (p.port != nullptr && p.port->getInitializer() != nullptr) {
        init = expr(*p.port->getInitializer());
    }
    std::vector<uint32_t> dkids = {init};
    dkids.insert(dkids.end(), unpacked.begin(), unpacked.end());
    uint32_t declr = node({ODIN3_AST_DECLARATOR, 0, 0, loc, end, name, 0}, dkids);
    return node({ODIN3_AST_PORT_DECL, sub, flags, loc, end, 0, 0},
                std::vector<uint32_t>{range, declr});
}

uint32_t Adapter::emitParamDecl(const ParameterSymbol &p, uint32_t loc, uint32_t end) {
    const ConstantValue &cv = p.getValue();
    if (!cv.isInteger() && !cv.isString()) {
        return 0;
    }
    uint32_t name = intern(p.name);
    uint32_t value = constant(cv, false, loc, end);
    std::vector<uint32_t> unpacked;
    uint32_t range = declRanges(p.getType(), unpacked, loc, end);
    uint32_t flags = (uint32_t)ODIN3_AST_F_PARAM_DECL_IN_HEADER;
    if (p.getType().isSigned()) {
        flags |= ODIN3_AST_F_PARAM_DECL_SIGNED;
    }
    std::vector<uint32_t> dkids = {value};
    dkids.insert(dkids.end(), unpacked.begin(), unpacked.end());
    uint32_t declr = node({ODIN3_AST_DECLARATOR, 0, 0, loc, end, name, 0}, dkids);
    return node({ODIN3_AST_PARAM_DECL, ODIN3_AST_PARAM_DECL_LOCALPARAM, flags, loc, end, 0, 0},
                std::vector<uint32_t>{range, declr});
}

uint32_t Adapter::emitNetOrVar(const Symbol &sym, bool force_net) {
    uint32_t loc = locOf(sym.location);
    uint32_t end = 0;
    bool is_net = sym.kind == SymbolKind::Net || force_net;
    const ValueSymbol &v = sym.as<ValueSymbol>();
    uint32_t net_sub = ODIN3_AST_NET_DECL_WIRE;
    if (sym.kind == SymbolKind::Net) {
        const NetSymbol &net = sym.as<NetSymbol>();
        if (!netAttributesOk(net, loc)) {
            return 0;
        }
        if (!netSub(net.netType, net_sub)) {
            failf(ODIN3_ERR_PARSE, loc,
                  "net '%s' of a net type the Odin AST has no declaration for is not supported",
                  std::string(net.name).c_str());
            return 0;
        }
    }
    std::vector<uint32_t> unpacked;
    uint32_t range = declRanges(v.getType(), unpacked, loc, end);
    uint32_t name = intern(symName(v));
    uint32_t init = 0;
    if (const Expression *e = v.getInitializer()) {
        init = expr(*e);
    }
    std::vector<uint32_t> dkids = {init};
    dkids.insert(dkids.end(), unpacked.begin(), unpacked.end());
    uint32_t declr = node({ODIN3_AST_DECLARATOR, 0, 0, loc, end, name, 0}, dkids);
    if (is_net) {
        uint32_t flags = v.getType().isSigned() ? (uint32_t)ODIN3_AST_F_NET_DECL_SIGNED : 0U;
        return node({ODIN3_AST_NET_DECL, net_sub, flags, loc, end, 0, 0},
                    std::vector<uint32_t>{0, range, 0, declr});
    }
    uint32_t sub = isIntegerVar(v.getType()) ? ODIN3_AST_VAR_DECL_INTEGER : ODIN3_AST_VAR_DECL_REG;
    uint32_t flags = v.getType().isSigned() ? (uint32_t)ODIN3_AST_F_VAR_DECL_SIGNED : 0U;
    return node({ODIN3_AST_VAR_DECL, sub, flags, loc, end, 0, 0},
                std::vector<uint32_t>{range, declr});
}

uint32_t Adapter::emitContinuous(const ContinuousAssignSymbol &ca, uint32_t loc, uint32_t end) {
    auto strength = ca.getDriveStrength();
    if (strength.first.has_value() || strength.second.has_value()) {
        failf(ODIN3_ERR_PARSE, loc, "a drive strength on a continuous assignment is not supported");
        return 0;
    }
    if (ca.getDelay() != nullptr) {
        failf(ODIN3_ERR_PARSE, loc, "a delay on a continuous assignment is not supported");
        return 0;
    }
    const Expression &e = ca.getAssignment();
    if (e.kind != ExpressionKind::Assignment) {
        failf(ODIN3_ERR_PARSE, loc, "a continuous assignment that is not an assignment");
        return 0;
    }
    const auto &ae = e.as<AssignmentExpression>();
    uint32_t lhs = expr(ae.left());
    uint32_t rhs = expr(ae.right());
    uint32_t assign = node({ODIN3_AST_NET_ASSIGN, 0, 0, loc, end, 0, 0}, {lhs, rhs});
    return node({ODIN3_AST_CONT_ASSIGN, 0, 0, loc, end, 0, 0}, {0, 0, assign});
}

uint32_t Adapter::emitAlways(const ProceduralBlockSymbol &pb, uint32_t loc, uint32_t end) {
    uint32_t sub = ODIN3_AST_ALWAYS_ALWAYS;
    switch (pb.procedureKind) {
    case ProceduralBlockKind::Always:
        sub = ODIN3_AST_ALWAYS_ALWAYS;
        break;
    case ProceduralBlockKind::AlwaysComb:
        sub = ODIN3_AST_ALWAYS_ALWAYS_COMB;
        break;
    case ProceduralBlockKind::AlwaysLatch:
        sub = ODIN3_AST_ALWAYS_ALWAYS_LATCH;
        break;
    case ProceduralBlockKind::AlwaysFF:
        sub = ODIN3_AST_ALWAYS_ALWAYS_FF;
        break;
    default:
        failf(ODIN3_ERR_PARSE, loc, "procedural block kind %d is not supported",
              (int)pb.procedureKind);
        return 0;
    }
    const Statement &body = pb.getBody();
    uint32_t stmtNode = 0;
    if (body.kind == StatementKind::Timed) {
        const auto &ts = body.as<TimedStatement>();
        uint32_t control = timing(ts.timing, locOf(ts.sourceRange.start()), endOf(ts.sourceRange));
        uint32_t inner = stmt(ts.stmt);
        stmtNode = node({ODIN3_AST_TIMING_STMT, 0, 0, locOf(body.sourceRange.start()),
                         endOf(body.sourceRange), 0, 0},
                        {control, inner});
    } else {
        stmtNode = stmt(body);
    }
    return node({ODIN3_AST_ALWAYS, sub, 0, loc, end, 0, 0}, {stmtNode});
}

uint32_t Adapter::emitInitial(const ProceduralBlockSymbol &pb, uint32_t loc, uint32_t end) {
    return node({ODIN3_AST_INITIAL, 0, 0, loc, end, 0, 0}, {stmt(pb.getBody())});
}

std::vector<uint32_t> Adapter::emitConnections(const InstanceBodySymbol &body,
                                               const InstanceSymbol *inst,
                                               const std::vector<PortInfo> &portList,
                                               uint32_t loc) {
    std::unordered_map<std::string_view, const PortConnection *> byName;
    if (inst != nullptr) {
        for (const PortConnection *conn : inst->getPortConnections()) {
            if (conn->port.kind == SymbolKind::Port) {
                byName[conn->port.name] = conn;
            }
        }
    }
    std::vector<uint32_t> conns;
    for (const PortInfo &p : portList) {
        ConnectionValue conn = portConnectionValue(p, inst, byName, loc);
        uint32_t name = intern(p.name);
        conns.push_back(
            node({ODIN3_AST_CONNECTION, 0, 0, conn.loc, conn.end, name, 0}, {conn.value}));
    }
    (void)body;
    return conns;
}

/* The parent expression and source range of one child port connection (an unconnected port
 * has neither). */
ConnectionValue Adapter::portConnectionValue(
    const PortInfo &p, const InstanceSymbol *inst,
    const std::unordered_map<std::string_view, const PortConnection *> &byName, uint32_t loc) {
    if (p.is_interface) {
        return inst != nullptr ? ifaceConnectionValue(p, *inst, loc) : ConnectionValue{};
    }
    ConnectionValue out;
    if (inst == nullptr || p.port == nullptr) {
        return out;
    }
    auto it = byName.find(p.port->name);
    if (it == byName.end() || it->second->getExpression() == nullptr) {
        return out;
    }
    const Expression *ce = it->second->getExpression();
    out.loc = locOf(ce->sourceRange.start());
    out.end = endOf(ce->sourceRange);
    /* An output or inout connection is an assignment-like expression; its left side is the
     * connection's lvalue. */
    if (ce->kind == ExpressionKind::Assignment) {
        ce = &ce->as<AssignmentExpression>().left();
    }
    out.value = expr(*ce);
    return out;
}

/* A child's interface-port connection: the flattened "<iface instance>.<signal>" net. */
ConnectionValue Adapter::ifaceConnectionValue(const PortInfo &p, const InstanceSymbol &inst,
                                              uint32_t loc) {
    ConnectionValue out;
    if (p.iface == nullptr) {
        return out;
    }
    for (const PortConnection *c : inst.getPortConnections()) {
        if (c->port.kind != SymbolKind::InterfacePort || c->port.name != p.iface->name) {
            continue;
        }
        auto ifaceConn = c->getIfaceConn();
        if (ifaceConn.first != nullptr && ifaceConn.first->kind == SymbolKind::Instance) {
            std::string name = std::string(ifaceConn.first->name) + "." + p.iface_signal;
            out.loc = locOf(ifaceConn.first->location);
            out.value = ident(name, loc, 0);
        }
    }
    return out;
}

uint32_t Adapter::emitInstantiation(const std::string &moduleName,
                                    const std::vector<const Symbol *> &insts,
                                    const InstanceBodySymbol *child, uint32_t loc, uint32_t end) {
    uint32_t params = 0;
    if (child != nullptr) {
        std::vector<uint32_t> pconns;
        for (const ParameterSymbolBase *base : child->getParameters()) {
            if (base->symbol.kind != SymbolKind::Parameter) {
                continue;
            }
            const ParameterSymbol &p = base->symbol.as<ParameterSymbol>();
            if (p.isLocalParam()) {
                continue;
            }
            const ConstantValue &cv = p.getValue();
            if (!cv.isInteger() && !cv.isString()) {
                continue;
            }
            uint32_t value = constant(cv, false, loc, end);
            uint32_t name = intern(p.name);
            pconns.push_back(node({ODIN3_AST_CONNECTION, 0, 0, loc, end, name, 0}, {value}));
        }
        params = pconns.empty() ? 0 : listNode(pconns, loc, end);
    }
    std::vector<PortInfo> portList;
    if (child != nullptr) {
        portList = ports(*child);
    }
    std::vector<uint32_t> instances;
    for (const Symbol *sym : insts) {
        if (sym->kind != SymbolKind::Instance) {
            continue;
        }
        const InstanceSymbol &inst = sym->as<InstanceSymbol>();
        std::vector<uint32_t> conns = emitConnections(inst.body, &inst, portList, loc);
        uint32_t name = intern(std::string(inst.name));
        std::vector<uint32_t> kids = {0};
        kids.insert(kids.end(), conns.begin(), conns.end());
        instances.push_back(
            node({ODIN3_AST_INSTANCE, 0, 0, locOf(inst.location), 0, name, 0}, kids));
    }
    uint32_t id = intern(moduleName);
    std::vector<uint32_t> kids = {params};
    kids.insert(kids.end(), instances.begin(), instances.end());
    return node({ODIN3_AST_INSTANTIATION, 0, 0, loc, 0, id, 0}, kids);
}

std::vector<uint32_t> Adapter::emitInterfaceAssigns(const InstanceSymbol &ifaceInst, uint32_t loc) {
    /* An interface instance's input ports: tie its internal nets to the parent's expressions. */
    std::vector<uint32_t> assigns;
    for (const Symbol *port : ifaceInst.body.getPortList()) {
        if (port->kind != SymbolKind::Port) {
            continue;
        }
        const PortSymbol &p = port->as<PortSymbol>();
        if (p.direction != ArgumentDirection::In || p.internalSymbol == nullptr) {
            continue;
        }
        const PortConnection *conn = ifaceInst.getPortConnection(p);
        if (conn == nullptr || conn->getExpression() == nullptr) {
            continue;
        }
        std::string lhsName =
            std::string(ifaceInst.name) + "." + std::string(p.internalSymbol->name);
        uint32_t lhs = ident(lhsName, loc, 0);
        uint32_t rhs = expr(*conn->getExpression());
        uint32_t assign = node({ODIN3_AST_NET_ASSIGN, 0, 0, loc, 0, 0, 0}, {lhs, rhs});
        assigns.push_back(node({ODIN3_AST_CONT_ASSIGN, 0, 0, loc, 0, 0, 0}, {0, 0, assign}));
    }
    return assigns;
}

uint32_t Adapter::emitModule(ModulePlan plan) {
    const InstanceBodySymbol &body = *plan.body;
    uint32_t loc = moduleLoc(body);
    collectModuleNames(body);
    std::vector<PortInfo> portList = ports(body);
    uint32_t portList_node = emitPortList(portList, loc);
    uint32_t paramList = emitParamList(body, loc);
    std::vector<uint32_t> items;
    /* Declarations and assignments, then instances and processes (source order within each). */
    emitModuleItems(body, portList, items);
    emitHoisted(items);
    /* MODULE slots: 0 = the params LIST, 1 = the ports LIST (check.c's LIST_WANT). */
    std::vector<uint32_t> kids = {paramList, portList_node};
    kids.insert(kids.end(), items.begin(), items.end());
    return node({ODIN3_AST_MODULE, 0, 0, loc, 0, intern(plan.name), 0}, kids);
}

uint32_t Adapter::moduleLoc(const InstanceBodySymbol &body) {
    uint32_t loc = locOf(body.getDefinition().location);
    return loc != 0 ? loc : locOf(body.location);
}

uint32_t Adapter::emitPortList(const std::vector<PortInfo> &portList, uint32_t loc) {
    std::vector<uint32_t> portDecls;
    for (const PortInfo &p : portList) {
        uint32_t ploc = locOf(p.loc);
        portDecls.push_back(emitPort(p, ploc != 0 ? ploc : loc, 0));
    }
    return listNode(portDecls, loc, 0);
}

uint32_t Adapter::emitParamList(const InstanceBodySymbol &body, uint32_t loc) {
    std::vector<uint32_t> paramDecls;
    for (const ParameterSymbolBase *base : body.getParameters()) {
        if (base->symbol.kind == SymbolKind::Parameter) {
            uint32_t decl =
                emitParamDecl(base->symbol.as<ParameterSymbol>(), locOf(base->symbol.location), 0);
            if (decl != 0) {
                paramDecls.push_back(decl);
            }
        }
    }
    return paramDecls.empty() ? 0 : listNode(paramDecls, loc, 0);
}

void Adapter::emitModuleItems(const InstanceBodySymbol &body, const std::vector<PortInfo> &portList,
                              std::vector<uint32_t> &items) {
    std::unordered_set<const Symbol *> portSymbols;
    for (const PortInfo &p : portList) {
        if (p.internal != nullptr) {
            portSymbols.insert(p.internal);
        }
    }
    for (const Symbol &m : body.members()) {
        if (portSymbols.count(&m) == 0) {
            emitMember(m, items);
        }
    }
}

/* One module member as an item: a declaration, an instance, an assignment or a process. */
void Adapter::emitMember(const Symbol &m, std::vector<uint32_t> &items) {
    switch (m.kind) {
    case SymbolKind::Net:
    case SymbolKind::Variable:
        emitDeclItem(m, items);
        break;
    case SymbolKind::Instance: {
        const InstanceSymbol &inst = m.as<InstanceSymbol>();
        if (inst.isInterface()) {
            emitInterfaceItem(inst, locOf(m.location), items);
        } else {
            emitInstanceItem(inst, items);
        }
        break;
    }
    case SymbolKind::InstanceArray:
        emitInstanceArrayItem(m.as<InstanceArraySymbol>(), locOf(m.location), items);
        break;
    case SymbolKind::ContinuousAssign:
        items.push_back(emitContinuous(m.as<ContinuousAssignSymbol>(), locOf(m.location), 0));
        break;
    case SymbolKind::ProceduralBlock:
        emitBlockItem(m.as<ProceduralBlockSymbol>(), locOf(m.location), items);
        break;
    default:
        break;
    }
}

/* A module-level net or variable declaration; a hoisted block variable is emitted once by
 * emitHoisted, and an interface's signals by emitInterfaceItem. */
void Adapter::emitDeclItem(const Symbol &m, std::vector<uint32_t> &items) {
    if (m.name.empty() || hoisted.count(&m) != 0) {
        return;
    }
    if (m.kind == SymbolKind::Variable && (m.as<VariableSymbol>().getType().isFloating() ||
                                           m.as<VariableSymbol>().getType().isString())) {
        failf(ODIN3_ERR_PARSE, locOf(m.location),
              "variable '%s' of real, realtime or string type is not supported",
              std::string(m.name).c_str());
        return;
    }
    items.push_back(emitNetOrVar(m));
}

/* A module instance: the child body becomes a MODULE and one INSTANTIATION names it. */
void Adapter::emitInstanceItem(const InstanceSymbol &inst, std::vector<uint32_t> &items) {
    queueBody(inst.body);
    std::vector<const Symbol *> one = {&inst};
    items.push_back(
        emitInstantiation(moduleNameFor(inst.body), one, &inst.body, locOf(inst.location), 0));
}

/* An interface instance: its signals become connection nets, its inputs get assignments. */
void Adapter::emitInterfaceItem(const InstanceSymbol &inst, uint32_t loc,
                                std::vector<uint32_t> &items) {
    for (const Symbol &inner : inst.body.members()) {
        if ((inner.kind == SymbolKind::Net || inner.kind == SymbolKind::Variable) &&
            !inner.name.empty()) {
            /* An interface's signals are connection nets here: the parent drives them and
             * children read them (EL-26's net rules). */
            items.push_back(emitNetOrVar(inner, true));
        }
    }
    std::vector<uint32_t> assignItems = emitInterfaceAssigns(inst, loc);
    items.insert(items.end(), assignItems.begin(), assignItems.end());
}

void Adapter::emitInstanceArrayItem(const InstanceArraySymbol &arr, uint32_t loc,
                                    std::vector<uint32_t> &items) {
    if (arr.elements.empty() || arr.elements[0] == nullptr ||
        arr.elements[0]->kind != SymbolKind::Instance) {
        failf(ODIN3_ERR_PARSE, loc, "instance array '%s' is not supported",
              std::string(arr.name).c_str());
        return;
    }
    const InstanceSymbol &first = arr.elements[0]->as<InstanceSymbol>();
    queueBody(first.body);
    std::vector<const Symbol *> elems(arr.elements.begin(), arr.elements.end());
    items.push_back(emitInstantiation(moduleNameFor(first.body), elems, &first.body, loc, 0));
}

/* A procedural block: an INITIAL, else an ALWAYS with its procedure kind. */
void Adapter::emitBlockItem(const ProceduralBlockSymbol &pb, uint32_t loc,
                            std::vector<uint32_t> &items) {
    uint32_t made = pb.procedureKind == ProceduralBlockKind::Initial ? emitInitial(pb, loc, 0)
                                                                     : emitAlways(pb, loc, 0);
    if (made != 0) {
        items.push_back(made);
    }
}

/* The variables a block or for loop hoisted to module scope, declared once each. */
void Adapter::emitHoisted(std::vector<uint32_t> &items) {
    for (const VariableSymbol *v : module_hoisted) {
        uint32_t hloc = locOf(v->location);
        std::vector<uint32_t> unpacked;
        uint32_t range = declRanges(v->getType(), unpacked, hloc, hloc);
        uint32_t hname = intern(hoisted[v]);
        std::vector<uint32_t> dkids = {0};
        dkids.insert(dkids.end(), unpacked.begin(), unpacked.end());
        uint32_t declr = node({ODIN3_AST_DECLARATOR, 0, 0, hloc, hloc, hname, 0}, dkids);
        uint32_t sub =
            isIntegerVar(v->getType()) ? ODIN3_AST_VAR_DECL_INTEGER : ODIN3_AST_VAR_DECL_REG;
        uint32_t hflags = v->getType().isSigned() ? (uint32_t)ODIN3_AST_F_VAR_DECL_SIGNED : 0U;
        items.push_back(node({ODIN3_AST_VAR_DECL, sub, hflags, hloc, hloc, 0, 0},
                             std::vector<uint32_t>{range, declr}));
    }
}

std::string Adapter::moduleNameFor(const InstanceBodySymbol &body) {
    const auto it = std::find_if(plans.begin(), plans.end(), [&body](const ModulePlan &p) {
        return p.body->hasSameType(body);
    });
    return it != plans.end() ? it->name : std::string(body.getDefinition().name);
}

/* --- driver -------------------------------------------------------------------------------- */

odin3_status Adapter::run(const RootSymbol &root, const char *top) {
    std::vector<const InstanceSymbol *> tops;
    std::copy_if(root.topInstances.begin(), root.topInstances.end(), std::back_inserter(tops),
                 [](const InstanceSymbol *inst) { return inst->isModule(); });
    if (tops.empty()) {
        failf(ODIN3_ERR_PARSE, 0, "the sources define no module");
        return st;
    }
    if (tops.size() > 1) {
        failf(ODIN3_ERR_PARSE, 0, "%zu modules are uninstantiated (%s); choose one with --top",
              tops.size(), std::string(tops[0]->getDefinition().name).c_str());
        return st;
    }
    const InstanceSymbol *topInst = tops[0];
    taken.insert(std::string(topInst->getDefinition().name));
    queueBody(topInst->body);
    std::vector<uint32_t> moduleNodes;
    for (size_t i = 0; i < plans.size(); i++) {
        ModulePlan plan = plans[i];
        uint32_t made = emitModule(plan);
        plans[i].node = made;
        moduleNodes.push_back(made);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    std::string topName(plans[0].name);
    if (top != NULL && top[0] != '\0' && topName != top) {
        failf(ODIN3_ERR_PARSE, 0, "top '%s' not found", top);
        return st;
    }
    uint32_t name = intern(topName);
    uint32_t unit = node({ODIN3_AST_UNIT, 0, 0, 0, 0, name, 0}, moduleNodes);
    (void)unit;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_status rc = odin3_ast_finish(ast);
    if (rc != ODIN3_OK) {
        return rc;
    }
    odin3_elab_options elab = {};
    elab.nolatches = opts.nolatches;
    elab.nomem2reg = opts.nomem2reg;
    elab.auto_blackbox = opts.auto_blackbox;
    elab.top = 0; /* the UNIT's first MODULE is the top, as EL-4 orders it */
    return odin3_ast_elaborate(design, ast, &elab);
}

/* True when the read's inputs are usable; otherwise logs why. */
bool validArguments(const odin3_design *design, uint32_t run, const odin3_sv_options *opts) {
    if (design == NULL || run == 0 || opts == NULL || opts->paths == NULL || opts->npaths == 0) {
        (void)odin3_log_write(ODIN3_LOG_ERROR, "read_slang: invalid argument");
        return false;
    }
    for (uint32_t i = 0; i < opts->npaths; i++) {
        if (opts->paths[i] == NULL || opts->paths[i][0] == '\0') {
            (void)odin3_log_write(ODIN3_LOG_ERROR, "read_slang: invalid path argument");
            return false;
        }
    }
    if (opts->elab.auto_blackbox) {
        (void)odin3_log_write(ODIN3_LOG_ERROR,
                              "read_slang: --auto-blackbox is not supported for SystemVerilog");
        return false;
    }
    return true;
}

/* The project's include directories and macro definitions as slang's preprocessor options. */
void fillPreprocessor(const odin3_sv_options &opts, parsing::PreprocessorOptions &pp) {
    for (uint32_t i = 0; i < opts.nincdirs; i++) {
        if (opts.incdirs[i] != NULL) {
            pp.additionalIncludePaths.emplace_back(opts.incdirs[i]);
        }
    }
    for (uint32_t i = 0; i < opts.ndefines; i++) {
        const odin3_sv_define &def = opts.defines[i];
        if (def.name == NULL) {
            continue;
        }
        /* "NAME=VALUE"; a define with no value is an empty body, as in the project record. */
        std::string pre(def.name);
        pre += '=';
        if (def.has_value && def.value != NULL) {
            pre += def.value;
        }
        pp.predefines.push_back(std::move(pre));
    }
}

/* Parses every source path into comp; false (and logged) when one cannot be read. */
bool addTrees(Compilation &comp, const odin3_sv_options &opts, SourceManager &slsm,
              const Bag &ppBag) {
    for (uint32_t i = 0; i < opts.npaths; i++) {
        auto tree = syntax::SyntaxTree::fromFile(opts.paths[i], slsm, ppBag);
        if (!tree) {
            char msg[NAME_BUF];
            (void)snprintf(msg, sizeof msg, "read_slang: cannot read '%s'", opts.paths[i]);
            (void)odin3_log_write(ODIN3_LOG_ERROR, msg);
            return false;
        }
        comp.addSyntaxTree(*tree);
    }
    return true;
}

/* Issues every slang diagnostic into the no-color client and logs each rendered line at the
 * severity of the worst one; true when an error was among them. Without the issue call the
 * client's text is empty and a syntax, include or semantic error would be reported with no
 * message at all. */
bool reportDiagnostics(DiagnosticEngine &diag, TextDiagnosticClient &client, Compilation &comp) {
    diag.issue(comp.getAllDiagnostics());
    bool errors = false;
    for (const Diagnostic &d : comp.getAllDiagnostics()) {
        if (diag.getSeverity(d.code, d.location) >= DiagnosticSeverity::Error) {
            errors = true;
        }
    }
    std::string rendered = client.getString();
    std::string line;
    for (char ch : rendered) {
        if (ch != '\n') {
            line.push_back(ch);
            continue;
        }
        if (!line.empty()) {
            (void)odin3_log_write(errors ? ODIN3_LOG_ERROR : ODIN3_LOG_WARN, line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) {
        (void)odin3_log_write(errors ? ODIN3_LOG_ERROR : ODIN3_LOG_WARN, line.c_str());
    }
    return errors;
}

/* Runs the adapter over the elaborated tree and drives stage 2 on the store it builds. */
odin3_status writeStore(odin3_design *design, uint32_t run, SourceManager &slsm,
                        const odin3_sv_options &opts, const RootSymbol &root) {
    odin3_srcman *sm = NULL;
    odin3_status rc = odin3_design_get_srcman(design, &sm);
    if (rc != ODIN3_OK) {
        return rc;
    }
    odin3_ast *ast = NULL;
    rc = odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, run, &ast);
    if (rc != ODIN3_OK) {
        return rc;
    }
    Adapter adapter(design, ast, sm, slsm, opts.elab);
    return adapter.run(root, opts.top);
}

} // namespace

extern "C" __attribute__((visibility("default"))) odin3_status
odin3_read_slang(odin3_design *design, uint32_t run, const odin3_sv_options *opts) {
    if (!validArguments(design, run, opts)) {
        return ODIN3_ERR_INVALID_ARG;
    }

    SourceManager slsm;
    DiagnosticEngine diag(slsm);
    auto client = std::make_shared<TextDiagnosticClient>();
    client->showColors(false);
    diag.addClient(client);

    CompilationOptions options;
    options.flags |= CompilationFlags::DisableInstanceCaching;
    if (opts->top != NULL && opts->top[0] != '\0') {
        options.topModules.emplace(opts->top);
    }
    parsing::PreprocessorOptions ppOptions;
    fillPreprocessor(*opts, ppOptions);
    Bag ppBag;
    ppBag.set(ppOptions);
    Compilation comp(options);
    if (!addTrees(comp, *opts, slsm, ppBag)) {
        return ODIN3_ERR_IO;
    }
    const RootSymbol &root = comp.getRoot();
    if (reportDiagnostics(diag, *client, comp)) {
        return ODIN3_ERR_PARSE;
    }
    return writeStore(design, run, slsm, *opts, root);
}
