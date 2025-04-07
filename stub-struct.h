//
//  File: %reb-struct.h
//  Summary: "Struct to C function"
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
// STRUCT! is an extension value type that models a C `struct {}` value.
// The cell holds a pointer to a node containing the data: a singular Array
// (a "StructInstance"), that typically holds just one BINARY! value with the
// memory of the instance.  Then, the LINK() field of this StructInstance
// points to a  StructField schema that models the names/types/sizes/offsets
// of the fields inside that memory block.
//
// A STRUCT!'s StructInstance can be seen as somewhat like an OBJECT!s VarList.
// But instead of a LINK() to a "keylist", it links to a StructField array with
// indexed elements corresponding to descriptor properties for the FFI (one of
// which is a dynamically created `ffi_type` for the structure, as required by
// libffi to use it).  As C structs can contain other structs, StructField
// can model not just a struct but also an element of a struct...so the
// top-level schema contains an array of the constitution StructField items.
//
// As with OBJECT! keylists, once a StructField schema is created, it may be
// shared among multiple instances that share that schema.
//
// With this model of a C struct in place, Rebol can own the memory underlying
// a structure.  Then it can choose to fill that memory (or leave it
// uninitialized to be filled), and pass it through to a C function that is
// expecting structs--either by pointer or by value.  It can access the
// structure with operations that do translated reads of the memory into Rebol
// values, or encode Rebol values as changing the right bytes at the right
// offset for a translated write.
//
///// NOTES ///////////////////////////////////////////////////////////////=//
//
// * See comments on ADDR-OF from the FFI about how the potential for memory
//   instability of content pointers may not be a match for the needs of an
//   FFI interface.  While calling into arbitrary C code with memory pointers
//   is fundamentally a dicey operation no matter what--there is a need for
//   some level of pointer locking if memory to mutable Rebol strings is
//   to be given out as raw UTF-8.
//
// * Atronix's initial implementation of the FFI used custom C structures to
//   describe things like the properties of a routine, or the schema of a
//   struct layout.  This required specialized hooks into the garbage
//   collector, that indicated locations in those C structs that pointers to
//   GC-managed elements lived.  Ren-C moved away from this, so that the
//   descriptors are ordinary Rebol arrays.  It's only a little bit less
//   efficient, and permitted the FFI to be migrated to an extension, so it
//   would not bring cost to builds that didn't use it (e.g. WASM build)
//
// * Because structs are not a built-in Cell type, they are "extension types",
//   and hence must sacrifice one of their four platform-sized pointer fields
//   for their type information (the "ExtraHeart").  So, the "extra" pointer
//   in the STRUCT! Cell is not available for other uses).
//

#include <ffi.h>


// The LIBRARY! concept modeling a .DLL or .so file is no longer a built-in
// type.  The "Library Extension" provides it.  There is no particularly good
// system for making dependent extensions, so we interact with LIBRARY! via
// the librebol API, e.g. rebValue("make library! ..."); instead of trying
// to make C function calls to the guts of the library extension.
//
/* #include "sys-library.h" */


// Returns an ffi_type* (which contains a ->type field, that holds the
// FFI_TYPE_XXX enum).
//
// Note: We avoid creating a "VOID" type in order to not give the illusion of
// void parameters being legal.  The VOID! return type is handled exclusively
// by the return value, to prevent potential mixups.
//
INLINE Option(ffi_type*) Get_Ffi_Type_For_Symbol(SymId id) {
    switch (id) {
      case EXT_SYM_UINT8: return &ffi_type_uint8;
      case EXT_SYM_INT8: return &ffi_type_sint8;
      case EXT_SYM_UINT16: return &ffi_type_uint16;
      case EXT_SYM_INT16: return &ffi_type_sint16;
      case EXT_SYM_UINT32: return &ffi_type_uint32;
      case EXT_SYM_INT32: return &ffi_type_sint32;
      case EXT_SYM_UINT64: return &ffi_type_uint64;
      case EXT_SYM_INT64: return &ffi_type_sint64;
      case EXT_SYM_FLOAT: return &ffi_type_float;
      case EXT_SYM_DOUBLE: return &ffi_type_double;
      case EXT_SYM_POINTER: return &ffi_type_pointer;
      case EXT_SYM_REBVAL: return &ffi_type_pointer;

    // !!! SYM_INTEGER, SYM_DECIMAL, SYM_STRUCT was "-1" in original table

      default:
        assert(false);
        return nullptr;
    }
}


