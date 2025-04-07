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

#include "reb-struct.h"


// There is a platform-dependent list of legal ABIs which the MAKE-ROUTINE
// and MAKE-CALLBACK natives take as an option via refinement.
//
// It was written as librebol code using a Rebol SWITCH, instead of as C
// code.  It would be more optimal to use the EXT_SYM_XXX symbols in plain
// C, but since this was written it serves as a good API test for now.  If
// performance of the FFI becomes an issue, we can revisit this.
//
// 1. In order to use #ifdefs inside the call, we cannot use the variadic
//    macro for rebUnboxInteger that injects rebEND and LIBREBOL_CONTEXT,
//    because it would try to wrap the #ifdefs in a __VA_ARGS__.  So we use
//    the plain non-macro call and add rebEND and LIBREBOL_SPECIFIER manually.
//    Hence we use the rebUnboxInteger_c89() variant.
//
//    (It also means we can't use u_cast(ffi_abi, xxx) because u_cast is
//    also a macro.  Could use old-style (ffi_abi) cast... but religion
//    dictates avoiding that, and instead casting as a separate step.)
//
// 2. !!! While these are defined on newer versions of LINUX X86/X64 FFI
//    older versions (e.g. 3.0.13) only have STDCALL/THISCALL/FASTCALL
//    on Windows.  We could detect the FFI version, but since basically
//    no one uses anything but the default punt on it for now.
//
static ffi_abi Abi_From_Word_Or_Nulled(const Value* word) {
    if (Is_Nulled(word))
        return FFI_DEFAULT_ABI;

    assert(Is_Word(word));

    intptr_t abi_int = rebUnboxInteger_c89(  // _c89 -> no macro, ifdef [1]
        LIBREBOL_BINDING_NAME(),

      "switch @", word, "[",

        "'default [", rebI(FFI_DEFAULT_ABI), "]",

      #ifdef X86_WIN64

        "'win64 [", rebI(FFI_WIN64), "]",

      #elif defined(X86_WIN32) \
            || defined(TO_LINUX_X86) || defined(TO_LINUX_X64)

        /* "'sysv [", rebI(FFI_SYSV), "]", */  // !!! Should this be defined?

        #ifdef X86_WIN32  // old Linux FFI doesn't have these [2]
            "'stdcall [", rebI(FFI_STDCALL), "]",
            "'thiscall [", rebI(FFI_THISCALL), "]",
            "'fastcall [", rebI(FFI_FASTCALL), "]",
        #endif

        #ifdef X86_WIN32
            "'ms-cdecl [", rebI(FFI_MS_CDECL), "]",
        #else
            "'unix64 [", rebI(FFI_UNIX64), "]",
        #endif

      #elif defined (TO_LINUX_ARM)

        "'vfp [", rebI(FFI_VFP), "]",
        "'sysv [", rebI(FFI_SYSV), "]",

      #elif defined (TO_LINUX_MIPS)

        "'o32 [", rebI(FFI_O32), "]",
        "'n32 [", rebI(FFI_N32), "]",
        "'n64 [", rebI(FFI_N64), "]",
        "'o32-soft-float [", rebI(FFI_O32_SOFT_FLOAT), "]",
        "'n32-soft-float [", rebI(FFI_N32_SOFT_FLOAT), "]",
        "'n64-soft-float [", rebI(FFI_N64_SOFT_FLOAT), "]",

      #endif  // X86_WIN64

        "fail [-{Unknown ABI for platform:}- @", word, "]",
      "]",
      rebEND  // <-- rebEND required, rebUnboxInteger_c89() is not a macro
    );

    return u_cast(ffi_abi, abi_int);
}


