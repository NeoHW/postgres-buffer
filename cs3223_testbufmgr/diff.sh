#!/usr/bin/env bash

# Compare your testresults files and provided solution files

policy=yaclock
result_dir=testresults-${policy}
soln_dir=${result_dir}-soln

for dir in ${result_dir} ${soln_dir}
do
	if [ ! -d ${dir} ]; then
		echo "ERROR: Directory ${dir} missing!"
		exit 1
	fi
done

for i in {0..9}
do
	echo "############### testcase $i "
	diff ${soln_dir}/result-$i.txt ${result_dir}/result-$i.txt 
	echo
done