//=//// FFI STRUCT SCHEMA DESCRIPTOR (FLD) ////////////////////////////////=//
//
// A "field" is a small BLOCK! of properties that describe what is basically
// a single item in a C struct (e.g. `struct { ... int field[3]; ....}`).  It
// has primary information like the type (`int`), name ("field"), and
// dimensionality (3).  But it also caches derived information, like the
// offset within the struct or the total size.
//
// Since you can embed structs in structs, this same field type for "one
// element" is the same type used for a toplevel overall schema of a struct.
//
// Schemas are StructField arrays, which contain all the information about
// the structure's layout, regardless of what offset it would find itself at
// inside of a data blob.  This includes the total size, and arrays of
// field definitions...essentially, the validated spec.  It also contains
// a HANDLE! for the `ffi_type`, a structure that needs to be made that
// coalesces the information the FFI has to know to interpret the binary.
//
// !!! Making this a VarList of an OBJECT! instead of an Array of a BLOCK!
// could be better, if this information is expected to be reflected out
// to the user, so they can see the description of the schema.

typedef Source StructField;  // alias to help find usages

enum {
    // A WORD! name for the field (or BLANK! if anonymous)
    //
    // https://gcc.gnu.org/onlinedocs/gcc-4.7.2/gcc/Unnamed-Fields.html
    //
    IDX_FIELD_NAME = 0,

    // WORD! type symbol or a BLOCK! of fields if this is a struct.  Symbols
    // generally map to FFI_TYPE_XXX constant (e.g. UINT8) but may also
    // be a special extension, such as REBVAL.
    //
    IDX_FIELD_TYPE = 1,

    // An INTEGER! of the array dimensionality, or BLANK! if not an array.
    //
    IDX_FIELD_DIMENSION = 2,

    // HANDLE! to the ffi_type* representing this entire field.  If it's a
    // premade ffi_type then it's a simple HANDLE! with no GC participation.
    // If it's a struct then it will use the shared form of HANDLE!, which
    // will GC the memory pointed to when the last reference goes away.
    //
    IDX_FIELD_FFTYPE = 3,

    // An INTEGER! of the offset this field is relative to the beginning
    // of its entire containing structure.  Will be BLANK! if the structure
    // is actually the root structure itself.
    //
    // !!! Comment said "size is limited by struct->offset, so only 16-bit"?
    //
    IDX_FIELD_OFFSET = 4,

    // An INTEGER! size of an individual field element ("wide"), in bytes.
    //
    IDX_FIELD_WIDE = 5,

    IDX_FIELD_MAX
};

#define Field_Detail(a,n) \
    Flex_At(Value, (a), (n))  // locate index access

INLINE Option(const Symbol*) Field_Name(StructField* f) {
    if (Is_Blank(Field_Detail(f, IDX_FIELD_NAME)))
        return nullptr;
    return Cell_Word_Symbol(Field_Detail(f, IDX_FIELD_NAME));
}

INLINE bool Field_Is_Struct(StructField* f) {
    if (Is_Block(Field_Detail(f, IDX_FIELD_TYPE)))
        return true;
    assert(Field_Name(f) != nullptr);  // only for toplevel struct schemas
    return false;
}

// 1. Handling for nested structs is sufficiently different in all cases that
//    having the client branch on Field_Is_Struct() is better than returning
//    SYM_STRUCT_X.
//
INLINE SymId Field_Type_Id(StructField* f) {
    assert(not Field_Is_Struct(f));  // better than returning SYM_STRUCT_X [1]
    assert(Is_Word(Field_Detail(f, IDX_FIELD_TYPE)));
    return unwrap Cell_Word_Id(Field_Detail(f, IDX_FIELD_TYPE));
}

