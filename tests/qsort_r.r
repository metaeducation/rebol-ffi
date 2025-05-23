REBOL [
    Title: "Demo tunneling of Cell* through routine to callback in FFI"

    Description: --{
        There are two versions of quicksort in the C library.  Plain `qsort`
        is written in such a way that if your comparator needs any information
        besides the two items to compare, it has to get that from global
        variables.  `qsort_r` takes an additional void pointer parameter
        which it passes through to the comparator, which could hold some
        state information and thus eliminate the requirement to use global
        variables for any parameterization of the comparator.

        This demonstrates the use of the FFI argument type of a value Cell.
        While the purpose of the FFI is to talk to libraries that likely
        are not linked to any Rebol APIs (and hence would not be able to
        make use of a Cell), this shows the use of it to "tunnel" a
        Rebol value through to a written-in-Rebol comparator callback.
    }--

    See-Also: --{
        More details about callbacks are mentioned in the demo file for the
        "plain" qsort, in the %qsort.r demo file.
    }--
]

recycle:torture

; Note: Plain qsort demo tests WRAP-CALLBACK independently.
;
cb: make-callback:fallback [
    return: [int64]
    a [pointer]
    b [pointer]
    arg [rebval]
][
    print ["Inside cb:" a, b, mold arg]

    assert [integer? a]
    assert [integer? b]
    assert [tag? arg]

    comment [
        fail "testing fallback behavior"
    ]

    let ai: make struct! compose:deep [
        [raw-memory: (a)]
        i [int32]
    ]
    let bi: make struct! compose:deep [
        [raw-memory: (b)]
        i [int32]
    ]
    return case [
        ai.i = bi.i [0]
        ai.i < bi.i [-1]
        <default> [
            assert [ai.i > bi.i]
            <- 1
        ]
     ]
] 0

libc: make library! %libc.so.6

x64?: 40 = fifth system.version
size_t: either x64? ['int64] ['int32]

qsort_r: make-routine libc "qsort_r" compose:deep [
    base [pointer]
    nmemb [(size_t)]
    size [(size_t)]
    comp [pointer]
    arg [rebval]
]

array: make vector! [integer! 32 5 [10 8 2 9 5]]
print ["before:" mold array]
probe (address of cb/)
qsort_r array 5 4 cb/ <A Tunneled Tag>
print ["after:" mold array]

assert [array = make vector! [integer! 32 5 [2 5 8 9 10]]]

close libc
