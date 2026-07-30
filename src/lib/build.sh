#!/bin/bash
rm -rf CSDVirt.o libCSDVirt.so
g++ -fPIC -c ./CSDVirt.cpp -o ./CSDVirt.o
g++ -shared -o ./libCSDVirt.so ./CSDVirt.o

sudo cp ./libCSDVirt.so /usr/lib/
