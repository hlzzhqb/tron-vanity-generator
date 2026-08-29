# 把 OpenCL 内核源码打包成一个 C 头文件里的 raw string。
# 用法: cmake -DIN=<in.cl> -DOUT=<out.h> -P embed_kernel.cmake
file(READ "${IN}" _src)
file(WRITE "${OUT}"
"// 自动生成，勿手改。源: kernels/secp256k1_tron.cl\n#pragma once\nstatic const char* kGpuKernelSource = R\"CLK(\n${_src}\n)CLK\";\n")
