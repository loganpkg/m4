```
Copyright (c) 2021 Logan Ryan McLintock

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

m4 macro processor
==================

A cross-platform implementation of the m4 macro processor.
It has many nice features:

* A single file of source code written in ANSI C that is easy to compile.
* High performance hash table with the built-in macro `htdist` that shows the
  distribution.
* Stack depth only limited by random-access memory (RAM).
* Can be used interactively.
* Implements all the original m4 built-in commands, except that `syscmd` is
  replaced by `esyscmd` and `eval` is replaced by `add`, `mult`, `sub`, `div`
  and `mod`. Does not follow the POSIX standards.

Install
-------
By default the esyscmd and maketemp built-in macros are excluded.
Set `ESYSCMD_MAKETEMP` to 1 to include them.
To compile:
```
$ cc -g -O3 m4.c && mv a.out m4
```
or
```
> cl /Ot m4.c
```
and place the executable somewhere in your PATH.

To use
------
```
$ m4 [file...]
```

References
----------

* Brian W. Kernighan and Dennis M. Ritchie, The M4 Macro Processor,
  Bell Laboratories, Murray Hill, New Jersey 07974, July 1, 1977.

Section 6.6 of:

* Brian W. Kernighan and Dennis M. Ritchie, The C Programming Language,
  Second Edition, Prentice Hall Software Series, New Jersey, 1988.
* Clovis L. Tondo and Scott E. Gimpel, The C Answer Book, Second Edition,
  PTR Prentice Hall Software Series, New Jersey, 1989.

Daniel J. Bernstein's djb2 algorithm from:
 * Hash Functions, http://www.cse.yorku.ca/~oz/hash.html

Mini-tutorial
-------------
These are the built-in macros, presented as a mini-tutorial:
``
changequote([, ])
define(cool, $1 and $2)
cool(goat, mice)
undefine([cool])
define(cool, wow)
dumpdef([cool], [y], [define])
hello dnl this will be removed
divnum
divert(2)
divnum
cool
divert(6)
divnum
y
undivert(2)
divert
undivert
incr(76)
len(goat)
index(elephant, ha)
substr(elephant, 2, 4)
translit(bananas, abcs, xyz)
ifdef([cool], yes defined, not defined)
define(y, 5)
ifelse(y, 5, true, false)
dnl By default the esyscmd and maketemp built-in macros are excluded
esyscmd(ifelse(dirsep, /, ls, dir))
esyscmd(echo hello > .test)
include(.test)
maketemp(XXXXXX)
errprint(oops there is an error)
htdist
add(8, 2, 4)
mult( , 5, , 3)
sub(80, 20, 5)
div(5, 2)
mod(5, 2)
```

Enjoy,
Logan =)_
