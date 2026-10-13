# generated: elab-check --write
attribute \top 1
module \f2_while
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 output 3 \c
  wire width 32 output 4 \y
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  process $proc$F2_while.v:16$7
    assign { $n47 $n46 $n45 $n44 } \a
    sync posedge \clk
      update \c { $n47 $n46 $n45 $n44 }
  end
  connect \y 32'00000000000000000000000000000100
end
