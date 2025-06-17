REBOL [
    Title: "Small GTK3 Smoke Test"
    Author: "Shixin Zeng"

    Description: --{
        This is a minimal GTK3 test, just enough to bring up a window and
        show it counting when you click a button.

        Shixin demonstrated a fuller GTK3 application based on an automated
        import of GTK's interfaces at the 2019 conference:

          https://youtu.be/fMeTqPyrNF4?t=1259
    }--

    Notes: --{
      * GTK4 was released in late 2020, but GTK3 is still in wide use by major
        projects in 2025 with no pressing plans to migrate, e.g. Chrome,
        Firefox, Inkscape, GIMP, and Handbrake.

      * GTK3 integer/boolean types are 32-bit by design, to keep API/ABI
        stability across platforms and ensure consistent behavior between
        32-bit and 64-bit systems.
    }--
]

recycle:torture

=== LOCATE LIBRARIES ===

libgtk: any [
    try make library! %libgtk-3.so
    try make library! %libgtk-3.so.0
    fail "Couldn't find libgtk-3.so"
]

libglib: any [
    try make library! %libglib-2.0.so
    try make library! %libglib-2.0.so.0
    fail "Couldn't find libglib-2.0.so"
]

libgob: any [
    try make library! %libgobject-2.0.so
    try make library! %libgobject-2.0.so.0
    fail "Couldn't find libgobject-2.0.so"
]


=== DEFINE GTK ROUTINE INTERFACES ===

; This is a small subset of GTK definitions extracted for this demo.  The full
; definitions of GTK are translated by a script:
;
;   https://github.com/metaeducation/c2r3

gtk-init:
    make-routine libgtk "gtk_init" [
        argc [pointer]
        argv [pointer]
    ]

gtk-window-new:
    make-routine libgtk "gtk_window_new" [
        type [int32]
        return: [pointer]
    ]

gtk-window-set-default-size:
    make-routine libgtk "gtk_window_set_default_size" [
        windown [pointer]
        width [int32]
        height [int32]
        return: [void]
    ]

gtk-window-set-resizable:
    make-routine libgtk "gtk_window_set_resizable" [
        window [pointer]
        resizable [int32]
        return: [void]
    ]

gtk-window-set-title:
    make-routine libgtk "gtk_window_set_title" [
        win [pointer]
        title [pointer]
    ]

gtk-widget-show:
    make-routine libgtk "gtk_widget_show" [
        widget [pointer]
    ]

gtk-hbox-new:
    make-routine libgtk "gtk_hbox_new" [
        homogeneous [int32]  ; gboolean, but FFI likely maps that to int
        spacing [int32]
        return: [pointer]
    ]

gtk-box-pack-start:
    make-routine libgtk "gtk_box_pack_start" [
        box [pointer]
        child [pointer]
        expand [uint8]
        fill [uint8]
        padding [uint32]
        return: [pointer]
    ]

gtk-box-set-spacing:
    make-routine libgtk "gtk_box_set_spacing" [
        box [pointer]
        spacing [int32]
        return: [void]
    ]

gtk-box-get-spacing:
    make-routine libgtk "gtk_box_get_spacing" [
        box [pointer]
        return: [int32]
    ]

gtk-toggle-button-new-with-label:
    make-routine libgtk "gtk_toggle_button_new_with_label" [
        label [pointer]
        return: [pointer]
    ]

gtk-font-button-new:
    make-routine libgtk "gtk_font_button_new" [
        return: [pointer]
    ]

gtk-font-chooser-widget-new:
    make-routine libgtk "gtk_font_chooser_widget_new" [
        return: [pointer]
    ]

gtk-font-chooser-set-font:
    make-routine libgtk "gtk_font_chooser_set_font" [
        fontchooser [pointer]
        fontname [pointer]
    ]

gtk-color-button-new:
    make-routine libgtk "gtk_color_button_new" [
        return: [pointer]
    ]

gtk-main:
    make-routine libgtk "gtk_main" []

gtk-main-quit:
    make-routine libgtk "gtk_main_quit" []

g-signal-connect-data:
    make-routine libgob "g_signal_connect_data" [
        instance [pointer]
        detailed-signal [pointer]
        c-handler [pointer]
        data [pointer]
        destroy-data [pointer]
        connect-flags [int32]
        return: [int64]
    ]

g-signal-connect: func [
    instance [integer!]
    detailed-signal [integer! text! binary!]
    c-handler [action!]
    data [null? integer!]
][
    g-signal-connect-data instance detailed-signal c-handler/ data 0 0
]

gtk-button-new-with-label:
    make-routine libgtk "gtk_button_new_with_label" [
        label [pointer]
        return: [pointer]
    ]

gtk-button-set-label:
    make-routine libgtk "gtk_button_set_label" [
        button [pointer]
        label [pointer]
    ]

gtk-container-add:
    make-routine libgtk "gtk_container_add" [
        container [pointer]
        elem [pointer]
    ]


