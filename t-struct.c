//
//  File: %t-struct.c
//  Summary: "C struct object datatype"
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


// The managed HANDLE! for a ffi_type will have a reference in structs that
// use it.  Basic non-struct FFI_TYPE_XXX use the stock ffi_type_xxx pointers
// that do not have to be freed, so they use simple HANDLE! which do not
// register this cleanup hook.
//
static void cleanup_ffi_type(const Value* v) {
    ffi_type *fftype = Cell_Handle_Pointer(ffi_type, v);
    if (fftype->type == FFI_TYPE_STRUCT)
        free(fftype->elements);
    free(fftype);
}


// 1. The parent data may be a singular array for a HANDLE! or a BLOB! series,
//    depending on whether the data is owned by Rebol or not.  That series
//    pointer is being referenced again by the child struct we give back.
//
static void Get_Scalar_In_Struct(
    Sink(Value) out,  // if EXT_SYM_REBVAL, could be any value
    StructInstance* stu,
    StructField* field,
    REBLEN n  // element index, starting from 0
){
    assert(n == 0 or Field_Is_C_Array(field));

    Offset offset =
        STRUCT_OFFSET(stu) + Field_Offset(field) + (n * Field_Width(field));

    if (Field_Is_Struct(field)) {
        StructInstance* sub_stu = Alloc_Singular(
            NODE_FLAG_MANAGED | STUB_FLAG_LINK_NODE_NEEDS_MARK
        );
        LINK_STRUCT_SCHEMA(sub_stu) = field;

        Copy_Cell(Struct_Storage(sub_stu), Struct_Storage(stu));  // [1]
        STRUCT_OFFSET(sub_stu) = offset;
        assert(Struct_Total_Size(sub_stu) == Field_Width(field));
        Init_Struct(out, sub_stu);
        return;
    }

    Byte* p = offset + Struct_Data_Head(stu);

    switch (Field_Type_Id(field)) {
      case EXT_SYM_UINT8:
        Init_Integer(out, *cast(uint8_t*, p));
        break;

      case EXT_SYM_INT8:
        Init_Integer(out, *cast(int8_t*, p));
        break;

      case EXT_SYM_UINT16:
        Init_Integer(out, *cast(uint16_t*, p));
        break;

      case EXT_SYM_INT16:
        Init_Integer(out, *cast(int8_t*, p));
        break;

      case EXT_SYM_UINT32:
        Init_Integer(out, *cast(uint32_t*, p));
        break;

      case EXT_SYM_INT32:
        Init_Integer(out, *cast(int32_t*, p));
        break;

      case EXT_SYM_UINT64:
        Init_Integer(out, *cast(uint64_t*, p));
        break;

      case EXT_SYM_INT64:
        Init_Integer(out, *cast(int64_t*, p));
        break;

      case EXT_SYM_FLOAT:
        Init_Decimal(out, *cast(float*, p));
        break;

      case EXT_SYM_DOUBLE:
        Init_Decimal(out, *cast(double*, p));
        break;

      case EXT_SYM_POINTER:  // !!! Should 0 come back as a NULL to Rebol?
        Init_Integer(out, i_cast(intptr_t, *cast(void**, p)));
        break;

      case EXT_SYM_REBVAL:
        Copy_Cell(out, cast(const Value*, p));
        break;

      default:
        assert(false);
        fail ("Unknown FFI type indicator");
    }
}


//
//  Struct_To_Array: C
//
// Used by MOLD to create a block.
//
// Cannot fail(), because fail() could call MOLD on a struct!, which will end
// up infinitive recursive calls.
//
Source* Struct_To_Array(StructInstance* stu)
{
    Array* fieldlist = Struct_Fields_Array(stu);
    Element* fields_item = Array_Head(fieldlist);
    Element* fields_tail = Array_Tail(fieldlist);

    StackIndex base = TOP_INDEX;

    for(; fields_item != fields_tail; ++fields_item) {
        StructField* field = Cell_Array_Known_Mutable(fields_item);

        Option(const Symbol*) name = Field_Name(field);
        if (not name)
            fail ("Anonymous fields not supported yet in Struct_To_Array()");
        Init_Set_Word(PUSH(), unwrap name); // required name

        Source* typespec = Make_Source(2); // required type

        if (Field_Is_Struct(field)) {
            Init_Word(Alloc_Tail_Array(typespec), EXT_CANON(STRUCT_X));

            DECLARE_VALUE (nested);
            Get_Scalar_In_Struct(nested, stu, field, 0);
            assert(Is_Struct(nested));

            Push_Lifeguard(nested); // is this guard still necessary?
            Init_Block(
                Alloc_Tail_Array(typespec),
                Struct_To_Array(Cell_Struct(nested))
            );
            Drop_Lifeguard(nested);
        }
        else {
            // Elemental type (from a fixed list of known C types)
            //
            Init_Word(
                Alloc_Tail_Array(typespec),
                Canon_Symbol(Field_Type_Id(field))
            );
        }

        // "optional dimension and initialization."
        //
        // !!! Comment said the initialization was optional, but it seems
        // that the initialization always happens (?)
        //
        if (Field_Is_C_Array(field)) {
            //
            // Dimension becomes INTEGER! in a BLOCK! (to look like a C array)
            //
            REBLEN dimension = Field_Dimension(field);
            Source* one_int = Alloc_Singular(NODE_FLAG_MANAGED);
            Init_Integer(Stub_Cell(one_int), dimension);
            Init_Block(Alloc_Tail_Array(typespec), one_int);

            // Initialization seems to be just another block after that (?)
            //
            Source* init = Make_Source(dimension);
            REBLEN n;
            for (n = 0; n < dimension; n ++) {
                DECLARE_VALUE (scalar);
                Get_Scalar_In_Struct(scalar, stu, field, n);
                if (Is_Antiform(scalar))
                    fail ("Can't put antiform in block for Struct_To_Array()");
                Copy_Cell(Array_At(init, n), cast(Element*, scalar));
            }
            Set_Flex_Len(init, dimension);
            Init_Block(Alloc_Tail_Array(typespec), init);
        }
        else {
            DECLARE_VALUE (scalar);
            Get_Scalar_In_Struct(scalar, stu, field, 0);
            if (Is_Antiform(scalar))
                fail ("Can't put antiform in block for Struct_To_Array()");
            Copy_Cell(Alloc_Tail_Array(typespec), Known_Element(scalar));
        }

        Init_Block(PUSH(), typespec); // required type
    }

    return Pop_Source_From_Stack(base);
}


