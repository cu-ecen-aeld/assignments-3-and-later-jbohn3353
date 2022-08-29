#!/bin/sh
# Counts string occurences in a directory
# Usage:
# ./finder.sh [dir] [string]
# Author: James Bohn

if [ $# -lt 2 ]; then
    echo "Not enough arguments."
    exit 1
fi

READDIR=$1
READSTR=$2

if ! [ -d ${READDIR} ]; then
    echo "Invalid search directory."
    exit 1
fi

FILES=$(grep -c ${READSTR} ${READDIR}/* | wc -l)
LINES=$(grep -o ${READSTR} ${READDIR}/* | wc -l)

echo "The number of files are ${FILES} and the number of matching lines are ${LINES}"
