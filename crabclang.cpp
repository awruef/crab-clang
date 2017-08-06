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

// Define types for crab. 
namespace crab {
  namespace cfg_impl {
    typedef cfg::var_factory_impl::str_variable_factory variable_factory_t;
    typedef typename variable_factory_t::varname_t varname_t;
    typedef std::string basic_block_label_t;
    template<> inline std::string get_label_str(std::string e) { return e; }

    typedef cfg::Cfg<basic_block_label_t, varname_t, ikos::z_number> cfg_t;
    typedef cfg::cfg_ref<cfg_t> cfg_ref_t;
    typedef cfg::cfg_rev<cfg_ref_t> cfg_rev_t;
    typedef cfg_t::basic_block_t basic_block_t;
    typedef ikos::variable<ikos::z_number, varname_t> z_var;
    typedef ikos::linear_expression<ikos::z_number, varname_t> lin_t;
    typedef ikos::linear_constraint<ikos::z_number, varname_t> lin_cst_t;
  }
}

using namespace clang::driver;
using namespace clang::tooling;
using namespace clang;
using namespace llvm; 
using namespace crab;
using namespace crab::cfg_impl;
using namespace crab::cfg;

static cl::OptionCategory CrabCat("crabclang options");
cl::opt<bool> Verbose("verbose",
                      cl::desc("Print verbose information"),
                      cl::init(false),
                      cl::cat(CrabCat));

template <typename T>
class GenericAction : public ASTFrontendAction {
public:
  GenericAction() {} 

  virtual std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &Compiler, StringRef InFile) {
    return std::unique_ptr<ASTConsumer>(new T(InFile, &Compiler.getASTContext()));
  }
};

class EVisitor : public RecursiveASTVisitor<EVisitor> {
  lin_t result;
  variable_factory_t  &vfac;
  public:

  explicit EVisitor(variable_factory_t &v) : vfac(v) { }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    result = z_var(vfac[DRE->getDecl()->getNameAsString()]);
    return false;
  }

  lin_t getResult() {
    return result;
  }
};

class SVisitor : public RecursiveASTVisitor<SVisitor> {
  lin_cst_t           result;
  variable_factory_t  &vfac;
  public:

  explicit SVisitor(variable_factory_t &v) : vfac(v) { }

  bool VisitBinGT(BinaryOperator *op) {
    EVisitor lhsv(vfac);
    EVisitor rhsv(vfac);
    lhsv.TraverseStmt(op->getLHS());
    rhsv.TraverseStmt(op->getRHS());

    result = (lhsv.getResult() - 1) >= rhsv.getResult();
    return false;
  }

  bool VisitBinGE(BinaryOperator *op) {
    EVisitor lhsv(vfac);
    EVisitor rhsv(vfac);
    lhsv.TraverseStmt(op->getLHS());
    rhsv.TraverseStmt(op->getRHS());

    result = lhsv.getResult() >= rhsv.getResult();
    return false;
  }

  bool VisitBinLE(BinaryOperator *op) {
    EVisitor lhsv(vfac);
    EVisitor rhsv(vfac);
    lhsv.TraverseStmt(op->getLHS());
    rhsv.TraverseStmt(op->getRHS());

    result = lhsv.getResult() <= rhsv.getResult();
    return false;
  }

  bool VisitBinLT(BinaryOperator *op) {
    EVisitor lhsv(vfac);
    EVisitor rhsv(vfac);
    lhsv.TraverseStmt(op->getLHS());
    rhsv.TraverseStmt(op->getRHS());

    result = lhsv.getResult() <= (rhsv.getResult() - 1);
    return false;
  }

  lin_cst_t getResult() {
    return result;
  }
};

void walkStmt(const Stmt *S, variable_factory_t &vf, basic_block_t &b) {
  if (const ReturnStmt *RS = dyn_cast<ReturnStmt>(S)) {
    // Return statement case. 
    if (const Expr *E = RS->getRetValue()) {
      if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
        z_var nv = vf[DRE->getDecl()->getNameAsString()];
        z_var rv = vf["__CRAB_return"];
        b.assign(rv, nv);
      }
    }
  } else {
    llvm_unreachable("Unsupported statement");
  }
}

class GVisitor : public RecursiveASTVisitor<GVisitor> {
private:
	ASTContext          *Ctx;
  variable_factory_t  vfac;

  // Make a string label for a basic block.
  std::string label(const CFGBlock &b) const {
    std::ostringstream  oss;
    oss << "BB#";
    oss << b.getBlockID();
    return oss.str();
  }
  
  // Convert a clang type to a crab type. 
  variable_type clangToCrabTy(QualType T) const {
    if (T->isPointerType())
      return PTR_TYPE;
    else if (T->isBooleanType())
      return BOOL_TYPE;
    else if (T->isIntegerType())
      return INT_TYPE;
    else
      return UNK_TYPE;

  }

  // Convert a clang ParmVarDecl into a crab declaration pair. 
  std::pair<varname_t, variable_type> toP(ParmVarDecl *pvd) {
    return std::pair<varname_t, variable_type>
      (vfac[pvd->getNameAsString()], clangToCrabTy(pvd->getType()));
  }

