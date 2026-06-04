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

# Plugin object + the OFX C++ Support library objects (built from openfx sources)
PLUGINOBJECTS = Expression.o \
                ofxsCore.o ofxsImageEffect.o ofxsInteract.o ofxsLog.o \
                ofxsMultiThread.o ofxsParams.o ofxsProperty.o ofxsPropertyValidation.o

# Anything in this list is copied into Contents/Resources of the bundle.
RESOURCES =

# Path back to the Support root (valid when placed in Support/Plugins/Expression).
PATHTOROOT = ../..

include $(PATHTOROOT)/Plugins/Makefile.master

# ExprEval.h is header-only; make sure a change to it triggers a rebuild.
Expression.o: Expression.cpp ExprEval.h
