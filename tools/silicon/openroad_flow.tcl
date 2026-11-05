# Step 2: OpenROAD flow, own Tcl, no Yosys. Env: PDK, OUT, TOP, CLK (clock port or empty).
set pdk $::env(PDK); set out $::env(OUT); set top $::env(TOP); set clk $::env(CLK)
set ref $pdk/libs.ref/sky130_fd_sc_hd
set orx $out/or
file mkdir $orx
set stepf [open $orx/steps.txt w]
set t0 [clock seconds]

proc dump_insts {file step} {
  set f [open $file w]
  puts $f "inst,master,x,y,orient,status,step"
  foreach i [[ord::get_db_block] getInsts] {
    set b [$i getBBox]
    puts $f "[$i getName],[[$i getMaster] getName],[$b xMin],[$b yMin],[$i getOrient],[$i getPlacementStatus],$step"
  }
  close $f
}
proc snap {name} {
  global orx stepf t0
  dump_insts $orx/$name.csv $name
  puts $stepf "$name,[expr {[clock seconds]-$t0}]"; flush $stepf
  puts "STEP $name"
}

read_lef $ref/techlef/sky130_fd_sc_hd__nom.tlef
read_lef $ref/lef/sky130_fd_sc_hd.lef
read_liberty $ref/lib/sky130_fd_sc_hd__tt_025C_1v80.lib
read_verilog $out/clean.v
link_design $top
if {$clk ne ""} { create_clock -name clk -period 10 [get_ports $clk] }
snap s00_link
proc cell_area_um2 {} {
  set blk [ord::get_db_block]
  set dbu [expr {double([$blk getDbUnitsPerMicron])}]
  set a 0.0
  foreach i [$blk getInsts] {
    set m [$i getMaster]
    set a [expr {$a + ([$m getWidth] / $dbu) * ([$m getHeight] / $dbu)}]
  }
  return $a
}

# A design of one cell or none gives a core with no site row at 25 percent utilization (the PDN
# step then stops on "empty command name"): use a fixed 30 um core for it. Others are unchanged.
set n_logic [llength [[ord::get_db_block] getInsts]]  ;# before tap cells
# Rows shorter than the tap distance (14 um) get no tap cell, so the wells (VPB, VNB) are tied to
# no supply and LVS sees nets of their own (Q-L13: 8 LVS mismatches): a core predicted below
# 16 um a side gets the fixed core as well.
set core_pred [expr {sqrt([cell_area_um2] / 0.25)}]
if {$n_logic <= 1 || $core_pred < 16.0} {
  set hi 36.0
  puts "WARNING precheck: $n_logic cells, predicted core side [format %.1f $core_pred] um: fixed core"
  initialize_floorplan -die_area "0 0 42 42" \
    -core_area "6 6 $hi $hi" -site unithd
} else {
  initialize_floorplan -utilization 25 -aspect_ratio 1 -core_space 6 -site unithd
}
# Precheck: the pins sit on the die edge, about 0.9 per um of perimeter (Prob023: 153 on 168 um).
# Size the die from the pin count when the floorplan gives too few; the log line is reported.
set n_pins [llength [[ord::get_db_block] getBTerms]]
set die [[ord::get_db_block] getDieArea]
set die_side [expr {min([$die dx], [$die dy]) / double([[ord::get_db_block] getDbUnitsPerMicron])}]
set need_side [expr {ceil($n_pins / 0.8 / 4.0) + 12}]
if {$need_side > $die_side} {
  puts "WARNING precheck: $n_pins pins need a die side of $need_side um, floorplan gave [format %.1f $die_side]: die enlarged"
  initialize_floorplan -die_area "0 0 $need_side $need_side" \
    -core_area "6 6 [expr {$need_side - 6}] [expr {$need_side - 6}]" -site unithd
}
# Die edge on the track grid (des, DRT-0255): the router gives a pin on the die edge an access
# point on the last track inside the die, and the via enclosure there sticks out of the die when
# that track is closer than about 0.15 um to the edge (a die side of 732.615 um left 0.065 um to
# the last met2 track and 0.085 um to the last met1 track; every output pin on the right edge
# then failed with DRT-0255 "Maze Route cannot find path"). The die (not the core, so the
# placement is unchanged) grows to the next edge with at least 0.17 um (the margin on the
# left and bottom edges) to the last track of met1 to met4, in x and in y.
proc edge_ok {d} {
  foreach {off pitch} {0.17 0.34 0.23 0.46 0.34 0.68 0.46 0.92} {
    set k [expr {floor(($d - $off) / $pitch + 1e-9)}]
    if {$d - ($off + $k * $pitch) < 0.17} { return 0 }
  }
  return 1
}
proc snap_edge {d} {
  set d [expr {round($d * 1000) / 1000.0}]
  while {![edge_ok $d]} { set d [expr {round(($d + 0.005) * 1000) / 1000.0}] }
  return $d
}
set blk_ [ord::get_db_block]
set dbu_ [expr {double([$blk_ getDbUnitsPerMicron])}]
set die_ [$blk_ getDieArea]
set core_ [$blk_ getCoreArea]
set dx_ [expr {[$die_ xMax] / $dbu_}]
set dy_ [expr {[$die_ yMax] / $dbu_}]
set sx_ [snap_edge $dx_]
set sy_ [snap_edge $dy_]
if {$sx_ != $dx_ || $sy_ != $dy_} {
  puts "WARNING precheck: die edge [format %.3f $dx_] x [format %.3f $dy_] um is within 0.17 um of a routing track: die enlarged to [format %.3f $sx_] x [format %.3f $sy_] um (core unchanged)"
  initialize_floorplan -die_area "0 0 $sx_ $sy_" \
    -core_area "[expr {[$core_ xMin] / $dbu_}] [expr {[$core_ yMin] / $dbu_}] [expr {[$core_ xMax] / $dbu_}] [expr {[$core_ yMax] / $dbu_}]" -site unithd
}
proc tracks {} {
  make_tracks li1 -x_offset 0.23 -x_pitch 0.46 -y_offset 0.17 -y_pitch 0.34
  make_tracks met1 -x_offset 0.17 -x_pitch 0.34 -y_offset 0.17 -y_pitch 0.34
  make_tracks met2 -x_offset 0.23 -x_pitch 0.46 -y_offset 0.23 -y_pitch 0.46
  make_tracks met3 -x_offset 0.34 -x_pitch 0.68 -y_offset 0.34 -y_pitch 0.68
  make_tracks met4 -x_offset 0.46 -x_pitch 0.92 -y_offset 0.46 -y_pitch 0.92
  make_tracks met5 -x_offset 1.70 -x_pitch 3.40 -y_offset 1.70 -y_pitch 3.40
}
tracks
snap s01_floorplan

