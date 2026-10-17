attribute \top 1
module \ff_unused
  wire input 1 \clk
  wire width 8 input 2 \d
  wire width 4 output 3 \y
  wire width 8 \q
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  cell $dff $c6
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d [3:0]
    connect \Q { $n33 $n32 $n31 $n30 }
  end
  connect \y { $n33 $n32 $n31 $n30 }
  connect \q [3:0] { $n33 $n32 $n31 $n30 }
end
