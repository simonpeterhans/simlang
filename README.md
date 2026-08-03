# The SimLang Scripting Language

Over the last years, I had the pleasure of working on the real-time strategy games Age of Mythology and Age of Empires,
and got in touch with the (legacy) scripting language that the games expose. Seeing its limitations and shortcomings and
the idea that it would be cool to implement your own compiler and VM, SimLang tries to address some of these issues (and
performance).

Since progress was quite good, I also ended up implementing some features that I do not think are needed for a minimal
version of the language itself. This project was (and still is) also an experimental playground of sorts, so I kept most
of them anyway, but it is not necessarily exactly the syntax and feature set of what I would ship in a real-world
project (or keep in this implementation in the future).

## Properties

- Can easily compete with Lua, Wren, and basically any other scripting language in performance (except those compiling
  to native code like LuaJIT or optimized Luau)
- Statically typed (explicit and implicit)
- Multi-pass compiler, explicit AST
- Stack-based virtual machine and instruction set
- Simple mark-and-sweep garbage collection
- Syntactically somewhere between Kotlin, C#, and C++, intentionally kept explicit to simplify parsing

## Current Features

- Primitives: `void`, `int`, `float`, `bool`, `string`
- `struct` value types
- `class` heap-reference types
- `interface` declarations and dynamic interface dispatch
- Growable heap lists `list<T>`
- Growable heap maps `map<K, V>`
- Type templates for structs, classes, and interfaces
- Modules and imports
- Formatted strings
- Host-provided constants and syscalls (allowing primitives as well as list/map params/return values)

## Missing Features

- Function pointers/lambdas (did not make up my mind about the design of this one yet)
- Registering native types for VM use
- Enums
- Null-safety when accessing members of nullable references (out of scope for now)
- Debugger (bytecode patching would easily be possible, but in my opinion the debugging options are also depending on
  the embedding engine)
- ...

## Code Sample

(The full reference will follow at a later point.)

```sim
// Importing a specific symbol from a module.
import math { Vec2 };

// Value-type struct.
struct Tile
{
    // Structs are created using "make ...".
    var pos: Vec2 = make Vec2 { x: 0, y: 0 };
    var height: int = 0;
}

// Reference-type class.
class Terrain
{
    var size: Vec2;
    // Classes, lists and maps are created using "new ...".
    var tiles = new list<Tile>();
    var peak = 0;

    init(width: int, height: int)
    {
        // Class members without defaults have to be initialized here.
        this.size = make Vec2 { x: width, y: height };
        // Reserve space for the list.
        this.tiles.reserve(width * height);
    }

    fun addTile(x: int, y: int, tileHeight: int) : void
    {
        var pos = make Vec2 { x, y };
        var tile = make Tile { pos, height: tileHeight };
        this.tiles.add(tile);

        if (tileHeight > this.peak)
        {
            this.peak = tileHeight;
        }
    }

    fun getInfo() : string
    {
        return $"terrain {this.size.x}x{this.size.y}: {this.tiles.size()} tiles, peak {this.peak}";
    }
}

fun main() : void
{
    // Assume game::cDefaultMapSize was registered by the host.
    const length = game::cDefaultMapSize;
    var terrain = new Terrain(length, length);
    
    for (var i = 0; i < length * length; i += 1)
    {
        // Assume syscall::rand(min, max) was registered by the host.
        var height = syscall::rand(0, 10);
        terrain.addTile(i % length, i / length, height);
    }

    print(terrain.getInfo());
}
```

## Host Bindings

The CLI folder has a small example of how to embed the compiler and VM in a host application.
Generally, you will want to use something like:

```cpp
#include <cstdio>
#include <memory>

#include "compiler.h"
#include "compileroptions.h"
#include "runtime/executableimage.h"
#include "runtime/runtimeerrorsink.h"
#include "runtime/vm/vm.h"
#include "util/textsinks.h"
#include "util/types.h"

static simlang::i32 randInt(simlang::i32 min, simlang::i32 max)
{
    static simlang::u32 state = 0x12345678U;
    state = state * 1664525U + 1013904223U;
    return min + static_cast<simlang::i32>(state % static_cast<simlang::u32>(max - min + 1));
}

int main()
{
    simlang::FileTextSink stdoutSink{stdout};
    simlang::FileTextSink stderrSink{stderr};

    simlang::Compiler compiler{stderrSink};

    // Register host bindings before compiling.
    compiler.registerConst("game", "cDefaultMapSize", simlang::i32{4});
    compiler.registerSyscall("syscall", "rand", &randInt);

    simlang::CompilerOptions options;
    options.mRootPath = "scripts";
    options.mSourcePath = "scripts/main.sim";
    options.mLogSink = &stderrSink;

    // Compile the script entry file.
    std::unique_ptr<simlang::ExecutableImage> image = compiler.compile(options);
    compiler.emitDiagnostics();
    if (image == nullptr)
    {
        return 1;
    }

    // Run the generated bytecode.
    simlang::TextRuntimeErrorSink runtimeErrors{stderrSink};
    simlang::VM vm{*image};
    vm.setOutput(&stdoutSink);
    vm.setRuntimeErrorSink(&runtimeErrors);

    bool result = vm.run() && vm.hasRuntimeErrors() == false;
    if (result == false)
    {
        return 1;
    }

    return 0;
}
```

## References

Various resources whose content and design influenced the design of SimLang (and might be of general interest if you
want to learn about compilers and interpreters):

- [Crafting Interpreters](https://craftinginterpreters.com/)
- [Wren](https://github.com/munificent/wren)
- [Lua](https://github.com/lua/lua)
