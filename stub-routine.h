//
//  File: %stub-routine.h
//  Summary: "Definitions for Routines (Callbacks and CFunction Interfaces)"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2014 Atronix Engineering, Inc.
// Copyright 2014-2019 Ren-C Open Source Contributors
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


typedef Details RoutineDetails;

enum {
    // The HANDLE! of a CFUNC*, obeying the interface of the C-format call.
    // If it's a routine, then it's the pointer to a pre-existing function
    // in the DLL that the routine intends to wrap.  If a callback, then
    // it's a fabricated function pointer returned by ffi_closure_alloc,
    // which presents the "thunk"...a C function that other C functions can
    // call which will then delegate to Rebol to call the wrapped ACTION!.
    //
    // Additionally, callbacks poke a data pointer into the HANDLE! with
    // ffi_closure*.  (The closure allocation routine gives back a void* and
    // not an ffi_closure* for some reason.  Perhaps because it takes a
    // size that might be bigger than the size of a closure?)
    //
    IDX_ROUTINE_CFUNC = 0,

    // An INTEGER! indicating which ABI is used by the CFUNC (enum ffi_abi)
    //
    // !!! It would be better to change this to use a WORD!, especially if
    // the routine descriptions will ever become user visible objects.
    //
    IDX_ROUTINE_ABI = 1,

    // The LIBRARY! the CFUNC* lives in if a routine, or the ACTION! to
    // be called if this is a callback.
    //
    IDX_ROUTINE_ORIGIN = 2,

    // The "schema" of the return type.  This is either a WORD! (which
    // is a symbol corresponding to the FFI_TYPE constant of the return) or
    // a BLOCK! representing a field (this REBFLD will hopefully become
    // OBJECT! at some point).  If it is BLANK! then there is no return type.
    //
    IDX_ROUTINE_RET_SCHEMA = 3,

    // An ARRAY! of the argument schemas; each also WORD! or ARRAY!, following
    // the same pattern as the return value...but not allowed to be blank
    // (no such thing as a void argument)
    //
    IDX_ROUTINE_ARG_SCHEMAS = 4,

    // A HANDLE! containing one ffi_cif*, or BLANK! if variadic.  The Call
    // InterFace (CIF) for a C function with fixed arguments can be created
    // once and then used many times.  For a variadic routine, it must be
    // created on each call to match the number and types of arguments.
    //
    IDX_ROUTINE_CIF = 5,

    // A HANDLE! which is actually an array of ffi_type*, so a C array of
    // pointers.  This array was passed into the CIF at its creation time,
    // and it holds references to them as long as you use that CIF...so this
    // array must survive as long as the CIF does.  BLANK! if variadic.
    //
    IDX_ROUTINE_ARG_FFTYPES = 6,

    // A LOGIC! of whether this routine is variadic.  Since variadic-ness is
    // something that gets exposed in the ACTION! interface itself, this
    // may become redundant as an internal property of the implementation.
    //
    IDX_ROUTINE_IS_VARIADIC = 7,

    // ffi_closure which for a callback stores the place where the CFunction*
    // lives, or BLANK! if the routine does not have a callback interface.
    //
    IDX_ROUTINE_CLOSURE = 8,

    IDX_ROUTINE_MAX
};

#define Routine_At(a,n)  Details_At((r), (n))

INLINE CFunction* Routine_Cfunc(RoutineDetails* r)
  { return Cell_Handle_Cfunc(Routine_At(r, IDX_ROUTINE_CFUNC)); }

INLINE ffi_abi Routine_Abi(RoutineDetails* r)
  { return cast(ffi_abi, VAL_INT32(Routine_At(r, IDX_ROUTINE_ABI))); }

INLINE bool Is_Routine_Callback(RoutineDetails* r) {
    if (Is_Action(Routine_At(r, IDX_ROUTINE_ORIGIN)))
        return true;
    assert(
        rebDid("library?", Routine_At(r, IDX_ROUTINE_ORIGIN))
        or Is_Blank(Routine_At(r, IDX_ROUTINE_ORIGIN))
    );
    return false;
}

INLINE ffi_closure* Routine_Closure(RoutineDetails* r) {
    assert(Is_Routine_Callback(r)); // only callbacks have ffi_closure
    return Cell_Handle_Pointer(ffi_closure, Routine_At(r, IDX_ROUTINE_CLOSURE));
}

INLINE Option(Element*) Routine_Lib(RoutineDetails* r) {
    assert(not Is_Routine_Callback(r));
    if (Is_Blank(Routine_At(r, IDX_ROUTINE_ORIGIN)))
        return nullptr;
    return Known_Element(Routine_At(r, IDX_ROUTINE_ORIGIN));
}

INLINE Value* Routine_Callback_Action(RoutineDetails* r) {
    assert(Is_Routine_Callback(r));
    return Routine_At(r, IDX_ROUTINE_ORIGIN);
}

INLINE Element* Routine_Return_Schema(RoutineDetails* r)
  { return Known_Element(Routine_At(r, IDX_ROUTINE_RET_SCHEMA)); }

INLINE REBLEN Routine_Num_Fixed_Args(RoutineDetails* r)
  { return Cell_Series_Len_Head(Routine_At(r, IDX_ROUTINE_ARG_SCHEMAS)); }

INLINE Element* Routine_Arg_Schema(
    RoutineDetails* r,
    Offset offset  // 0-based
){
    Value* arg_schemas = Routine_At(r, IDX_ROUTINE_ARG_SCHEMAS);
    return Array_At(Cell_Array_Known_Mutable(arg_schemas), offset);
}

INLINE ffi_cif* Routine_Cif(RoutineDetails* r)
  { return Cell_Handle_Pointer(ffi_cif, Routine_At(r, IDX_ROUTINE_CIF)); }

INLINE ffi_type** RIN_ARG_FFTYPES(RoutineDetails* r) {  // !!! unused?
    return Cell_Handle_Pointer(ffi_type*,
        Routine_At(r, IDX_ROUTINE_ARG_FFTYPES)
    );
}

INLINE bool Is_Routine_Variadic(RoutineDetails* r)
    { return Cell_Logic(Routine_At(r, IDX_ROUTINE_IS_VARIADIC)); }


//=//// TEST IF ACTION IS A ROUTINE ///////////////////////////////////////=//
//
// In historical Rebol, there were many different datatypes for functions.
// This meant you could typecheck specifically against ROUTINE!.  If you
// wanted to allow any function as a parameter, you'd say ANY-FUNCTION?
//
// Ren-C regularized the interfaces for all function types and created a
// common FRAME! interface, with ACTION! as the antiform of that frame.  But
// this meant there was no longer type checking specifically for subtypes
// of functions.
//
// Perhaps with the ability to have unlimited datatypes it would be worth it
// to bring back the multiple types?  In any case, we could offer ROUTINE?
// that is a type constraint on FRAME!, that checks the dispatcher.
//

extern Bounce Routine_Dispatcher(Level *L);

INLINE bool Is_Action_Routine(const Value* v) {
    Phase* phase = Cell_Frame_Phase(v);

    if (not Is_Stub_Details(phase))
        return false;  // !!! review cases where specializations could work

    if (Details_Dispatcher(cast(Details*, phase)) != &Routine_Dispatcher)
        return false;

    return true;
}
