//
//  File: %mod-ffi.c
//  Summary: "Foreign function interface main C file"
//  Section: Extension
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 Atronix Engineering
// Copyright 2012-2017 Ren-C Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//

#include "sys-core.h"
#include "tmp-mod-ffi.h"

#include <ffi.h>
#include "stub-struct.h"
#include "stub-routine.h"



//
//  export addr-of: native [
//
//  "Get the memory address of an FFI STRUCT! or routine/callback"
//
//      return: "Memory address expressed as an up-to-64-bit integer"
//          [integer!]
//      value "Fixed address structure or routine to get the address of"
//          [action! struct!]
//  ]
//
DECLARE_NATIVE(ADDR_OF)
//
// 1. The CFunction is fabricated by the FFI if it's a callback, or just the
//    wrapped DLL function if it's an ordinary routine
//
// 2. !!! If a structure wasn't mapped onto "raw-memory" from the C,
//    then currently the data for that struct is a BINARY!, not a handle to
//    something which was malloc'd.  Much of the system is designed to be
//    able to handle memory relocations of a series data, but if a pointer is
//    given to code it may expect that address to be permanent.  Data
//    pointers currently do not move (e.g. no GC compaction) unless there is
//    a modification to the series, but this may change...in which case a
//    "do not move in memory" bit would be needed for the BINARY! or a
//    HANDLE! to a non-moving malloc would need to be used instead.
{
    INCLUDE_PARAMS_OF_ADDR_OF;

    Value* v = ARG(VALUE);

    if (Is_Action(v)) {
        if (not Is_Action_Routine(v))
            return FAIL(
                "Can only take address of ACTION!s created though FFI"
            );

        RoutineDetails* r = Ensure_Cell_Frame_Details(v);
        return Init_Integer(
            OUT,
            i_cast(intptr_t, Routine_Cfunc(r))  // fabricated or wrapped [1]
        );
    }

    assert(Is_Struct(v));

    return Init_Integer(OUT, i_cast(intptr_t, Cell_Struct_Data_At(v)));  // [2]
}


//
//  export alloc-value-pointer: native [
//
//  "Persistently allocate a cell that can be referenced from FFI routines"
//
//      return: [integer!]
//      value "Initial value for the cell"
//          [any-value?]
//  ]
//
DECLARE_NATIVE(ALLOC_VALUE_POINTER)
//
// !!! Would it be better to not bother with the initial value parameter and
// just start the cell out as nothing?
{
    INCLUDE_PARAMS_OF_ALLOC_VALUE_POINTER;

    Value* allocated = Copy_Cell(Alloc_Value(), ARG(VALUE));
    rebUnmanage(allocated);

    return Init_Integer(OUT, i_cast(intptr_t, allocated));
}


//
//  export free-value-pointer: native [
//
//  "Free a cell that was allocated by ALLOC-VALUE-POINTER"
//
//      return: [~]
//      pointer [integer!]
//  ]
//
DECLARE_NATIVE(FREE_VALUE_POINTER)
//
// 1. Although currently unmanaged API handles are used, it would also be
//    possible to use a managed ones.
//
//    Currently there's no way to make GC-visible references to the returned
//    pointer.  So the only value of using a managed strategy would be to
//    have the GC clean up leaks on exit instead of complaining in the
//    debug build.  For now, assume complaining is better.
{
    INCLUDE_PARAMS_OF_FREE_VALUE_POINTER;

    Value* cell = p_cast(Value*, cast(intptr_t, VAL_INT64(ARG(POINTER))));

    rebFree(cell);  // unmanaged [1]

    return nullptr;
}


//
//  export get-at-pointer: native [
//
//  "Get the contents of a cell, e.g. one returned by ALLOC-VALUE-POINTER"
//
//      return: "If the source looks up to a value, that value--else null"
//          [~null~ any-value?]
//      source "A pointer to a Rebol value"
//          [integer!]
//  ]
//
DECLARE_NATIVE(GET_AT_POINTER)
//
// !!! In an ideal future, the FFI would probably add a user-defined-type
// for a POINTER!, and then GET could be overloaded to work with it.  No
// such mechanisms have been designed yet.  In the meantime, the interface
// for GET-AT-POINTER should not deviate too far from GET.
//
// !!! Alloc_Value() doesn't currently prohibit nulled cells mechanically,
// but libRebol doesn't allow them.  What should this API do?
{
    INCLUDE_PARAMS_OF_GET_AT_POINTER;

    Value* source = p_cast(Value*, cast(intptr_t, VAL_INT64(ARG(SOURCE))));

    Copy_Cell(OUT, source);
    return OUT;  // don't return `source` (would do a rebRelease())
}


//
//  export set-at-pointer: native [
//
//  "Set the contents of a cell, e.g. one returned by ALLOC-VALUE-POINTER"
//
//      return: "The value that was set to"
//          [any-value?]
//      target "A pointer to a Rebol value"
//          [integer!]
//      ^value "Value to assign"
//          [any-value?]
//      :any "Do not error on NOTHING! or TRIPWIRE!"
//  ]
//
DECLARE_NATIVE(SET_AT_POINTER)
//
// !!! See notes on GET-AT-POINTER about keeping interface roughly compatible
// with the SET native.
{
    INCLUDE_PARAMS_OF_SET_AT_POINTER;

    Value* v = Meta_Unquotify_Decayed(ARG(VALUE));

    if ((Is_Nothing(v) or Is_Tripwire(v)) and not Bool_ARG(ANY)) {
        // !!! current philosophy is to allow all assignments
    }

    Value* target = p_cast(Value*, cast(intptr_t, VAL_INT64(ARG(TARGET))));
    Copy_Cell(target, v);

    return COPY(v);  // Returning target would rebRelease() it
}


//
//  startup*: native [
//
//  "Startup FFI Extension"
//
//      return: [~]
//  ]
//
DECLARE_NATIVE(STARTUP_P)
{
    INCLUDE_PARAMS_OF_STARTUP_P;

    return NOTHING;
}


//
//  shutdown*: native [
//
//  "Shutdown FFI Extensions"
//
//      return: [~]
//  ]
//
DECLARE_NATIVE(SHUTDOWN_P)
{
    INCLUDE_PARAMS_OF_SHUTDOWN_P;

    return NOTHING;
}
