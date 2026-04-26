# For now, these are common across versions.

# Only used for path construction (bottom of this file)
ROOT=/p/vast1/rountree/machines/${LCSCHEDCLUSTER}
export SPINDLE_REPO=/p/vast1/rountree/repos/Spindle
export SPINDLE_SCRIPTS=${SPINDLE_REPO}/local/${LCSCHEDCLUSTER}
export TEST_RESOURCE_MANAGER=serial

# Get the current branch and commit
cd $SPINDLE_REPO
SPINDLE_BRANCH=`git branch --show-current`
SPINDLE_COMMIT=`git rev-parse --short HEAD`
if [ -z "${SPINDLE_BRANCH}" ];
then
    SPINDLE_TAG=$SPINDLE_COMMIT
else
    SPINDLE_TAG=$SPINDLE_BRANCH
fi
cd - > /dev/null

# Sets build, patch, and install directories
export SPINDLE_BUILD=${ROOT}/build/Spindle-${SPINDLE_TAG}-${TEST_RESOURCE_MANAGER}
export SPINDLE_INSTALL=${ROOT}/install/Spindle-${SPINDLE_TAG}
export SPINDLE_PATCHES=${ROOT}/patches/Spindle-${SPINDLE_TAG}

# Prevents flux from using the system spindle.
export SPINDLE_FLUXOPT=disable

# Set Spindle log level (max=3)
export SPINDLE_DEBUG=3

# Hmmmm....
export FLUXRC=${SPINDLE_BUILD}/testsuite/spindle.rc

# Allows test scripts to be able to find libmodules.so.1
export LD_LIBRARY_PATH=/opt/cray/pe/cce/18.0.1/cce/x86_64/lib/:${LD_LIBRARY_PATH}

# Points to our version of Spindle
export LD_LIBRARY_PATH=${SPINDLE_INSTALL}/lib:${LD_LIBRARY_PATH}

export PATH=${HOME}/v/machines/tuolumne/install/gcc-15.1.0/bin:$PATH
export LD_LIBRARY_PATH=${HOME}/v/machines/tuolumne/install/gcc-15.1.0/lib64:${LD_LIBRARY_PATH}



