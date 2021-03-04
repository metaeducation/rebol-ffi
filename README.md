# FFI Extension (Work In Progress) for Ren-C

This repository contains code for Ren-C.  It was adapted from the Atronix code
for R3-Alpha, which is documented in this video:

https://www.youtube.com/watch?v=fMeTqPyrNF4

The code was working in July 2019.  But as the project focus shifted to the
web, limited development resources mean that desktop-only features like FFI
have no owners.  Hence the code has atrophied, and is broken out from the
maintained extensions into an archival repository.

There are no scheduled plans to invest in the code.  But the design is overall
relatively sound.  Anyone wanting FFI interoperability in Ren-C should
*definitely* start from this extension and information presented in the video.


## Alternatives to FFI

Almost always, a more rigorous form of interoperability can be achieved by
building C code as an extension linked to the interpreter core.  This provides
more robust interaction than using brittle FFI calls from a GC-based language
with unmanaged pointers.  (This is true of basically every language FFI.)

However, this means needing to have access to a compiler.  If not using a
compiler is desired, another simple way that frequently works for triggering
"foreign" functionality is to use CALL to invoke a command-line program that
can perform the needed action.  It can then write any returned information to
a file (or stdout).


## Discussion

https://forum.rebol.info/t/putting-aside-the-ffi-for-now/1537
