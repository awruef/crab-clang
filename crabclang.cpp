#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Analysis/CFG.h>
#include <clang/Analysis/Analyses/PostOrderCFGView.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/TargetSelect.h>

#include <crab/config.h>
#include <crab/cfg/cfg.hpp>
#include <crab/cfg/var_factory.hpp>

#include <crab/domains/split_dbm.hpp>
#include <crab/domains/intervals.hpp>
#include <crab/domains/apron_domains.hpp>

#include <crab/analysis/fwd_analyzer.hpp>

#include <boost/variant.hpp>
#include <boost/bimap.hpp>

using namespace std;

// Define types for crab. 
namespace crab {
  typedef cfg::var_factory_impl::str_variable_factory variable_factory_t;
  typedef typename variable_factory_t::varname_t varname_t;

  namespace cfg_impl {
    typedef string basic_block_label_t;
    template<> inline string get_label_str(string e) { return e; }

    typedef cfg::Cfg<basic_block_label_t, varname_t, ikos::z_number> cfg_t;
    typedef cfg::cfg_ref<cfg_t> cfg_ref_t;
    typedef cfg::cfg_rev<cfg_ref_t> cfg_rev_t;
    typedef cfg_t::basic_block_t basic_block_t;
    typedef ikos::variable<ikos::z_number, varname_t> z_var;
    typedef ikos::linear_expression<ikos::z_number, varname_t> lin_t;
    typedef ikos::linear_constraint<ikos::z_number, varname_t> lin_cst_t;
    typedef ikos::linear_constraint_system<ikos::z_number, varname_t> lin_cst_sys_t;

  }

  namespace domains { 
    typedef interval_domain<ikos::z_number,varname_t>       z_interval_domain_t;
    typedef apron_domain<ikos::z_number,varname_t,APRON_PK> z_apron_domain_t;
  }

  namespace analyzer {
    typedef intra_fwd_analyzer<cfg_impl::cfg_ref_t, domains::z_interval_domain_t> num_analyzer_t;
    typedef intra_fwd_analyzer<cfg_impl::cfg_ref_t, domains::z_apron_domain_t> apron_analyzer_t;
  }
}

using namespace clang::driver;
using namespace clang::tooling;
using namespace clang;
using namespace llvm; 
using namespace crab;
using namespace crab::cfg_impl;
using namespace crab::cfg;
using namespace crab::domains;
using namespace ikos;

static cl::OptionCategory CrabCat("crabclang options");
cl::opt<bool> Verbose("verbose",
                      cl::desc("Print verbose information"),
                      cl::init(false),
                      cl::cat(CrabCat));

template <typename T>
class GenericAction : public ASTFrontendAction {
public:
  GenericAction() {} 

  virtual unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &Compiler, StringRef InFile) {
    return unique_ptr<ASTConsumer>(new T(InFile, &Compiler.getASTContext()));
  }
};

typedef boost::variant<boost::blank, lin_t, lin_cst_t> CResult;
typedef enum _CResultI {
  EMPTY,
  EXPR,
  CONSTRAINT
} CResultI;

// Convert a clang type to a crab type. 
variable_type clangToCrabTy(QualType T) {
  if (T->isPointerType())
    return PTR_TYPE;
  else if (T->isBooleanType())
    return BOOL_TYPE;
  else if (T->isIntegerType())
    return INT_TYPE;
  else
    return UNK_TYPE;
}

// Make a temporary variable name based on the current number and a 
// specified base name. 
string mkTempVarName(unsigned &curVn, string baseName) {
  ostringstream oss;
  oss << curVn++ << baseName;
  return oss.str();
}

