# generated: elab-check --write
attribute \top 1
module \t1_edges
  wire input 1 \clk
  wire input 2 \rst
  wire input 3 \rst_n
  wire input 4 \d
  wire output 5 \q1
  wire output 6 \q2
  wire output 7 \q3
  wire output 8 \q4
  wire $n9
  wire $n10
  wire $n11
  wire $n14
  wire $n15
  cell $logic_not $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst_n
    connect \Y $n15
  end
  process $proc$T1_edge_events.v:3$9
    assign $n9 \d
    sync posedge \clk
      update \q1 $n9
  end
  process $proc$T1_edge_events.v:5$10
    assign $n10 \d
    sync negedge \clk
      update \q2 $n10
  end
  process $proc$T1_edge_events.v:7$13
    assign $n11 \q3
    switch \rst
      case 1'1
        assign $n11 1'0
      case
        assign $n11 \d
    end
    sync posedge \clk
      update \q3 $n11
    sync posedge \rst
      update \q3 $n11
  end
  process $proc$T1_edge_events.v:12$15
    assign $n14 \q4
    switch $n15
      case 1'1
        assign $n14 1'0
      case
        assign $n14 \d
    end
    sync posedge \clk
      update \q4 $n14
    sync negedge \rst_n
      update \q4 $n14
  end
end
