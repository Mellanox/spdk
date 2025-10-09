#!/bin/bash -exEl
set -o pipefail

progname=$(basename $0)

DEFECTS_EXPECTED=0

function usage()
{
	cat << HEREDOC
   Usage: $progname [--pre_script "./autogen.sh;./configure"] [--build_cmd "make all"] [--ignore_files "devx gtest"]  [--verbose]
   optional arguments:
     -h, --help           			show this help message and exit
     -p, --pre_script STRING        Preparation commands to run prior running coverity
     -b, --build_script STRING      Build command to pass to coverity
     -i, --ignore_files STRING      Space separated list of files/dirs to ignore
     --url STRING                   Coverity Server URL
     --user STRING                  Login to Coverity Server
     --password STRING              Password to Coverity Server
     --stream STRING                Stream on Coverity Server where to upload the report
     --upload                       Upload report to Coverity Server (--url, --user, --password are required)
     --doca-version STRING          DOCA SDK version
     -v, --verbose        	    increase the verbosity of the bash script
HEREDOC
exit 0
}



while [[ "$#" -gt 0 ]]; do
    case $1 in
        -p|--pre_script) pre_cmd="$2"; shift ;;
        -b|--build_script) build_cmd="$2"; shift ;;
        -i|--include_files) include_list="$2"; shift ;;
	--url) url="$2"; shift ;;
	--user) user="$2"; shift ;;
	--password) password="$2"; shift ;;
	--stream) stream="$2"; shift ;;
	--upload) upload=true;;
	--doca-version) DOCA_VERSION="$2"; shift ;;
        -h|--help) usage ;;
        -v|--verbose) set +x ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done

if [ ! -d .git ]; then
	echo "Error: should be run from project root"
	exit 1
fi

if [ ! -z ${upload} ]; then
	[ ! -z "$url" ] || { echo "Error: --url must be provided when --upload is set!"; exit 1; }
	[ ! -z "$user" ] || { echo "Error: --user must be provided when --upload is set!"; exit 1; }
	[ ! -z "$password" ] || { echo "Error: --password must be provided when --upload is set!"; exit 1; }
	[ ! -z "$stream" ] || { echo "Error: --stream must be provided when --upload is set!"; exit 1; }
fi


ncpus=$(cat /proc/cpuinfo|grep processor|wc -l)

# Current coverity version (2023.12) supports GCC <= 11
if ! command -v gcc-11 &> /dev/null; then
	echo "Error: gcc-11 is not installed!"
	exit 1
fi

export CC=gcc-11
export CXX=g++-11


# Build and install NVMF Target Offload

if [ -n "${pre_cmd}" ]; then

    echo "==== Running Pre-commands ===="

    set +eE
    /bin/bash -c "$pre_cmd"
    rc=$?

    if [ $rc -ne 0 ]; then
        echo pre-commands failed
        exit 1
    fi

    set -eE
fi

cov_build="cov_build"
rm -rf $cov_build

echo "==== Running coverity ===="

export PATH="$PATH:/auto/sw_tools/Commercial/Synopsys/Coverity/Coverity_2023.12/linux_arm64/bin"

cov-build --dir $cov_build $build_cmd

if [ -n "${include_list}" ]; then
    echo "==== Restricting analysis to include list ===="
    
    # Generate a list of all captured files
    set -x
    all_files=$(cov-manage-emit --dir ${cov_build} list | grep ">"| awk '{print $3}')
    echo "All files: $all_files"
    # Compute files to delete (those NOT in allow_list)
    for file in ${all_files}; do
        count=0
        for f in ${include_list}; do
            if echo "${file}" | grep -q "$f"; then
                count=$((count + 1))
            fi
        done
        if [ $count -eq 0 ]; then
            cov-manage-emit --dir ${cov_build} --tu-pattern "file('${file}')" delete ||:
        fi
    done
fi


echo "==== Running anaysis ===="

cov-analyze --jobs 1 --security \
	    --enable INTEGER_OVERFLOW \
	    --enable AUDIT.SPECULATIVE_EXECUTION_DATA_LEAK \
	    --concurrency --dir $cov_build

if [ ! -z ${upload} ]; then

    echo "==== Uploading report ===="

    cov-commit-defects --ssl --on-new-cert trust \
	    --url $url --user $user --password $password \
	    --dir $cov_build \
	    --stream $stream
fi

cov-format-errors --dir $cov_build --html-output $cov_build/html

nerrors=$(cov-format-errors --dir $cov_build --emacs-style |& tee $cov_build/coverity.log | grep -c 'Type:'||true)

echo -e "Number of Defects: ${nerrors} (expected $DEFECTS_EXPECTED)\n"

if (( $nerrors > $DEFECTS_EXPECTED )); then
    echo "FAIL"
    echo "New defects were added."
    echo "Number of defects ($nerrors) > ($DEFECTS_EXPECTED) defects expected!"
    echo "Please fix new defects or mark them as false-positive by incrementing the DEFECTS_EXPECTED in .ci/coverity.sh"
    exit $nerrors
elif (( $nerrors < $DEFECTS_EXPECTED )); then
    echo "FAIL"
    echo "Defects were removed without updating the expected number."
    echo "Number of defects ($nerrors) < ($DEFECTS_EXPECTED) defects expected!"
    echo "Please update DEFECTS_EXPECTED to $nerrors in .ci/coverity.sh"
    exit $nerrors
else
    exit 0
fi
