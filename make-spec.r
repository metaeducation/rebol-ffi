Rebol [
    name: FFI
    notes: "See %extensions/README.md for the format and fields of this file"

    extended-words: [
        struct!
        uint8 int8
        uint16 int16
        uint32 int32
        uint64 int64
        float
        double
        pointer
        rebval
        raw-memory raw-size
        extern
        varargs  ; used as the name of synthesized variadic argument
    ]

    extended-types: [struct! library! vector!]
]


cflags: [
    ; ffi_closure has an alignment specifier, which causes
    ; padding, and MSVC warns about that.
    ;
    <msc:/wd4324>
]

includes: compose [
    ;
    ; Note: FFI once statically linked to `Trap_Find_Function_In_Library()`
    ; in the LIBRARY! extension.  Now it uses librebol calls to execute
    ; "make library! ..." etc., and gets POINTER! back.  So it does not
    ; need to do that any longer
    ;
    (comment [compose %(repo-dir)/extensions/library/])

    ; Note: Vectors are used to model C array structures, and so you have
    ; to have the vector extension available if you're going to do any FFI
    ; with arrays.  But similar to the library extension, VECTOR! is now
    ; accessed through API calls with HANDLE!.
    ;
    (comment [compose %(repo-dir)/extensions/vector/])
]

sources: [
    mod-ffi.c
    t-struct.c
    t-routine.c
]

comment [
    switch user-config/with-ffi [
        'static 'dynamic [
            for-each var [includes cflags searches ldflags][
                x: rebmake/pkg-config
                    try any [user-config/pkg-config {pkg-config}]
                    var
                    %libffi
                if not empty? x [
                    set (in cfg-ffi var) x
                ]
            ]

            libs: rebmake/pkg-config
                try any [user-config/pkg-config {pkg-config}]
                'libraries
                %libffi

            cfg-ffi/libraries: map-each lib libs [
                make rebmake/ext-dynamic-class [
                    output: lib
                    flags: either user-config/with-ffi = 'static [[static]][_]
                ]
            ]
        ]
        'none [
            noop
        ]

        panic [
            "WITH-FFI should be one of [dynamic static none]"
            "not" (user-config/with-ffi)
        ]
    ]
]

searches: []

ldflags: []

libraries: [%ffi]