  std::pair<varname_t, variable_type> toR(QualType returnType) {
    return std::pair<varname_t, variable_type>
      (vfac["__CRAB_return"], clangToCrabTy(returnType)); 
  }

  // Convert a clang CFG into a crab CFG.
  std::shared_ptr<cfg_t> 
    toCrab( std::unique_ptr<CFG>      &cfg,
            FunctionDecl              *FD,
            std::vector<ParmVarDecl*> params) 
  {
    std::vector<std::pair<varname_t,variable_type> >  cparams;
    std::vector<std::pair<varname_t,variable_type> >  rparams; 

    for (const auto &p : params) 
      cparams.push_back(toP(p));

    // For now, CRAB functions only return the values their C versions do.
    std::pair<varname_t, variable_type> returnV = toR(FD->getReturnType());
    rparams.push_back(returnV);

    CFGBlock  &entry = cfg->getEntry();
    CFGBlock  &exit = cfg->getExit();
    std::shared_ptr<cfg_t>  c(new cfg_t(label(entry), label(exit)));

    // One pass to make new blocks in the crab cfg. 
    for (const auto &b : *cfg) 
      c->insert(label(*b));

    // Create the initial structure: havoc the return values, and return them
    // at the end of the crab CFG.     
    basic_block_t &crab_entry = c->get_node(label(entry));
    basic_block_t &crab_exit = c->get_node(label(exit));
    crab_entry.havoc(returnV.first);
    crab_exit.ret(returnV.first, returnV.second);

    // Iterate over the clang CFG, adding statements as appropriate. 
    for (auto &b : *cfg) {
      basic_block_t &cur = c->get_node(label(*b));

      // Get the terminator statement from the clang CFG. 
      Stmt *Term = b->getTerminatorCondition(true);
      SVisitor TermSV(vfac);
      if (Term) 
        TermSV.TraverseStmt(Term);
      
      bool didIf = false;
      bool didElse = false;
      // Update the structure of the CFG, with branches. 
      for (const auto &s : b->succs()) {
        basic_block_t &sb = c->get_node(label(*s));
        cur >> sb;

        // Is this successor the true or the false case? 
        // If it's the true case, assume the terminator. 
        // If it's the false case, assume the negation of the terminator.
        if (Term) {
          lin_cst_t TermConstraint = TermSV.getResult();

          if (didIf == false) {
            didIf = true;
            sb.assume(TermConstraint);
          } else if (didElse == false) {
            didElse = false;
            sb.assume(TermConstraint.negate());
          } else {
            llvm_unreachable("Don't deal with more than two successors right now.");
          }
        }
      }
    }

    for (auto &b : *cfg) {
      basic_block_t &cur = c->get_node(label(*b));
      // Iterate over the statements in the current node. 
      for (auto &s : *b) {
        const Stmt *St = nullptr;
        switch(s.getKind()) {
          case CFGElement::Statement:
            St = s.castAs<CFGStmt>().getStmt();
            // It would be super nice to be able to use a RecursiveASTVisitor 
            // here, however, we can't strip the const off of St without 
            // bad stuff happening and the visitors aren't const. 
            walkStmt(St, vfac, cur);
            break;
          default:
            llvm_unreachable("Unsupported CFGElement type");
        }
      }
    }


    function_decl<varname_t>  decl(vfac[FD->getNameAsString()], cparams, rparams);
    c->set_func_decl(decl);
    return c;
  } 

public:
	explicit GVisitor(ASTContext *C) : Ctx(C) {} 

  bool VisitFunctionDecl(FunctionDecl *D) {
    variable_factory_t  vars;

    if (D->hasBody() && D->isThisDeclarationADefinition()) {
      CFG::BuildOptions BO;
      std::unique_ptr<CFG>  cfg = CFG::buildCFG(D, D->getBody(), Ctx, BO);

      if (cfg ) {
        cfg->dump(Ctx->getLangOpts(), true);
        std::shared_ptr<cfg_t>  crabCfg = toCrab( cfg, 
                                                  D, 
                                                  D->parameters());

        crab::outs() << *crabCfg;
      }
    }

    return true;
  }
};

class CFGBuilderConsumer : public ASTConsumer {
public:
	explicit CFGBuilderConsumer(StringRef File, ASTContext *C) : Ctx(C), File(File) {}

  virtual void HandleTranslationUnit(ASTContext &);

private:
	ASTContext *Ctx;
	std::string	File;
};

void CFGBuilderConsumer::HandleTranslationUnit(ASTContext &C) {
  GVisitor	V(&C);

  for (const auto &D : C.getTranslationUnitDecl()->decls()) 
    V.TraverseDecl(D);

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

	std::unique_ptr<ToolAction>	Action = newFrontendActionFactory<CFGBuildAction>();

  if (Action) 
		Tool.run(Action.get());
  else
    llvm_unreachable("No action!");

  return 0;
}