// Many exprs produce temporaries. Those temporaries should be reflected as 
// variables in the CRAB CFG. Destruct an expr, E, and create temporaries
// and operations on those temporaries as appropriate. 
CResult makeVarForExpr(const Expr *E, unsigned &curVn, variable_factory_t &vf, basic_block_t &b) {
  // Destruct E and figure out what we need to do for it. 
  // This is basically a visitor pattern, but for const expr. 
  lin_t               v;
  CResult             res;
  
  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
    // Construct a binary operation from lhs, rhs of BO. 
    CResult lhs = makeVarForExpr(BO->getLHS()->IgnoreParenImpCasts(), curVn, vf, b);
    CResult rhs = makeVarForExpr(BO->getRHS()->IgnoreParenImpCasts(), curVn, vf, b);

    z_var   var(vf[mkTempVarName(curVn, "_temp")], clangToCrabTy(BO->getType()), 32);
    v =     lin_t(var);

    if ((lhs.which() == EXPR) && (rhs.which() == EXPR)) {
      lin_t lhse = boost::get<lin_t>(lhs);
      lin_t rhse = boost::get<lin_t>(rhs);

      switch(BO->getOpcode()) {
        case BO_Add:
          b.assign(var, lhse + rhse);
          res = CResult(v);
          break;
        case BO_Sub:
          b.assign(var, lhse - rhse);
          res = CResult(v);
          break;
        // In these cases, we make whole constraints instead of expressions. 
        case BO_LT:
          res = CResult(lhse <= (rhse-1));
          break;
        case BO_LE:
          res = CResult(lhse <= rhse);
          break;
        case BO_GT:
          res = CResult((lhse-1) >= rhse);
          break;
        case BO_GE:
          res = CResult(lhse >= rhse);
          break;
        default:
          llvm_unreachable("Unsupported opcode!");
      } 
    } else {
      llvm_unreachable("Unsupported lhs/rhs operand types!");
    }
  } 
  else if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    // Look up the variable referenced by DRE, and give it back. 
    res = CResult(lin_t(z_var(vf[DRE->getDecl()->getNameAsString()], clangToCrabTy(E->getType()), 32)));
  } 
  else if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E)) {
    // Give back the constant value by converting the APInt to an mpz. 
    res = CResult(lin_t(z_number(IL->getValue().getSExtValue())));
  }
  else if (const CallExpr *CE = dyn_cast<CallExpr>(E)) {
    // Look up the symbol we're calling. 
    llvm_unreachable("Not Implemented");
  }
  else {
    llvm_unreachable("Not Implemented");
  }

  return res;
}

CResult walkStmt(const Stmt *S, unsigned &curvn, variable_factory_t &vf, basic_block_t &b) {
  CResult res; 
  if (const ReturnStmt *RS = dyn_cast<const ReturnStmt>(S)) {
    // Return statement case. 
    if (const Expr *E = RS->getRetValue()) {
      CResult v = makeVarForExpr(E->IgnoreParenImpCasts(), curvn, vf, b);
      if (v.which() == EXPR) {
        z_var rv(vf["__CRAB_return"], clangToCrabTy(E->getType()), 32);
        b.assign(rv, boost::get<lin_t>(v));
      } else {
        llvm_unreachable("Did not get expr from a return values expr!");
      }
    }
  } else if (const IfStmt *I = dyn_cast<const IfStmt>(S)) {
    llvm_unreachable("We shouldn't get here");
  } else if (const BinaryOperator *BO = dyn_cast<const BinaryOperator>(S)) {
    return makeVarForExpr(BO, curvn, vf, b);
  } else if (const ImplicitCastExpr *ICE = dyn_cast<const ImplicitCastExpr>(S)) {
    return walkStmt(ICE->getSubExpr(), curvn, vf, b);
  } else if (const DeclRefExpr *DRE = dyn_cast<const DeclRefExpr>(S)) {
    return makeVarForExpr(DRE, curvn, vf, b);
  } else {
    S->dump();
    llvm_unreachable("Unsupported statement");
  }

  return res;
}

// This visits statements in the clang AST in post-order. 
// As it encounters expressions, it translates those expressions 
// into crab, printing them into the current basic block. 

class SVisitor : public RecursiveASTVisitor<SVisitor> {
public:
  typedef boost::bimap<z_var,Stmt *>        CrabClangBimap;
  typedef std::map<const Stmt *,lin_cst_t>  SCMap;
private:
  basic_block_t       &Current;
  unsigned            &varPrefix;
  CrabClangBimap      &CCB;
  SCMap               &SCM;
  variable_factory_t  &vfac;
  SourceManager       &SM;

