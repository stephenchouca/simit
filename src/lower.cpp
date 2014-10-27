#include "lower.h"

#include "ir.h"
#include "domain.h"
#include "ir_mutator.h"
#include "usedef.h"
#include "sig.h"
#include "indexvar.h"
#include "util.h"

using namespace std;

namespace simit {
namespace ir {


class LoopVars : public SIGVisitor {
public:
  LoopVars() : ud(nullptr) {}
  LoopVars(const SIG &sig, const UseDef *ud) : ud(ud) {apply(sig);}

  const pair<Var,ForDomain> &getVar(const IndexVar &var) const {
    return vertexLoopVars.at(var);
  }

  const pair<Var,ForDomain> &getVar(const Var &tensor) const {
    return edgeLoopVars.at(tensor);
  }

  bool hasVar(const Var &tensor) const {
    return edgeLoopVars.find(tensor) != edgeLoopVars.end();
  }

private:
  const UseDef *ud;

  map<IndexVar, pair<Var,ForDomain>> vertexLoopVars;
  map<Var, pair<Var,ForDomain>> edgeLoopVars;

  Var prevLVar = Var();
  Expr prevEdgeIndexSet;

  bool startOfTraversal = true;

  void visit(const SIGVertex *v) {
    bool startOfTraversal = this->startOfTraversal;
    this->startOfTraversal = false;
    SIGVisitor::visit(v);

    Var lvar(v->iv.getName(), Int());

    ForDomain ldom;
    if (!prevLVar.defined()) {
      // The vertex is unconstrained and loops over it's whole domain.
      ldom = v->iv.getDomain().getIndexSets()[0];
    }
    else {
        ldom = ForDomain(prevEdgeIndexSet, prevLVar, ForDomain::Endpoints);
    }

    vertexLoopVars[v->iv] = pair<Var,ForDomain>(lvar,ldom);

    this->prevLVar = (startOfTraversal) ? Var() : lvar;
    this->startOfTraversal = startOfTraversal;
  }

  void visit(const SIGEdge *e) {
    bool startOfTraversal = this->startOfTraversal;
    this->startOfTraversal = false;
    SIGVisitor::visit(e);

    std::string varName = "e";
    for (SIGVertex *nbr : e->endpoints) {
      varName += nbr->iv.getName();
    }
    Var lvar(varName, Int());

    ForDomain ldom;
    if (!prevLVar.defined()) {
      NOT_SUPPORTED_YET;
    }
    else {
      VarDef varDef = ud->getDef(e->tensor);
      if (varDef.getKind() != VarDef::Map) {
        prevLVar = Var();
        return;
      }

      const Map *mapStmt = to<Map>(varDef.getStmt());
      ldom = ForDomain(mapStmt->target, prevLVar, ForDomain::Edges);

      prevEdgeIndexSet = mapStmt->target;
    }

    edgeLoopVars[e->tensor] = pair<Var,ForDomain>(lvar,ldom);

    this->prevLVar = (startOfTraversal) ? Var() : lvar;
    this->startOfTraversal = startOfTraversal;
  }
};


/// Specialize a statement containing an index expression to compute one value.
class SpecializeIndexExprStmt : public IRMutator {
public:
  SpecializeIndexExprStmt(const LoopVars &lvs) : lvs(lvs) {}

private:
  const LoopVars &lvs;

  void visit(const AssignStmt *op) {
    assert(isa<IndexExpr>(op->value) && "Can only specialize IndexExpr stmts");
    const IndexExpr *indexExpr = to<IndexExpr>(op->value);

    Var var = op->var;
    Expr value = mutate(indexExpr);

    if (indexExpr->resultVars.size() == 0) {
      stmt = AssignStmt::make(var, value);
    }
    else {
      Expr varExpr = VarExpr::make(var);
      std::vector<Expr> indices;
      for (IndexVar const& iv : indexExpr->resultVars) {
        indices.push_back(lvs.getVar(iv).first);
      }
      stmt = TensorWrite::make(varExpr, indices, value);
    }
  }

  void visit(const FieldWrite *op) {
    assert(isa<IndexExpr>(op->value) && "Can only specialize IndexExpr stmts");
    const IndexExpr *indexExpr = to<IndexExpr>(op->value);

    Expr elementOrSet = mutate(op->elementOrSet);
    std::string fieldName = op->fieldName;
    Expr value = mutate(indexExpr);

    if (indexExpr->resultVars.size() == 0) {
      stmt = FieldWrite::make(elementOrSet, fieldName, value);
    }
    else {
      std::vector<Expr> indices;
      for (IndexVar const& iv : indexExpr->resultVars) {
        indices.push_back(lvs.getVar(iv).first);
      }
      Expr field = FieldRead::make(elementOrSet, fieldName);
      stmt = TensorWrite::make(field, indices, value);
    }
  }