IMPLEMENT_GENERIC(MOLDIFY, Is_Struct)
{
    INCLUDE_PARAMS_OF_MOLDIFY;

    Element* cell = Element_ARG(ELEMENT);
    Molder* mo = Cell_Handle_Pointer(Molder, ARG(MOLDER));
    bool form = Bool_ARG(FORM);

    UNUSED(form); // no difference between MOLD and FORM at this time

    Append_Ascii(mo->string, "#[struct! ");

    Array* array = Struct_To_Array(Cell_Struct(cell));
    Mold_Array_At(mo, array, 0, "[]");
    Free_Unmanaged_Flex(array);

    Append_Ascii(mo->string, "]");

    return NOTHING;
}


static bool Same_Fields(const Array* a_fieldlist, const Array* b_fieldlist)
{
    if (Array_Len(a_fieldlist) != Array_Len(b_fieldlist))
        return false;

    const Element* a_item = Array_Head(a_fieldlist);
    const Element* a_tail = Array_Tail(a_fieldlist);
    const Element* b_item = Array_Head(b_fieldlist);
    const Element* b_tail = Array_Tail(b_fieldlist);

    for (; a_item != a_tail; ++a_item, ++b_item) {
        StructField* a = Cell_Array_Known_Mutable(a_item);
        StructField* b = Cell_Array_Known_Mutable(b_item);

        if (Field_Is_Struct(a)) {
            if (not Field_Is_Struct(b))
                return false;

            if (not Same_Fields(
                Field_Subfields_Array(a),
                Field_Subfields_Array(b)
            )){
                return false;
            }
        }

        if (Field_Type_Id(a) != Field_Type_Id(b))
            return false;

        if (Field_Is_C_Array(a)) {
            if (not Field_Is_C_Array(b))
                return false;

            if (Field_Dimension(a) != Field_Dimension(b))
                return false;
        }

        if (Field_Offset(a) != Field_Offset(b))
            return false;

        assert(Field_Width(a) == Field_Width(b));
    }

    assert(b_item == b_tail);

    return true;
}


static Option(Error*) Trap_Set_Scalar_In_Struct_core(
    Byte* data_head,
    REBLEN offset,
    StructField* field,
    REBLEN n,
    const Value* val
){
    assert(n == 0 or Field_Is_C_Array(field));

    void *data = data_head +
        offset + Field_Offset(field) + (n * Field_Width(field));

    if (Field_Is_Struct(field)) {
        if (not Is_Struct(val))
            return Error_Invalid_Type_Raw(Datatype_Of(val));

        if (Field_Width(field) != Cell_Struct_Total_Size(val))
            return Error_Bad_Value(val);

        if (not Same_Fields(
            Field_Subfields_Array(field),
            Cell_Struct_Fields_Array(val)
        )){
            return Error_Bad_Value(val);
        }

        memcpy(data, Cell_Struct_Data_At(val), Field_Width(field));

        return nullptr;
    }

    // All other types take numbers

    int64_t i;
    double d;

    switch (Type_Of(val)) {
      case TYPE_DECIMAL:
        d = VAL_DECIMAL(val);
        i = cast(int64_t, d);
        break;

      case TYPE_INTEGER:
        i = VAL_INT64(val);
        d = cast(double, i);
        break;

      default:
        // !!! REBVAL in a STRUCT! is likely not a good feature (see the
        // ALLOC-VALUE-POINTER routine for a better solution).  However, the
        // same code is used to process FFI function arguments and struct
        // definitions, and the feature may be useful for function args.

        if (Field_Type_Id(field) != EXT_SYM_REBVAL)
            return Error_Invalid_Type_Raw(Datatype_Of(val));

        // Avoid uninitialized variable warnings (should not be used)
        //
        i = 1020;
        d = 304;
    }

    switch (Field_Type_Id(field)) {
      case EXT_SYM_INT8:
        if (i > 0x7f or i < -128)
            return Error_Overflow_Raw();
        *cast(int8_t*, data) = cast(int8_t, i);
        break;

      case EXT_SYM_UINT8:
        if (i > 0xff or i < 0)
            return Error_Overflow_Raw();
        *cast(uint8_t*, data) = cast(uint8_t, i);
        break;

      case EXT_SYM_INT16:
        if (i > 0x7fff or i < -0x8000)
            return Error_Overflow_Raw();
        *cast(int16_t*, data) = cast(int16_t, i);
        break;

      case EXT_SYM_UINT16:
        if (i > 0xffff or i < 0)
            return Error_Overflow_Raw();
        *cast(uint16_t*, data) = cast(uint16_t, i);
        break;

      case EXT_SYM_INT32:
        if (i > INT32_MAX or i < INT32_MIN)
            return Error_Overflow_Raw();
        *cast(int32_t*, data) = cast(int32_t, i);
        break;

      case EXT_SYM_UINT32:
        if (i > UINT32_MAX or i < 0)
            return Error_Overflow_Raw();
        *cast(uint32_t*, data) = cast(uint32_t, i);
        break;

      case EXT_SYM_INT64:
        *cast(int64_t*, data) = i;
        break;

      case EXT_SYM_UINT64:
        if (i < 0)
            return Error_Overflow_Raw();
        *cast(uint64_t*, data) = cast(uint64_t, i);
        break;

      case EXT_SYM_FLOAT:
        *cast(float*, data) = cast(float, d);
        break;

      case EXT_SYM_DOUBLE:
        *cast(double*, data) = d;
        break;

      case EXT_SYM_POINTER: {
        size_t sizeof_void_ptr = sizeof(void*); // avoid constant conditional
        if (sizeof_void_ptr == 4 and i > UINT32_MAX)
            return Error_Overflow_Raw();
        *cast(void**, data) = p_cast(void*, cast(intptr_t, i));
        break; }

      case EXT_SYM_REBVAL:
        //
        // !!! This is a dangerous thing to be doing in generic structs, but
        // for the main purpose of cells (tunneling) it should be okay so
        // long as the Value* that is passed in is actually a pointer into
        // a frame's args.
        //
        *cast(const Value**, data) = val;
        break;

      default:
        assert(false);  // should not happen
        fail ("unknown Field_Type_Id()");
    }

    return nullptr;
}