  typedef boost::variant<boost::blank, z_var, z_number> SResult;
  typedef enum _SResultI { EMPTY, VAR, NUM} SResultI;

  z_var varFromDecl(ValueDecl *VD) {
    SourceRange VDSource = VD->getSourceRange();
    bool        inv = false;
    unsigned i = SM.getSpellingLineNumber(VDSource.getBegin(), &inv);
    auto VarName = VD->getNameAsString();
    return z_var(vfac[VarName+"_"+to_string(i)], INT_TYPE, 32);
  }

  SResult getResult(Expr *E) {
    auto *tE = E->IgnoreParenImpCasts();
    // Is E a direct variable reference of something? Just return the variable.
    if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(tE)) {
      return SResult(varFromDecl(DRE->getDecl()));
    } else if (IntegerLiteral *IL = dyn_cast<IntegerLiteral>(tE)) {
      // Don't need to do anything, just give back the number.
      return SResult(z_number(IL->getValue().getSExtValue()));
    }

    // If it is not, then we need to find the statement that produced it 
    // somewhere. It should have already been produced and stored in the 
    // bimap.  
    auto I = CCB.right.find(E);
    // We should be able to find it, if we can't, there has been an error.
    assert(I != CCB.right.end());
    return SResult(I->second);
  }

  lin_t unwrap(SResult r) {
    switch(r.which()) {
      case EMPTY:
        assert(!"Trying to unwrap an empty value");
        break;
      case VAR:
        return lin_t(boost::get<z_var>(r));
        break;
      case NUM:
        return lin_t(boost::get<z_number>(r));
        break;
    }
    llvm_unreachable("Should never get here");
  }

public:
  SVisitor( basic_block_t       &C, 
            unsigned            &V, 
            CrabClangBimap      &B, 
            SCMap               &M,
            variable_factory_t  &F,
            SourceManager       &S) : 
    Current(C),varPrefix(V),CCB(B),SCM(M),vfac(F),SM(S) { } 

  bool shouldTraversePostOrder() const { return true; }

  bool VisitVarDecl(VarDecl *Var) {
    z_var v = varFromDecl(Var);

    if (Var->hasInit()) 
      Current.assign(v, unwrap(getResult(Var->getInit())));
    else 
      Current.havoc(v);

    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    auto LHS = unwrap(getResult(BO->getLHS()));
    auto RHS = unwrap(getResult(BO->getRHS()));

    // Store the result in a fresh temporary. 
    z_var         res = z_var(vfac[mkTempVarName(varPrefix, "_tmp")], INT_TYPE, 32);
    switch(BO->getOpcode()) {
      case BO_Mul:
      case BO_Shr:
      case BO_Shl: 
      case BO_Rem:
      case BO_Div:
        Current.havoc(res); // TODO: we can write this in to the crab CFG but 
                            //       not as expressions.
        CCB.insert(CrabClangBimap::value_type(res, BO));
        break;
      // These generate assignment statements of some sort. 
      case BO_Add:
        Current.assign(res, LHS + RHS);
        CCB.insert(CrabClangBimap::value_type(res, BO));
        break;
      case BO_Sub:
        Current.assign(res, LHS - RHS);
        CCB.insert(CrabClangBimap::value_type(res, BO));
        break;
      case BO_AddAssign:
        Current.assign(res, LHS + RHS);
        Current.assign(boost::get<z_var>(getResult(BO->getLHS())), LHS + RHS);
        CCB.insert(CrabClangBimap::value_type(res, BO));
        break;
      case BO_Assign:
        Current.assign(res, RHS);
        Current.assign(boost::get<z_var>(getResult(BO->getLHS())), res);
        CCB.insert(CrabClangBimap::value_type(res, BO));
        break;
      // These generate constraints rather than statements. 
      case BO_LT:
        SCM.insert(make_pair(BO, LHS < RHS));
        break;
      case BO_LE:
        SCM.insert(make_pair(BO, LHS <= RHS));
        break;
      case BO_GT:
        SCM.insert(make_pair(BO, LHS > RHS));
        break;
      case BO_GE:
        SCM.insert(make_pair(BO, LHS >= RHS));
        break;
      case BO_EQ:
        SCM.insert(make_pair(BO, LHS == RHS));
        break;
      case BO_NE:
        SCM.insert(make_pair(BO, LHS != RHS));
        break;
      default:
        llvm_unreachable("Unhandled opcode");
    } 

    return true;
  }

  // If a return value is present, write it into the functions
  // return slot. 
  bool VisitReturnStmt(ReturnStmt *R) {
    if (Expr *ReturnStmt = R->getRetValue()) {
      SResult RV = getResult(ReturnStmt);
      z_var  rv(vfac["__CRAB_return"], 
                clangToCrabTy(ReturnStmt->getType()), 32); 

      switch(RV.which()) {
        case EMPTY:
          break;
        case VAR:
          Current.assign(rv, boost::get<z_var>(RV));
          break;
        case NUM:
          Current.assign(rv, boost::get<z_number>(RV));
          break;
      }
    }

    return true;
  }
};

