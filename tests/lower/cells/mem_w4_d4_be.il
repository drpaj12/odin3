attribute \top 1
module \m
  wire input 1 \clk
  wire width 2 input 2 \we
  wire width 2 input 3 \wa
  wire width 4 input 4 \wd
  wire width 2 input 5 \ra
  wire width 4 output 6 \rd
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n40
  wire $n41
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n83
  wire $n84
  cell $mem $c11
    parameter \MEMID "\\mem"
    parameter \SIZE 4
    parameter \OFFSET 0
    parameter \ABITS 2
    parameter \WIDTH 4
    parameter \RD_PORTS 1
    parameter \WR_PORTS 2
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 2'11
    parameter \WR_CLK_POLARITY 2'11
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \ra
    connect \RD_DATA { $n19 $n18 $n17 $n16 }
    connect \WR_CLK { \clk \clk }
    connect \WR_EN { $n84 $n84 4'0000 $n83 $n83 }
    connect \WR_ADDR { $n41 $n40 $n21 $n20 }
    connect \WR_DATA { $n47 $n46 $n45 $n44 $n27 $n26 $n25 $n24 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 2
    connect \A 2'xx
    connect \B \wa
    connect \S \we [0]
    connect \Y { $n21 $n20 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { 2'00 \wd [1:0] }
    connect \S \we [0]
    connect \Y { $n27 $n26 $n25 $n24 }
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 2
    connect \A 2'xx
    connect \B \wa
    connect \S \we [1]
    connect \Y { $n41 $n40 }
  end
  attribute \full_case 1
  cell $mux $c16
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { \wd [3:2] 2'xx }
    connect \S \we [1]
    connect \Y { $n47 $n46 $n45 $n44 }
  end
  attribute \full_case 1
  cell $mux $c18
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we [0]
    connect \Y $n83
  end
  attribute \full_case 1
  cell $mux $c19
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we [1]
    connect \Y $n84
  end
  connect \rd { $n19 $n18 $n17 $n16 }
end