INLINE Option(Error*) Trap_Set_Scalar_In_Struct(
    StructInstance* stu,
    StructField* field,
    REBLEN n,
    const Value* val
){
    return Trap_Set_Scalar_In_Struct_core(
        Struct_Data_Head(stu), STRUCT_OFFSET(stu), field, n, val
    );
}


static Option(Error*) Trap_Parse_Struct_Attribute(
    const Element* block,
    REBINT *raw_size,
    uintptr_t *raw_addr
){
    const Element* tail;
    const Element* attr = Cell_List_At(&tail, block);

    *raw_size = -1;
    *raw_addr = 0;

    while (attr != tail) {
        if (not Is_Set_Word(attr))
            return Error_Bad_Value(attr);

        switch (Cell_Word_Id(attr)) {
          case EXT_SYM_RAW_SIZE:
            ++attr;
            if (attr == tail or not Is_Integer(attr))
                return Error_Bad_Value(attr);
            if (*raw_size > 0)
                return Error_User("FFI: duplicate raw size");
            *raw_size = VAL_INT64(attr);
            if (*raw_size <= 0)
                return Error_User("FFI: raw size cannot be zero");
            break;

          case EXT_SYM_RAW_MEMORY:
            ++attr;
            if (attr == tail or not Is_Integer(attr))
                return Error_Bad_Value(attr);
            if (*raw_addr != 0)
                return Error_User("FFI: duplicate raw memory");
            *raw_addr = cast(REBU64, VAL_INT64(attr));
            if (*raw_addr == 0)
                return Error_User("FFI: void pointer illegal for raw memory");
            break;

          case EXT_SYM_EXTERN: {
            ++attr;

            if (*raw_addr != 0)
                return Error_User("FFI: raw memory is exclusive with extern");

            if (attr == tail or not Is_Block(attr) or Cell_Series_Len_At(attr) != 2)
                return Error_Bad_Value(attr);

            const Element* lib = Cell_List_Item_At(attr);
            if (rebNot("library! = type of", lib))
                return Error_Bad_Value(attr);

            const Element* linkname = Cell_List_Item_At(attr) + 1;
            if (not Any_String(linkname))
                return Error_Bad_Value(linkname);

            RebolValue* result = rebEntrap("pick", lib, linkname);
            if (Is_Error(result))
                return Cell_Error(result);

            Unquotify(Known_Element(result));
            assert(Is_Handle(result));
            CFunction* addr = Cell_Handle_Cfunc(result);
            *raw_addr = i_cast(uintptr_t, addr);
            break; }

        // !!! This alignment code was commented out for some reason.
        /*
        case EXT_SYM_ALIGNMENT:
            ++ attr;
            if (not Is_Integer(attr))
                fail (attr);

            alignment = VAL_INT64(attr);
            break;
        */

          default:
            return Error_Bad_Value(attr);
        }

        ++attr;
    }

    return nullptr;  // no error
}


// The managed handle logic always assumes a cleanup function, so it doesn't
// have to test for nullptr.
//
static void cleanup_noop(const Value* v) {
    assert(Is_Handle(v));
    UNUSED(v);
}


//
// set storage memory to external addr: raw_addr
//
// "External Storage" is the idea that a STRUCT! which is modeling a C
// struct doesn't use a BINARY! series as the backing store, rather a pointer
// that is external to the system.  When Atronix added the FFI initially,
// this was done by creating a separate type of REBSER that could use an
// external pointer.  This uses a managed HANDLE! for the same purpose, as
// a less invasive way of doing the same thing.
//
static Option(Error*) Trap_Set_Struct_Storage_External(
    StructInstance* stu,
    REBLEN len,
    REBINT raw_size,
    uintptr_t raw_addr
) {
    if (raw_size >= 0 and raw_size != cast(REBINT, len)) {
        DECLARE_ELEMENT (i);
        Init_Integer(i, raw_size);
        return Error_Invalid_Data_Raw(i);
    }

    Init_Handle_Cdata_Managed(
        Struct_Storage(stu),
        p_cast(Byte*, raw_addr),
        len,
        &cleanup_noop
    );

    return nullptr;
}


//
//  Total_Struct_Dimensionality: C
//
// This recursively counts the total number of data elements inside of a
// struct.  This includes for instance every array element inside a
// nested struct's field, along with its fields.
//
// !!! Is this really how char[1000] would be handled in the FFI?  By
// creating 1000 ffi_types?  :-/
//
static REBLEN Total_Struct_Dimensionality(Array* fields)
{
    REBLEN n_fields = 0;

    const Element* item = Array_Head(fields);
    const Element* tail = Array_Tail(fields);
    for (; item != tail; ++item) {
        StructField* field = Cell_Array_Known_Mutable(item);

        if (Field_Is_Struct(field))
            n_fields += Total_Struct_Dimensionality(Field_Subfields_Array(field));
        else
            n_fields += Field_Is_C_Array(field) ? Field_Dimension(field) : 1;
    }
    return n_fields;
}


