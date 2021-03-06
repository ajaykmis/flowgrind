#!/bin/sh

SCRIPTDIR=$(dirname $(readlink -f $0))

cd $SCRIPTDIR/../src

mkdir -p new1 new2

for i in *.[hc]; do
    unexpand $i > new1/$i;
done

mv new1/* .

for i in *.[hc]; do
    sed -e "s/[ \t]*$//" $i > new2/$i;
done

mv new2/* .

rm -rf new[12]

exit 0
