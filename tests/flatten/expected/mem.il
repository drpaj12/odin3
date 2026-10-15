module \top
  wire input 1 \clk
  wire input 2 \we
  wire width 4 input 3 \addr
  wire width 8 input 4 \d
  wire width 8 output 5 \q1
  wire width 8 output 6 \q2
  wire \u1.clk
  wire \u1.we
  wire width 4 \u1.addr
  wire width 8 \u1.d
  wire width 8 \u1.q
  wire \u2.clk
  wire \u2.we
  wire width 4 \u2.addr
  wire width 8 \u2.d
  wire width 8 \u2.q
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
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
  wire $n78
  wire $n79
  wire $n80
  wire $n81
  wire $n82
  wire $n83
  wire $n84
  wire $n85
  wire $n86
  wire $n87
  wire $n88
  wire $n89
  wire $n90
  wire $n91
  wire $n92
  wire $n93
  wire $n94
  wire $n95
  wire $n96
  wire $n97
  wire $n98
  cell $not $c8
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \we
    connect \Y \u2.we
  end
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \d
    connect \Y \u2.d
  end
  cell $mem $c14
    parameter \MEMID "\\u1.mem"
    parameter \SIZE 16
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
    connect \RD_ADDR \addr
    connect \RD_DATA { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 }
    connect \WR_CLK \clk
    connect \WR_EN { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 }
    connect \WR_ADDR { $n54 $n53 $n52 $n51 }
    connect \WR_DATA { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \addr
    connect \S \we
    connect \Y { $n54 $n53 $n52 $n51 }
  end
  attribute \full_case 1
  cell $mux $c16
    parameter \WIDTH 8
    connect \A 8'xxxxxxxx
    connect \B \d
    connect \S \we
    connect \Y { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
  end
  attribute \full_case 1
  cell $mux $c17
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'11111111
    connect \S \we
    connect \Y { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 }
  end
  cell $dff $c18
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 }
    connect \Q \q1
  end
  cell $mem $c19
    parameter \MEMID "\\u2.mem"
    parameter \SIZE 16
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
    connect \RD_ADDR \addr
    connect \RD_DATA { $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 }
    connect \WR_CLK \clk
    connect \WR_EN { $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 }
    connect \WR_ADDR { $n82 $n81 $n80 $n79 }
    connect \WR_DATA { $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 }
  end
  attribute \full_case 1
  cell $mux $c20
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \addr
    connect \S \u2.we
    connect \Y { $n82 $n81 $n80 $n79 }
  end
  attribute \full_case 1
  cell $mux $c21
    parameter \WIDTH 8
    connect \A 8'xxxxxxxx
    connect \B \u2.d
    connect \S \u2.we
    connect \Y { $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 }
  end
  attribute \full_case 1
  cell $mux $c22
    parameter \WIDTH 8
    connect \A 8'00000000
    connect \B 8'11111111
    connect \S \u2.we
    connect \Y { $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 }
  end
  cell $dff $c23
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 }
    connect \Q \q2
  end
  connect \u1.clk \clk
  connect \u1.we \we
  connect \u1.addr \addr
  connect \u1.d \d
  connect \u1.q \q1
  connect \u2.clk \clk
  connect \u2.addr \addr
  connect \u2.q \q2
end
