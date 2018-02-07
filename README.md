-----------------------------------------------------------------------------
ECM (Error Code Modeler) encode/decode utilities
Version 1.0
Copyright 2002 Neill Corlett
Distributed under the terms of the GNU GPL; read source code for details
-----------------------------------------------------------------------------

Introduction
------------

The ECM format allows you to reduce the size of a typical CD image file
(BIN, CDI, NRG, CCD, or any other format that uses raw sectors; results may
vary).

It works by eliminating the Error Correction/Detection Codes (ECC/EDC) from
each sector whenever possible.  The encoder automatically adjusts to
different sector types and automatically skips any headers it encounters.

The results will vary depending on how much redundant ECC/EDC data is
present.  Note that for "cooked" ISO files, there will be no reduction.


Setup / Usage
-------------

Compile ecm.c and unecm.c if necessary, or use the included Win32 EXE files.

Run ECM with no parameters to see a simple usage reference:

    usage: ecm cdimagefile [ecmfile]

Where "cdimagefile" is the name of the CD image file, and "ecmfile"
(optional) is the name of the ECM file.  If you don't specify ecmfile, it
defaults to cdimagefile plus a .ecm suffix.

UNECM works the same way, but in reverse:

    usage: unecm [--cue] ecmfile [outputfile]

"ecmfile" must end in .ecm.  If outputfile is not specified, it defaults
to ecmfile minus the .ecm suffix.

including --cue allows to create a .cue file


Thanks to
---------

ProtoCat for inspiring me to write this.


Where to find me
----------------

email: corlett@lfx.org