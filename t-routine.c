//
//  File: %t-routine.c
//  Summary: "Support for calling non-Rebol C functions in DLLs w/Rebol args)"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2014 Atronix Engineering, Inc.
// Copyright 2014-2017 Ren-C Open Source Contributors
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


// This table repeats FFI-TYPE-MAPPINGS in the extension Rebol code.  Seems
// like a good thing to do in usermode, as what is actually needed here are
// PARAMETER! definitions.  (MAKE PARAMETER! doesn't exist yet, but it could.)
//
// 1. ACTION! is legal if routine or callback.  Is Rebol's ~NULL~ sensible to
//    pass as a nullptr?
//
static struct {
    Option(SymId) symid;
    const char* typespec;
} syms_to_typesets[] = {
    {SYM_VOID, "trash?"},  // TRASH is closest to C void (vs. Rebol VOID)
    {EXT_SYM_UINT8, "integer!"},
    {EXT_SYM_INT8, "integer!"},
    {EXT_SYM_UINT16, "integer!"},
    {EXT_SYM_INT16, "integer!"},
    {EXT_SYM_UINT32, "integer!"},
    {EXT_SYM_INT32, "integer!"},
    {EXT_SYM_UINT64, "integer!"},
    {EXT_SYM_INT64, "integer!"},
    {EXT_SYM_FLOAT, "decimal!"},
    {EXT_SYM_DOUBLE, "decimal!"},
    {EXT_SYM_POINTER, "null? integer! text! blob! vector! action!"},  // [1]
    {EXT_SYM_REBVAL, "any-value?"},
    {SYM_0, 0}
};


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
// Writes into `schema_out` a Rebol value which describes either a basic FFI
// type or the layout of a STRUCT! (not including data).
//
static Option(Error*) Trap_Make_Schema_From_Block(
    Sink(Element) schema_out,  // => INTEGER! or HANDLE! for struct
    Option(Sink(Element)) param_out,  // => parameter for use in ACTION!s
    const Element* block,
    const Symbol* symbol  // could be used in error reporting
){
    UNUSED(symbol);  // not currently used

    assert(Is_Block(block));
    if (Cell_Series_Len_At(block) == 0)
        return Error_Bad_Value(block);

    const Element* tail;
    const Element* item = Cell_List_At(&tail, block);

    DECLARE_ELEMENT (def);
    DECLARE_ELEMENT (temp);

    if (Is_Word(item) and Cell_Word_Id(item) == EXT_SYM_STRUCT_X) {
        //
        // [struct! [...struct definition...]]

        ++item;
        if (item == tail or not Is_Block(item))
            return Error_Bad_Value(block);

        // Use the block spec to build a temporary structure through the same
        // machinery that implements `make struct! [...]`

        Derelativize(def, item, Cell_List_Binding(block));

        Option(Error*) e = Trap_Make_Struct(temp, def);
        if (e)
            return e;

        assert(Is_Struct(temp));

        // !!! It should be made possible to create a schema without going
        // through a struct creation.  There are "raw" structs with no memory,
        // which would avoid the data series (but not the StructInstance stub)
        //
        Init_Block(schema_out, Cell_Struct_Schema(temp));

        if (param_out) {
            Init_Unconstrained_Parameter(
                unwrap param_out,
                FLAG_PARAMCLASS_BYTE(PARAMCLASS_NORMAL)
            );
            // TBD: constrain with STRUCT!
        }
        return SUCCESS;
    }

    if (Is_Struct(item)) {
        Init_Block(schema_out, Cell_Struct_Schema(item));
        if (param_out) {
            Init_Unconstrained_Parameter(
                unwrap param_out,
                FLAG_PARAMCLASS_BYTE(PARAMCLASS_NORMAL)
            );
            // TBD: constrain with STRUCT!
        }
        return SUCCESS;
    }

    if (Cell_Series_Len_At(block) != 1)
        return Error_Bad_Value(block);

    // !!! It was presumed the only parameter convention that made sense was
    // a normal args, but quoted ones could work too.  In particular, anything
    // passed to the C as a REBVAL*.  Not a huge priority.
    //
    if (not Is_Word(item))
        return Error_Bad_Value(block);

    Option(SymId) id = Cell_Word_Id(item);
    if (id == SYM_VOID) {
        Init_Space(schema_out);
    }
    else
        Init_Word(schema_out, Cell_Word_Symbol(item));

    if (param_out) {
        int index = 0;
        for (; ; ++index) {
            if (syms_to_typesets[index].symid == TYPE_0)
                return Error_User("Invalid FFI type indicator");

            if ((unwrap syms_to_typesets[index].symid) == id)
                continue;

            Init_Unconstrained_Parameter(
                unwrap param_out,
                FLAG_PARAMCLASS_BYTE(PARAMCLASS_NORMAL)
            );
            // tbd: constrain with syms_to_typesets[index].typeset
            break;
        }
    }

    return SUCCESS;
}


// According to the libffi documentation, the arguments "must be suitably
// aligned; it is the caller's responsibility to ensure this".
//
// We assume the store's data pointer will have suitable alignment for any
// type (currently Make_Series() is expected to match malloc() in this way).
// This will round the offset positions to an alignment appropriate for the
// type size given.
//
// This means sequential arguments in the store may have padding between them.
//
INLINE static void* Expand_And_Align_Core(
    Sink(Offset) offset_out,
    REBLEN align,
    Binary* store,
    REBLEN size
){
    REBLEN padding = Binary_Len(store) % align;
    if (padding != 0)
        padding = align - padding;

    *offset_out = Binary_Len(store) + padding;
    Expand_Flex_Tail(store, padding + size);
    return Flex_Data(store) + *offset_out;
}

