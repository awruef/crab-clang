#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Analysis/CFG.h>
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

#include <crab/analysis/fwd_analyzer.hpp>

#include <boost/variant.hpp>

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

  }

  namespace domains { 
    typedef interval_domain<ikos::z_number,varname_t> z_interval_domain_t;
  }

  namespace analyzer {
    typedef intra_fwd_analyzer<cfg_impl::cfg_ref_t, domains::z_interval_domain_t> num_analyzer_t;
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
    // Just do the guard, for now. 
  } else if (const BinaryOperator *BO = dyn_cast<const BinaryOperator>(S)) {
    return makeVarForExpr(BO, curvn, vf, b);
  } else {
    S->dump();
    llvm_unreachable("Unsupported statement");
  }

  return res;
}

class GVisitor : public RecursiveASTVisitor<GVisitor> {
private:
	ASTContext                        *Ctx;
  variable_factory_t                vfac;
  set<shared_ptr<cfg_t>>  cfgs;

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

    vector<z_var> cp;
    vector<z_var> rp; 

    for (const auto &p : FD->parameters())
      cp.push_back(toP(p));
    auto returnV = toR(FD->getReturnType());
    rp.push_back(returnV);

    // Make a function declaration. 
    function_decl<z_number, varname_t> CD(FD->getNameAsString(), cp, rp);
    shared_ptr<cfg_t> c(new cfg_t(label(entry), label(exit), CD));

    unsigned  varPrefix = 0;
    // One pass to make new blocks in the crab cfg. 
    for (const auto &b : *cfg) 
      c->insert(label(*b));
    
    // Create the initial structure: havoc the return values, and return them
    // at the end of the crab CFG.     
    // Iterate over the clang CFG, adding statements as appropriate. 
    basic_block_t &crab_entry = c->get_node(label(entry));
    basic_block_t &crab_exit = c->get_node(label(exit));
    crab_entry.havoc(returnV);
    crab_exit.ret(returnV);

    for (auto &b : *cfg) {
      basic_block_t &cur = c->get_node(label(*b));

      // Walk every statement in the basic block. 
      for (auto &s : *b) 
        if (Optional<CFGStmt>   orStmt = s.getAs<CFGStmt>()) 
          walkStmt(orStmt.getValue().getStmt(), varPrefix, vfac, cur);
      
      Stmt *Term = b->getTerminatorCondition(true);
      lin_cst_t TermConstraint;
      if (Term) {
        CResult r = walkStmt(Term, varPrefix, vfac, cur);

        if (r.which() == CONSTRAINT) 
          TermConstraint = boost::get<lin_cst_t>(r);
        else 
          llvm_unreachable("Should have got a constraint!");
      }

      // Walk the statements in this node. 

      bool didIf = false;
      bool didElse = false;
      // Update the structure of the CFG, with branches. 
      for (const auto &s : b->succs()) {
        basic_block_t &sb = c->get_node(label(*s));
        basic_block_t &assume_b = c->insert(label(*b) + "_to_" + label(*s));

        // Update the structure of the CFG. 
        cur >> assume_b;
        assume_b >> sb;

        // Is this successor the true or the false case? 
        // If it's the true case, assume the terminator. 
        // If it's the false case, assume the negation of the terminator.
        // Note that these invariants come from clang itself. 
        if (Term) {
          if (didIf == false) {
            didIf = true;
            assume_b.assume(TermConstraint);
          } else if (didElse == false) {
            didElse = false;
            assume_b.assume(TermConstraint.negate());
          } else {
            llvm_unreachable("Don't deal with more than two successors right now.");
          }
        }
      }
    }

    return c;
  } 

public:
	explicit GVisitor(ASTContext *C) : Ctx(C) {} 

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
          crab::outs() << *crabCfg;

        cfgs.insert(crabCfg);
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

  // Run analyzer. 
  for (auto &c : V.getCfgs()) {
    analyzer::num_analyzer_t a(*c, z_interval_domain_t::top()); 
    a.run();

    for (auto &b : *c) {
			auto pre = a.get_pre (b.label ());
    	auto post = a.get_post (b.label ());
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