class GVisitor : public RecursiveASTVisitor<GVisitor> {
private:
	ASTContext                *Ctx;
  variable_factory_t        vfac;
  set<shared_ptr<cfg_t>>    cfgs;
  SVisitor::CrabClangBimap  CCB;

  // Make a string label for a basic block.
  string label(const CFGBlock &b) const {
    ostringstream  oss;
    oss << "BB#";
    oss << b.getBlockID();
    return oss.str();
  }
  
  // Convert a clang ParmVarDecl into a crab declaration pair. 
  z_var toP(ParmVarDecl *pvd) {
    return z_var( vfac[pvd->getNameAsString()], 
                  clangToCrabTy(pvd->getType()), 32);
  }

  z_var toR(QualType returnType) {
    return z_var(vfac["__CRAB_return"], clangToCrabTy(returnType), 32); 
  }

  // Convert a clang CFG into a crab CFG.
  shared_ptr<cfg_t> toCrab(unique_ptr<CFG> &cfg, FunctionDecl *FD) {
    auto &entry = cfg->getEntry();
    auto &exit = cfg->getExit();

    SVisitor::SCMap   SCM;
    vector<z_var>     cp;
    vector<z_var>     rp;
    PostOrderCFGView  POCV(cfg.get());

    for (const auto &p : FD->parameters())
      cp.push_back(toP(p));
    auto returnV = toR(FD->getReturnType());
    rp.push_back(returnV);

    function_decl<z_number, varname_t> CD(FD->getNameAsString(), cp, rp);
    shared_ptr<cfg_t> c(new cfg_t(label(entry), label(exit), CD));

    unsigned  varPrefix = 0;
    // One pass to make new blocks in the crab cfg. 
    for (const auto &b : POCV) 
      c->insert(label(*b));
    
    basic_block_t &crab_entry = c->get_node(label(entry));
    basic_block_t &crab_exit = c->get_node(label(exit));
    crab_entry.havoc(returnV);
    crab_exit.ret(returnV);

    for (const auto &b : POCV) {
      basic_block_t &cur = c->get_node(label(*b));
      SVisitor      S(cur, varPrefix, CCB, SCM, vfac, Ctx->getSourceManager());
     
      for (const auto &s : *b) 
        if (Optional<CFGStmt>   orStmt = s.getAs<CFGStmt>()) 
          S.TraverseStmt((Stmt *)orStmt.getValue().getStmt());
  
      vector<basic_block_t *> succs;
      // Add conditional control flow. 
      for (const auto &s : b->succs()) 
        if (s) {
          basic_block_t &sb = c->get_node(label(*s));
          basic_block_t &assume_b = c->insert(label(*b) + "_to_" + label(*s));

          // Update the structure of the CFG. 
          cur >> assume_b;
          assume_b >> sb;
          succs.push_back(&assume_b);
        } else {
          succs.push_back(nullptr);
        }

      // Switch on the form of the terminator, if present. 
      if (const Stmt *Term = b->getTerminator().getStmt()) {
        if (isa<WhileStmt>(Term) || isa<IfStmt>(Term)) {
          const Stmt *TS = b->getTerminatorCondition();
          auto I = SCM.find(TS);
          if (I == SCM.end()) 
            if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(TS)) 
              I = SCM.find(BO->getRHS());
          assert(I != SCM.end());

          if (succs[0])
            succs[0]->assume(I->second);

          if (succs[1])
            succs[1]->assume(I->second.negate());
        } else if (const SwitchStmt *Switch = dyn_cast<SwitchStmt>(Term)) {
          // In each of the successor blocks, assume that the crab variable
          // for the terminating statement is equal to the lable. 
        } else if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Term)) {
          // Sometimes, the terminator is BinaryOperator for && or ||.
          auto I = SCM.find(BO->getLHS());
          assert(I != SCM.end());

          if (succs[0])
            succs[0]->assume(I->second);

          if (succs[1])
            succs[1]->assume(I->second.negate());
        } else {
          Term->dump();
          llvm_unreachable("Unknown terminator type!");
        }
      }
    }
    
    if (c) 
      c->simplify();

    return c;
  }