//
//  Prepare_Field_For_Ffi: C
//
// The main reason structs exist is so that they can be used with the FFI,
// and the FFI requires you to set up a "ffi_type" C struct describing
// each datatype. This is a helper function that sets up proper ffi_type.
// There are stock types for the primitives, but each structure needs its
// own.
//
static void Prepare_Field_For_Ffi(StructField* schema)
{
    assert(Not_Cell_Readable(Field_Detail(schema, IDX_FIELD_FFTYPE)));

    ffi_type* fftype;

    if (not Field_Is_Struct(schema)) {
        fftype = maybe Get_Ffi_Type_For_Symbol(Field_Type_Id(schema));
        assert(fftype != nullptr);

        // The FFType pointers returned by Get_Ffi_Type_For_Symbol should not be
        // freed, so a "simple" handle is used that just holds the pointer.
        //
        Init_Handle_Cdata(
            Field_Detail(schema, IDX_FIELD_FFTYPE),
            fftype,
            sizeof(&fftype)
        );
        return;
    }

    // For struct fields--on the other hand--it's necessary to do a custom
    // allocation for a new type registered with the FFI.
    //
    fftype = cast(ffi_type*, malloc(sizeof(ffi_type)));
    fftype->type = FFI_TYPE_STRUCT;

    // "This is set by libffi; you should initialize it to zero."
    // http://www.atmark-techno.com/~yashi/libffi.html#Structures
    //
    fftype->size = 0;
    fftype->alignment = 0;

    Array* fieldlist = Field_Subfields_Array(schema);

    REBLEN dimensionality = Total_Struct_Dimensionality(fieldlist);
    fftype->elements = cast(ffi_type**,
        malloc(sizeof(ffi_type*) * (dimensionality + 1)) // nullptr term
    );

    Element* item = Array_Head(fieldlist);
    Element* tail = Array_Tail(fieldlist);

    REBLEN j = 0;
    for (; item != tail; ++item) {
        StructField* field = Cell_Array_Known_Mutable(item);
        REBLEN dimension = Field_Is_C_Array(field) ? Field_Dimension(field) : 1;

        REBLEN n = 0;
        for (n = 0; n < dimension; ++n)
            fftype->elements[j++] = Field_Ffi_Type(field);
    }

    fftype->elements[j] = nullptr;

    Init_Handle_Cdata_Managed(
        Field_Detail(schema, IDX_FIELD_FFTYPE),
        fftype,
        dimensionality + 1,
        &cleanup_ffi_type
    );
}


// This takes a spec like `[int32 [2]]` and sets the output field's properties
// by recognizing a finite set of FFI type keywords defined in %words.r.
//
// This also allows for embedded structure types.  If the type is not being
// included by reference, but rather with a sub-definition inline, then it
// will actually be creating a new `inner` STRUCT! value.  Since this value
// is managed and not referred to elsewhere, there can't be evaluations.
//
static Option(Error*) Trap_Parse_Field_Type_May_Fail(
    StructField* field,
    const Element* spec,
    Sink(Element) inner  // will be set only if STRUCT!
){
    const Element* tail;
    const Element* val = Cell_List_At(&tail, spec);

    if (val == tail)
        return Error_User("Empty field type in FFI");

    if (Is_Word(val)) {
        Option(SymId) id = Cell_Word_Id(val);

        // Use WORD! as the field type by default (will be overwritten in the
        // EXT_SYM_STRUCT_X cases, type not a simple word if field is struct).
        //
        Copy_Cell(Field_Detail(field, IDX_FIELD_TYPE), val);

        switch (id) {
          case EXT_SYM_UINT8:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 1);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_INT8:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 1);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_UINT16:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 2);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_INT16:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 2);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_UINT32:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 4);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_INT32:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 4);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_UINT64:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 8);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_INT64:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 8);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_FLOAT:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 4);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_DOUBLE:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), 8);
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_POINTER:
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), sizeof(void*));
            Prepare_Field_For_Ffi(field);
            break;

          case EXT_SYM_STRUCT_X: {
            ++val;
            if (not Is_Block(val))
                fail (Error_Unexpected_Type(TYPE_BLOCK, Datatype_Of(val)));

            DECLARE_ELEMENT (specific);
            Derelativize(specific, val, Cell_List_Binding(spec));

            Push_Lifeguard(specific);
            Option(Error*) e = Trap_Make_Struct(inner, specific);
            Drop_Lifeguard(specific);

            if (e)
                return e;

            Init_Integer(
                Field_Detail(field, IDX_FIELD_WIDE),
                Cell_Struct_Data_Size(inner)
            );
            Init_Block(
                Field_Detail(field, IDX_FIELD_TYPE),
                Cell_Struct_Fields_Array(inner)
            );

            // Borrow the same ffi_type* that was built for the inner struct
            // (What about just storing the STRUCT! value itself in the type
            // field, instead of the array of fields?)
            //
            Copy_Cell(
                Field_Detail(field, IDX_FIELD_FFTYPE),
                Field_Detail(Cell_Struct_Schema(inner), IDX_FIELD_FFTYPE)
            );
            break; }

          case EXT_SYM_REBVAL: {
            //
            // While most data types have some kind of proxying of when you
            // pass a Rebol value in (such as turning an INTEGER! into bits
            // for a C `int`) if the argument is marked as being a REBVAL
            // then the Type_Of is ignored, and it acts like a pointer to
            // the actual argument in the frame...whatever that may be.
            //
            // !!! The initial FFI implementation from Atronix would actually
            // store sizeof(REBVAL) in the struct, not sizeof(REBVAL*).  The
            // struct's binary data was then hooked into the garbage collector
            // to make sure that cell was marked.  Because the intended use
            // of the feature is "tunneling" a value from a routine's frame
            // to a callback's frame, the lifetime of the REBVAL* should last
            // for the entirety of the routine it was passed to.
            //
            Init_Integer(Field_Detail(field, IDX_FIELD_WIDE), sizeof(Cell*));
            Prepare_Field_For_Ffi(field);
            break; }

          default:
            fail (val);
        }
    }
    else if (Is_Struct(val)) {
        //
        // [b: [struct-a] val-a]
        //
        Init_Integer(
            Field_Detail(field, IDX_FIELD_WIDE),
            Cell_Struct_Data_Size(val)
        );
        Init_Block(
            Field_Detail(field, IDX_FIELD_TYPE),
            Cell_Struct_Fields_Array(val)
        );

        // Borrow the same ffi_type* that the struct uses, see above note
        // regarding alternative ideas.
        //
        Copy_Cell(
            Field_Detail(field, IDX_FIELD_FFTYPE),
            Field_Detail(Cell_Struct_Schema(val), IDX_FIELD_FFTYPE)
        );
        Derelativize(inner, val, Cell_List_Binding(spec));
    }
    else
        return Error_Invalid_Type_Raw(Datatype_Of(val));

    ++val;

    // Find out the array dimension (if there is one)
    //
    if (val == tail) {
        Init_Blank(Field_Detail(field, IDX_FIELD_DIMENSION)); // scalar
    }
    else if (Is_Block(val)) {
        //
        // make struct! [a: [int32 [2]] [0 0]]
        //
        DECLARE_ELEMENT (ret);
        Context* derived = Derive_Binding(Cell_List_Binding(spec), val);
        if (Eval_Any_List_At_Throws(ret, val, derived))
            return Error_No_Catch_For_Throw(TOP_LEVEL);

        if (not Is_Integer(ret))
            return Error_Unexpected_Type(TYPE_INTEGER, Datatype_Of(val));

        Init_Integer(Field_Detail(field, IDX_FIELD_DIMENSION), VAL_INT64(ret));
        ++val;
    }
    else
        return Error_Invalid_Type_Raw(Datatype_Of(val));

    return nullptr;
}


