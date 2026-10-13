attribute \top 1
module \t
  wire input 1 \c
  wire input 2 \we
  wire width 3 input 3 \wa
  wire width 3 input 4 \ra
  wire width 8 input 5 \d
  wire width 8 output 6 \q
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  cell $mem $c11
    parameter \MEMID "\\m"
    parameter \SIZE 8
    parameter \OFFSET 0
    parameter \ABITS 3
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
    connect \RD_ADDR \ra
    connect \RD_DATA { $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
    connect \WR_CLK \c
    connect \WR_EN { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
    connect \WR_ADDR { $n35 $n34 $n33 }
    connect \WR_DATA { $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 3
    connect \A 3'xxx
    connect \B \wa
    connect \S \we
    connect \Y { $n35 $n34 $n33 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 8
    connect \A 8'xxxxxxxx
    connect \B \d
    connect \S \we
    connect \Y { $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
  end
  attribute \full_case 1
  cell $mux $c14
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'11111111
    connect \S \we
    connect \Y { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
  end
  connect \q { $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
end