public:
	explicit GVisitor(ASTContext *C) : Ctx(C) {} 

  SVisitor::CrabClangBimap  getMap(void) { return CCB; }

  bool VisitFunctionDecl(FunctionDecl *D) {
    variable_factory_t  vars;

    if (D->hasBody() && D->isThisDeclarationADefinition()) {
      CFG::BuildOptions BO;
      unique_ptr<CFG>  cfg = CFG::buildCFG(D, D->getBody(), Ctx, BO);

      if (cfg ) {
        if (Verbose)
          cfg->dump(Ctx->getLangOpts(), true);
        shared_ptr<cfg_t> crabCfg = toCrab(cfg, D);

        if (Verbose)
          if (crabCfg)
            crab::outs() << *crabCfg;
        
        if (crabCfg) {
          cfgs.insert(crabCfg);
        } else {
          crab::errs() << "Could not get CFG for function ";
          crab::errs()  << D->getNameAsString() << "\n";
        }
      }
    }

    return true;
  }

  set<shared_ptr<cfg_t>> getCfgs() { return cfgs; }
};

class CFGBuilderConsumer : public ASTConsumer {
public:
	explicit CFGBuilderConsumer(StringRef File, ASTContext *C) : Ctx(C), File(File) {}

  virtual void HandleTranslationUnit(ASTContext &);

private:
	ASTContext *Ctx;
	string	File;
};

void CFGBuilderConsumer::HandleTranslationUnit(ASTContext &C) {
  GVisitor	V(&C);

  // Build up CFG. 
  for (const auto &D : C.getTranslationUnitDecl()->decls()) 
    V.TraverseDecl(D);

  auto CCB = V.getMap();
  // Run analyzer. 
  for (auto &c : V.getCfgs()) {
    analyzer::apron_analyzer_t aa(*c, z_apron_domain_t::top()); 
    aa.run();

    for (auto &b : *c) {
			auto pre = aa.get_pre (b.label ());
    	auto post = aa.get_post (b.label ());
  
     	crab::outs() << get_label_str (b.label ()) << "="
               << pre
               << " ==> "
               << post << "\n";
    }
  }

  return;
}

typedef GenericAction<CFGBuilderConsumer> CFGBuildAction;

int main(int argc, const char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);

  // Initialize targets for clang module support.
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  CommonOptionsParser OptionsParser(argc, argv, CrabCat);
  tooling::CommandLineArguments args = OptionsParser.getSourcePathList();
  ClangTool Tool(OptionsParser.getCompilations(), args);

	unique_ptr<ToolAction>	Action = newFrontendActionFactory<CFGBuildAction>();

  if (Action) 
		Tool.run(Action.get());
  else
    llvm_unreachable("No action!");

  return 0;
}
