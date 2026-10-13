# generated: elab-check --write
attribute \top 1
module \n9_memory
  wire input 1 \clk
  wire input 2 \we
  wire width 4 input 3 \addr
  wire width 8 input 4 \d
  wire width 8 output 5 \y1
  wire width 8 output 6 \y2
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
  cell $mem $c11
    parameter \MEMID "\\mem"
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
    connect \RD_DATA { $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 }
    connect \WR_CLK \clk
    connect \WR_EN { $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 }
    connect \WR_ADDR { $n50 $n49 $n48 $n47 }
    connect \WR_DATA { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 }
  end
  cell $mem $c12
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
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \addr
    connect \RD_DATA { $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  process $proc$N9_memories.v:5$10
    assign { $n50 $n49 $n48 $n47 } { $n54 $n53 $n52 $n51 }
    assign { $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 } { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 }
    assign { $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 } { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 }
    switch \we
      case 1'1
        assign { $n54 $n53 $n52 $n51 } \addr
        assign { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 } \d
        assign { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 } 8'11111111
      case
        assign { $n54 $n53 $n52 $n51 } 4'xxxx
        assign { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 } 8'xxxxxxxx
        assign { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 } 8'00000000
    end
    sync posedge \clk
  end
  connect \y1 { $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 }
  connect \y2 { $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
end
