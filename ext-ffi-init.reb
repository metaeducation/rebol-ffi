REBOL [
    Title: "FFI Extension"
    Name: FFI
    Type: Module
    Version: 1.0.0
    License: "Apache 2.0"

    Notes: --{
        The FFI was not initially implemented with any usermode code.  But
        just as with the routines in the SYS context, there's opportunity for
        replacing some of the non-performance-critical C that does parsing and
        processing into Rebol.  This is especially true since FFI was changed
        to use fewer specialized structures to represent ROUTINE! and
        STRUCT!, instead using arrays...to permit it to be factored into an
        extension.
    }--
]

ffi-type-mappings: [
    void [<opt>]

    uint8 [integer!]
    int8 [integer!]
    uint16 [integer!]
    int16 [integer!]
    uint32 [integer!]
    int32 [integer!]
    uint64 [integer!]

    float [decimal!]
    double [decimal!]

    ; Note: ACTION! is only legal to pass to pointer arguments if it is was
    ; created with MAKE-ROUTINE or WRAP-CALLBACK
    ;
    pointer [integer! text! binary! vector! action!]

    rebval [any-value?]

    ; ...struct...
]


; The idea of MAKE-CALLBACK is to allow you to write something like:
;
;     callback: make-callback [
;         return: [int64]
;         a [pointer]
;         b [pointer]
;     ][
;         ; body of function, Rebol code
;     ]
;
; And it will produce an ACTION! with legal Rebol types, e.g.
;
;     temp: function [
;         return: [integer!]
;         a [integer!]
;         b [integer!]
;     ][
;         ; body of function, Rebol code
;     ]
;
; Then it will wrap that callback up with the FFI interface:
;
;     wrap-callback temp/ [return: [int64] a [pointer] b [pointer]]
;
; So it just saves you the extra step.
;
; (At time of writing it does not map the types, it just creates the function
; with no types, e.g. (function [a b] [...]), but that's easy enough.)
;
export make-callback: function [
    "Helper for WRAP-CALLBACK that auto-generates the action to be wrapped"

    return: [action!]
    args [block!]
    body [block!]
    :fallback "If untrapped panic occurs during callback, return value"
        [any-value?]
][
    let r-args: copy []

    ; !!! TBD: Use type mappings to mark up the types of the Rebol arguments,
    ; so that HELP will show useful types.
    ;
    let arg-rule: [
        let a: word! (append r-args a)
        block!
        opt text!
    ]

    ; !!! TBD: Should check fallback value for compatibility here, e.g.
    ; make sure [return: [pointer]] has a fallback value that's an INTEGER!.
    ; Because if an improper type is given as the reaction to an error, that
    ; just creates *another* error...so you'll still get a crash() anyway.
    ; Better to just FAIL during the MAKE-CALLBACK call so the interpreter
    ; does not crash.
    ;
    let attr-rule: [
        set-word?/ block!
            |
        word!
            |
        a: across [tag! some word!] (append r-args spread a)
    ]

    parse args [
        opt text!
        opt some [arg-rule | attr-rule]
        <end>
    ] except [
        panic ["Unrecognized pattern in MAKE-CALLBACK function spec" args]
    ]

    comment [
        print ["r-args:" mold r-args]
    ]

    ; Wrapping process is a bit more complex in modern binding, because the
    ; block we get is already bound, and we need some mechanism to invasively
    ; "overbind" it or it will not see the function's arguments:
    ;
    ; https://rebol.metaeducation.com/t/func-variant-that-auto-returns/2124
    ;
    let safe: function r-args
        (if fallback [
            compose2:deep @{} inside body '[
                trap [return {as group! bindable body}] then error -> [
                    print "** TRAPPED CRITICAL ERROR DURING FFI CALLBACK:"
                    print mold error
                    return {^fallback}
                ]
            ]
        ] else [
            body
        ])

    parse args [  ; !!! remove all the locals?
        opt some [
            remove [tag! some word!]
            | one
        ]
        <end>
    ]

    comment [
        print ["args:" mold args]
    ]

    return wrap-callback safe/ args
]
