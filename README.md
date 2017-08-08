# crab-clang

Attempt at generating a CRAB CFG from the clang AST. 

# why?

Usability of verification tools is an interesting story. A typical story for 
these kinds of tools is a workflow that looks something like this: 

1. Compile the program to LLVM using `clang -g -c -emit-llvm`.
2. Run a tool on the resulting pile of LLVM to pre-process the LLVM, usually 
   to ensure it's in SSA (or, better still, some SSA variant that isn't part 
   of LLVM), run a points-to analysis, perform constant folding, or something
   else.
3. Run another tool on this pre-processed pile of LLVM to generate some 
   representation suitable for analysis. For example, generate some other 
   mid-level IR, generate SMT formulas for verification conditions, and so on.
4. Actually get some results out and analyze them for bugs. 
5. Communicate those bugs to the user by projecting back through your analyzer
   results to the LLVM to the original source code, probably relying on debug
   meta-data emitted in the first place by clang and hopefully preserved by the
   pre-processing and analysis pipeline. 

This is long and involved. The story also sucks for end users of these tools. 
What if instead we did the analysis much closer to the programs text? 

Also, this way, we can integrate analyses that CRAB has into analysis and 
developer tool chains that use clang `libtooling`. For example, automatic 
re-factoring tools could benefit from a sound inter-procedural nullity 
analysis, or integer range analysis. 

# how?

CRAB provides a simple IR for its fixpoint engine and abstract domains. We 
will write a `RecursiveASTVisitor` that converts the clang AST into a CRAB
program. Then, frontends can instantiate that `RecursiveASTVisitor` 
parameterized by the analysis they want. 

clang provides a representation of the CFG at the AST level. This CFG is very
close to compatible to CRAB, it has one entry and one exit, and it represents
successors and predecessors and statements contained within each block. 
We just need to iterate over the clang CFG to build up a CRAB CFG, translating
the structure into CRAB.

Since CRAB incorporates an inter-procedural analysis as well, perhaps we will
do this in a two-step process, where one `RecursiveASTVisitor` builds a CRAB
representation of the whole program and then we run the inter-procedural 
analysis on the whole program, across all compilation units. 

# risks?

CRAB-LLVM uses a phased analysis, where an up-front points-to analysis on the 
LLVM guides the creation of CRAB arrays and pointers. By running on the AST,
we can't benefit from this analysis, and we're stuck with the source code. 
Maybe this will be a precision problem later, but I'm not really sure how 
we're that worse off. 

# progress?

Very skeletal so far. We are exploring how to represent the AST in CRAB. 
Right now, it only works for the ludicrously simple example program. 

# immediate to-do?

1. The organization of visitors is a catastrophe. I'm not totally sure what
   to do between the distinction between a CRAB _constraint_ and a CRAB
   _expression_. It seems like sometimes, you want to run an _expression_
   down to a temporary that holds the results, and other times, you want 
   to translate a _statement_ into modifications on a basic block. 
2. To compound this catastrophe-ness, when you're iterating over the contents
   of a `CFGBlock` the `Stmt`'s are all `const` qualified and the 
   `RecursiveASTVisitor` isn't. So, there will need to be a recursive function 
   that _is_ `const` qualified (right now it's a function named `walkStmt`) 
   that will do a recursive traversal of `Stmt` into CRAB CFG. 
3. The `assume` generation needs to be different. If there's a branch in block
   *A* and that branch calculates a bunch of intermediate stuff (say like, 
   `f() < g()`) then the code for calculation needs to be in block *A* and 
   the assumes need to be in terms of the temporaries produced by the 
   calculation. 

   One way to deal with this that I thought of today: as a pre-pass, identify 
   all basic blocks that are the target of a branch, and create pre versions of
   those basic blocks only to hold assume statements. Actually, this doesn't need
   to be done as a pre-pass, it could probably be done during the insertion of
   the assume statements. At the point when the assume statements are being 
   generated, we're in block _A_ with succs _B_ and _C_. Create block _B'_ and 
   _C'_, and make _A_ branch to _B'_ and _C'_. Put `assume` statements into 
   _B'_ and _C'_, then have _B'_ branch to _B_ and _C'_ branch to _C_. 
