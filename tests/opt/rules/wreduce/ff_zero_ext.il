attribute \top 1
module \ff_zero_ext
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 8 output 4 \q
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  cell $add $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { $n39 $n38 $n37 $n36 $n35 }
  end
  cell $dff $c10
    parameter \WIDTH 5
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n39 $n38 $n37 $n36 $n35 }
    connect \Q { $n44 $n43 $n42 $n41 $n40 }
  end
  connect \q { 3'000 $n44 $n43 $n42 $n41 $n40 }
end
