#pragma once

// =================================================================
// Logging
// =================================================================
// The console only exists in a debug build, so every printf in a
// release build formats a string and then throws it at a handle
// nobody is listening to. Harmless, but it is dead weight in the
// binary and it is the kind of thing that ends up firing inside a
// tick loop.
//
// PH_LOG compiles to nothing unless the console is actually there.
// Anything the USER needs to know does not belong here at all: it
// goes through iOS::Notify, which they can see.
//
// The do/while is the usual macro guard, so PH_LOG can be the body
// of an if without swallowing the else.
// =================================================================

#if defined(_DEBUG) || defined(PHANTOM_CONSOLE)

    #include <cstdio>

    #define PH_LOG(fmt, ...)  do { std::printf(fmt "\n", ##__VA_ARGS__); } while (0)

#else

    // The arguments still have to parse, so typos are caught in
    // release too, but nothing is emitted and nothing is evaluated.
    #define PH_LOG(fmt, ...)  do { (void)sizeof(fmt); } while (0)

#endif