//
//  export make-routine: native [
//
//  "Create a bridge for interfacing with arbitrary C code in a DLL"
//
//      return: [action!]
//      lib "Library DLL that C function lives in (from MAKE LIBRARY!)"
//          [library!]
//      name "Linker name of the C function in the DLL"
//          [text!]
//      ffi-spec "Description of what C argument types the C function takes"
//          [block!]
//      :abi "Application Binary Interface ('CDECL, 'FASTCALL, etc.)"
//          [word!]
//  ]
//
DECLARE_NATIVE(MAKE_ROUTINE)
{
    INCLUDE_PARAMS_OF_MAKE_ROUTINE;

    ffi_abi abi = Abi_From_Word_Or_Nulled(ARG(ABI));

    Element* spec = Element_ARG(FFI_SPEC);

    RebolValue* handle = rebEntrap("pick", ARG(LIB), ARG(NAME));
    if (Is_Error(handle))  // PICK returned raised error, entrap made it plain
        return FAIL(Cell_Error(handle));

    Unquotify(Known_Element(handle));  // rebEntrap() is quoted for non-raised
    assert(Is_Handle_Cfunc(handle));

    RoutineDetails* r;
    Option(Error*) e = Trap_Alloc_Ffi_Action_For_Spec(&r, spec, abi);
    if (e)
        return FAIL(unwrap e);

    Copy_Cell(Routine_At(r, IDX_ROUTINE_CFUNC), handle);
    Init_Blank(Routine_At(r, IDX_ROUTINE_CLOSURE));
    Copy_Cell(Routine_At(r, IDX_ROUTINE_ORIGIN), ARG(LIB));

    return Init_Action(OUT, r, ANONYMOUS, UNBOUND);
}


//
//  export make-routine-raw: native [
//
//  "Create a bridge for interfacing with a C function, by pointer"
//
//      return: [action!]
//      pointer "Raw address of C function in memory"
//          [integer!]
//      ffi-spec "Description of what C argument types the C function takes"
//          [block!]
//      :abi "Application Binary Interface ('CDECL, 'FASTCALL, etc.)"
//          [word!]
//  ]
//
DECLARE_NATIVE(MAKE_ROUTINE_RAW)
//
// !!! Would be nice if this could just take a filename and the lib management
// was automatic, e.g. no LIBRARY! type.
{
    INCLUDE_PARAMS_OF_MAKE_ROUTINE_RAW;

    ffi_abi abi = Abi_From_Word_Or_Nulled(ARG(ABI));

    Element* spec = Element_ARG(FFI_SPEC);

    CFunction* cfunc = p_cast(CFunction*,  // can't directly cast on 32-bit
        cast(uintptr_t, VAL_INT64(ARG(POINTER))
    ));
    if (cfunc == nullptr)
        return FAIL("FFI: nullptr pointer not allowed for raw MAKE-ROUTINE");

    RoutineDetails* r;
    Option(Error*) e = Trap_Alloc_Ffi_Action_For_Spec(&r, spec, abi);
    if (e)
        return FAIL(unwrap e);

    Init_Handle_Cfunc(Routine_At(r, IDX_ROUTINE_CFUNC), cfunc);
    Init_Blank(Routine_At(r, IDX_ROUTINE_CLOSURE));
    Init_Blank(Routine_At(r, IDX_ROUTINE_ORIGIN)); // no LIBRARY! in this case.

    return Init_Action(OUT, r, ANONYMOUS, UNBOUND);
}


