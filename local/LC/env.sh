# Only used for path construction (bottom of this file)
SPINDLE_WORKSPACE=/p/vast1/${USER}/${LCSCHEDCLUSTER}/sandbox/workspace-Spindle
export SPINDLE_REPO=/p/vast1/${USER}/${LCSCHEDCLUSTER}/sandbox/workspace-Spindle/Spindle
export SPINDLE_SCRIPTS=${SPINDLE_REPO}/local/LC

# If invoked without a parameter, default to flux.
if [[ $# = 0 ]]; then
    set -- "flux"
fi
# Parameter checking.
case "$1" in
    slurm)
        export TEST_RESOURCE_MANAGER=slurm
        ;;
    slurm-plugin)
        export TEST_RESOURCE_MANAGER=slurm-plugin
        ;;
    flux)
        export TEST_RESOURCE_MANAGER=flux
        ;;
    serial)
        export TEST_RESOURCE_MANAGER=serial
        ;;
    *)
        echo $(date) Unknown resource manager requested: ${1};
        export TEST_RESOURCE_MANAGER=unknown
        ;;
esac

# Set environment variables if we have a known cluster and resource manager.
if [[ "$TEST_RESOURCE_MANAGER" != "unknown" && -v LCSCHEDCLUSTER ]]; then
    echo $(date) Building on ${LCSCHEDCLUSTER} with resource manager ${TEST_RESOURCE_MANAGER}.

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
    export SPINDLE_BUILD=${SPINDLE_WORKSPACE}/build/Spindle-${SPINDLE_TAG}-${TEST_RESOURCE_MANAGER}
    export SPINDLE_INSTALL=${SPINDLE_WORKSPACE}/install/Spindle-${SPINDLE_TAG}

    # Set Spindle log level (max=3)
    export SPINDLE_DEBUG=3

    # Tell flux what we're doing.
    export FLUXRC=${SPINDLE_BUILD}/testsuite/spindle.rc

    # Prevents flux from using the system spindle.
    export SPINDLE_FLUXOPT=disable

    # Allows test scripts to be able to find libmodules.so.1
    export LD_LIBRARY_PATH=/opt/cray/pe/cce/18.0.1/cce/x86_64/lib/:${LD_LIBRARY_PATH}

    # Points to our version of Spindle
    export LD_LIBRARY_PATH=${SPINDLE_INSTALL}/lib:${LD_LIBRARY_PATH}

    echo TEST_RESOURCE_MANAGER""=${TEST_RESOURCE_MANAGER}
    echo LCSCHEDCLUSTER"       "=${LCSCHEDCLUSTER}
    echo SPINDLE_BUILD"        "=${SPINDLE_BUILD}
    echo SPINDLE_INSTALL"      "=${SPINDLE_INSTALL}
    echo SPINDLE_DEBUG"        "=${SPINDLE_DEBUG}
fi


