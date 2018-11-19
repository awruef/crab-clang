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

#include <optional>

using namespace std;

// Define types for crab. 
namespace crab {
  typedef cfg::var_factory_impl::str_variable_factory variable_factory_t;
  typedef typename variable_factory_t::varname_t varname_t;

  namespace cfg_impl {
    typedef string basic_block_label_t;
    template<> inline string get_label_str(string e) { return e; }

    typedef cfg::cfg<basic_block_label_t, varname_t, ikos::z_number> cfg_t;
    typedef cfg::cfg_ref<cfg_t> cfg_ref_t;
    typedef cfg::cfg_rev<cfg_ref_t> cfg_rev_t;
    typedef cfg_t::basic_block_t basic_block_t;
    typedef ikos::variable<ikos::z_number, varname_t> z_var;
    typedef cfg::function_decl<ikos::z_number, varname_t> z_func;
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

// This visits statements in the clang AST in post-order. 
// As it encounters expressions, it translates those expressions 
// into crab, printing them into the current basic block. 

class SVisitor : public RecursiveASTVisitor<SVisitor> {
public:
  typedef boost::bimap<z_var,const Stmt *>                    CrabClangBimap;
  typedef std::map<const Stmt *,lin_cst_t>                    SCMap;
  typedef std::map<string, const Stmt *>                      CrabStMap;
  typedef std::map<const FunctionDecl *,shared_ptr<z_func> >  FDToFunc;
private:
  basic_block_t       &Current;
  unsigned            &varPrefix;
  CrabClangBimap      &CCB;
  SCMap               &SCM;
  CrabStMap           &CSM;
  FDToFunc            &FTF;
  variable_factory_t  &vfac;
  SourceManager       &SM;

public:
  typedef boost::variant<boost::blank, z_var, z_number, lin_t> SResult;
  typedef enum _SResultI { EMPTY, VAR, NUM, LIN} SResultI;

  z_var varFromDecl(const ValueDecl *VD) {
    const SourceRange VDSource = VD->getSourceRange();
    bool              inv = false;
    unsigned          i = SM.getSpellingLineNumber(VDSource.getBegin(), &inv);
    auto              VarName = VD->getNameAsString();

    return z_var(vfac[VarName+"_"+to_string(i)], INT_TYPE, 32);
  }

  SResult getResult(const Expr *E) {
    auto *tE = E->IgnoreParenImpCasts();
    // Is E a direct variable reference of something? Just return the variable.
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(tE)) {
      return SResult(varFromDecl(DRE->getDecl()));
    } else if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(tE)) {
      // Don't need to do anything, just give back the number.
      return SResult(z_number(IL->getValue().getSExtValue()));
    } else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(tE)) {
      auto sE = getResult(UO->getSubExpr());
      assert(sE.which() != EMPTY);
      switch(sE.which()) {
        case EMPTY:
          llvm_unreachable("infeasible");
          break;
        case NUM:
          return SResult(z_number(-1)*boost::get<z_number>(sE));
          break;
        case VAR:
          return SResult(z_number(-1)*boost::get<z_var>(sE));
          break;
        case LIN:
          return SResult(z_number(-1)*boost::get<lin_t>(sE));
          break;
      }
    }