tapcell -distance 14 -tapcell_master sky130_fd_sc_hd__tapvpwrvgnd_1
snap s02_tapcell

add_global_connection -net VDD -pin_pattern {^VPWR$} -power
add_global_connection -net VDD -pin_pattern {^VPB$} -power
add_global_connection -net VSS -pin_pattern {^VGND$} -ground
add_global_connection -net VSS -pin_pattern {^VNB$} -ground
set_voltage_domain -name CORE -power VDD -ground VSS
define_pdn_grid -name grid -pins {met4 met5}
add_pdn_stripe -layer met1 -width 0.48 -followpins
add_pdn_stripe -layer met4 -width 1.6 -pitch 20 -offset 3
add_pdn_stripe -layer met5 -width 1.6 -pitch 20 -offset 3
add_pdn_connect -layers {met1 met4}
add_pdn_connect -layers {met4 met5}
pdngen
snap s03_pdn

# RePlAce diverges (GPL-0304) on a design of one logic cell or none: put the cell at the core
# centre and let detailed_placement legalize it (the tap cells are fixed already).
if {$n_logic <= 1} {
  puts "SMALL_DESIGN $n_logic cells: centre placement, no global_placement"
  set core [[ord::get_db_block] getCoreArea]
  foreach i [[ord::get_db_block] getInsts] {
    $i setOrient R0
    $i setLocation [expr {([$core xMin] + [$core xMax]) / 2}] [expr {([$core yMin] + [$core yMax]) / 2}]
    $i setPlacementStatus PLACED
  }
  place_pins -hor_layers met3 -ver_layers met2
} else {
  global_placement -skip_io -density 0.5
  place_pins -hor_layers met3 -ver_layers met2
  global_placement -density 0.5
}
snap s04_global_place

set_wire_rc -signal -layer met2
set_wire_rc -clock -layer met5
estimate_parasitics -placement
repair_design
snap s05_repair_design

detailed_placement
check_placement -verbose
snap s06_detailed_place

if {$clk ne ""} {
  clock_tree_synthesis -root_buf sky130_fd_sc_hd__clkbuf_4 -buf_list sky130_fd_sc_hd__clkbuf_4
  set_propagated_clock [all_clocks]
  detailed_placement
  snap s07_cts
  estimate_parasitics -placement
  repair_timing -hold
  detailed_placement
  snap s08_repair_timing
}

set_routing_layers -signal met1-met5 -clock met1-met5
global_route -guide_file $orx/route.guide -congestion_iterations 30
snap s09_global_route
detailed_route -output_drc $orx/drc.rpt -bottom_routing_layer met1 -top_routing_layer met5 -verbose 0
snap s10_detail_route
filler_placement {sky130_fd_sc_hd__fill_8 sky130_fd_sc_hd__fill_4 sky130_fd_sc_hd__fill_2 sky130_fd_sc_hd__fill_1}
snap s11_fill
write_def $orx/route.def
write_verilog $orx/final.v
write_db $orx/final.odb
close $stepf
exit