// a: make struct! [uint 8 i: 1]
// b: make a [i: 10]
//
Option(Error*) Trap_Init_Struct_Fields(
    Sink(Element) ret,
    const Element* spec
){
    const Element* spec_tail;
    const Element* spec_item = Cell_List_At(&spec_tail, spec);

    while (spec_item != spec_tail) {
        const Element* word;
        if (Is_Block(spec_item)) {  // options: raw-memory, etc
            REBINT raw_size = -1;
            uintptr_t raw_addr = 0;

            // make sure no other field initialization
            if (Cell_Series_Len_Head(spec) != 1)
                return Error_Bad_Value(spec);

            Option(Error*) e1 = Trap_Parse_Struct_Attribute(
                spec_item, &raw_size, &raw_addr
            );
            if (e1)
                return e1;

            Option(Error*) e2 = Trap_Set_Struct_Storage_External(
                Cell_Struct(ret),
                Cell_Struct_Total_Size(ret),
                raw_size,
                raw_addr
            );
            if (e2)
                return e2;

            break;
        }
        else {
            word = spec_item;
            if (not Is_Set_Word(word))
                return Error_Bad_Value(word);
        }

        const Element* fld_val = spec_item + 1;
        if (fld_val == spec_tail)
            return Error_Need_Non_End_Raw(fld_val);

        Array* fieldlist = Cell_Struct_Fields_Array(ret);
        Element* field_item = Array_Head(fieldlist);
        Element* fields_tail = Array_Tail(fieldlist);

        for (; field_item != fields_tail; ++field_item) {
            StructField* field = Cell_Array_Known_Mutable(field_item);

            if (Field_Name(field) != Cell_Word_Symbol(word))
                continue;

            if (Field_Is_C_Array(field)) {
                if (Is_Block(fld_val)) {
                    REBLEN dimension = Field_Dimension(field);

                    if (Cell_Series_Len_At(fld_val) != dimension)
                        return Error_Bad_Value(fld_val);

                    REBLEN n = 0;
                    const Element* at = Cell_List_Item_At(fld_val);
                    for (n = 0; n < dimension; ++n, ++at) {
                        Option(Error*) e = Trap_Set_Scalar_In_Struct(
                            Cell_Struct(ret), field, n, at
                        );
                        if (e)
                            return Error_Bad_Value(fld_val);
                    }
                }
                else if (Is_Integer(fld_val)) { // interpret as a data pointer
                    void *ptr = p_cast(void *,
                        cast(intptr_t, VAL_INT64(fld_val))
                    );

                    // assuming valid pointer to enough space
                    memcpy(
                        Cell_Struct_Data_Head(ret) + Field_Offset(field),
                        ptr,
                        Field_Total_Size(field)
                    );
                }
                else
                    return Error_Bad_Value(fld_val);
            }
            else {
                Option(Error*) e = Trap_Set_Scalar_In_Struct(
                    Cell_Struct(ret),
                    field,
                    0,
                    fld_val
                );
                if (e)
                    return e;
            }
            goto next_spec_pair;
        }

        return Error_User("FFI: field not in the parent struct");

      next_spec_pair:

        spec_item += 2;
    }

    return nullptr;
}