  void visit(const IndexedTensor *op) {
    // TODO: Flatten IndexExpr. E.g. ((i) A(i,j) *  ((m) c(m)+b(m))(j) )
    if (isa<IndexExpr>(op->tensor)) {
      NOT_SUPPORTED_YET;
    }

    if (op->indexVars.size() == 0) {
      expr = op->tensor;
    }
    else {
      // Check if the tensor inlined.
      if (isa<VarExpr>(op->tensor) && lvs.hasVar(to<VarExpr>(op->tensor)->var)){
        Var var = to<VarExpr>(op->tensor)->var;
        expr = Load::make(op->tensor, lvs.getVar(var).first);
      }
      else {
        std::vector<Expr> indices;
        for (IndexVar const& iv : op->indexVars) {
          indices.push_back(lvs.getVar(iv).first);
        }
        expr = TensorRead::make(op->tensor, indices);
      }
    }
  }

  void visit(const IndexExpr *op) {
    expr = mutate(op->value);
  }
};


class GetReductionTmpName : public IRVisitor {
public:
  string get(const TensorWrite *op) {
    op->tensor.accept(this);
    for (auto &index : op->indices) {
      index.accept(this);
    }
    return name;
  }

private:
  std::string name;

  void visit(const VarExpr *op) {
    IRVisitor::visit(op);
    name += op->var.name;
  }
};


/// Rewrites rstmt to reduce it's computed value into a temporary reduction
/// variable using the rop ReductionOperation.
class ReduceOverVar : public IRMutator {
public:
  ReduceOverVar(Stmt rstmt, ReductionOperator rop) : rstmt(rstmt), rop(rop) {}

  Var getTmpVar() {return tmpVar;}

  /// Retrieve a statement that writes the tmp variable to the original
  /// location of the rewritten statement.  If result is !defined then the
  /// reduction variable does not ned to be written back.
  Stmt getTmpWriteStmt() {return tmpWriteStmt;}

private:
  Stmt rstmt;
  ReductionOperator rop;

  Var tmpVar;
  Stmt tmpWriteStmt;

  void visit(const AssignStmt *op) {
    if (op == rstmt) {
      assert(isScalarTensor(op->value.type()) &&
             "assignment non-scalars should have been lowered by now");
      switch (rop.getKind()) {
        case ReductionOperator::Sum: {
          Expr varExpr = VarExpr::make(op->var);
          tmpVar = op->var;
          stmt = AssignStmt::make(op->var, Add::make(varExpr, op->value));
          break;
        }
        case ReductionOperator::Undefined:
          assert(false);
          break;
      }
    }
    else {
      stmt = op;
    }
  }

  void visit(const TensorWrite *op) {
    if (op == rstmt) {
      Expr tensor = op->tensor;
      std::vector<Expr> indices = op->indices;

      assert(tensor.type().isTensor());
      switch (rop.getKind()) {
        case ReductionOperator::Sum: {
          ScalarType componentType = tensor.type().toTensor()->componentType;
          string tmpVarName = GetReductionTmpName().get(op);
          tmpVar = Var(tmpVarName, TensorType::make(componentType));
          stmt = AssignStmt::make(tmpVar, Add::make(tmpVar, op->value));
          break;
        }
        case ReductionOperator::Undefined:
          assert(false);
          break;
      }
      tmpWriteStmt = TensorWrite::make(tensor, indices, VarExpr::make(tmpVar));
    }
  }
};


class LoopBuilder : public SIGVisitor {
public:
  LoopBuilder(const UseDef *ud) : ud(ud) {}

  Stmt create(const IndexExpr *indexExpr, Stmt indexStmt) {
    SIG sig = SIGBuilder(ud).create(indexExpr);
    lvs = LoopVars(sig, ud);

    componentType = indexExpr->type.toTensor()->componentType;
    body = SpecializeIndexExprStmt(lvs).mutate(indexStmt);
    stmt = body;

    apply(sig);
    Stmt result = stmt;
    stmt = Stmt();
    return result;
  }

private:
  const UseDef *ud;
  LoopVars lvs;
  ScalarType componentType;
  Stmt body;

  Stmt stmt;

  void visit(const SIGVertex *v) {
    pair<Var,ForDomain> loopVar = lvs.getVar(v->iv);
    Var lvar = loopVar.first;
    ForDomain ldom = loopVar.second;

    if (v->iv.isFreeVar()) {
      stmt = For::make(lvar, ldom, stmt);
    }
    else {
      ReduceOverVar rov(body, v->iv.getOperator());
      Stmt loopBody = rov.mutate(stmt);
      Var tmpVar = rov.getTmpVar();
      assert(tmpVar.defined());

      Stmt alloc = AssignStmt::make(tmpVar, Literal::make(tmpVar.type, {0}));
      Stmt loop = For::make(lvar, ldom, loopBody);

      Stmt tmpWriteStmt = rov.getTmpWriteStmt();
      if (tmpWriteStmt.defined()) {
        stmt = Block::make(alloc, Block::make(loop, tmpWriteStmt));
      }
      else {
        stmt = Block::make(alloc, loop);
      }
    }

    SIGVisitor::visit(v);
  }

