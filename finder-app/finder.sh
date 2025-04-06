#!/bin/sh

filesdir=$1
searchstr=$2

if [ "$#" -ne 2 ];then
    echo "wrong parameter:usage:finder.sh filesdir searchstr"
    exit 1
fi

if [ ! -d $1 ];then
    echo "filesdir does not represent a directory on the filesystem"
    exit 1
fi

str_matching_num=`grep -r -c "$searchstr" $filesdir|awk -F ':' '{sum+=$2}END{print sum}'`
file_num=`find "$filesdir" -type f | wc -l`

echo "The number of files are $file_num and the number of matching lines are $str_matching_num" 
