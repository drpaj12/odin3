attribute \top 1
module \ff_sdff
  wire input 1 \clk
  wire input 2 \r
  wire width 4 input 3 \a
  wire width 4 input 4 \b
  wire width 8 output 5 \q
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  cell $add $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { $n50 $n49 $n48 $n47 $n46 }
  end
  cell $sdff $c15
    parameter \WIDTH 5
    parameter \CLK_POLARITY 1
    parameter \SRST_POLARITY 1
    parameter \SRST_VALUE 5'00101
    connect \CLK \clk
    connect \SRST \r
    connect \D { $n50 $n49 $n48 $n47 $n46 }
    connect \Q { $n55 $n54 $n53 $n52 $n51 }
  end
  connect \q { 3'000 $n55 $n54 $n53 $n52 $n51 }
end
