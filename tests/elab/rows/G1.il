# generated: elab-check --write
attribute \top 1
module \g1_region
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 output 3 \y
  wire output 4 \q
  wire width 4 \t
  wire $n15
  cell $not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \Y \t
  end
  process $proc$G1_generate_region.v:10$6
    assign $n15 \a [0]
    sync posedge \clk
      update \q $n15
  end
  connect \y \t
end
