attribute \top 1
module \t
  wire input 1 \c
  wire input 2 \we
  wire width 3 input 3 \wa
  wire width 3 input 4 \ra
  wire width 4 input 5 \d
  wire width 8 output 6 \q
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
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
    connect \RD_DATA { $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 }
    connect \WR_CLK \c
    connect \WR_EN { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 }
    connect \WR_ADDR { $n31 $n30 $n29 }
    connect \WR_DATA { $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 3
    connect \A 3'xxx
    connect \B \wa
    connect \S \we
    connect \Y { $n31 $n30 $n29 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 8
    connect \A 8'xxxxxxxx
    connect \B { 4'0000 \d }
    connect \S \we
    connect \Y { $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 }
  end
  attribute \full_case 1
  cell $mux $c14
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'00001111
    connect \S \we
    connect \Y { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 }
  end
  connect \q { $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 }
end
