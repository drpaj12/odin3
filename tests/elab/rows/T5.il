# generated: elab-check --write
attribute \top 1
module \t5_initial
  wire input 1 \clk
  wire width 4 input 2 \addr
  wire width 8 output 3 \y
  wire width 8 \r
  wire width 32 signed \i
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  wire $n66
  wire $n67
  wire $n68
  wire $n69
  wire $n70
  wire $n71
  wire $n72
  wire $n73
  wire $n74
  wire $n75
  wire $n76
  wire $n77
  cell $xor $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A { $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 }
    connect \B { $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 }
    connect \Y { $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 }
  end
  cell $xor $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A { $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 }
    connect \B 8'00000101
    connect \Y \y
  end
  cell $mem $c9
    parameter \MEMID "\\mem"
    parameter \SIZE 16
    parameter \OFFSET 0
    parameter \ABITS 4
    parameter \WIDTH 8
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 128'00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \addr
    connect \RD_DATA { $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  cell $mem $c10
    parameter \MEMID "\\rom"
    parameter \SIZE 16
    parameter \OFFSET 0
    parameter \ABITS 4
    parameter \WIDTH 8
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 128'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx1111000011011110101111001001101001111000000001000000001000000001
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \addr
    connect \RD_DATA { $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  connect \r 8'00000101
  connect \i 32'00000000000000000000000000010000
end
