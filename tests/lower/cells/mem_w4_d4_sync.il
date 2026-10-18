attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \we
  wire width 2 input 3 \wa
  wire width 4 input 4 \wd
  wire width 2 input 5 \ra
  wire width 4 output 6 \rd
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n56
  cell $mem $c11
    parameter \MEMID "\\mem"
    parameter \SIZE 4
    parameter \OFFSET 0
    parameter \ABITS 2
    parameter \WIDTH 4
    parameter \RD_PORTS 1
    parameter \WR_PORTS 1
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'1
    parameter \WR_CLK_POLARITY 1'1
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \ra
    connect \RD_DATA { $n18 $n17 $n16 $n15 }
    connect \WR_CLK \clk
    connect \WR_EN { $n56 $n56 $n56 $n56 }
    connect \WR_ADDR { $n20 $n19 }
    connect \WR_DATA { $n26 $n25 $n24 $n23 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 2
    connect \A 2'xx
    connect \B \wa
    connect \S \we
    connect \Y { $n20 $n19 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \wd
    connect \S \we
    connect \Y { $n26 $n25 $n24 $n23 }
  end
  cell $dff $c15
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n18 $n17 $n16 $n15 }
    connect \Q \rd
  end
  attribute \full_case 1
  cell $mux $c16
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n56
  end
end
