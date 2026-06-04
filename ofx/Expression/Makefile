# Makefile for the Expression OFX plugin.
#
# Easiest build path: drop this folder into the OpenFX Support examples tree so
# the Support library + bundle rules come for free:
#
#   git clone https://github.com/AcademySoftwareFoundation/openfx
#   cp -r Expression openfx/Support/Plugins/Expression
#   cd openfx/Support/Plugins/Expression
#   make            # -> Expression.ofx.bundle/Contents/<arch>/Expression.ofx
#
# Makefile.master (from the openfx repo) compiles the Support sources listed
# below and assembles the .ofx.bundle, including its Info.plist.

PLUGINNAME = Expression

# Just our plugin object(s). The OFX C++ Support library objects are supplied
# separately by Makefile.master (as SUPPORTOBJECTS) and linked in automatically.
PLUGINOBJECTS = Expression.o

# On macOS, add the Metal GPU render path: compile the Obj-C++ dispatch unit,
# define HAVE_METAL for Expression.cpp, and link the Metal/Foundation frameworks.
# Other platforms build the CPU-only plugin unchanged (CUDA comes via its own
# unit later). CXXFLAGS_ADD / LDFLAGS_ADD are honoured by Makefile.master.
ifeq ($(shell uname -s),Darwin)
  PLUGINOBJECTS += ExprMetal.o
  CXXFLAGS_ADD  += -DHAVE_METAL
  LDFLAGS_ADD   += -framework Metal -framework Foundation
endif

# Anything in this list is copied into Contents/Resources of the bundle.
RESOURCES =

# Path back to the Support root (valid when placed in Support/Plugins/Expression).
PATHTOROOT = ../..

include $(PATHTOROOT)/Plugins/Makefile.master

# Compile Objective-C++ (.mm) units (Makefile.master only ships a .cpp rule).
$(OBJECTPATH)/%.o : %.mm
	mkdir -p $(OBJECTPATH)
	$(CXX) -c $(CXXFLAGS) $(CXXSTANDARD) $(CXXARCH) $< -o $@

# Header-only deps: rebuild objects when the shared headers change. (Prefixed
# with $(OBJECTPATH) so they attach to the real targets built by the pattern rule.)
$(OBJECTPATH)/Expression.o: Expression.cpp ExprEval.h ExprKernel.h ExprKernelMetal.h ExprMetal.h
$(OBJECTPATH)/ExprMetal.o:  ExprMetal.mm ExprMetal.h ExprKernelMetal.h
