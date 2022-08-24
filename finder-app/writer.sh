#!/bin/sh
# Creates a file and writes a string to it
# Usage:
# ./writer.sh [filePath] [string]
# Author: James Bohn

WRITEFILE=$1
WRITESTR=$2

if [ $# -lt 2 ]; then
    echo "Not enough arguments."
    exit 1
fi

WRITEDIR=$(dirname ${WRITEFILE})

mkdir -p ${WRITEDIR}

if [ $? -ne 0 ]; then
    echo "Directory creation failed."
    exit 1
fi

echo ${WRITESTR} > ${WRITEFILE}

