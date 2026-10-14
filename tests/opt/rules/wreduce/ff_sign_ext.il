attribute \top 1
module \ff_sign_ext
  wire input 1 \clk
  wire width 4 input 2 \d
  wire width 8 output 3 \q
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  cell $dff $c6
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d
    connect \Q { $n25 $n24 $n23 $n22 }
  end
  connect \q { $n25 $n25 $n25 $n25 $n25 $n24 $n23 $n22 }
end
