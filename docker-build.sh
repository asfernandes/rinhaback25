#!/bin/sh
set -e

docker buildx build --progress plain -t asfernandes/rinhaback25:boost-lmdb .