    // If it is not, then we need to find the statement that produced it 
    // somewhere. It should have already been produced and stored in the 
    // bimap.  
    auto I = CCB.right.find(tE);
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
      case LIN:
        return boost::get<lin_t>(r);
        break;
    }
    llvm_unreachable("Should never get here");
  }

  SVisitor( basic_block_t       &C, 
            unsigned            &V, 
            CrabClangBimap      &B, 
            SCMap               &M,
            CrabStMap           &U,
            FDToFunc            &FM,
            variable_factory_t  &F,
            SourceManager       &S) : 
    Current(C),varPrefix(V),CCB(B),SCM(M),CSM(U),FTF(FM),vfac(F),SM(S) { } 

  bool shouldTraversePostOrder() const { return true; }

  bool VisitVarDecl(VarDecl *Var) {
    z_var v = varFromDecl(Var);

    if (Var->hasInit()) 
      Current.assign(v, unwrap(getResult(Var->getInit())));
    else 
      Current.havoc(v);

    return true;
  }

  bool VisitCallExpr(CallExpr *Call) {
    // This only works if there is a FunctionDecl on the other end of this Call
    Call->dump();
    FunctionDecl *CalledFunction = Call->getDirectCallee();
    assert(CalledFunction != nullptr);
    CalledFunction->dump();
    llvm_unreachable("NIY");
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    auto uLHS = getResult(BO->getLHS());
    auto uRHS = getResult(BO->getRHS());
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
        Current.assign(boost::get<z_var>(uLHS), res);
        CCB.insert(CrabClangBimap::value_type(res, BO));
        break;
      case BO_Assign:
        Current.assign(res, RHS);
        Current.assign(boost::get<z_var>(uLHS), res);
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
  SVisitor::SCMap           SCM;
  SVisitor::CrabStMap       CSM;
  SVisitor::FDToFunc        FTF;

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

    vector<z_var>     cp;
    vector<z_var>     rp;
    PostOrderCFGView  POCV(cfg.get());

    for (const auto &p : FD->parameters())
      cp.push_back(toP(p));
    auto returnV = toR(FD->getReturnType());
    rp.push_back(returnV);

    shared_ptr<z_func> CD(new z_func(FD->getNameAsString(), cp, rp));
    if(!FTF.insert(make_pair(FD, CD)).second)
      CD = FTF[FD];
    shared_ptr<cfg_t> c(new cfg_t(label(entry), label(exit), *CD));

    unsigned  varPrefix = 0;
    // One pass to make new blocks in the crab cfg. 
    for (const auto &b : POCV) 
      c->insert(label(*b));
    
    basic_block_t &crab_entry = c->get_node(label(entry));
    basic_block_t &crab_exit = c->get_node(label(exit));

    // Set up the return values.
    crab_entry.havoc(returnV);
    crab_exit.ret(returnV);

    // Set up local copies of the parameters. 
    SVisitor Se(crab_entry, varPrefix, CCB, SCM, CSM, FTF, vfac, Ctx->getSourceManager());
    for (const auto &p : FD->parameters()) {
      z_var lp = Se.varFromDecl(p);
      z_var pp = toP(p);
      crab_entry.assign(lp, pp);
    }

    for (const auto &b : POCV) {
      basic_block_t &cur = c->get_node(label(*b));
      SVisitor      S(cur, varPrefix, CCB, SCM, CSM, FTF, vfac, Ctx->getSourceManager());

      // Populate the map from crab basic blocks to statements.  
      const Stmt *Beginning = nullptr;
      for (const auto &s : *b) 
        if (Optional<CFGStmt>   orStmt = s.getAs<CFGStmt>()) {
          const Stmt *St = orStmt.getValue().getStmt();
          S.TraverseStmt((Stmt *)St);
          if (Beginning == nullptr)
            Beginning = St;
        }
   
      if (Beginning)
        CSM.insert(make_pair(label(*b), Beginning));

      vector<basic_block_t *>   succs;
      vector<const CFGBlock *>  csuccs;
      // Add conditional control flow. 
      for (const auto &s : b->succs()) 
        if (s) {
          basic_block_t &sb = c->get_node(label(*s));
          basic_block_t &assume_b = c->insert(label(*b) + "_to_" + label(*s));

          // Update the structure of the CFG. 
          cur >> assume_b;
          assume_b >> sb;
          succs.push_back(&assume_b);
          csuccs.push_back(s);
        } else {
          succs.push_back(nullptr);
          csuccs.push_back(nullptr);
        }

      // Switch on the form of the terminator, if present. 
      if (const Stmt *Term = b->getTerminator().getStmt()) {
        if (isa<WhileStmt>(Term) || isa<IfStmt>(Term)) {
          const Stmt *Body = nullptr;
          const Stmt *Else = nullptr;
          if (const WhileStmt *WS = dyn_cast<WhileStmt>(Term)) {
            Body = WS->getBody();

            // If we have a successor in the 'else' case, use that to get the 
            // 'else' branch of the while loop, since it isn't in the AST. 
            if (csuccs[1])
              for (const auto &t : *csuccs[1])
                if (Optional<CFGStmt>   orStmt = t.getAs<CFGStmt>()) {
                  Else = orStmt.getPointer()->getStmt();
                  break;
                }
          } else if (const IfStmt *IS = dyn_cast<IfStmt>(Term)) {
            Body = IS->getThen();
            Else = IS->getElse();
          }

          const Stmt *TS = b->getTerminatorCondition();
          assert(TS != nullptr);
          auto I = SCM.find(TS);
          if (I == SCM.end()) 
            if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(TS)) 
              I = SCM.find(BO->getRHS());
          assert(I != SCM.end());

          if (succs[0]) {
            succs[0]->assume(I->second);
            if (Body)
              CSM.insert(make_pair(succs[0]->name(), Body));
          }

          if (succs[1]) {
            succs[1]->assume(I->second.negate());

            if (Else)
              CSM.insert(make_pair(succs[1]->name(), Else));
          }
        } else if (const SwitchStmt *Switch = dyn_cast<SwitchStmt>(Term)) {
          // In each of the successor blocks, assume that the crab variable
          // for the terminating statement is equal to the label. 
          vector<lin_cst_t> cases; 
          auto Cond = S.getResult(Switch->getCond());
          assert(Cond.which() != SVisitor::EMPTY);
          auto CondVar = S.unwrap(Cond);

          assert(succs.size() == csuccs.size());

          for (unsigned i = 0; i < succs.size(); i++) {
            auto Label = dyn_cast<CaseStmt>(csuccs[i]->getLabel());
            if (Label != nullptr) {
              assert(Label->getLHS() != nullptr);
              auto V = S.unwrap(S.getResult(Label->getLHS()));

              lin_cst_t c = (CondVar == V);
              cases.push_back(c);
              succs[i]->assume(c);
            } else {
              for (auto &C : cases)
                succs[i]->assume(C.negate());
            }
          }
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

  SVisitor::CrabStMap getMap(void) { return CSM; }

  bool VisitFunctionDecl(FunctionDecl *D) {
    variable_factory_t  vars;

    if (D->hasBody() && D->isThisDeclarationADefinition()) {
      CFG::BuildOptions BO;
      auto              cfg = CFG::buildCFG(D, D->getBody(), Ctx, BO);

      if (cfg) {
        if (Verbose) {
          D->dump();
          cfg->dump(Ctx->getLangOpts(), true);
        }
        auto crabCfg = toCrab(cfg, D);

        if (Verbose)
          if (crabCfg)
            crab::errs() << *crabCfg;
        
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

class AnalyzerSwitcher {
public:
  enum AnalyzerKind {
    Apron,
    Interval
  };

private:
  AnalyzerKind                _kind;
  analyzer::apron_analyzer_t  aa;
  analyzer::num_analyzer_t    na;
public:

  AnalyzerSwitcher(AnalyzerKind k, shared_ptr<cfg_t> cfg) : _kind(k),
    aa(*cfg, z_apron_domain_t::top()),
    na(*cfg, z_interval_domain_t::top()) { }

  void run() {
    switch(_kind) {
      case Apron:
        aa.run();
        break;
      case Interval:
        na.run();
        break;
    }
  }

  lin_cst_sys_t invariants_at_point(basic_block_label_t n) {
    switch(_kind) {
      case Apron:
        return aa.get_post(n).to_linear_constraint_system();
        break;
      case Interval:
        return na.get_post(n).to_linear_constraint_system();
        break;
    }

    return lin_cst_sys_t();
  }

  string invariants_to_c(const lin_cst_sys_t &csts);
  string invariant_to_c(const lin_cst_t &c);
  string expr_to_c(const lin_t &e);
};

string AnalyzerSwitcher::invariants_to_c(const lin_cst_sys_t &csts) {
  string result = "";

  for (auto &c : csts) {
    result = result + "," + invariant_to_c(c);
  }

  return result;
}

string AnalyzerSwitcher::invariant_to_c(const lin_cst_t &c) {
  string res = "";

  switch (c.kind()) {
    case lin_cst_t::EQUALITY:
      res = res + expr_to_c(c.expression()) + " = 0;";
      break;
    case lin_cst_t::STRICT_INEQUALITY:
      res = res + expr_to_c(c.expression())+ " < 0;";
      break;
    case lin_cst_t::INEQUALITY:
      res = res + expr_to_c(c.expression()) + " <= 0;";
      break;
    case lin_cst_t::DISEQUATION:
      res = res + expr_to_c(c.expression()) + " != 0";
      break;
  }

  return res;
}

string AnalyzerSwitcher::expr_to_c(const lin_t &e) {
  crab_string_os o;
  o << e;
  return o.str();
}


cl::opt<AnalyzerSwitcher::AnalyzerKind> AnalyzerDomain(
  "analyzer-domain",
	cl::desc("Choose analyzer domain:"),
  cl::init(AnalyzerSwitcher::Apron),
  cl::cat(CrabCat),
  cl::values(
    clEnumValN(AnalyzerSwitcher::Apron, "APRON", "Use the APRON domain"),
    clEnumValN(AnalyzerSwitcher::Interval, "Interavals", "Use the interval domain")));

void CFGBuilderConsumer::HandleTranslationUnit(ASTContext &C) {
  GVisitor	        V(&C);

  // Build up CFG. 
  for (const auto &D : C.getTranslationUnitDecl()->decls()) 
    V.TraverseDecl(D);

  auto          CSM = V.getMap();
  SourceManager &SM = Ctx->getSourceManager();
  Rewriter      R(SM, Ctx->getLangOpts());
  // Run analyzer. 
  for (auto &c : V.getCfgs()) {
    AnalyzerSwitcher  a(AnalyzerDomain, c);
    a.run();

    for (auto &b : *c) {
      auto post = a.invariants_at_point(b.label());
 
      const Stmt *St = nullptr;
      auto I = CSM.find(b.label());
      if (I != CSM.end())
        St = I->second;

      if (St) {
        SourceLocation sl = St->getLocStart();
        // Let's look at what kind of Stmt St is.
        St->dump();
        if (const IfStmt *If = dyn_cast<const IfStmt>(St)) {
          sl = If->getThen()->getLocStart();
        } else if (const CompoundStmt *Cmp = dyn_cast<const CompoundStmt>(St)) {
          sl = Cmp->getLBracLoc();
        }
        // Pretty print 'post' at 'St. 
        sl.dump(Ctx->getSourceManager());
        llvm::errs() << "\n";
        string ex = a.invariants_to_c(post);
        llvm::errs() << ex << "\n";
        //R.InsertTextBefore(sl, "/*"+ex+"*/\n");
      } 
    }
  }

  if (const RewriteBuffer *B = R.getRewriteBufferFor(SM.getMainFileID()))
    B->write(llvm::outs());
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