  void visit(const SIGEdge *e) {
    pair<Var,ForDomain> loopVar = lvs.getVar(e->tensor);
    Var lvar = loopVar.first;
    ForDomain ldom = loopVar.second;
    stmt = For::make(lvar, ldom, stmt);
    SIGVisitor::visit(e);
  }
};

class LowerIndexExpressions : public IRMutator {
public:
  LowerIndexExpressions(const UseDef *ud) : ud(ud) {}

private:
  const UseDef *ud;

  Stmt lower(const IndexExpr *indexExpr, Stmt stmt) {
    return LoopBuilder(ud).create(indexExpr, stmt);
  }

  void visit(const IndexExpr *op) {
    assert(false &&
           "IndexExprs must be assigned to a var/field/tensor before lowering");
  }

  void visit(const AssignStmt *op) {
    if (isa<IndexExpr>(op->value)) {
      stmt = lower(to<IndexExpr>(op->value), op);
    }
    else {
      IRMutator::visit(op);
    }
  }

  void visit(const FieldWrite *op) {
    if (isa<IndexExpr>(op->value)) {
      stmt = lower(to<IndexExpr>(op->value), op);
    }
    else {
      IRMutator::visit(op);
    }
  }

  void visit(const TensorWrite *op) {
    NOT_SUPPORTED_YET;
  }
};


Func lowerIndexExpressions(Func func) {
  UseDef ud(func);
  return LowerIndexExpressions(&ud).mutate(func);
}


class LowerTensorAccesses : public IRMutator {
  void visit(const TensorRead *op) {
    assert(op->type.isTensor() && op->tensor.type().toTensor());

    const TensorType *type = op->tensor.type().toTensor();
    assert(type->order() == op->indices.size());

    // TODO: Generalize to n-order tensors and remove assert (also there's no
    //       need to have specialized code for vectors and matrices).
    assert(type->order() <= 2);

    if (type->order() == 1) {
      Expr tensor = mutate(op->tensor);
      Expr index = mutate(op->indices[0]);
      expr = Load::make(tensor, index);
    }
    else if (type->order() == 2) {
      // TODO: Clearly we need something more sophisticated here (for sparse
      // tensors or nested dense tensors).  For example, a tensor type could
      // carry a 'TensorStorage' object and we could ask this TensorStorage to
      // give us an Expr that computes an i,j location, or an Expr that gives us
      // a row/column.
      Expr tensor = mutate(op->tensor);

      Expr i = mutate(op->indices[0]);
      Expr j = mutate(op->indices[1]);

      IndexDomain dim1 = type->dimensions[1];
      Expr d1;
      if (dim1.getIndexSets().size() == 1 &&
          dim1.getIndexSets()[0].getKind() == IndexSet::Range) {
        // TODO: Add support for unsigned ScalarTypes
        assert(dim1.getSize() < (size_t)(-1));
        int dimSize = static_cast<int>(dim1.getSize());
        d1 = Literal::make(i.type(), &dimSize);
      }
      else {
        NOT_SUPPORTED_YET;
      }
      assert(d1.defined());

      Expr index = Add::make(Mul::make(i, d1), j);
      expr = Load::make(tensor, index);
    }
    else {
      NOT_SUPPORTED_YET;
    }
  }

  void visit(const TensorWrite *op) {
    assert(op->tensor.type().isTensor());

    const TensorType *type = op->tensor.type().toTensor();
    assert(type->order() == op->indices.size());

    // TODO: Generalize to n-order tensors and remove assert (also there's no
    //       need to have specialized code for vectors and matrices).
    assert(type->order() <= 2);

    if (type->order() == 1) {
      Expr tensor = mutate(op->tensor);
      Expr index = mutate(op->indices[0]);
      Expr value = mutate(op->value);
      stmt = Store::make(tensor, index, value);
    }
    else if (type->order() == 2) {
      // TODO: Clearly we need something more sophisticated here (for sparse
      // tensors or nested dense tensors).  For example, a tensor type could
      // carry a 'TensorStorage' object and we could ask this TensorStorage to
      // give us an Expr that computes an i,j location, or an Expr that gives us
      // a row/column.
      Expr tensor = mutate(op->tensor);

      Expr i = mutate(op->indices[0]);
      Expr j = mutate(op->indices[1]);

      IndexDomain dim1 = type->dimensions[1];
      Expr d1;
      if (dim1.getIndexSets().size() == 1 &&
          dim1.getIndexSets()[0].getKind() == IndexSet::Range) {
        // TODO: Add support for unsigned ScalarTypes
        assert(dim1.getSize() < (size_t)(-1));
        int dimSize = static_cast<int>(dim1.getSize());
        d1 = Literal::make(i.type(), &dimSize);
      }
      else {
        NOT_SUPPORTED_YET;
      }
      assert(d1.defined());

      Expr index = Add::make(Mul::make(i, d1), j);
      Expr value = mutate(op->value);
      stmt = Store::make(tensor, index, value);
    }
    else {
      NOT_SUPPORTED_YET;
    }
  }
};


Func lowerTensorAccesses(Func func) {
  return LowerTensorAccesses().mutate(func);
}

}}
