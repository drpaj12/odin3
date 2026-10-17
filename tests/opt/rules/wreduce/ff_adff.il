attribute \top 1
module \ff_adff
  wire input 1 \clk
  wire input 2 \r
  wire width 4 input 3 \a
  wire width 4 input 4 \b
  wire width 8 output 5 \q
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  cell $add $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { $n43 $n42 $n41 $n40 $n39 }
  end
  cell $adff $c14
    parameter \WIDTH 5
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 5'01010
    connect \CLK \clk
    connect \ARST \r
    connect \D { $n43 $n42 $n41 $n40 $n39 }
    connect \Q { $n48 $n47 $n46 $n45 $n44 }
  end
  connect \q { 3'000 $n48 $n47 $n46 $n45 $n44 }
end
