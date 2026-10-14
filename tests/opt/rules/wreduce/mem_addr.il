attribute \top 1
module \mem_addr
  wire input 1 \clk
  wire width 8 input 2 \addr
  wire width 8 input 3 \d
  wire input 4 \we
  wire width 8 output 5 \q
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n118
  wire $n127
  wire $n128
  wire $n129
  wire $n130
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 8
    connect \A 8'xxxxxxxx
    connect \B \d
    connect \S \we
    connect \Y { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 }
  end
  cell $dff $c14
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 }
    connect \Q \q
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n118
  end
  cell $mem $c16
    parameter \MEMID "\\mem"
    parameter \SIZE 10
    parameter \OFFSET 0
    parameter \ABITS 4
    parameter \WIDTH 8
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
    connect \RD_ADDR \addr [3:0]
    connect \RD_DATA { $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 }
    connect \WR_CLK \clk
    connect \WR_EN { $n118 $n118 $n118 $n118 $n118 $n118 $n118 $n118 }
    connect \WR_ADDR { $n130 $n129 $n128 $n127 }
    connect \WR_DATA { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 }
  end
  attribute \full_case 1
  cell $mux $c17
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \addr [3:0]
    connect \S \we
    connect \Y { $n130 $n129 $n128 $n127 }
  end
end
