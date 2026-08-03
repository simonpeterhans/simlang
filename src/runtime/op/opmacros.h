#pragma once

// Use code if COUNT is 0.
#define SIMLANG_OP_IF0_0(...) __VA_ARGS__
#define SIMLANG_OP_IF0_1(...)
#define SIMLANG_OP_IF0_2(...)
#define SIMLANG_OP_IF0(COUNT, ...) SIMLANG_OP_IF0_##COUNT(__VA_ARGS__)

// Use code if COUNT is 1.
#define SIMLANG_OP_IF1_0(...)
#define SIMLANG_OP_IF1_1(...) __VA_ARGS__
#define SIMLANG_OP_IF1_2(...)
#define SIMLANG_OP_IF1(COUNT, ...) SIMLANG_OP_IF1_##COUNT(__VA_ARGS__)

// Use code if COUNT is >= 1.
#define SIMLANG_OP_IFGE1_0(...)
#define SIMLANG_OP_IFGE1_1(...) __VA_ARGS__
#define SIMLANG_OP_IFGE1_2(...) __VA_ARGS__
#define SIMLANG_OP_IFGE1(COUNT, ...) SIMLANG_OP_IFGE1_##COUNT(__VA_ARGS__)

// Use code if COUNT is 2.
#define SIMLANG_OP_IF2_0(...)
#define SIMLANG_OP_IF2_1(...)
#define SIMLANG_OP_IF2_2(...) __VA_ARGS__
#define SIMLANG_OP_IF2(COUNT, ...) SIMLANG_OP_IF2_##COUNT(__VA_ARGS__)
