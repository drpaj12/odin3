# generated: elab-check --write
attribute \top 1
module \t7_blocking
  wire input 1 \clk
  wire input 2 \a
  wire input 3 \b
  wire output 4 \y
  wire output 5 \r
  wire \t
  wire $n7
  wire $n8
  wire $n9
  wire $n10
  wire $n11
  cell $xor $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n8
  end
  cell $and $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n11
  end
  process $proc$T7_blocking.v:4$7
    assign $n7 $n8
    sync always
      update \y $n7
  end
  process $proc$T7_blocking.v:6$9
    assign $n10 $n11
    assign $n9 $n11
    sync posedge \clk
      update \r $n9
      update \t $n10
  end
end
