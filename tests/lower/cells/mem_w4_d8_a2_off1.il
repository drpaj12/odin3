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
  wire $n21
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n55
  cell $mem $c11
    parameter \MEMID "\\mem"
    parameter \SIZE 8
    parameter \OFFSET 1
    parameter \ABITS 3
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
    connect \RD_ADDR { 1'0 \ra }
    connect \RD_DATA { $n18 $n17 $n16 $n15 }
    connect \WR_CLK \clk
    connect \WR_EN { $n55 $n55 $n55 $n55 }
    connect \WR_ADDR { $n21 $n20 $n19 }
    connect \WR_DATA { $n28 $n27 $n26 $n25 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 3
    connect \A 3'xxx
    connect \B { 1'0 \wa }
    connect \S \we
    connect \Y { $n21 $n20 $n19 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \wd
    connect \S \we
    connect \Y { $n28 $n27 $n26 $n25 }
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n55
  end
  connect \rd { $n18 $n17 $n16 $n15 }
end
