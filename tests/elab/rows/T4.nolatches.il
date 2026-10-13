# generated: elab-check --write
attribute \top 1
module \t4_at_ident
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \d
  wire output 4 \q
  wire output 5 \r
  wire $n6
  wire $n7
  wire $n8
  cell $and $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \d
    connect \B \en
    connect \Y $n8
  end
  process $proc$T4_event_without_parens.v:3$6
    assign $n6 \d
    sync always
      update \q $n6
  end
  process $proc$T4_event_without_parens.v:5$8
    assign $n7 $n8
    sync posedge \clk
      update \r $n7
  end
end