//
//  Trap_Make_Struct: C
//
// Field definitions look like:
//
//     make struct! [
//         field1 [type1]
//         field2: [type2] field2-init-value
//         field3: [struct [field1 [type1]]]
//         field4: [type1 [3]]
//         ...
//     ]
//
// (!!! field3 and field4 are set-words above, but do not seem to have
// initialization.  Is that right?)
//
Option(Error*) Trap_Make_Struct(Sink(Element) out, const Element* arg)
{
    if (Cell_Series_Len_At(arg) == 0)
        return Error_User("Empty Struct Definitions not legal");

    Level* L = Make_Level_At(&Stepper_Executor, arg, LEVEL_MASK_NONE);
    const Element* at = At_Level(L);

    Push_Level_Erase_Out_If_State_0(nullptr, L);

    REBINT max_fields = 16;

  set_up_schema: /////////////////////////////////////////////////////////////

    // Every struct has a "schema"--this is a description (potentially
    // hierarchical) of its fields, including any nested structs.  The
    // schema should be shared between common instances of the same struct.
    //
    // Though the schema is not managed until the end of this creation, the
    // MAKE process runs evaluations, so the fields must be GC valid.
    //
    // 1. Since structs can be nested within structs as fields, the "schema"
    //    for a struct itself uses the same data structure as fields do.  The
    //    difference is that while a struct field can be an array of structs,
    //    the schema for a struct declaration itself has no dimensionality.
    //
    // 2. Similar to how the top level struct itself has no dimensionality,
    //    it also has no offset.  While we could conceivably say that the
    //    offset was 0, the question would be "offset 0 into what?" because
    //    the struct itself is not a member of an aggregate.

    StructField* schema = Make_Source(IDX_FIELD_MAX);
    Init_Unreadable(Field_Detail(schema, IDX_FIELD_TYPE));  // will fill in
    Init_Blank(Field_Detail(schema, IDX_FIELD_DIMENSION));  // not used [1]
    Init_Unreadable(Field_Detail(schema, IDX_FIELD_FFTYPE));  // will fill in
    Init_Blank(Field_Detail(schema, IDX_FIELD_NAME));  // no symbol for structs
    Init_Blank(Field_Detail(schema, IDX_FIELD_OFFSET));  // not used [2]
    Init_Unreadable(Field_Detail(schema, IDX_FIELD_WIDE));  // will fill in
    Set_Flex_Len(schema, IDX_FIELD_MAX);

  process_fields: ////////////////////////////////////////////////////////////

    // Note: This is a *lot* of C code for validating and digesting a Rebol
    // spec block.  Much of the work should be done in Rebol itself, giving
    // a fully validated spec to the C code.  (In particular because it's not
    // performance-critical to set up an FFI spec to call a function...because
    // C functions are generally finite in number and this interface is
    // generated only once.)
    //
    // 1. !!! This would suggest raw-size, raw-addr, or extern can be leading
    //    in the struct definition, perhaps as:
    //
    //        make struct! [[raw-size] ...]

    uint64_t offset = 0; // offset in data

    REBINT raw_size = -1;
    uintptr_t raw_addr = 0;

    if (Is_Block(at)) {  // leading block? [1]
        DECLARE_ELEMENT (specific);
        Derelativize(specific, at, Cell_List_Binding(arg));

        Option(Error*) e = Trap_Parse_Struct_Attribute(
            specific, &raw_size, &raw_addr
        );
        if (e)
            return e;

        Fetch_Next_In_Feed(L->feed);
        at = At_Level(L);
    }

    Binary* data_bin;
    if (raw_addr == 0)
        data_bin = Make_Binary(max_fields << 2);  // !!! blob for each level?
    else
        data_bin = nullptr; // not used, but avoid maybe uninitialized warning

    REBINT field_idx = 0; // for field index

    StackIndex base = TOP_INDEX; // accumulate fields (BLOCK!s) on stack

    DECLARE_ELEMENT (spec);
    DECLARE_ELEMENT (init); // for result to save in data

  check_if_more_fields: ///////////////////////////////////////////////////=//

    if (Is_Level_At_End(L))
        goto pop_fields_from_stack;

    goto process_next_field;

  process_next_field: { ///////////////////////////////////////////////////=//

    // 1. Currently this spec block processing code is written stackfully,
    //    so it calls into the evaluator invoking a new trampoline.  The goal
    //    is to have as little such code as possible... it means that you
    //    can't call generators from the GROUP!s in the spec (for instance).
    //    But if you do leave your code on the stack while calling the
    //    evaluator, you need to protect or not manage the Stubs you create.
    //
    //    (The right answer here isn't to upgrade this code to a state machine
    //    that uses BOUNCE_CONTINUE and is properly stackless...but rather to
    //    have all the evaluations and validations of the spec done in usermode
    //    so this code is doing a very minimal amount of work.)

    const Element* at = At_Level(L);

    StructField* field = Make_Source(IDX_FIELD_MAX);  // don't manage [1]
    Init_Unreadable(Field_Detail(field, IDX_FIELD_TYPE));
    Init_Unreadable(Field_Detail(field, IDX_FIELD_DIMENSION));
    Init_Unreadable(Field_Detail(field, IDX_FIELD_FFTYPE));
    Init_Unreadable(Field_Detail(field, IDX_FIELD_NAME));
    Init_Integer(Field_Detail(field, IDX_FIELD_OFFSET), offset);
    Init_Unreadable(Field_Detail(field, IDX_FIELD_WIDE));
    Set_Flex_Len(field, IDX_FIELD_MAX);

    bool expect_init;
    if (Is_Set_Word(at)) {  // Set-words initialize
        expect_init = true;
        if (raw_addr)  // initialize not allowed for raw memory struct
            return Error_Bad_Value(at);
    }
    else if (Is_Word(at))  // Words don't initialize
        expect_init = false;
    else
        return Error_Bad_Value(at);

    Init_Word(Field_Detail(field, IDX_FIELD_NAME), Cell_Word_Symbol(at));

    Fetch_Next_In_Feed(L->feed);
    if (Is_Level_At_End(L))
        return Error_User("Invalid end of input");

    at = At_Level(L);

    if (not Is_Block(at))
        return Error_Bad_Value(at);

    Derelativize(spec, at, Cell_List_Binding(arg));

    // Fills in the width, dimension, type, and ffi_type (if needed)
    //
    Option(Error*) e = Trap_Parse_Field_Type_May_Fail(field, spec, init);
    if (e)
        return unwrap e;

    REBLEN dimension = Field_Is_C_Array(field) ? Field_Dimension(field) : 1;

    if (Field_Width(field) > UINT32_MAX)
        return Error_Size_Limit_Raw(maybe Field_Name(field));

    if (dimension > UINT32_MAX) {
        DECLARE_ELEMENT (dim);
        Init_Integer(dim, dimension);
        return Error_Size_Limit_Raw(dim);
    }

    uint64_t step =
        cast(uint64_t, Field_Width(field)) * cast(uint64_t, dimension);

    if (step > VAL_STRUCT_LIMIT)
        return Error_Size_Limit_Raw(out);

    if (raw_addr == 0)
        Expand_Flex_Tail(data_bin, step);

    Fetch_Next_In_Feed(L->feed);
    Corrupt_If_Debug(at);

    if (expect_init) {
        if (Is_Level_At_End(L))
            return Error_Bad_Value(arg);

        at = At_Level(L);

        if (Is_Block(at)) {
            DECLARE_ELEMENT (specific);
            Derelativize(specific, at, Level_Binding(L));

            Push_Lifeguard(specific);
            RebolValue* reduced = rebValue("reduce", specific);
            Drop_Lifeguard(specific);

            Copy_Cell(init, Known_Element(reduced));
            rebRelease(reduced);

            Fetch_Next_In_Feed(L->feed);
            Corrupt_If_Debug(at);
        }
        else {
            if (Eval_Step_Throws(init, L))
                return Error_No_Catch_For_Throw(TOP_LEVEL);
        }

        if (Field_Is_C_Array(field)) {
            if (Is_Integer(init)) {  // interpreted as a C pointer
                void *ptr = p_cast(void*, cast(intptr_t, VAL_INT64(init)));

                // assume valid pointer to enough space
                memcpy(
                    Flex_At(Byte, data_bin, cast(REBLEN, offset)),
                    ptr,
                    Field_Total_Size(field)
                );
            }
            else if (Is_Block(init)) {
                if (Cell_Series_Len_At(init) != Field_Dimension(field))
                    return Error_Bad_Value(init);

                const Element* at = Cell_List_Item_At(init);

                for (Offset n = 0; n < Field_Dimension(field); ++n, ++at) {
                    Option(Error*) e = Trap_Set_Scalar_In_Struct_core(
                        Binary_Head(data_bin),
                        offset,
                        field,
                        n,
                        at
                    );
                    if (e)
                        return e;
                }
            }
            else
                return Error_Unexpected_Type(
                    TYPE_BLOCK, Datatype_Of(At_Level(L))
                );
        }
        else {  // scalar
            Option(Error*) e = Trap_Set_Scalar_In_Struct_core(
                Binary_Head(data_bin), offset, field, 0, init
            );
            if (e)
                return e;
        }
    }
    else if (raw_addr == 0) {
        if (Field_Is_Struct(field)) {
            REBLEN n = 0;
            for (
                n = 0;
                n < (Field_Is_C_Array(field) ? Field_Dimension(field) : 1);
                ++n
            ){
                memcpy(
                    Flex_At(
                        Byte,
                        data_bin,
                        cast(REBLEN, offset) + (n * Field_Width(field))
                    ),
                    Cell_Struct_Data_Head(init),
                    Field_Width(field)
                );
            }
        }
        else {
            memset(
                Flex_At(Byte, data_bin, cast(REBLEN, offset)),
                0,
                Field_Total_Size(field)
            );
        }
    }

    offset += step;

    //if (alignment != 0) {
    //  offset = ((offset + alignment - 1) / alignment) * alignment;

    if (offset > VAL_STRUCT_LIMIT)
        return Error_Size_Limit_Raw(out);

    ++field_idx;

    Init_Block(PUSH(), field);

    goto check_if_more_fields;

} pop_fields_from_stack: { ///////////////////////////////////////////////////

    Source* fieldlist = Pop_Managed_Source_From_Stack(base);

    Init_Block(Field_Detail(schema, IDX_FIELD_TYPE), fieldlist);
    Prepare_Field_For_Ffi(schema);

    Init_Integer(Field_Detail(schema, IDX_FIELD_WIDE), offset); // total size known

} finalize_struct: { /////////////////////////////////////////////////////////

    StructInstance* stu = Prep_Stub(STUB_MASK_STRUCT, Alloc_Stub());
    Manage_Flex(schema);
    LINK_STRUCT_SCHEMA(stu) = schema;

    if (raw_addr) {
        Option(Error*) e = Trap_Set_Struct_Storage_External(
            stu,
            Field_Total_Size(schema),
            raw_size,
            raw_addr
        );
        if (e)
            return e;
    }
    else {
        Term_Binary(data_bin);
        Init_Blob(Struct_Storage(stu), data_bin);
    }

    Drop_Level(L);  // has to be after the pop and all nodes managed

    Init_Struct(out, stu);

    return nullptr;  // no error
}}