INLINE Source* Field_Subfields_Array(StructField* f) {
    assert(Field_Is_Struct(f));
    return Cell_Array_Known_Mutable(Field_Detail(f, IDX_FIELD_TYPE));
}

INLINE bool Field_Is_C_Array(StructField* f) {
    if (Is_Blank(Field_Detail(f, IDX_FIELD_DIMENSION)))
        return false;
    assert(Is_Integer(Field_Detail(f, IDX_FIELD_DIMENSION)));
    return true;
}

INLINE REBLEN Field_Dimension(StructField* f) {
    assert(Field_Is_C_Array(f));
    return VAL_UINT32(Field_Detail(f, IDX_FIELD_DIMENSION));
}

INLINE ffi_type* Field_Ffi_Type(StructField* f)
    { return Cell_Handle_Pointer(ffi_type, Field_Detail(f, IDX_FIELD_FFTYPE)); }

INLINE REBLEN Field_Offset(StructField* f)
    { return VAL_UINT32(Field_Detail(f, IDX_FIELD_OFFSET)); }

INLINE REBLEN Field_Width(StructField* f)
    { return VAL_UINT32(Field_Detail(f, IDX_FIELD_WIDE)); }

INLINE REBLEN Field_Total_Size(StructField* f) {
    if (Field_Is_C_Array(f))
        return Field_Width(f) * Field_Dimension(f);
    return Field_Width(f);
}

INLINE ffi_type* SCHEMA_FFTYPE(const Element* schema) {
    if (Is_Block(schema)) {
        StructField* field = Cell_Array_Known_Mutable(schema);
        return Field_Ffi_Type(field);
    }
    return unwrap Get_Ffi_Type_For_Symbol(unwrap Cell_Word_Id(schema));
}


#define VAL_STRUCT_LIMIT UINT32_MAX


//=//// STRUCTURE INSTANCE (StructInstance) ///////////////////////////////=//
//
// A StructInstance is a singular array, typically holding a BLOB! value of
// bytes which represent the memory for the struct instance.  (If the struct
// is actually describing something at an absolute location in memory that
// Rebol does not control, it will be a HANDLE! with that pointer instead.)
//
// The Stub.link field of this singular array points to a StructField* that
// describes the "schema" of the struct.
//

typedef Stub StructInstance;

#define STUB_MASK_STRUCT ( \
    FLAG_FLAVOR(CELLS) \
        | NODE_FLAG_MANAGED \
        | STUB_FLAG_LINK_NODE_NEEDS_MARK)

#define LINK_STRUCT_SCHEMA(stu)     STUB_LINK(stu)  // StructField*
#define MISC_STRUCT_OFFSET(stu)     (stu)->misc.u32
// INFO is not currently used
// BONUS is not currently used...


INLINE StructField* Struct_Schema(StructInstance* stu) {
    StructField* schema = cast(StructField*, LINK_STRUCT_SCHEMA(stu));
    assert(Field_Is_Struct(schema));
    return schema;
}

#define Struct_Storage(stu) \
    cast(Element*, Stub_Cell(stu))  // BINARY! or HANDLE!

#define STRUCT_OFFSET(stu) \
    MISC_STRUCT_OFFSET(stu)

INLINE Source* Struct_Fields_Array(StructInstance* stu)
  { return Field_Subfields_Array(Struct_Schema(stu)); }

INLINE Size Struct_Total_Size(StructInstance* stu)
  { return Field_Width(Struct_Schema(stu)); }

#define Struct_Ffi_Type(stu) \
    Field_Ffi_Type(Struct_Schema(stu))

INLINE Byte* Struct_Data_Head(StructInstance* stu) {
    Element* data = Struct_Storage(stu);
    if (Is_Blob(data))
        return Binary_Head(Cell_Binary_Known_Mutable(data));

    assert(Cell_Handle_Len(data) != 0);  // is HANDLE!
    return Cell_Handle_Pointer(Byte, data);
}

