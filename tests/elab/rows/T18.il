# generated: elab-check --write
attribute \top 1
module \t18_block_decls
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 output 3 \q
  wire width 4 output 4 \n
  wire width 4 \body.t
  wire width 32 signed \body.i
  attribute \keep 1
  wire \body.u
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
  wire $n99
  wire $n100
  wire $n101
  cell $add $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000010
    connect \Y { $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 $n66 }
  end
  cell $xor $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n69
    connect \B $n66
    connect \Y $n98
  end
  cell $xor $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n68
    connect \B $n66
    connect \Y $n99
  end
  cell $xor $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n67
    connect \B $n66
    connect \Y $n100
  end
  cell $xor $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n66
    connect \B $n66
    connect \Y $n101
  end
  process $proc$T18_block_declarations.v:3$12
    assign { $n62 $n61 $n60 $n59 } { $n69 $n68 $n67 $n66 }
    assign $n63 $n66
    assign $n55 $n98
    assign $n56 $n99
    assign $n57 $n100
    assign $n58 $n101
    assign { $n54 $n53 $n52 $n51 } { $n69 $n68 $n67 $n66 }
    sync posedge \clk
      update \q { $n54 $n53 $n52 $n51 }
      update \n { $n58 $n57 $n56 $n55 }
      update \body.t { $n62 $n61 $n60 $n59 }
      update \body.u $n63
  end
end
