# FFI Extension for Ren-C

FFI support in Rebol originated with Rebol2's paid version, "Rebol/Command":

  https://www.rebol.com/docs/library.html

It wasn't part of the Rebol3 open-source release, but Shixin Zeng of Atronix
Engineering implemented a version of it, described in this 2019 video:

  https://www.youtube.com/watch?v=fMeTqPyrNF4

That implementation was baked in to the core of Atronix/R3.  This is a
migration of that code out of the core, and into an extension for Ren-C.

Time available to support this code is limited, as it's not a key focus for
the Ren-C roadmap.  However, it's meant to be kept running as a useful and
non-trivial example of the extension mechanism.


## Alternatives to FFI

Almost always, a more rigorous form of interoperability can be achieved by
building C code as an extension linked to the interpreter core.  This provides
more robust interaction than using brittle FFI calls from a GC-based language
with unmanaged pointers.  (This is true of basically every language's FFI.)

However, this means needing to have access to a compiler.  If not using a
compiler is desired, another simple way that frequently works for triggering
"foreign" functionality is to use CALL to invoke a command-line program that
can perform the needed action.  It can then write any returned information to
a file (or stdout).