=== GTK INIT HELPER ===

; The GTK3 initialization function takes an optional ARGC and an ARGV, for the
; purpose of slipstreaming GTK settings from your command line into GTK's
; behavior.
;
; It scans for GTK-specific arguments (like --gtk-debug, --display, --sync),
; and removes them from the argument list. That way, your program can still
; process its own command-line arguments after GTK has had a chance to look at
; and remove the ones it cares about.
;
; GTK modifies argc and argv in-place. For example, if argv originally had 5
; arguments, and GTK consumed 2 of them, the remaining list will contain 3
; arguments (and argc will be updated accordingly).  It does not free any
; memory and will only reduce the count of argc and remove pointers from
; the argv array.
;
; GTK4 operates completely differently.  There is no gtk_init() function.
;
; So we could ignore this and pass nulls, the original script passed pointers
; to argv and argc, so that's kept to show at least one way of doing this.
; It also allows us to pass "--gtk-debug".

init-gtk: function [app] [
    let arg0: make struct! compose:deep [
        appn [uint8 [(1 + length of app)]]
    ]
    arg0.appn: append (encode 'UTF-8 app) #{00}

    let flag: "--gtk-debug"
    let arg1: make struct! compose:deep [
        flagn [uint8 [(1 + length of flag)]]
    ]
    arg1.flagn: append (encode 'UTF-8 flag) #{00}

    let argv: make struct! [
        args [pointer [3]]  ; C standard requires null termination
    ]

    ; !!! At time of writing this is broken.  argv.args returns a BLOCK! that
    ; gets synthesized from the values.  But then (argv.args).1: winds up
    ; doing an assignment into that transient block, which does not affect
    ; the original data.  This concept of "subcell addressing" is likely best
    ; addressed by making the VECTOR! type able to map directly onto the
    ; FFI memory as the pick step... though it would have to be fixed-size
    ; and prohibit expansions/etc.
    ;
    print ["assigning pointers:" (address of arg0) (address of arg1)]
    print ["arg.args is:" mold argv.args]
    argv.args.1: address of arg0
    argv.args.2: address of arg1
    argv.args.3: 0

    print ["argv:" argv]
    let argc: make struct! [
        c: [int32] 1
    ]

    let addr-argv: make struct! [
        addr: [pointer] (address of argv)
    ]

    print ["addr-argv:" addr-argv]
    print ["addr of addr-argv:" address of addr-argv]

    gtk-init (address of argc) (address of addr-argv)
    print ["argc:" argc "argv:" argv]
]


=== TEST APPLICATION ===

on-click-callback: make-callback [
    widget [pointer]
    data   [pointer]
][
    print ["clicked"]
    let i: make struct! compose:deep [
        [
            raw-memory: (data)
            raw-size: 4
        ]
        i [int32]
    ]
    i.i: i.i + 1
    gtk-button-set-label widget spaced [
        "clicked" i.i (either i.i = 1 ["time"] ["times"])
    ]
]

app-quit-callback: make-callback [
][
    print ["app quiting"]
    gtk-main-quit
]

GTK_WINDOW_TOPLEVEL: 0
GTK_WINDOW_POPUP: 1

init-gtk system.options.boot
print ["gtk initialized"]

win: gtk-window-new GTK_WINDOW_TOPLEVEL
gtk-window-set-default-size win 10 10
gtk-window-set-resizable win 1
print ["win:" win]
g-signal-connect win "destroy" app-quit-callback/ NULL
gtk-window-set-title win "gtk+ from rebol"

hbox: gtk-hbox-new 0 0  ; could pass spacing here
gtk-box-set-spacing hbox 10

gtk-container-add win hbox

but1: gtk-button-new-with-label "button 1"
gtk-box-pack-start hbox but1 1 1 0
print ["hbox spacing:" gtk-box-get-spacing hbox]

n-clicked: make struct! [i: [int32] 0]
g-signal-connect but1 "clicked" on-click-callback/ (address of n-clicked)

but2: gtk-button-new-with-label "button 2"
gtk-box-pack-start hbox but2 1 1 0

but3: gtk-toggle-button-new-with-label "toggle"
gtk-box-pack-start hbox but3 1 1 0

comment [  ; larger font chooser, presumably commented out for being too big
    font-chooser: gtk-font-chooser-widget-new
    gtk-box-pack-start hbox font-chooser 1 1 1  ; last flag is expand/fill
    gtk-font-chooser-set-font font-chooser "Times Bold 18"
]

font-button: gtk-font-button-new
gtk-box-pack-start hbox font-button 1 1 0

color-button: gtk-color-button-new
gtk-box-pack-start hbox color-button 1 1 0

gtk-widget-show color-button
gtk-widget-show font-button
gtk-widget-show but1
gtk-widget-show but2
gtk-widget-show but3
gtk-widget-show hbox
gtk-widget-show win
gtk-main