INLINE REBLEN Struct_Storage_Len(StructInstance* stu) {
    Element* data = Struct_Storage(stu);
    if (Is_Blob(data))
        return Cell_Series_Len_At(data);

    assert(Cell_Handle_Len(data) != 0);  // is HANDLE!
    return Cell_Handle_Len(data);
}


// Just as with the varlist of an object, the struct's data is a node for the
// instance that points to the schema.
//
// !!! The series data may come from an outside pointer, hence Cell_Struct_Storage
// may be a handle instead of a BINARY!.

INLINE StructInstance* Cell_Struct(const Cell* cell) {
    assert(Cell_Extra_Heart(cell) == EXTRA_HEART_STRUCT);
    StructInstance* stu = cast(StructInstance*, CELL_NODE1(cell));

    Element* data = Struct_Storage(stu);
    if (Is_Blob(data)) {
        // it's not "external", so never inaccessible
    }
    else {
        assert(Is_Handle(data));
        if (Cell_Handle_Len(data) == 0) {  // inaccessible data
            DECLARE_ELEMENT (i);
            Init_Integer(i, i_cast(intptr_t, Struct_Data_Head(stu)));
            fail (Error_Bad_Memory_Raw(i, i));  // !!! Can't pass stu?
        }
    }
    return stu;
}

#define Cell_Struct_Schema(v) \
    Struct_Schema(Cell_Struct(v))

#define Cell_Struct_Total_Size(v) \
    Struct_Total_Size(Cell_Struct(v))

#define Cell_Struct_Data_Head(v) \
    Struct_Data_Head(Cell_Struct(v))


INLINE Byte* Cell_Struct_Data_At(const Cell* cell) {
    StructInstance* stu = Cell_Struct(cell);
    return Struct_Data_Head(stu) + STRUCT_OFFSET(stu);
}

INLINE REBLEN Cell_Struct_Data_Size(const Cell* cell) {
    return Struct_Storage_Len(Cell_Struct(cell));
}

#define Cell_Struct_Fields_Array(v) \
    Struct_Fields_Array(Cell_Struct(v))

#define VAL_STRUCT_FFTYPE(v) \
    Struct_Ffi_Type(Cell_Struct(v))

INLINE Element* Init_Struct(Init(Element) out, StructInstance* stu) {
    assert(Is_Node_Managed(stu));
    STRUCT_OFFSET(stu) = 0;  // !!! should this be done here?

    Reset_Extended_Cell_Header_Noquote(
        out,
        EXTRA_HEART_STRUCT,
        (not CELL_FLAG_DONT_MARK_NODE1)  // array node needs mark
            | CELL_FLAG_DONT_MARK_NODE2  // offset shouldn't be marked
    );

    CELL_NODE1(out) = stu;

    return out;
}


//=//// FFI ROUTINE INFO DESCRIPTOR (RIN) /////////////////////////////////=//
//
// ...

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


// !!! FORWARD DECLARATIONS
//
// Currently there is no auto-processing of the files in extensions to look
// for C functions and extract their prototypes to be used within that
// extension.  Maintain manually for the moment.
//

extern StructInstance* Copy_Struct_Managed(StructInstance* src);
extern Option(Error*) Trap_Init_Struct_Fields(
    Sink(Element) ret,
    const Element* spec
);
extern Option(Error*) Trap_Alloc_Ffi_Action_For_Spec(
    Sink(RoutineDetails*) r,
    const Element* ffi_spec,
    ffi_abi abi
);
extern void callback_dispatcher(
    ffi_cif *cif,
    void *ret,
    void **args,
    void *user_data
);
extern void cleanup_ffi_closure(const RebolValue* v);

extern Option(Error*) Trap_Make_Struct(Sink(Element) out, const Element* arg);


extern Bounce Routine_Dispatcher(Level *L);

INLINE bool Is_Action_Routine(const Value* v) {
    Phase* phase = Cell_Frame_Phase(v);

    if (not Is_Stub_Details(phase))
        return false;  // !!! review cases where specializations could work

    if (Details_Dispatcher(cast(Details*, phase)) != &Routine_Dispatcher)
        return false;

    return true;
}
