if [[ -v SPINDLE_SCRIPTS ]]; then
    echo "Using scripts in " $SPINDLE_SCRIPTS
else
    echo "SPINDLE_SCRIPTS not set, please source env.h.  Exiting."
    exit
fi

flux alloc --queue=pbatch --time-limit=4h --nodes=1 --exclusive