IMPLEMENT_GENERIC(MAKE, Is_Struct)
{
    INCLUDE_PARAMS_OF_MAKE;

    UNUSED(ARG(TYPE));

    Element* arg = Element_ARG(DEF);

    if (not Is_Block(arg))
        return FAIL(PARAM(DEF));

    Option(Error*) e = Trap_Make_Struct(OUT, arg);
    if (e)
        return FAIL(unwrap e);

    return OUT;
}


// 1. Structs contain packed data for the field type in an array.  If you
//    don't have the VECTOR! type loaded, we could only return this as a
//    BINARY! which wouldn't be that useful.  Not only could a VECTOR!
//    conceivably store and interpret the extracted data, but it might be
//    able to use the raw pointer into the struct.
//
//    For now, the information is expaned out and translated into a BLOCK!.
//
IMPLEMENT_GENERIC(PICK, Is_Struct)
{
    INCLUDE_PARAMS_OF_PICK;

    Element* location = Element_ARG(LOCATION);
    Element* picker = Element_ARG(PICKER);

    if (not Is_Word(picker))
        return FAIL(PARAM(PICKER));

    StructInstance* stu = Cell_Struct(location);

    Array* fieldlist = Struct_Fields_Array(stu);

    Element* fields_tail = Array_Tail(fieldlist);
    Element* fields_item = Array_Head(fieldlist);

    for (; fields_item != fields_tail; ++fields_item) {
        StructField* field = Cell_Array_Known_Mutable(fields_item);
        Option(const Symbol*) field_name = Field_Name(field);
        if (field_name != Cell_Word_Symbol(picker))  // C is case-sensitive
            continue;

        if (not Field_Is_C_Array(field)) {
            Get_Scalar_In_Struct(OUT, stu, field, 0);  // index 0
            continue;
        }

        REBLEN dimension = Field_Dimension(field);
        Source* arr = Make_Source(dimension);  // return VECTOR! instead? [1]
        REBLEN n;
        for (n = 0; n < dimension; ++n) {
            DECLARE_VALUE (scalar);
            Get_Scalar_In_Struct(scalar, stu, field, n);
            if (Is_Antiform(scalar))
                return FAIL("Antiforms can't be put in block for PICK");
            Copy_Cell(Array_At(arr, n), Known_Element(scalar));
        }
        Set_Flex_Len(arr, dimension);

        return Init_Block(OUT, arr);
    }

    return RAISE(Error_Bad_Pick_Raw(picker));  // TRY can suppress
}


