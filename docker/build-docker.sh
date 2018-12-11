#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $DIR/..

DOCKER_IMAGE=${DOCKER_IMAGE:-mazacoin/mazad-develop}
DOCKER_TAG=${DOCKER_TAG:-latest}

BUILD_DIR=${BUILD_DIR:-.}

rm docker/bin/*
mkdir docker/bin
cp $BUILD_DIR/src/mazad docker/bin/
cp $BUILD_DIR/src/maza-cli docker/bin/
cp $BUILD_DIR/src/maza-tx docker/bin/
strip docker/bin/mazad
strip docker/bin/maza-cli
strip docker/bin/maza-tx

docker build --pull -t $DOCKER_IMAGE:$DOCKER_TAG -f docker/Dockerfile docker
