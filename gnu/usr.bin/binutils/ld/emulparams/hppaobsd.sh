. ${srcdir}/emulparams/hppaelf.sh
 
#override hppaelf.sh
SCRIPT_NAME=elf
ELFSIZE=32
OUTPUT_FORMAT="elf32-hppa"

# other necessary defines, similar but not the same as linux.
MAXPAGESIZE=0x1000
ENTRY="__start"
MACHINE=hppa1.1    # We use 1.1 specific features.
OTHER_READONLY_SECTIONS=".PARISC.unwind ${RELOCATING-0} : { *(.PARISC.unwind) }"
DATA_START_SYMBOLS='PROVIDE ($global$ = .);'
DATA_NONEXEC_PLT=
GENERATE_SHLIB_SCRIPT=yes

. ${srcdir}/emulparams/elf_obsd.sh