IMPLEMENT_GENERIC(POKE, Is_Struct)
{
    INCLUDE_PARAMS_OF_POKE;

    Element* location = Element_ARG(LOCATION);
    Element* picker = Element_ARG(PICKER);
    Value* poke = Element_ARG(VALUE);

    StructInstance* stu = Cell_Struct(location);

    if (not Is_Word(picker))
        return FAIL(PARAM(PICKER));

    Array* fieldlist = Struct_Fields_Array(stu);
    Element* fields_item = Array_Head(fieldlist);
    Element* fields_tail = Array_Tail(fieldlist);

    for (; fields_item != fields_tail; ++fields_item) {
        StructField* field = Cell_Array_Known_Mutable(fields_item);

        if (Cell_Word_Symbol(picker) != Field_Name(field))
            continue;

        if (not Field_Is_C_Array(field)) {
            Option(Error*) e = Trap_Set_Scalar_In_Struct(stu, field, 0, poke);
            if (e)
                return FAIL(unwrap e);
            return nullptr;  // no need to write back
        }

        if (not Is_Block(poke))
            return FAIL("Setting array field in STRUCT! requires BLOCK! atm");

        REBLEN dimension = Field_Dimension(field);
        if (dimension != Cell_Series_Len_At(poke))
            return FAIL("Dimension mismatch of array field");

        const Element* at = Cell_List_Item_At(poke);
        REBLEN n = 0;
        for(n = 0; n < dimension; ++n, ++at) {
            Option(Error*) e = Trap_Set_Scalar_In_Struct(stu, field, n, at);
            if (e)
                return FAIL(unwrap e);
        }
    }

    return nullptr;
}


IMPLEMENT_GENERIC(EQUAL_Q, Is_Struct)
{
    INCLUDE_PARAMS_OF_EQUAL_Q;

    Element* a = Element_ARG(VALUE1);
    Element* b = Element_ARG(VALUE2);
    bool strict = Bool_ARG(STRICT);
    UNUSED(strict);

    if (Cell_Struct_Fields_Array(a) != Cell_Struct_Fields_Array(b))
        return Init_Logic(OUT, false);

    assert(Cell_Struct_Total_Size(a) == Cell_Struct_Total_Size(b));
    assert(Same_Fields(Cell_Struct_Fields_Array(a), Cell_Struct_Fields_Array(b)));

    return LOGIC(memcmp(
        Cell_Struct_Data_Head(a),
        Cell_Struct_Data_Head(b),
        Cell_Struct_Total_Size(a)
    ) == 0);
}


//
//  Copy_Struct_Managed: C
//
// !!! "Note that the offset is left intact, and as written will make a copy
//     as big as struct the instance is embedded into if nonzero offset." (?)
//
StructInstance* Copy_Struct_Managed(StructInstance* src)
{
    StructInstance* copy = Prep_Stub(STUB_MASK_STRUCT, Alloc_Stub());

    LINK_STRUCT_SCHEMA(copy) = LINK_STRUCT_SCHEMA(src);  // share the schema
    MISC_STRUCT_OFFSET(copy) = MISC_STRUCT_OFFSET(src);  // copies offset

    Binary* bin_copy = Make_Binary(Struct_Storage_Len(src));  // copy data
    memcpy(
        Binary_Head(bin_copy),
        Struct_Data_Head(src),
        Struct_Storage_Len(src)
    );
    Term_Binary_Len(bin_copy, Struct_Storage_Len(src));
    Init_Blob(Struct_Storage(copy), bin_copy);

    return copy;
}


IMPLEMENT_GENERIC(OLDGENERIC, Is_Struct)
{
    Element* val = Known_Element(ARG_N(1));
    const Symbol* verb = Level_Verb(LEVEL);

    switch (Symbol_Id(verb)) {
      case SYM_CHANGE: {
        Value* arg = ARG_N(2);
        if (not Is_Blob(arg))
            return FAIL(Error_Unexpected_Type(TYPE_BLOB, Datatype_Of(arg)));

        if (Cell_Series_Len_At(arg) != Cell_Struct_Data_Size(val))
            return FAIL(arg);

        memcpy(
            Cell_Struct_Data_Head(val),
            Binary_Head(Cell_Binary(arg)),
            Cell_Struct_Data_Size(val)
        );
        Copy_Cell(OUT, val);
        return OUT; }

      default:
        break;
    }

    return UNHANDLED;
}


IMPLEMENT_GENERIC(LENGTH_OF, Is_Struct)
{
    INCLUDE_PARAMS_OF_LENGTH_OF;

    Element* elem = Element_ARG(ELEMENT);
    return Init_Integer(OUT, Cell_Struct_Data_Size(elem));
}


IMPLEMENT_GENERIC(VALUES_OF, Is_Struct)
{
    INCLUDE_PARAMS_OF_VALUES_OF;

    Element* val = Element_ARG(ELEMENT);

    Binary* bin = Make_Binary(Cell_Struct_Total_Size(val));
    memcpy(
        Binary_Head(bin),
        Cell_Struct_Data_At(val),
        Cell_Struct_Total_Size(val)
    );
    Term_Binary_Len(bin, Cell_Struct_Total_Size(val));

    return Init_Blob(OUT, bin);
}

/*
    IMPLEMENT_GENERIC(SPEC_OF, Is_Struct)
    {
        INCLUDE_PARAMS_OF_SPEC_OF;
        return Init_Block(D_OUT, Struct_To_Array(Cell_Struct(val)));
    }
*/


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
//  export destroy-struct-storage: native [
//
//  "Destroy the external memory associated the struct"
//
//      return: [~]
//      struct [struct!]
//      :free "Specify the function to free the memory"
//          [action!]  ; [1]
//  ]
//
DECLARE_NATIVE(DESTROY_STRUCT_STORAGE)
//
// 1. This used to constrain the FREE function to being ROUTINE!.  But if
//    a function takes a pointer, then it seems that it should a candidate
//    for performing the free.
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

    if (Bool_ARG(FREE))
        rebElide(rebRUN(ARG(FREE)), pointer);  // may not be routine [1]

    return NOTHING;
}
