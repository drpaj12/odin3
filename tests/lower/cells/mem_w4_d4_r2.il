attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \we
  wire input 3 \we2
  wire width 2 input 4 \wa
  wire width 2 input 5 \wa2
  wire width 4 input 6 \wd
  wire width 4 input 7 \wd2
  wire width 2 input 8 \ra
  wire width 2 input 9 \rb
  wire width 4 output 10 \rd
  wire width 4 output 11 \rd2
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n69
  cell $mem $c16
    parameter \MEMID "\\mem"
    parameter \SIZE 4
    parameter \OFFSET 0
    parameter \ABITS 2
    parameter \WIDTH 4
    parameter \RD_PORTS 2
    parameter \WR_PORTS 1
    parameter \RD_CLK_ENABLE 2'00
    parameter \RD_CLK_POLARITY 2'00
    parameter \RD_TRANSPARENT 2'00
    parameter \WR_CLK_ENABLE 1'1
    parameter \WR_CLK_POLARITY 1'1
    parameter \INIT 0'
    connect \RD_CLK 2'xx
    connect \RD_EN 2'11
    connect \RD_ADDR { \rb \ra }
    connect \RD_DATA { $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 }
    connect \WR_CLK \clk
    connect \WR_EN { $n69 $n69 $n69 $n69 }
    connect \WR_ADDR { $n37 $n36 }
    connect \WR_DATA { $n43 $n42 $n41 $n40 }
  end
  attribute \full_case 1
  cell $mux $c17
    parameter \WIDTH 2
    connect \A 2'xx
    connect \B \wa
    connect \S \we
    connect \Y { $n37 $n36 }
  end
  attribute \full_case 1
  cell $mux $c18
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \wd
    connect \S \we
    connect \Y { $n43 $n42 $n41 $n40 }
  end
  attribute \full_case 1
  cell $mux $c20
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n69
  end
  connect \rd { $n31 $n30 $n29 $n28 }
  connect \rd2 { $n35 $n34 $n33 $n32 }
end
