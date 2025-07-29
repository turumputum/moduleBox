@echo off

cd ..\

call script.env.cmd

rm -rf build
mkdir build

cmake . -B build
