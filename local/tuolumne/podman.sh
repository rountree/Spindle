cd /g/g24/rountree/v/repos/Spindle
#enable-podman
#podman build -f ./containers/spindle-serial-ubuntu/Dockerfile --build-arg=replicas=4   \
#    --build-arg=flux_sched_version=noble-v0.48.0-amd64 --userns-uid-map=0:0:1          \
#    --userns-uid-map=1:1:1999 --userns-uid-map=65534:2000:2 .
#podman compose -f ./containers/spindle-serial-ubuntu/docker-compose.yml up --detach
#podman compose -f ./containers/spindle-serial-ubuntu/docker-compose.yml down
#podman ps
podman exec -it spindlenode bash
cd -



podman build -f ./containers/spindle-slurm-ubuntu/testing-plugin/Dockerfile --build-arg=replicas=4 --build-arg=flux_sched_version=noble-v0.48.0-amd64 --userns-uid-map=0:0:1 --userns-uid-map=1:1:1999 --userns-uid-map=65534:2000:2 .

podman compose -f ./containers/spindle-slurm-ubuntu/testing-plugin/docker-compose.yml down


 podman compose -f ./containers/spindle-slurm-ubuntu/testing-plugin/docker-compose.yml logs
