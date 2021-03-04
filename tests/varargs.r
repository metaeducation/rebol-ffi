REBOL []

recycle/torture

libc: switch fourth system/version [
    3 [
        make library! %msvcrt.dll
    ]
    4 [
        make library! %libc.so.6
    ]
]

printf: make-routine libc "printf" [
    "An example of wrapping variadic functions"
    fmt [pointer] "fixed"
    ... "variadic"
    return: [int32]
]

sprintf: make-routine libc "sprintf" [
    "An example of wrapping variadic functions"
    buf [pointer] "destination buffer, must be big enough"
    fmt [pointer] "fixed"
    ... "variadic"
    return: [int32]
]

i: 1000
j: make struct! [x [double]]
j/x: 12.34
(printf
    join "1. i: %d, %f" newline
    i [int64]
    j [struct! [x [double]]]
)

(printf "2. hello %p%c"
    "ffi" [pointer]
    (to integer! newline) [int8]
)

| printf
    "3. hello %s%c"
    "world" [pointer]
    (to integer! newline) [int8]
|

do compose [
    printf
    "4. hello %s%c"
    "ffi" [pointer]
    (to integer! newline) [int8]
]

h: make struct! [
    a [uint8 [128]]
]
len: (sprintf
    addr-of h
    join "hello %s" newline
    "world" [pointer]
)

print ["5. h:" copy/part to text! values of h len]
