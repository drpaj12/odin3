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

initialize_floorplan -utilization 25 -aspect_ratio 1 -core_space 6 -site unithd
make_tracks li1 -x_offset 0.23 -x_pitch 0.46 -y_offset 0.17 -y_pitch 0.34
make_tracks met1 -x_offset 0.17 -x_pitch 0.34 -y_offset 0.17 -y_pitch 0.34
make_tracks met2 -x_offset 0.23 -x_pitch 0.46 -y_offset 0.23 -y_pitch 0.46
make_tracks met3 -x_offset 0.34 -x_pitch 0.68 -y_offset 0.34 -y_pitch 0.68
make_tracks met4 -x_offset 0.46 -x_pitch 0.92 -y_offset 0.46 -y_pitch 0.92
make_tracks met5 -x_offset 1.70 -x_pitch 3.40 -y_offset 1.70 -y_pitch 3.40
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

global_placement -skip_io -density 0.5
place_pins -hor_layers met3 -ver_layers met2
global_placement -density 0.5
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
