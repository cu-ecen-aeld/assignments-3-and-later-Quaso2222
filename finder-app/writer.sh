#!/bin/bash

writefile=$1
writestr=$2

if [ "$#" -ne 2 ];then
    echo "usage:./writer.sh writefile writestr"
    exit 1
fi

mkdir -p `dirname  $writefile`
touch $writefile

if [ -e "$writefile" ]; then
    echo "file created"
else
    echo "File does not exist"
    exit 1
fi

echo "$writestr" > "$writefile"

exit 0

