#!/bin/bash
PATH=$PATH:/home/lyy/milkv-sdk/duo-examples/duo-sdk/riscv64-linux-musl-arm64/bin
#echo "PATH=PATH:/home/lyy/milkv-sdk/duo-examples/duo-sdk/riscv64-linux-musl-arm64/bin"
./compile_sample.sh
sleep 3
scp-milkv cvi_tdl/sample_my_dec sample_my_dec
