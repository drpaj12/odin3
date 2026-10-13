attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \s
  wire width 4 input 4 \d
  wire width 4 input 5 \e
  wire width 4 output 6 \q
  wire width 4 output 7 \r
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  cell $add $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \q
    connect \B 4'0001
    connect \Y { $n23 $n22 $n21 $n20 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 4
    connect \A \e
    connect \B \d
    connect \S \s
    connect \Y { $n37 $n36 $n35 $n34 }
  end
  cell $mux $c13
    parameter \WIDTH 4
    connect \A \e
    connect \B { $n37 $n36 $n35 $n34 }
    connect \S \en
    connect \Y { $n27 $n26 $n25 $n24 }
  end
  cell $dff $c14
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n23 $n22 $n21 $n20 }
    connect \Q \q
  end
  cell $dff $c15
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n27 $n26 $n25 $n24 }
    connect \Q \r
  end
end
