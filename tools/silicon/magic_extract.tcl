# Magic: GDS -> extracted SPICE. Env: OUT TOP MODE (flat|hier). flat: std cells flattened to transistors.
set out $::env(OUT); set top $::env(TOP); set mode $::env(MODE)
if {$mode eq "flat"} { gds flatglob *__* }
gds read $out/$top.gds
load $top
select top cell
extract do local
extract all
ext2spice lvs
if {$mode eq "hier"} { ext2spice hierarchy on } else { ext2spice hierarchy off }
ext2spice subcircuit top on
ext2spice -o $out/ext_$mode.spice
quit -noprompt
