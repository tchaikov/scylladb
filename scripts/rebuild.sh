#!/bin/bash

# rebuild.sh - Scylla Executable Rebuild Script
#
# This script is designed to rebuild Scylla executables for postmortem debugging.
# While release builds are available as relocatable tarballs in the cloud for
# easy downloading, debugging non-release executables (e.g., from CI test failures)
# can be challenging. This script helps reproduce the exact build locally for
# debugging coredumps generated during CI testing.
#
# The script:
# 1. Takes a version string that uniquely identifies a build
# 2. Creates a git worktree at the exact commit
# 3. Configures and builds Scylla with the specified build type
#
# Example usage:
#   ./rebuild.sh --version "6.3.0~dev-20250114.2eac7a2d616f" --build debug
#
# This will:
# - Parse the version into: 6.3.0 (version), dev (type), 20250114 (date), 2eac7a2d616f (commit)
# - Create a worktree at rebuild-2eac7a2d616f (or specified path)
# - Build Scylla with debug symbols for debugging
#
# This is particularly useful when:
# - Investigating CI test failures with coredumps
# - Debugging non-release builds
# - Reproducing specific versions for testing

set -e

# Default values
build_mode="release"
worktree_path=""
jenkins_job="next"
artifact=scylla
# the directoy should be identical to the one used by jenkins, which
# clones the repo in this directory, and build in-the-source.
source_dir="/jenkins/workspace/scylla-${branch}/${jenkins_job}/scylla"

usage() {
    echo "Usage: $0 --version <version> [--build <build_mode>] [--worktree <worktree>] [artifact]"
    echo "  --version: Version string in format 'X.Y.Z~type-date.sha1'"
    echo "  --build: Build type (debug|release|dev), defaults to ${build_mode}"
    echo "  --worktree: Optional path for the worktree, defaults to rebuild-<sha1>"
    echo "  artifact: Optional name of the artifact to build, defaults to 'scylla'"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            if [[ -z "$2" ]]; then
                echo "Error: --version requires a value"
                usage 1
            fi
            version="$2"
            shift 2
            ;;
        --build)
            if [[ -z "$2" ]]; then
                echo "Error: --build requires a value"
                usage 1
            fi
            build_mode="$2"
            if [[ ! "$build_mode" =~ ^(debug|release|dev)$ ]]; then
                echo "Error: build type must be one of: debug, release, dev"
                usage 1
            fi
            shift 2
            ;;
        --worktree)
            if [[ -z "$2" ]]; then
                echo "Error: --worktree requires a value"
                usage 1
            fi
            worktree_path="$2"
            shift 2
            ;;
        -h|--help)
            usage 0
            ;;
        *)
            artifact="$1"
            shift
            ;;
    esac
done

# Check if version is provided
if [[ -z "$version" ]]; then
    echo "Error: --version is required"
    usage
fi

# Parse version string
# the version looks like "6.3.0~dev-20250114.2eac7a2d616f"
if [[ ! "$version" =~ ^([0-9]+\.[0-9]+\.[0-9]+)~([^-]+)-([0-9]{8})\.([a-f0-9]+)$ ]]; then
    echo "Error: invalid version format"
    usage
fi


version_build="${BASH_REMATCH[1]}-${BASH_REMATCH[2]}"
version_date="${BASH_REMATCH[3]}"
commit_sha="${BASH_REMATCH[4]}"

# Set default worktree path if not provided
if [[ -z "$worktree_path" ]]; then
    worktree_path="rebuild-${commit_sha}"
fi

# Create worktree
echo "Creating worktree at ${worktree_path}..."
if ! git worktree add "$worktree_path" "$commit_sha"; then
    echo "Error: failed to create worktree"
    exit 1
fi

# Change to worktree directory
cd "$worktree_path"

# Generate build system
echo "Generating build system..."
if ! ./tools/toolchain/dbuild --source-dir "${source_dir}" -- ./configure.py --date-stamp "${version_date}" --mode "${build_mode}" --debuginfo 1; then
    echo "Error: failed to generate build system"
    exit 1
fi

target="build/${build_mode}/${artifact}"
# Build scylla
echo "Building scylla (${build_mode})..."
if ! ./tools/toolchain/dbuild -- ninja -C . "${target}"; then
    echo "Error: build failed"
    exit 1
fi

echo "${artifact} ${version_build}-${version_date} built at: ${worktree_path}/${target}"