//
//  export wrap-callback: native [
//
//  "Wrap an ACTION! so it can be called by raw C code via a memory address"
//
//      return: [action!]
//      action "The existing Rebol action whose behavior is being wrapped"
//          [action!]
//      ffi-spec "What C types each Rebol argument should map to"
//          [block!]
//      :abi "Application Binary Interface ('CDECL, 'FASTCALL, etc.)"
//          [word!]
//  ]
//
DECLARE_NATIVE(WRAP_CALLBACK)
//
// 1. It's the FFI's fault for using the wrong type for the thunk.  Use a
//    memcpy in order to get around strict checks that absolutely refuse to
//    let you do a cast here.
{
    INCLUDE_PARAMS_OF_WRAP_CALLBACK;

    ffi_abi abi = Abi_From_Word_Or_Nulled(ARG(ABI));

    Element* spec = Element_ARG(FFI_SPEC);

    RoutineDetails* r;
    Option(Error*) e = Trap_Alloc_Ffi_Action_For_Spec(&r, spec, abi);
    if (e)
        return FAIL(unwrap e);

    void *thunk;  // actually CFUNC (FFI uses void*, may not be same size!)
    ffi_closure *closure = cast(ffi_closure*, ffi_closure_alloc(
        sizeof(ffi_closure), &thunk
    ));

    if (closure == nullptr)
        return FAIL("FFI: Couldn't allocate closure");

    ffi_status status = ffi_prep_closure_loc(
        closure,
        Routine_Cif(r),
        callback_dispatcher,  // when thunk is called, calls this function...
        r,  // ...and this piece of data is passed to callback_dispatcher
        thunk
    );

    if (status != FFI_OK)
        return FAIL("FFI: Couldn't prep closure");

    bool check = true;  // avoid "conditional expression is constant"
    if (check and sizeof(void*) != sizeof(CFunction*))
        return FAIL("FFI requires void* size equal C function pointer size");

    CFunction* cfunc_thunk;  // FFI uses wrong type [1]
    memcpy(&cfunc_thunk, &thunk, sizeof(cfunc_thunk));

    Init_Handle_Cfunc(Routine_At(r, IDX_ROUTINE_CFUNC), cfunc_thunk);
    Init_Handle_Cdata_Managed(
        Routine_At(r, IDX_ROUTINE_CLOSURE),
        closure,
        sizeof(&closure),
        &cleanup_ffi_closure
    );
    Copy_Cell(Routine_At(r, IDX_ROUTINE_ORIGIN), ARG(ACTION));

    return Init_Action(OUT, r, ANONYMOUS, UNBOUND);
}


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
//  export make-similar-struct: native [
//
//  "Create a STRUCT! that reuses the underlying spec of another STRUCT!"
//
//      return: [struct!]
//      spec "Struct with interface to copy"
//          [struct!]
//      body "keys and values defining instance contents (bindings modified)"
//          [block! any-context? blank!]
//  ]
//
DECLARE_NATIVE(MAKE_SIMILAR_STRUCT)
//
// !!! Compatibility for `MAKE some-struct [...]` from Atronix R3.  There
// isn't any real "inheritance management" for structs, but it allows the
// re-use of the structure's field definitions, so it is a means of saving on
// memory (?)  Code retained for examination.
{
    INCLUDE_PARAMS_OF_MAKE_SIMILAR_STRUCT;

    Element* spec = Element_ARG(SPEC);
    Element* body = Element_ARG(BODY);

    Init_Struct(OUT, Copy_Struct_Managed(Cell_Struct(spec)));

    Option(Error*) e = Trap_Init_Struct_Fields(OUT, body);
    if (e)
        return FAIL(unwrap e);

    return OUT;
}


//
//  destroy-struct-storage: native [  ; EXPORT ?
//
//  "Destroy the external memory associated the struct"
//
//      return: [~]
//      struct [struct!]
//      :free "Specify the function to free the memory"
//          [action!]
//  ]
//
DECLARE_NATIVE(DESTROY_STRUCT_STORAGE)
{
    INCLUDE_PARAMS_OF_DESTROY_STRUCT_STORAGE;

    if (Is_Blob(Struct_Storage(Cell_Struct(ARG(STRUCT)))))
        return FAIL("Can't use external storage with DESTROY-STRUCT-STORAGE");

    Element* handle = Struct_Storage(Cell_Struct(ARG(STRUCT)));

    DECLARE_ELEMENT (pointer);
    Init_Integer(pointer, i_cast(intptr_t, Cell_Handle_Pointer(void, handle)));

    if (Cell_Handle_Len(handle) == 0)
        return FAIL("DESTROY-STRUCT-STORAGE given already destroyed handle");

    CELL_HANDLE_LENGTH_U(handle) = 0;  // !!! assert correct for mem block size

    if (Bool_ARG(FREE)) {
        if (not Is_Action_Routine(ARG(FREE)))
            return FAIL(Error_Free_Needs_Routine_Raw());

        rebElide(rebRUN(ARG(FREE)), pointer);
    }

    return NOTHING;
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
