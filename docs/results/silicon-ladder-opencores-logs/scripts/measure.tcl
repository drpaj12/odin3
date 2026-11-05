# measure.tcl: env PDK OUT CLK. Reads final.odb; prints MEASURE key=value lines.
set pdk $::env(PDK); set out $::env(OUT); set clk $::env(CLK)
set ref $pdk/libs.ref/sky130_fd_sc_hd
read_lef $ref/techlef/sky130_fd_sc_hd__nom.tlef
read_lef $ref/lef/sky130_fd_sc_hd.lef
read_liberty $ref/lib/sky130_fd_sc_hd__tt_025C_1v80.lib
read_db $out/or/final.odb
set blk [ord::get_db_block]
set dbu [expr {double([$blk getDbUnitsPerMicron])}]
set die [$blk getDieArea]; set core [$blk getCoreArea]
puts "MEASURE die_um2=[expr {[$die dx]/$dbu*[$die dy]/$dbu}]"
puts "MEASURE core_um2=[expr {[$core dx]/$dbu*[$core dy]/$dbu}]"
set a 0.0; set nf 0.0; set n 0; set nl 0
foreach i [$blk getInsts] {
  set m [$i getMaster]; set mn [$m getName]
  set ar [expr {([$m getWidth] / $dbu) * ([$m getHeight] / $dbu)}]
  set a [expr {$a + $ar}]
  incr n
  if {![regexp {__(fill|decap|tap|diode)} $mn]} { set nf [expr {$nf + $ar}]; incr nl }
}
puts "MEASURE inst_area_all_um2=$a"
puts "MEASURE logic_area_um2=$nf"
puts "MEASURE inst_count_all=$n"
puts "MEASURE logic_count=$nl"
if {$clk ne ""} {
  create_clock -name clk -period 10 [get_ports $clk]
  set_wire_rc -signal -layer met2
  set_wire_rc -clock -layer met5
  estimate_parasitics -placement
  puts "MEASURE wns_ns=[sta::worst_slack -max]"
  puts "MEASURE tns_ns=[sta::total_negative_slack -max]"
  puts "MEASURE hold_ws_ns=[sta::worst_slack -min]"
}
puts "MEASURE done=1"
exit