INLINE static void* Expand_And_Align(
    Sink(Offset) offset_out,
    Binary* store,
    REBLEN size // assumes align == size
){
    return Expand_And_Align_Core(offset_out, size, store, size);
}


// Convert a Rebol value into a bit pattern suitable for the expectations of
// the FFI for how a C argument would be represented.  (e.g. turn an
// INTEGER! into the appropriate representation of an `int` in memory.)
//
static Option(Error*) Trap_Cell_To_Ffi(
    Sink(Offset) offset_out,
    Binary* store,
    void *dest,
    const Value* arg,
    const Element* schema,
    Option(const Symbol*) label,
    const Key* key,  // may be RETURN (not actually a named argument)
    const Param* param
){
    // Only one of dest or store should be non-nullptr.  This allows to write
    // either to a known pointer of sufficient size (dest) or to a series
    // that will expand enough to accommodate the data (store).
    //
    assert(store == nullptr ? dest != nullptr : dest == nullptr);

    if (dest == nullptr)
        *offset_out = 0;
    else
        *offset_out = 10200304;  // shouldn't be used, but avoid warning

    if (Is_Block(schema)) {
        StructField* top = Cell_Array_Known_Mutable(schema);

        assert(Field_Is_Struct(top));
        assert(not Field_Is_C_Array(top));  // !!! wasn't supported--should be?

        // !!! In theory a struct has to be aligned to its maximal alignment
        // needed by a fundamental member.  We'll assume that the largest
        // is sizeof(void*) here...this may waste some space in the padding
        // between arguments, but that shouldn't have any semantic effect.
        //
        if (dest == nullptr)
            dest = Expand_And_Align_Core(
                offset_out,
                sizeof(void*),
                store,
                Field_Width(top)  // !!! What about Field_Total_Size ?
            );

        if (arg == nullptr) {
            //
            // Return values don't have an incoming argument to fill into the
            // calling frame.
            //
            return SUCCESS;
        }

        // !!! There wasn't any compatibility checking here before (not even
        // that the arg was a struct.  :-/  It used a stored STRUCT! from
        // when the routine was specified to know what the size should be,
        // and didn't pay attention to the size of the passed-in struct.
        //
        // (One reason it didn't use the size of the passed-struct is
        // because it couldn't do so in the return case where arg was null)

        if (not Is_Struct(arg))
            return Error_Arg_Type(label, key, param, arg);

        Size size = Struct_Storage_Len(Cell_Struct(arg));
        if (size != Field_Width(top))
            return Error_Arg_Type(label, key, param, arg);

        memcpy(dest, Cell_Struct_Data_At(arg), size);

        Term_Binary_Len(store, *offset_out + size);
        return SUCCESS;
    }

    assert(Is_Word(schema));

    union {
        uint8_t u8;
        int8_t i8;
        uint16_t u16;
        int16_t i16;
        uint32_t u32;
        int32_t i32;
        int64_t i64;

        float f;
        double d;

        intptr_t ipt;
    } buffer;

    char *data;
    Size size;

    switch (Cell_Word_Id(schema)) {
      case EXT_SYM_UINT8: {
        if (not arg)
            buffer.u8 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.u8 = cast(uint8_t, VAL_INT64(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.u8);
        size = sizeof(buffer.u8);
        break; }

      case EXT_SYM_INT8: {
        if (not arg)
            buffer.i8 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.i8 = cast(int8_t, VAL_INT64(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.i8);
        size = sizeof(buffer.i8);
        break; }

      case EXT_SYM_UINT16: {
        if (not arg)
            buffer.u16 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.u16 = cast(uint16_t, VAL_INT64(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.u16);
        size = sizeof(buffer.u16);
        break; }

      case EXT_SYM_INT16: {
        if (not arg)
            buffer.i16 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.i16 = cast(int16_t, VAL_INT64(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.i16);
        size = sizeof(buffer.i16);
        break; }

      case EXT_SYM_UINT32: {
        if (not arg)
            buffer.u32 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.u32 = cast(int32_t, VAL_INT64(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.u32);
        size = sizeof(buffer.u32);
        break; }

      case EXT_SYM_INT32: {
        if (not arg)
            buffer.i32 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.i32 = cast(int32_t, VAL_INT64(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.i32);
        size = sizeof(buffer.i32);
        break; }

      case EXT_SYM_UINT64:
      case EXT_SYM_INT64: {
        if (not arg)
            buffer.i64 = 0;  // return value, make space (but initialize)
        else if (Is_Integer(arg))
            buffer.i64 = VAL_INT64(arg);
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.i64);
        size = sizeof(buffer.i64);
        break; }

      case EXT_SYM_POINTER: {
        //
        // Note: Function pointers and data pointers may not be same size.
        //
        if (not arg) {
            buffer.ipt = 0xDECAFBAD;  // return value, make space (but init)
        }
        else if (Heart_Of_Is_0(arg)) {
            if (rebNot("vector! = type of @", arg))
                return Error_User(
                    "VECTOR! is only extension type FFI accepts by pointer"
                );
            buffer.ipt = cast(intptr_t, rebUnboxInteger("address-of", arg));
            size = sizeof(buffer.ipt);
        }
        else if (Is_Nulled(arg)) {
            buffer.ipt = 0;
        }
        else switch (Type_Of(arg)) {
          case TYPE_INTEGER:
            buffer.ipt = VAL_INT64(arg);
            break;

        // !!! This is a questionable idea, giving out pointers directly into
        // Rebol series data.  The data may be relocated in memory if any
        // modifications happen during a callback (or in the future, just for
        // GC compaction even if not changed)...so the memory is not "stable".
        //
          case TYPE_TEXT:  // !!! copies a *pointer*!
            buffer.ipt = i_cast(intptr_t, Cell_Utf8_At(arg));
            break;

          case TYPE_BLOB:  // !!! copies a *pointer*!
            buffer.ipt = i_cast(intptr_t, Cell_Bytes_At(nullptr, arg));
            break;

          case TYPE_ACTION: {
            if (not Is_Action_Routine(arg))
                return Error_User(  // but routines, too?
                    "Only callback functions may be passed by FFI pointer"
                );

            RoutineDetails* r = Ensure_Cell_Frame_Details(arg);
            CFunction* cfunc = Routine_C_Function(r);
            size_t sizeof_cfunc = sizeof(cfunc);  // avoid conditional const
            if (sizeof_cfunc != sizeof(intptr_t))  // not necessarily true
                fail ("intptr_t size not equal to function pointer size");
            memcpy(&buffer.ipt, &cfunc, sizeof(intptr_t));
            break; }

          default:
            return Error_Arg_Type(label, key, param, arg);
        }

        data = cast(char*, &buffer.ipt);
        size = sizeof(buffer.ipt);
        break; }  // end case FFI_TYPE_POINTER

      case EXT_SYM_REBVAL: {
        if (not arg)
            buffer.ipt = 0xDECAFBAD;  // return value, make space (but init)
        else
            buffer.ipt = i_cast(intptr_t, arg);

        data = cast(char*, &buffer.ipt);
        size = sizeof(buffer.ipt);
        break; }

      case EXT_SYM_FLOAT: {
        if (not arg)
            buffer.f = 0;  // return value, make space (but initialize)
        else if (Is_Decimal(arg))
            buffer.f = cast(float, VAL_DECIMAL(arg));
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.f);
        size = sizeof(buffer.f);
        break; }

      case EXT_SYM_DOUBLE: {
        if (not arg)
            buffer.d = 0;
        else if (Is_Decimal(arg))
            buffer.d = VAL_DECIMAL(arg);
        else
            return Error_Arg_Type(label, key, param, arg);

        data = cast(char*, &buffer.d);
        size = sizeof(buffer.d);
        break;}

      case EXT_SYM_STRUCT_X:
        //
        // structs should be processed above by the HANDLE! case, not WORD!
        //
        assert(false);
        return Error_Bad_Value(arg);

      case SYM_VOID:
        //
        // can't return a meaningful offset for "void"--it's only valid for
        // return types, so caller should check and not try to pass it in.
        //
        assert(false);
        return Error_Bad_Value(arg);

      default:
        assert(false);
        return Error_Bad_Value(arg);
    }

    if (store) {
        assert(dest == nullptr);
        dest = Expand_And_Align(offset_out, store, size);
    }

    memcpy(dest, data, size);

    if (store)
        Term_Binary_Len(store, *offset_out + size);

    return SUCCESS;
}


// Convert a C value into a Rebol value.  Reverse of Trap_Cell_To_Ffi().
//
static void Ffi_To_Cell(
    Sink(Value) out,
    const Element* schema,
    void* ffi_rvalue
){
    if (Is_Block(schema)) {
        StructField* top = Cell_Array_Known_Mutable(schema);

        assert(Field_Is_Struct(top));
        assert(not Field_Is_C_Array(top));  // !!! wasn't supported, should be?

        StructInstance* stu = Prep_Stub(STUB_MASK_STRUCT, Alloc_Stub());
        Force_Erase_Cell(Stub_Cell(stu));
        LINK_STRUCT_SCHEMA(stu) = top;
        STRUCT_OFFSET(stu) = 0;

        Binary* data = Make_Binary_Core(
            NODE_FLAG_MANAGED,
            Field_Width(top)  // not Field_Is_C_Array, so no Field_Total_Size ?
        );
        memcpy(Binary_Head(data), ffi_rvalue, Field_Width(top));
        Term_Binary_Len(data, Field_Width(top));

        Reset_Extended_Cell_Header_Noquote(
            out,
            EXTRA_HEART_STRUCT,
            (not CELL_FLAG_DONT_MARK_NODE1)  // StructInstance needs mark
                | CELL_FLAG_DONT_MARK_NODE2  // Offset shouldn't be marked
        );
        CELL_NODE1(out) = stu;

        Init_Blob(Stub_Cell(stu), data);

        assert(Struct_Data_Head(stu) == Binary_Head(data));
        return;
    }

    assert(Is_Word(schema));

    switch (Cell_Word_Id(schema)) {
      case EXT_SYM_UINT8:
        Init_Integer(out, *cast(uint8_t*, ffi_rvalue));
        break;

      case EXT_SYM_INT8:
        Init_Integer(out, *cast(int8_t*, ffi_rvalue));
        break;

      case EXT_SYM_UINT16:
        Init_Integer(out, *cast(uint16_t*, ffi_rvalue));
        break;

      case EXT_SYM_INT16:
        Init_Integer(out, *cast(int16_t*, ffi_rvalue));
        break;

      case EXT_SYM_UINT32:
        Init_Integer(out, *cast(uint32_t*, ffi_rvalue));
        break;

      case EXT_SYM_INT32:
        Init_Integer(out, *cast(int32_t*, ffi_rvalue));
        break;

      case EXT_SYM_UINT64:
        Init_Integer(out, *cast(uint64_t*, ffi_rvalue));
        break;

      case EXT_SYM_INT64:
        Init_Integer(out, *cast(int64_t*, ffi_rvalue));
        break;

      case EXT_SYM_POINTER:  // !!! Should 0 come back as a NULL to Rebol?
        Init_Integer(out, i_cast(uintptr_t, *cast(void**, ffi_rvalue)));
        break;

      case EXT_SYM_FLOAT:
        Init_Decimal(out, *cast(float*, ffi_rvalue));
        break;

      case EXT_SYM_DOUBLE:
        Init_Decimal(out, *cast(double*, ffi_rvalue));
        break;

      case EXT_SYM_REBVAL:
        Copy_Cell(out, *cast(const Value**, ffi_rvalue));
        break;

      case SYM_VOID:
        assert(false); // not covered by generic routine.
        goto unknown_ffi_type;

      unknown_ffi_type:
      default:
        assert(false);
        //
        // !!! Was reporting Error_Invalid_Arg on uninitialized `out`
        //
        fail ("Unknown FFI type indicator");
    }
}


//
//  Routine_Dispatcher: C
//
Bounce Routine_Dispatcher(Level* const L)
{
    USE_LEVEL_SHORTHANDS (L);

    StackIndex base = TOP_INDEX;  // variadic args pushed to stack, save base

    RoutineDetails* r = Ensure_Level_Details(L);

    if (Is_Routine_Callback(r) or not Routine_Lib(r)) {
        //
        // lib is nullptr when routine is constructed from address directly,
        // so there's nothing to track whether that gets loaded or unloaded
    }
    else {
        if (rebNot("open?", unwrap Routine_Lib(r)))
            return FAIL("Library closed in Routine_Dispatcher()");
    }

    Count num_fixed = Routine_Num_Fixed_Args(r);
    Count num_args = num_fixed;  // we'll add num_variable if variadic
    Count num_variable = 0;  // will count them if variadic

    if (not Is_Routine_Variadic(r))
        goto make_backing_store;

  count_variadic_arguments: { ////////////////////////////////////////////////

    // Evaluate the VARARGS! feed of values to the data stack.  This way
    // they will be available to be counted, to know how big to make the
    // FFI argument series.
    //
    // 1. !!! The Atronix va_list interface required a type to be specified
    //    for each argument--achieving what you would get if you used a
    //    C cast on each variadic argument.  Such as:
    //
    //        printf reduce ["%d, %f" 10 + 20 [int32] 12.34 [float]]
    //
    //    While this provides generality, it may be useful to use defaulting
    //    like C's where integer types default to `int` and floating point
    //    types default to `double`.  In the VARARGS!-based syntax it could
    //    offer several possibilities:
    //
    //        (printf "%d, %f" (10 + 20) 12.34)
    //        (printf "%d, %f" [int32 10 + 20] 12.34)
    //        (printf "%d, %f" [int32] 10 + 20 [float] 12.34)
    //
    //     For the moment, this is following the idea that there must be
    //     pairings of values and then blocks (though the values are evaluated
    //     expressions).

    Phase* phase = Level_Phase(L);
    assert(Phase_Num_Params(phase) == num_fixed + 1);  // +1 for `...`

    Value* vararg = Level_Arg(L, num_fixed + 1); // 1-based
    assert(Is_Varargs(vararg));

    do {
        if (Do_Vararg_Op_Maybe_End_Throws(
            OUT,
            VARARG_OP_TAKE,
            vararg
        )){
            return THROWN;
        }

        if (Is_Barrier(OUT))
            break;

        Copy_Cell(PUSH(), stable_OUT);
    } while (true);

    if ((TOP_INDEX - base) % 2 != 0)  // must be paired [1]
        return FAIL("Variadic FFI functions must alternate blocks and values");

    num_variable = (TOP_INDEX - base) / 2;
    num_args += num_variable;

} make_backing_store: /////////////////////////////////////////////////////=//

    // The FFI arguments are passed by void*.  Those void pointers point to
    // transformations of the Rebol arguments into ranges of memory of
    // various sizes.  This is the backing store for those arguments, which
    // is appended to for each one.  The memory is freed after the call.
    //
    // The offsets array has one element for each argument.  These point at
    // indexes of where each FFI variable resides.  Offsets are used instead
    // of pointers in case the store has to be resized, which may move the
    // base of the series.  Hence the offsets must be mutated into pointers
    // at the last minute before the FFI call.
    //
    // 1. Shouldn't be used (assigned to nullptr later) but avoid maybe
    //    uninitialized warning.

    Binary* store = Make_Binary(1);

    Option(Element*) ret_schema = Routine_Return_Schema_Unless_Void(r);
    void* ret_offset;
    if (ret_schema) {
        Offset offset;
        const Symbol* ret_sym = CANON(RETURN);
        const Key* key = &ret_sym;  // return values, no name
        const Param* param = nullptr;
        Option(Error*) e = Trap_Cell_To_Ffi(
            &offset,
            store,  // ffi-converted arg appended here
            nullptr,  // dest pointer must be nullptr if store is non-nullptr
            nullptr,  // arg: none (only making space--leave uninitialized)
            unwrap ret_schema,
            Level_Label(L),
            key,
            param
        );
        if (e)
            return FAIL(unwrap e);
        ret_offset = p_cast(void*, offset);
    }
    else
        ret_offset = p_cast(void*, cast(Offset, 0xDECAFBAD));  // unused [1]

    Flex* arg_offsets;
    if (num_args == 0)
        arg_offsets = nullptr;  // don't waste time with the alloc + free
    else {
        arg_offsets = Make_Flex(FLAG_FLAVOR(POINTERS), Flex, num_args);
        Set_Flex_Len(arg_offsets, num_args);
    }

  gather_fixed_parameters: { /////////////////////////////////////////////////

    // Fixed parameters are known to be of correct general types (they were
    // typechecked in the call).  But a STRUCT! might not be compatible with
    // the type of STRUCT! in the parameter specification.  They might also be
    // out of range, e.g. a too-large or negative INTEGER! passed to a uint8.
    // So we could fail here.
    //
    // 1. We will convert this offset to a pointer later.

    Option(const Symbol*) label = Level_Label(L);

    REBLEN i = 0;
    for (; i < num_fixed; ++i) {
        const Param* param = Phase_Param(Level_Phase(L), i + 1);  // 1-based
        const Key* key = Varlist_Key(Level_Varlist(L), i + 1);  // 1-based
        const Value* arg = Level_Arg(L, i + 1);  // 1-based
        const Element* schema = Routine_Arg_Schema(r, i);  // 0-based

        Offset offset;
        Option(Error*) e = Trap_Cell_To_Ffi(
            &offset,
            store,  // ffi-converted arg appended here
            nullptr,  // dest pointer must be nullptr if store is non-null
            arg,
            schema,
            label,
            key,
            param
        );
        if (e)
            return FAIL(unwrap e);

        *Flex_At(void*, arg_offsets, i) = p_cast(void*, offset);  // offset [1]
    }

} create_cif_call_interface: /////////////////////////////////////////////////

    // These pointers need to be freed by HANDLE! cleanup.
    //
    // 1. If an FFI routine takes a fixed number of arguments, then its Call
    //    InterFace (CIF) can be created just once, and stored in the routine.
    //    However a variadic routine requires a CIF that matches the number
    //    and types of arguments for that specific call.
    //
    // 2. CIF creation requires a C array of argument descriptions that is
    //    contiguous across both the fixed and variadic parts.  Start by
    //    filling in the ffi_type*s for all the fixed args.
    //
    // 3. This param is used with the variadic type spec, and is initialized
    //    as it would be for an ordinary FFI argument.  This means its allowed
    //    type flags are set, which is not really necessary.

    ffi_cif *cif;  // pre-made if not variadic, built for this call otherwise
    ffi_type **args_fftypes = nullptr;  // ffi_type*[] if num_variable > 0

    if (not Is_Routine_Variadic(r)) {  // fixed args, CIF created once [1]
        cif = Routine_Call_Interface(r);
    }
    else {
        assert(Not_Cell_Readable(Routine_At(rin, IDX_ROUTINE_CIF)));
        assert(Not_Cell_Readable(Routine_At(rin, IDX_ROUTINE_ARG_FFTYPES)));

        args_fftypes = rebAllocN(ffi_type*, num_fixed + num_variable);  // [2]

        REBLEN i;
        for (i = 0; i < num_fixed; ++i)
            args_fftypes[i] = Schema_Ffi_Type(Routine_Arg_Schema(r, i));

        DECLARE_ELEMENT (schema);
        DECLARE_ELEMENT (param);

        const Symbol* varargs_symbol = EXT_CANON(VARARGS);
        const Key* key = &varargs_symbol;

        StackIndex dsp;
        for (dsp = base + 1; i < num_args; dsp += 2, ++i) {
            Option(Error*) e1 = Trap_Make_Schema_From_Block(  // [3]
                schema,
                param, // sets type bits in param
                Data_Stack_At(Element, dsp + 1), // errors if not a block
                varargs_symbol  // symbol will appear in error reports
            );
            if (e1)
                return FAIL(unwrap e1);

            args_fftypes[i] = Schema_Ffi_Type(schema);

            Offset offset;
            const Param* param = nullptr;
            Option(Error*) e2 = Trap_Cell_To_Ffi(
                &offset,
                store,  // data appended to store
                nullptr,  // dest pointer must be null if store is non-null
                Data_Stack_At(Value, dsp),  // arg
                schema,
                Level_Label(L),
                key,  // REVIEW: need key for error messages
                param
            );
            if (e2)
                return FAIL(unwrap e2);

            *Flex_At(void*, arg_offsets, i) = p_cast(void*, offset);
        }

        Drop_Data_Stack_To(base);  // done w/args (converted to bytes in store)

        cif = rebAlloc(ffi_cif);

        Option(Element*) ret_schema = Routine_Return_Schema_Unless_Void(r);
        ffi_status status = ffi_prep_cif_var(  // _var-iadic prep_cif version
            cif,
            Routine_Abi(r),
            num_fixed,  // just fixed
            num_args,  // fixed plus variable
            ret_schema
                ? Schema_Ffi_Type(unwrap ret_schema)
                : &ffi_type_void,
            args_fftypes  // arguments FFI types
        );

        if (status != FFI_OK) {
            rebFree(cif);  // would free automatically on fail
            rebFree(args_fftypes);  // would free automatically on fail
            return FAIL(Error_User("FFI: Couldn't prep CIF_VAR"));
        }
    }

  change_arg_offsets_into_pointers: { ////////////////////////////////////////

    // Now that all the additions to store have been made, we want to change
    // the offsets of each FFI argument into actual pointers (since the
    // data won't be relocated)

    if (Routine_Return_Schema_Unless_Void(r))
        ret_offset = Flex_Data(store) + i_cast(Offset, ret_offset);
    else
        ret_offset = nullptr;  // void return, no associated storage

    REBLEN i;
    for (i = 0; i < num_args; ++i) {
        Offset off = i_cast(Offset, *Flex_At(void*, arg_offsets, i));
        assert(off == 0 or off < Binary_Len(store));
        *Flex_At(void*, arg_offsets, i) = Binary_At(store, off);
    }

} make_actual_ffi_call: { ////////////////////////////////////////////////////

    // Note that the "offsets" are now direct pointers.  Also note that
    // any callbacks which run Rebol code during the course of calling this
    // arbitrary C code are not allowed to propagate failures out of the
    // callback--they'll panic and crash the interpreter, since they don't
    // know what to do otherwise.  See MAKE-CALLBACK/FALLBACK for some
    // mitigation of this problem.

    ffi_call(
        cif,
        Routine_C_Function(r),
        ret_offset,  // actually a real pointer now (no longer an offset)
        (num_args == 0)
            ? nullptr
            : Flex_Head(void*, arg_offsets)  // also real pointers now
    );

    Option(Element*) ret_schema = Routine_Return_Schema_Unless_Void(r);
    if (ret_schema)
        Ffi_To_Cell(OUT, unwrap ret_schema, ret_offset);
    else
        Init_Tripwire(OUT);  // !!! Is ~ antiform best return result for void?

    if (num_args != 0)
        Free_Unmanaged_Flex(arg_offsets);

    Free_Unmanaged_Flex(store);

    if (Is_Routine_Variadic(r)) {
        rebFree(cif);
        rebFree(args_fftypes);
    }

    return OUT;  // Note: cannot "throw" a Rebol value across an FFI boundary.
}}


//
//  Routine_Details_Querier: C
//
bool Routine_Details_Querier(
    Sink(Value) out,
    Details* details,
    SymId property
){
    RoutineDetails* r = details;

    switch (property) {
      case SYM_RETURN_OF: {
        Extract_Paramlist_Returner(out, Phase_Paramlist(details), SYM_RETURN);
        return true; }

      case SYM_BODY_OF: {
        assert(!"Body of not supported by Routine yet");
        Init_Space(out);
        return true; }

    // 1. The CFunction is fabricated by the FFI if it's a callback, or just
    //    the wrapped DLL function if it's an ordinary routine

      case SYM_ADDRESS_OF:
        return Init_Integer(
            out,
            i_cast(intptr_t, Routine_C_Function(r))  // fabricated/wrapped [1]
        );

      default:
        break;
    }

    return false;
}


//
// cleanup_ffi_closure: C
//
// The GC-able HANDLE! used by callbacks contains a ffi_closure pointer that
// needs to be freed when the handle references go away (really only one
// reference is likely--in the ACT_BODY of the callback, but still this is
// how the GC gets hooked in Ren-C)
//
void cleanup_ffi_closure(const Value* closure_handle) {
    ffi_closure_free(Cell_Handle_Pointer(ffi_closure, closure_handle));
}

static void cleanup_cif(const Value* cif_handle) {
    Free_Memory(ffi_cif, Cell_Handle_Pointer(ffi_cif, cif_handle));
}

static void cleanup_args_fftypes(const Value* fftypes_handle) {
    Free_Memory_N(ffi_type*,
        Cell_Handle_Len(fftypes_handle),
        Cell_Handle_Pointer(ffi_type*, fftypes_handle)
    );
}


//
//   callback_dispatcher: C
//
// Callbacks allow C code to call Rebol functions.  It does so by creating a
// stub function pointer that can be passed in slots where C code expected
// a C function pointer.  When such stubs are triggered, the FFI will call
// this dispatcher--which was registered using ffi_prep_closure_loc().
//
// An example usage of this feature is in %qsort.r, where the C library
// function qsort() is made to use a custom comparison function that is
// actually written in Rebol.
//
// 1. We pass a RoutineDetails*, but if we passed an actual Value* of the
//    routine's ACTION! we could have access to the cached symbol for error
//    reporting (which may just be a panic() here, but useful even so).
//
void callback_dispatcher(  // client C code calls this, not the trampoline
    ffi_cif* cif,
    void* ret,
    void** args,
    void* user_data
){
    RoutineDetails* r = cast(RoutineDetails*, user_data);

    Option(const Symbol*) label = nullptr;  // tunnel symbol cache? [1]

  build_array_that_represents_call: //////////////////////////////////////////

    // The first item in that array will be the callback function value, and
    // then the arguments will be the remaining values.

    assert(not Is_Routine_Variadic(r));  // not supported
    assert(cif->nargs == Routine_Num_Fixed_Args(r));

    Source* arr = Make_Source(1 + cif->nargs);
    Element* elem = Array_Head(arr);
    Copy_Meta_Cell(elem, Routine_Callback_Action(r));
    QUOTE_BYTE(elem) = NOQUOTE_1;
    assert(Is_Frame(elem));

    ++elem;

    REBLEN i;
    for (i = 0; i != cif->nargs; ++i, ++elem) {
        DECLARE_VALUE (value);
        Ffi_To_Cell(value, Routine_Arg_Schema(r, i), args[i]);
        Copy_Meta_Cell(elem, value);
    }

    Set_Flex_Len(arr, 1 + cif->nargs);
    Manage_Flex(arr);  // DO requires managed arrays (guarded while running)

    DECLARE_ELEMENT (code);
    Init_Block(code, arr);

    DECLARE_ATOM (result);

  RESCUE_SCOPE_IN_CASE_OF_ABRUPT_FAILURE {  //////////////////////////////////

    // 1. If a callback encounters an un-trapped fail() in mid-run, or if the
    //    execution attempts to throw (e.g. CONTINUE or THROW natives called)
    //    there's nothing we can do here to guess what its C contract return
    //    value should be.  And we can't just jump up to the next trap point,
    //    because that would cross unknown client C code using the FFI (if it
    //    were C++, the destructors might not run, etc.)
    //
    //    See MAKE-CALLBACK:FALLBACK for the usermode workaround.

    if (Eval_Any_List_At_Throws(result, code, SPECIFIED))
        panic (Error_No_Catch_For_Throw(TOP_LEVEL));  // THROW, CONTINUE... [1]

    Decay_If_Unstable(result);  // RAISED! fail()s, jumps to ON_ABRUPT_FAILURE

    CLEANUP_BEFORE_EXITING_RESCUE_SCOPE;
    goto finished_rebol_call;

} ON_ABRUPT_FAILURE(error) {  ////////////////////////////////////////////////

    panic (error);  // can't give meaningful return value on fail() [1]

} finished_rebol_call: { /////////////////////////////////////////////////////

    Option(Element*) ret_schema = Routine_Return_Schema_Unless_Void(r);
    if (cif->rtype->type == FFI_TYPE_VOID)
        assert(not ret_schema);
    else {
        assert(ret_schema);

        const Symbol* spelling = CANON(RETURN);
        const Param* param = nullptr;
        Offset offset;
        Option(Error*) e = Trap_Cell_To_Ffi(
            &offset,
            nullptr,  // store must be null if dest is non-null,
            ret,  // destination pointer
            cast(Value*, result),
            unwrap ret_schema,
            label,
            &spelling,  // parameter used for symbol in error only
            param
        );
        if (e)
            fail (unwrap e);
        UNUSED(offset);
    }
}}


//
//  Alloc_Ffi_Action_For_Spec: C
//
// This allocates an ACTION! designed for using with the FFI--though it does
// not fill in the actual code to run.  That is done by the caller, which
// needs to be done differently if it runs a C function (routine) or if it
// makes Rebol code callable as if it were a C function (callback).
//
// It has a HANDLE! holding a Routine INfo structure (RIN) which describes
// the FFI argument types.  For callbacks, this cannot be automatically
// deduced from the parameters of the Rebol function it wraps--because there
// are multiple possible mappings (e.g. differently sized C types all of
// which are passed in from Rebol's INTEGER!)
//
// The spec format is a block which is similar to the spec for functions:
//
// [
//     "document"
//     arg1 [type1 type2] "note"
//     arg2 [type3] "note"
//     ...
//     argn [typen] "note"
//     return: [type] "note"
// ]
//
Option(Error*) Trap_Alloc_Ffi_Action_For_Spec(
    Sink(RoutineDetails*) out,
    const Element* ffi_spec,
    ffi_abi abi
){
    assert(Is_Block(ffi_spec));

    StackIndex base = TOP_INDEX;

    RoutineDetails* r;
    Count num_fixed = 0;  // number of fixed (non-variadic) arguments
    bool is_variadic = false;  // default to not being variadic

  build_paramlist_on_data_stack: { ///////////////////////////////////////////

    // arguments can be complex, defined as structures.  A "schema" is a
    // REBVAL that holds either an INTEGER! for simple types, or a HANDLE!
    // for compound ones.
    //
    // Note that in order to avoid deep walking the schemas after construction
    // to convert them from unmanaged to managed, they are managed at the
    // time of creation.  This means that the array of them has to be
    // guarded across any evaluations, since the routine being built is not
    // ready for GC visibility.
    //
    // !!! Should the spec analysis be allowed to do evaluation? (it does)

    const REBLEN capacity_guess = 8;  // !!! Magic number...why 8? (can grow)
    Source* args_schemas = Make_Source_Managed(capacity_guess);
    Push_Lifeguard(args_schemas);

    DECLARE_ELEMENT (ret_schema_or_space);
    Init_Space(ret_schema_or_space);  // defaults SPACE (e.g. void C func)
    Push_Lifeguard(ret_schema_or_space);

    const Element* tail;
    const Element* item = Cell_List_At(&tail, ffi_spec);
    for (; item != tail; ++item) {
        if (Is_Text(item))  // comment or argument description
            continue;  // !!! TBD: extract adjunct info from spec notes

        if (Is_Set_Word(item)) {  // TYPE_CHAIN, not TYPE_SET_WORD
            if (Cell_Word_Id(item) != SYM_RETURN)
                return Error_Bad_Value(item);

            if (not Is_Space(ret_schema_or_space))
                return Error_User("FFI: Return already specified");

            ++item;

            DECLARE_ELEMENT (block);
            Derelativize(block, item, Cell_List_Binding(ffi_spec));

            Option(Error*) e = Trap_Make_Schema_From_Block(
                ret_schema_or_space,
                nullptr,  // dummy (return/output has no arg to typecheck)
                block,
                CANON(RETURN)
            );
            if (e)
                return e;
        }
        else if (Is_Word(item)) {
            const Symbol* name = Cell_Word_Symbol(item);

            if (Are_Synonyms(name, CANON(ELLIPSIS_1))) {  // variadic
                if (is_variadic)
                    return Error_User("FFI: Duplicate ... indicating variadic");

                is_variadic = true;

                // !!! Originally, a feature in VARARGS! was that they would
                // "chain" by default, if VARARGS! was not explicitly added.
                // This feature was removed, but may be re-added:
                //
                // https://github.com/metaeducation/ren-c/issues/801
                //
                // For that reason, varargs was not in the list by default.
                //
                Init_Word(PUSH(), EXT_CANON(VARARGS));
                Init_Unconstrained_Parameter(
                    PUSH(),
                    FLAG_PARAMCLASS_BYTE(PARAMCLASS_NORMAL)
                        | PARAMETER_FLAG_VARIADIC
                );
            }
            else {  // ordinary argument
                if (is_variadic)
                    return Error_User("FFI: Variadic must be final parameter");

                ++item;

                DECLARE_ELEMENT (block);
                Derelativize(block, item, Cell_List_Binding(ffi_spec));

                Init_Word(PUSH(), name);
                Option(Error*) e = Trap_Make_Schema_From_Block(
                    Alloc_Tail_Array(args_schemas),  // schema (out)
                    PUSH(),  // param (out)
                    block,  // block (in)
                    name
                );
                if (e)
                    return e;

                ++num_fixed;
            }
        }
        else
            return Error_Bad_Value(item);
    }

  pop_paramlist_and_create_routine: { ////////////////////////////////////////

    Option(Phase*) prior = nullptr;
    Option(VarList*) prior_coupling = nullptr;

    ParamList* paramlist;
    Option(Error*) e = Trap_Pop_Paramlist(
        &paramlist, base, prior, prior_coupling
    );
    if (e)
        return e;

    r = Make_Dispatch_Details(
        DETAILS_MASK_NONE,
        Phase_Archetype(paramlist),
        &Routine_Dispatcher,
        MAX_IDX_ROUTINE  // details array len
    );

    Init_Integer(Routine_At(r, IDX_ROUTINE_ABI), abi);

    Init_Unreadable(Routine_At(r, IDX_ROUTINE_CFUNC));  // caller must update
    Init_Unreadable(Routine_At(r, IDX_ROUTINE_CLOSURE));  // "
    Init_Unreadable(Routine_At(r, IDX_ROUTINE_ORIGIN));  // " LIBRARY!/ACTION!

    Copy_Cell(Routine_At(r, IDX_ROUTINE_RET_SCHEMA), ret_schema_or_space);
    Drop_Lifeguard(ret_schema_or_space);

    Init_Logic(Routine_At(r, IDX_ROUTINE_IS_VARIADIC), is_variadic);

    Assert_Array(args_schemas);
    Init_Block(Routine_At(r, IDX_ROUTINE_ARG_SCHEMAS), args_schemas);
    Drop_Lifeguard(args_schemas);

}} build_cif_call_interface_if_not_variadic: { ///////////////////////////////

    // If a routine is variadic, then each individual invocation needs to use
    // `ffi_prep_cif_var` to make the proper variadic CIF for that call.
    //
    // But if it's not variadic, the same CIF can be used each time.  The CIF
    // must stay alive for the lifetime of the args_fftyps (apparently).

    if (Is_Routine_Variadic(r)) {
        Init_Unreadable(Routine_At(r, IDX_ROUTINE_CIF));
        Init_Unreadable(Routine_At(r, IDX_ROUTINE_ARG_FFTYPES));
        *out = r;
        return SUCCESS;
    }

    ffi_cif* cif = Try_Alloc_Memory(ffi_cif);

    ffi_type** args_fftypes;
    if (num_fixed == 0)
        args_fftypes = nullptr;
    else
        args_fftypes = Try_Alloc_Memory_N(ffi_type*, num_fixed);

    REBLEN i;
    for (i = 0; i < num_fixed; ++i)
        args_fftypes[i] = Schema_Ffi_Type(Routine_Arg_Schema(r, i));

    Option(Element*) ret_schema = Routine_Return_Schema_Unless_Void(r);
    if (
        FFI_OK != ffi_prep_cif(
            cif,
            abi,
            num_fixed,
            ret_schema
                ? Schema_Ffi_Type(unwrap ret_schema)
                : &ffi_type_void,
            args_fftypes  // nullptr if 0 fixed args
        )
    ){
        return Error_User("FFI: Couldn't prep CIF");
    }

    Init_Handle_Cdata_Managed(
        Routine_At(r, IDX_ROUTINE_CIF),
        cif,
        sizeof(&cif),
        &cleanup_cif
    );

    if (args_fftypes == nullptr)
        Init_Space(Routine_At(r, IDX_ROUTINE_ARG_FFTYPES));
    else
        Init_Handle_Cdata_Managed(
            Routine_At(r, IDX_ROUTINE_ARG_FFTYPES),
            args_fftypes,
            num_fixed,
            &cleanup_args_fftypes
        );  // lifetime must match cif lifetime

    *out = r;
    return SUCCESS;
}}


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
    rebRelease(handle);

    Init_Space(Routine_At(r, IDX_ROUTINE_CLOSURE));
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
    Init_Space(Routine_At(r, IDX_ROUTINE_CLOSURE));
    Init_Space(Routine_At(r, IDX_ROUTINE_ORIGIN)); // no LIBRARY! in this case.

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
        Routine_Call_Interface(r),
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
