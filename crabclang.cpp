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

using namespace clang::driver;
using namespace clang::tooling;
using namespace clang;
using namespace llvm; 

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

class GVisitor : public RecursiveASTVisitor<GVisitor> {
private:
	ASTContext *Ctx;

  // COnvert a clang expr into a crab expr. 

  // Convert a clang statement into a crab statement.

  // Convert a clang CFG into a crab CFG.
public:
	explicit GVisitor(ASTContext *C) : Ctx(C) {} 

  bool VisitFunctionDecl(FunctionDecl *D) {

    if (D->hasBody() && D->isThisDeclarationADefinition()) {
      CFG::BuildOptions BO;
      std::unique_ptr<CFG>  cfg = CFG::buildCFG(D, D->getBody(), Ctx, BO);

      if (cfg ) {
        cfg->dump(Ctx->getLangOpts(), true);

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
