#!/bin/bash

ctags -R --exclude=*.asm .

find . -name "*.[chS]" > cscope.files
cscope -b -q -i cscope.files
