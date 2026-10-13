# generated: elab-check --write
attribute \top 1
module \g3_gen_items
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 output 3 \y
  wire width 4 output 4 \z
  wire width 4 output 5 \w
  wire \lane[0].t
  wire \lane[0].r
  wire \lane[1].t
  wire \lane[1].r
  wire \lane[2].t
  wire \lane[2].r
  wire \lane[3].t
  wire \lane[3].r
  wire \genblk2[0].n
  wire \genblk2[1].n
  wire \genblk2[2].n
  wire \genblk2[3].n
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
  wire $n102
  wire $n103
  wire $n104
  wire $n105
  wire $n106
  wire $n107
  wire $n108
  wire $n109
  wire $n110
  wire $n111
  wire $n112
  wire $n113
  wire $n114
  wire $n115
  wire $n116
  wire $n117
  wire $n118
  wire $n119
  wire $n120
  wire $n121
  wire $n122
  wire $n123
  wire $n124
  wire $n125
  wire $n126
  wire $n127
  wire $n128
  wire $n129
  wire $n130
  wire $n131
  wire $n132
  wire $n133
  wire $n134
  wire $n135
  wire $n136
  wire $n137
  wire $n138
  wire $n139
  wire $n140
  wire $n141
  wire $n142
  wire $n143
  wire $n144
  wire $n145
  wire $n146
  wire $n147
  wire $n148
  wire $n149
  wire $n150
  wire $n151
  wire $n152
  wire $n153
  wire $n154
  wire $n155
  wire $n156
  wire $n157
  wire $n158
  wire $n159
  wire $n160
  wire $n161
  wire $n162
  wire $n163
  cell \g3_leaf \lane[0].u
    connect \d \a [0]
    connect \q \z [0]
  end
  cell \g3_leaf \lane[1].u
    connect \d \a [1]
    connect \q \z [1]
  end
  cell \g3_leaf \lane[2].u
    connect \d \a [2]
    connect \q \z [2]
  end
  cell \g3_leaf \lane[3].u
    connect \d \a [3]
    connect \q \z [3]
  end
  cell $xor $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a [0]
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 }
  end
  cell $xor $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a [1]
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 $n66 $n65 $n64 }
  end
  cell $xor $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a [2]
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 $n96 }
  end
  cell $xor $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a [3]
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n159 $n158 $n157 $n156 $n155 $n154 $n153 $n152 $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 }
  end
  cell $not $c16
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [0]
    connect \Y \genblk2[0].n
  end
  cell $not $c17
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [1]
    connect \Y \genblk2[1].n
  end
  cell $not $c18
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [2]
    connect \Y \genblk2[2].n
  end
  cell $not $c19
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a [3]
    connect \Y \genblk2[3].n
  end
  process $proc$G3_generate_block_items.v:13$20
    assign $n160 $n32
    sync posedge \clk
      update \lane[0].r $n160
  end
  process $proc$G3_generate_block_items.v:13$21
    assign $n161 $n64
    sync posedge \clk
      update \lane[1].r $n161
  end
  process $proc$G3_generate_block_items.v:13$22
    assign $n162 $n96
    sync posedge \clk
      update \lane[2].r $n162
  end
  process $proc$G3_generate_block_items.v:13$23
    assign $n163 $n128
    sync posedge \clk
      update \lane[3].r $n163
  end
  connect \y { \lane[3].r \lane[2].r \lane[1].r \lane[0].r }
  connect \w { \genblk2[3].n \genblk2[2].n \genblk2[1].n \genblk2[0].n }
  connect \lane[0].t $n32
  connect \lane[1].t $n64
  connect \lane[2].t $n96
  connect \lane[3].t $n128
end
module \g3_leaf
  wire input 1 \d
  wire output 2 \q
  cell $not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \d
    connect \Y \q
  end
end
