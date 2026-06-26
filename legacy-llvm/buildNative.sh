#!/bin/bash

# currently assumes you have a bc generated with
# ./cstar -i test.str -o test.bc

inputFile=$1 # common name.bc
outputFile=$2 # common name.out


# can use llc to emit to file with -o somefile.s
llc $inputFile -o temp.s
gcc temp.s -o $outputFile
rm temp.s