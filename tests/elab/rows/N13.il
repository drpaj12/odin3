# generated: elab-check --write
attribute \top 1
module \n13_word_bit_select
  wire width 2 input 1 \i
  wire width 3 input 2 \j
  wire output 3 \y
  wire width 4 output 4 \p
  wire width 4 output 5 \q
  wire width 2 output 6 \r
  wire $n17
  wire $n18
  wire $n19
  wire $n20
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
  wire $n32
  wire $n33
  wire $n34
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
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  cell $shiftx $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A { $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 }
    connect \B \j
    connect \Y \y
  end
  cell $shiftx $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 2
    connect \A { $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 }
    connect \B \j
    connect \Y \r
  end
  cell $mem $c12
    parameter \MEMID "\\m"
    parameter \SIZE 4
    parameter \OFFSET 0
    parameter \ABITS 32
    parameter \WIDTH 8
    parameter \RD_PORTS 4
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 4'0000
    parameter \RD_CLK_POLARITY 4'0000
    parameter \RD_TRANSPARENT 4'0000
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 0'
    connect \RD_CLK 4'xxxx
    connect \RD_EN 4'1111
    connect \RD_ADDR { 30'000000000000000000000000000000 \i 62'00000000000000000000000000000010000000000000000000000000000000 \i 30'000000000000000000000000000000 \i }
    connect \RD_DATA { $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  connect \p { $n28 $n27 $n26 $n25 }
  connect \q { $n40 $n39 $n38 $n37 }
end
