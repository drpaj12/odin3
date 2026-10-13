# generated: elab-check --write
attribute \top 1
module \t8_nonblocking
  wire input 1 \clk
  wire input 2 \d
  wire width 4 input 3 \v
  wire output 4 \q1
  wire output 5 \q2
  wire width 4 output 6 \qv
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  process $proc$T8_nonblocking.v:3$7
    assign $n13 \d
    assign $n14 \q1
    assign { $n18 $n17 $n16 $n15 } \v
    sync posedge \clk
      update \q1 $n13
      update \q2 $n14
      update \qv { $n18 $n17 $n16 $n15 }
  end
end
